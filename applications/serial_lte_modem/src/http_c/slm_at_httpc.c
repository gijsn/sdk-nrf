/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <logging/log.h>
#include <zephyr.h>
#include <stdio.h>
#include <net/socket.h>
#include <net/tls_credentials.h>
#include <net/http_client.h>
#include <nrf_socket.h>
#include "slm_at_host.h"
#include "slm_at_httpc.h"
#include "slm_util.h"
#include "slm_native_tls.h"

LOG_MODULE_REGISTER(slm_httpc, CONFIG_SLM_LOG_LEVEL);

#define HTTPC_METHOD_LEN	20
#define HTTPC_RES_LEN		256
#define HTTPC_HEADER_LEN	1024
#define HTTPC_REQ_LEN		(HTTPC_METHOD_LEN + HTTPC_RES_LEN + HTTPC_HEADER_LEN + 3)
#define HTTPC_FRAG_SIZE		NET_IPV4_MTU
#define HTTPC_BUF_LEN		2048
#if HTTPC_REQ_LEN > HTTPC_BUF_LEN
# error "Please specify larger HTTPC_BUF_LEN"
#endif
#define HTTPC_REQ_TO_S		10

/* Buffers for HTTP client. */
static uint8_t data_buf[HTTPC_BUF_LEN];

/**@brief HTTP connect operations. */
enum slm_httpccon_operation {
	HTTPC_DISCONNECT,
	HTTPC_CONNECT,
	HTTPC_CONNECT6
};

/**@brief HTTP client state */
enum httpc_state {
	HTTPC_INIT,
	HTTPC_REQ_DONE,
	HTTPC_RES_HEADER_DONE,
	HTTPC_COMPLETE
};

static struct slm_httpc_ctx {
	int fd;				/* HTTPC socket */
	int family;			/* Socket address family */
	uint32_t sec_tag;		/* security tag to be used */
	char host[SLM_MAX_URL + 1];	/* HTTP server address */
	uint16_t port;			/* HTTP server port */
	char *method_str;		/* request method */
	char *resource;			/* resource */
	char *headers;			/* headers */
	size_t pl_len;			/* payload length */
	size_t total_sent;		/* payload has been sent to server */
	size_t rsp_header_length;	/* Length of headers in HTTP response */
	size_t rsp_body_length;		/* Length of body in HTTP response */
	enum httpc_state state;		/* HTTPC state */
	
} httpc;

/* global variable defined in different resources */
extern struct at_param_list at_param_list;
extern char rsp_buf[SLM_AT_CMD_RESPONSE_MAX_LEN];

static struct k_thread httpc_thread;
#define HTTPC_THREAD_STACK_SIZE       KB(2)
#define HTTPC_THREAD_PRIORITY         K_LOWEST_APPLICATION_THREAD_PRIO
static K_THREAD_STACK_DEFINE(httpc_thread_stack, HTTPC_THREAD_STACK_SIZE);

static K_SEM_DEFINE(http_req_sem, 0, 1);


static void response_cb(struct http_response *rsp,
			enum http_final_call final_data,
			void *user_data)
{
	ARG_UNUSED(user_data);

	/* Process response header if required */
	if (httpc.state >= HTTPC_REQ_DONE && httpc.state < HTTPC_COMPLETE) {
		if (httpc.state != HTTPC_RES_HEADER_DONE) {
			/* Look for end of response headers */
			if (rsp->body_start) {
				size_t headers_len = rsp->body_start - rsp->recv_buf;
				/* Send last chunk of headers and URC */
				sprintf(rsp_buf, "#XHTTPHEADERS: ");
				rsp_send(rsp_buf, strlen(rsp_buf));
				data_send(rsp->recv_buf, headers_len);
				httpc.rsp_header_length += headers_len;
				sprintf(rsp_buf, "\r\n#XHTTPCRSP: %d,%hu\r\n",
					httpc.rsp_header_length,
					final_data);
				rsp_send(rsp_buf, strlen(rsp_buf));
				httpc.state = HTTPC_RES_HEADER_DONE;
				/* Send first chunk of body */
				sprintf(rsp_buf, "#XHTTPRESPONSE: ");
				rsp_send(rsp_buf, strlen(rsp_buf));
				data_send(rsp->recv_buf + headers_len,
					  rsp->data_len - headers_len);
				httpc.rsp_body_length += rsp->data_len - headers_len;
			} else {
				/* All headers */
				data_send(rsp->recv_buf, rsp->data_len);
				httpc.rsp_header_length += rsp->data_len;
			}
		} else {
			/* All body */
			data_send(rsp->recv_buf, rsp->data_len);
			httpc.rsp_body_length += rsp->data_len;
		}
	}

	if (final_data == HTTP_DATA_FINAL) {
		sprintf(rsp_buf, "\r\n#XHTTPCRSP: %d,%hu\r\n", httpc.rsp_body_length, final_data);
		rsp_send(rsp_buf, strlen(rsp_buf));
		httpc.state = HTTPC_COMPLETE;
	}
	LOG_DBG("Response data received (%zd bytes)", rsp->data_len);
}

