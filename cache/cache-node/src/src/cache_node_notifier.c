/* SPDX-License-Identifier: MIT */

#include "cache_node.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

static bool cache_node_http_status_ok(const char *response)
{
	if (!response)
		return false;
	if (!strncmp(response, "HTTP/1.1 2", 10))
		return true;
	if (!strncmp(response, "HTTP/1.0 2", 10))
		return true;
	return false;
}

struct cache_node_http_notifier {
	char *host;
	char *path;
	unsigned short port;
};

static int cache_node_parse_url(const char *url, struct cache_node_http_notifier *ctx)
{
	const char *prefix = "http://";
	const char *url_start;
	const char *path_sep;
	char *colon;
	char *host_port;
	size_t host_len;

	if (!url || !ctx)
		return -1;

	if (strncmp(url, prefix, strlen(prefix)) != 0)
		return -1;
	url_start = url + strlen(prefix);

	path_sep = strchr(url_start, '/');
	if (path_sep) {
		ctx->path = strdup(path_sep);
	} else {
		ctx->path = strdup("/v1/events");
	}
	if (!ctx->path)
		return -1;

	if (path_sep)
		host_len = (size_t)(path_sep - url_start);
	else
		host_len = strlen(url_start);
	if (host_len == 0) {
		free(ctx->path);
		ctx->path = NULL;
		return -1;
	}

	host_port = strndup(url_start, host_len);
	if (!host_port) {
		free(ctx->path);
		ctx->path = NULL;
		return -1;
	}

	colon = strchr(host_port, ':');
	if (colon) {
		size_t port_len;

		*colon = '\0';
		port_len = strlen(colon + 1);
		if (port_len == 0) {
			free(host_port);
			free(ctx->path);
			ctx->path = NULL;
			return -1;
		}
		ctx->port = (unsigned short)atoi(colon + 1);
		if (ctx->port == 0) {
			free(host_port);
			free(ctx->path);
			ctx->path = NULL;
			return -1;
		}
	} else {
		ctx->port = 80;
	}

	ctx->host = host_port;
	return 0;
}

static int cache_node_http_post(const struct cache_node_http_notifier *ctx,
			       const char *payload)
{
	struct addrinfo hints;
	struct addrinfo *result = NULL;
	struct addrinfo *rp;
	char request[2048];
	char response[256];
	char port_buf[8];
	int sock = -1;
	int rc;
	size_t written = 0;
	ssize_t sent;
	ssize_t nread;
	int request_len;

	if (!ctx || !ctx->host || !ctx->path || !payload)
		return -1;
	snprintf(port_buf, sizeof(port_buf), "%hu", ctx->port);

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(ctx->host, port_buf, &hints, &result) != 0)
		return -1;

	for (rp = result; rp; rp = rp->ai_next) {
		sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if (sock < 0)
			continue;
		if (connect(sock, rp->ai_addr, rp->ai_addrlen) == 0)
			break;
		close(sock);
		sock = -1;
	}
	freeaddrinfo(result);
	if (sock < 0)
		return -1;

	request_len = snprintf(request, sizeof(request),
			      "POST %s HTTP/1.1\r\n"
			      "Host: %s:%hu\r\n"
			      "Content-Type: application/json\r\n"
			      "Connection: close\r\n"
			      "Content-Length: %zu\r\n"
			      "\r\n"
			      "%s",
			      ctx->path, ctx->host, ctx->port, strlen(payload),
			      payload);
	if (request_len < 0 || (size_t)request_len >= sizeof(request)) {
		close(sock);
		return -1;
	}

	while (written < (size_t)request_len) {
		sent = send(sock, request + written,
			    request_len - (int)written, 0);
		if (sent <= 0) {
			close(sock);
			return -1;
		}
		written += (size_t)sent;
	}

	nread = recv(sock, response, sizeof(response) - 1, 0);
	if (nread <= 0) {
		close(sock);
		return -1;
	}
	response[nread] = '\0';
	close(sock);

	rc = cache_node_http_status_ok(response) ? 0 : -1;

	return rc;
}