static int headers_cb(int sock, struct http_request *req, void *user_data)
{
	size_t len;
	int ret = 0, offset = 0;

	len = strlen(httpc.headers);
	while (len > 0) {
		ret = send(sock, httpc.headers + offset, len, 0);
		if (ret < 0) {
			LOG_ERR("send header fail: %d", ret);
			return ret;
		}
		LOG_DBG("send header: %d bytes", ret);
		len -= ret;
		offset += ret;
	}

	return len;
}

int do_send_payload(const uint8_t *data, int len)
{
	/* payload sent to server */
	ssize_t pl_sent = 0;
	/* payload to send to server */
	size_t pl_to_send = 0;

	if (data == NULL || len <= 0) {
		return -EINVAL;
	}

	/* Verity payload length to be sent */
	if (httpc.total_sent + len > httpc.pl_len) {
		LOG_WRN("send unexpected payload");
		pl_to_send = httpc.pl_len - httpc.total_sent;
	} else {
		pl_to_send = len;
	}

	/* Start to send payload */
	while (pl_sent < pl_to_send) {
		ssize_t ret;

		ret = send(httpc.fd, data + pl_sent,
			   MIN(pl_to_send - pl_sent, HTTPC_FRAG_SIZE), 0);
		if (ret < 0) {
			LOG_ERR("Fail to send payload: %d", ret);
			httpc.total_sent = -errno;
			k_sem_give(&http_req_sem);
			return -errno;
		}
		LOG_DBG("send %d bytes payload", ret);
		pl_sent += ret;
		httpc.total_sent += ret;
	}

	if (httpc.total_sent == httpc.pl_len) {
		LOG_DBG("Successfully send %d bytes payload", httpc.total_sent);
		k_sem_give(&http_req_sem);
	}

	return 0;
}

int httpc_datamode_callback(uint8_t op, const uint8_t *data, int len)
{
	int ret = 0;

	if (data == NULL || len <= 0) {
		LOG_ERR("Wrong raw data");
		return -EINVAL;
	}
	if (op == DATAMODE_SEND) {
		ret = do_send_payload(data, len);
	} else if (op == DATAMODE_EXIT) {
		k_sem_give(&http_req_sem);
	}

	return ret;
}

static int payload_cb(int sock, struct http_request *req, void *user_data)
{
	if (httpc.pl_len > 0) {
		enter_datamode(httpc_datamode_callback);
		sprintf(rsp_buf, "\r\n#XHTTPCREQ: 1\r\n");
		rsp_send(rsp_buf, strlen(rsp_buf));
		/* Wait until all payload is sent */
		LOG_DBG("wait until payload is ready");
		k_sem_take(&http_req_sem, K_FOREVER);
	}
	httpc.state = HTTPC_REQ_DONE;
	sprintf(rsp_buf, "\r\n#XHTTPCREQ: 0\r\n");
	rsp_send(rsp_buf, strlen(rsp_buf));

	return httpc.total_sent;
}

static int do_http_connect(void)
{
	int ret;

	if (httpc.fd != INVALID_SOCKET) {
		LOG_ERR("Already connected to server.");
		return -EINVAL;
	}

	/* Open socket */
	if (httpc.sec_tag == INVALID_SEC_TAG) {
		ret = socket(httpc.family, SOCK_STREAM, IPPROTO_TCP);
	} else {
		ret = socket(httpc.family, SOCK_STREAM, IPPROTO_TLS_1_2);

	}
	if (ret < 0) {
		LOG_ERR("socket() failed: %d", -errno);
		return ret;
	}
	httpc.fd = ret;

	/* Set socket options */
	const uint32_t timeout_ms = HTTPC_REQ_TO_S * MSEC_PER_SEC;
	struct timeval timeo = {
		.tv_sec = (timeout_ms / 1000),
		.tv_usec = (timeout_ms % 1000) * 1000,
	};

	LOG_DBG("Configuring socket timeout (%ld s)", timeo.tv_sec);
	ret = setsockopt(httpc.fd, SOL_SOCKET, SO_SNDTIMEO, &timeo, sizeof(timeo));
	if (ret) {
		LOG_ERR("setsockopt(SO_SNDTIMEO) error: %d", -errno);
		ret = -errno;
		goto exit_cli;
	}
	ret = setsockopt(httpc.fd, SOL_SOCKET, SO_RCVTIMEO, &timeo, sizeof(timeo));
	if (ret) {
		LOG_ERR("setsockopt(SO_SNDTIMEO) error: %d", -errno);
		ret = -errno;
		goto exit_cli;
	}
	if (httpc.sec_tag != INVALID_SEC_TAG) {
		sec_tag_t sec_tag_list[] = { httpc.sec_tag };
		int peer_verify = TLS_PEER_VERIFY_REQUIRED;

		ret = setsockopt(httpc.fd, SOL_TLS, TLS_SEC_TAG_LIST, sec_tag_list,
				 sizeof(sec_tag_t));
		if (ret) {
			LOG_ERR("setsockopt(TLS_SEC_TAG_LIST) error: %d", -errno);
			ret = -errno;
			goto exit_cli;
		}
		ret = setsockopt(httpc.fd, SOL_TLS, TLS_PEER_VERIFY, &peer_verify,
				 sizeof(peer_verify));
		if (ret) {
			LOG_ERR("setsockopt(TLS_PEER_VERIFY) error: %d", -errno);
			ret = -errno;
			goto exit_cli;
		}
		ret = setsockopt(httpc.fd, SOL_TLS, TLS_HOSTNAME, httpc.host,
				 strlen(httpc.host) + 1);
		if (ret) {
			LOG_ERR("setsockopt(TLS_HOSTNAME) error: %d", -errno);
			ret = -errno;
			goto exit_cli;
		}
	}

	/* Connect to HTTP server */
	struct sockaddr sa = {
		.sa_family = AF_UNSPEC
	};

	ret = util_resolve_host(0, httpc.host, httpc.port, httpc.family, &sa);
	if (ret) {
		LOG_ERR("getaddrinfo() error: %s", log_strdup(gai_strerror(ret)));
		goto exit_cli;
	}
	if (sa.sa_family == AF_INET) {
		ret = connect(httpc.fd, &sa, sizeof(struct sockaddr_in));
	} else {
		ret = connect(httpc.fd, &sa, sizeof(struct sockaddr_in6));
	}

	if (ret) {
		LOG_ERR("connect() failed: %d", -errno);
		ret = -errno;
		goto exit_cli;
	}

	sprintf(rsp_buf, "\r\n#XHTTPCCON: 1\r\n");
	rsp_send(rsp_buf, strlen(rsp_buf));
	return 0;

exit_cli:
	close(httpc.fd);
	httpc.fd = INVALID_SOCKET;
	sprintf(rsp_buf, "\r\n#XHTTPCCON: 0\r\n");
	rsp_send(rsp_buf, strlen(rsp_buf));

	return ret;
}