static int cache_node_json_escape(const char *input, char *out, size_t out_len)
{
	size_t pos = 0;
	size_t i;

	if (!input || !out || out_len == 0)
		return -1;

	for (i = 0; i < strlen(input); i++) {
		char c = input[i];

		if (c == '\\' || c == '"') {
			if (pos + 2 >= out_len)
				return -1;
			out[pos++] = '\\';
			out[pos++] = c;
			continue;
		}
		if (pos + 1 >= out_len)
			return -1;
		out[pos++] = c;
	}
	out[pos] = '\0';
	return 0;
}

static int cache_node_build_payload(const struct cache_node_event *event,
				   char *payload, size_t payload_len)
{
	char timestamp[CACHE_NODE_MAX_TIMESTAMP_LEN];
	char instance_id[CACHE_NODE_MAX_INSTANCE_ID];
	char key[CACHE_NODE_MAX_KEY_LEN];
	char source[CACHE_NODE_MAX_INSTANCE_ID];
	int len;

	if (!event || !payload)
		return -1;

	memset(timestamp, 0, sizeof(timestamp));
	memset(instance_id, 0, sizeof(instance_id));
	memset(key, 0, sizeof(key));
	memset(source, 0, sizeof(source));

	if (cache_node_json_escape(event->timestamp, timestamp,
				  sizeof(timestamp)) < 0 ||
	    cache_node_json_escape(event->instance_id, instance_id,
				  sizeof(instance_id)) < 0 ||
	    cache_node_json_escape(event->key, key, sizeof(key)) < 0)
		return -1;
	if (cache_node_json_escape(event->source_instance_id, source,
				  sizeof(source)) < 0)
		return -1;

	if (event->type == CACHE_NODE_EVENT_KEY_EVICTED) {
		len = snprintf(payload, payload_len,
			       "{"
			       "\"instanceId\":\"%s\","
			       "\"eventType\":\"key_evicted\","
			       "\"timestamp\":\"%s\","
			       "\"key\":\"%s\","
			       "\"sourceInstanceId\":\"%s\","
			       "\"retrievalTimeMs\":%llu"
			       "}",
			       instance_id, timestamp, key, source,
			       event->retrieval_time_ms);
	} else {
		len = snprintf(payload, payload_len,
			       "{"
			       "\"instanceId\":\"%s\","
			       "\"eventType\":\"%s\","
			       "\"timestamp\":\"%s\","
			       "\"key\":\"%s\","
			       "\"size\":%zu,"
			       "\"sourceInstanceId\":\"%s\","
			       "\"retrievalTimeMs\":%llu"
			       "}",
			       instance_id,
			       event->type == CACHE_NODE_EVENT_KEY_ADDED ?
				       "key_added" :
				       "key_updated",
			       timestamp, key, event->key_len, source,
			       event->retrieval_time_ms);
	}

	return len >= 0 && (size_t)len < payload_len ? 0 : -1;
}

int cache_node_http_notifier(const struct cache_node_event *event, void *ctx)
{
	struct cache_node_http_notifier *notifier = ctx;
	char payload[1024];

	if (!event || !notifier)
		return -1;
	if (cache_node_build_payload(event, payload, sizeof(payload)) != 0)
		return -1;
	return cache_node_http_post(notifier, payload);
}

struct cache_node_http_notifier *cache_node_http_notifier_create(
	const char *endpoint_url)
{
	struct cache_node_http_notifier *ctx;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return NULL;
	if (cache_node_parse_url(endpoint_url, ctx) != 0) {
		free(ctx);
		return NULL;
	}
	return ctx;
}

void cache_node_http_notifier_destroy(struct cache_node_http_notifier *ctx)
{
	if (!ctx)
		return;
	free(ctx->host);
	free(ctx->path);
	free(ctx);
}