static int do_http_disconnect(void)
{
	/* Close socket if it is connected. */
	if (httpc.fd != INVALID_SOCKET) {
		close(httpc.fd);
		httpc.fd = INVALID_SOCKET;
	}
	sprintf(rsp_buf, "\r\n#XHTTPCCON: 0\r\n");
	rsp_send(rsp_buf, strlen(rsp_buf));

	return 0;
}

static int http_method_str_enum(uint8_t *method_str)
{
	static const char * const method_strings[] = {
		"DELETE", "GET", "HEAD", "POST", "PUT", "CONNECT", "OPTIONS",
		"TRACE", "COPY", "LOCK", "MKCOL", "MOVE", "PROPFIND",
		"PROPPATCH", "SEARCH", "UNLOCK", "BIND", "REBIND", "UNBIND",
		"ACL", "REPORT", "MKACTIVITY", "CHECKOUT", "MERGE", "M-SEARCH",
		"NOTIFY", "SUBSCRIBE", "UNSUBSCRIBE", "PATCH", "PURGE",
		"MKCALENDAR", "LINK", "UNLINK"};

	for (int i = HTTP_DELETE; i <= HTTP_UNLINK; i++) {
		if (!strncmp(method_str, method_strings[i],
			HTTPC_METHOD_LEN)) {
			return i;
		}
	}
	return -1;
}

static int do_http_request(void)
{
	int err;
	struct http_request req;
	int method;
	int32_t timeout = SYS_FOREVER_MS;

	if (httpc.fd == INVALID_SOCKET) {
		LOG_ERR("Remote host is not connected.");
		return -EINVAL;
	}

	method = http_method_str_enum(httpc.method_str);
	if (method < 0) {
		LOG_ERR("Request method is not allowed.");
		return -EINVAL;
	}

	memset(&req, 0, sizeof(req));
	req.method = (enum http_method)method;
	req.url = httpc.resource;
	req.host = httpc.host;
	req.protocol = "HTTP/1.1";
	req.response = response_cb;
	req.recv_buf = data_buf;
	req.recv_buf_len = HTTPC_BUF_LEN;
	req.payload_cb =  payload_cb;
	req.optional_headers_cb = headers_cb;
	err = http_client_req(httpc.fd, &req, timeout, "");
	if (err < 0) {
		/* Socket send/recv error */
		sprintf(rsp_buf, "\r\n#XHTTPCREQ: %d\r\n", err);
		rsp_send(rsp_buf, strlen(rsp_buf));
	} else if (httpc.state != HTTPC_COMPLETE) {
		/* Socket was closed by remote */
		err = -ECONNRESET;
		sprintf(rsp_buf, "\r\n#XHTTPCRSP: 0,%d\r\n", err);
		rsp_send(rsp_buf, strlen(rsp_buf));
	} else {
		err = 0;
	}

	return err;
}

/**@brief handle AT#XHTTPCCON commands
 *  AT#XHTTPCCON=<op>[,<host>,<port>[,<sec_tag>]]
 *  AT#XHTTPCCON? READ command not supported
 *  AT#XHTTPCCON=?
 */
int handle_at_httpc_connect(enum at_cmd_type cmd_type)
{
	int err = -EINVAL;
	uint16_t op;
	size_t host_sz = SLM_MAX_URL;

	switch (cmd_type) {
	case AT_CMD_TYPE_SET_COMMAND:
		err = at_params_unsigned_short_get(&at_param_list, 1, &op);
		if (err) {
			return err;
		}
		if (op == HTTPC_CONNECT || op == HTTPC_CONNECT6) {
			err = util_string_get(&at_param_list, 2, httpc.host, &host_sz);
			if (err) {
				return err;
			}
			err = at_params_unsigned_short_get(&at_param_list, 3, &httpc.port);
			if (err) {
				return err;
			}
			httpc.sec_tag = INVALID_SEC_TAG;
			if (at_params_valid_count_get(&at_param_list) > 4) {
				err = at_params_unsigned_int_get(&at_param_list, 4,
							&httpc.sec_tag);
				if (err) {
					return err;
				}
			}
			httpc.family = (op == HTTPC_CONNECT) ? AF_INET : AF_INET6;
			err = do_http_connect();
			break;
		} else if (op == HTTPC_DISCONNECT) {
			err = do_http_disconnect();
		} else {
			err = -EINVAL;
		} break;

	case AT_CMD_TYPE_READ_COMMAND:
		if (httpc.sec_tag != INVALID_SEC_TAG) {
			sprintf(rsp_buf, "\r\n#XHTTPCCON: %d,\"%s\",%d,%d\r\n",
				(httpc.fd == INVALID_SOCKET) ? 0 : 1,
				httpc.host, httpc.port, httpc.sec_tag);
		} else {
			sprintf(rsp_buf, "\r\n#XHTTPCCON: %d,\"%s\",%d\r\n",
				(httpc.fd == INVALID_SOCKET) ? 0 : 1,
				httpc.host, httpc.port);
		}
		rsp_send(rsp_buf, strlen(rsp_buf));
		err = 0;
		break;

	case AT_CMD_TYPE_TEST_COMMAND:
		sprintf(rsp_buf,
			"\r\n#XHTTPCCON: (%d,%d,%d),<host>,<port>,<sec_tag>\r\n",
			HTTPC_DISCONNECT, HTTPC_CONNECT, HTTPC_CONNECT6);
		rsp_send(rsp_buf, strlen(rsp_buf));
		err = 0;
		break;

	default:
		break;
	}

	return err;
}

static void httpc_thread_fn(void *arg1, void *arg2, void *arg3)
{
	int err;

	err = do_http_request();
	(void)exit_datamode(DATAMODE_EXIT_URC);
	if (err < 0) {
		LOG_ERR("do_http_request fail:%d", err);
		/* Disconnect from server */
		err = do_http_disconnect();
		if (err) {
			LOG_ERR("Fail to disconnect. Error: %d", err);
		}
	}

	LOG_INF("HTTP thread terminated");
}

#define HTTP_CRLF_STR "\\r\\n"

static int http_headers_preprocess(size_t size)
{
	const char crlf_str[] = {'\r', '\n', '\0'};
	const char crlf_crlf_str[] = {'\r', '\n', '\r', '\n', '\0'};

	if (size == 0) {
		return 0;
	}

	char *http_crlf = strstr(httpc.headers, HTTP_CRLF_STR);
	int size_adjust = size;

	while (http_crlf) {
		char *tmp = http_crlf + sizeof(HTTP_CRLF_STR) - 1;

		memcpy(http_crlf, crlf_str, strlen(crlf_str));
		memmove(http_crlf + strlen(crlf_str), tmp, strlen(tmp));
		size_adjust -= sizeof(HTTP_CRLF_STR) - 1 - strlen(crlf_str);
		memset(httpc.headers + size_adjust, 0x00, 1);

		tmp = http_crlf;
		http_crlf = strstr(tmp, HTTP_CRLF_STR);
	}

	/* There should be exactly one <CR><LF> in the end of headers
	 * HTTP client will add <CR><LF> between headers and message body
	 * Refer to zephyr/blob/main/subsys/net/lib/http/http_client.c#L50
	 * Refer to https://www.w3.org/Protocols/rfc2616/rfc2616-sec5.html
	 */
	if (strcmp(httpc.headers + strlen(httpc.headers) - strlen(crlf_str),
		   crlf_str) != 0) {
		LOG_ERR("Missing <CR><LF> for headers");
		return -EINVAL;
	}
	if (strcmp(httpc.headers + strlen(httpc.headers) - strlen(crlf_crlf_str),
		   crlf_crlf_str) == 0) {
		/* two or more <CR><LF> after headers */
		LOG_ERR("Too many <CR><LF> after headers");
		return -EINVAL;
	}

	return 0;
}

/**@brief handle AT#XHTTPCREQ commands
 *  AT#XHTTPCREQ=<method>,<resource>,<headers>[,<payload_length>]
 *  AT#XHTTPCREQ? READ command not supported
 *  AT#XHTTPCREQ=?
 */
int handle_at_httpc_request(enum at_cmd_type cmd_type)
{
	int err = -EINVAL;
	int param_count;
	size_t method_sz = HTTPC_METHOD_LEN;
	size_t resource_sz = HTTPC_RES_LEN;
	size_t headers_sz = HTTPC_HEADER_LEN;
	size_t offset;

	if (httpc.fd == INVALID_SOCKET) {
		LOG_ERR("Remote host is not connected.");
		return err;
	}

	switch (cmd_type) {
	case AT_CMD_TYPE_SET_COMMAND:
		param_count = at_params_valid_count_get(&at_param_list);
		httpc.pl_len = 0;
		httpc.total_sent = 0;
		httpc.state = HTTPC_INIT;
		memset(data_buf, 0, sizeof(data_buf));
		err = util_string_get(&at_param_list, 1, data_buf, &method_sz);
		if (err < 0) {
			LOG_ERR("Fail to get method string: %d", err);
			return err;
		}
		httpc.method_str = (char *)data_buf;
		offset = method_sz + 1;
		/* Get resource path string */
		err = util_string_get(&at_param_list, 2, data_buf + offset, &resource_sz);
		if (err < 0) {
			LOG_ERR("Fail to get resource string: %d", err);
			return err;
		}
		httpc.resource = (char *)(data_buf + offset);
		offset = offset + resource_sz + 1;
		/* Get header string */
		err = util_string_get(&at_param_list, 3, data_buf + offset, &headers_sz);
		if (err < 0) {
			LOG_ERR("Fail to get option string: %d", err);
			return err;
		}
		httpc.headers = (char *)(data_buf + offset);
		err = http_headers_preprocess(headers_sz);
		if (err) {
			return err;
		}
		if (param_count >= 5) {
			err = at_params_unsigned_int_get(&at_param_list, 4, &httpc.pl_len);
			if (err != 0) {
				return err;
			}
		}
		httpc.total_sent = 0;
		httpc.state = HTTPC_INIT;
		httpc.rsp_header_length = 0;
		httpc.rsp_body_length = 0;
		/* start http request thread */
		k_thread_create(&httpc_thread, httpc_thread_stack,
				K_THREAD_STACK_SIZEOF(httpc_thread_stack),
				httpc_thread_fn, NULL, NULL, NULL,
				HTTPC_THREAD_PRIORITY, K_USER, K_NO_WAIT);
		break;

	case AT_CMD_TYPE_TEST_COMMAND:
		break;

	default:
		break;
	}

	return err;
}

// int handle_at_httpc_payload(enum at_cmd_type cmd_type){
// 	int err = -EINVAL;

// 	switch (cmd_type) {
// 	case AT_CMD_TYPE_READ_COMMAND:
// 		sprintf(rsp_buf, "\r\n#XHTTPRESPONSE: ");
// 		rsp_send(rsp_buf, strlen(rsp_buf));
// 		rsp_send(data_buf2, strlen(data_buf2));
// 		err = 0;
// 		break;

// 	case AT_CMD_TYPE_TEST_COMMAND:
// 		break;

// 	default:
// 		break;
// 	}

// 	return err;
// }

// int handle_at_httpc_headers(enum at_cmd_type cmd_type){
// 	int err = -EINVAL;
// 	switch (cmd_type) {
// 	case AT_CMD_TYPE_READ_COMMAND:
// 		sprintf(rsp_buf, "\r\n#XHTTPHEADERS: ");
// 		rsp_send(rsp_buf, strlen(rsp_buf));
// 		rsp_send(headers_buf, strlen(headers_buf));
// 		err = 0;
// 		break;

// 	case AT_CMD_TYPE_TEST_COMMAND:
// 		break;

// 	default:
// 		break;
// 	}

// 	return err;	
// }

int slm_at_httpc_init(void)
{
	httpc.fd = INVALID_SOCKET;
	httpc.state = HTTPC_INIT;
	httpc.pl_len = 0;
	httpc.total_sent = 0;

	return 0;
}

int slm_at_httpc_uninit(void)
{
	int err = 0;

	if (httpc.fd != INVALID_SOCKET) {
		err = do_http_disconnect();
		if (err != 0) {
			LOG_ERR("Fail to disconnect. Error: %d", err);
		}
	}

	return err;
}
