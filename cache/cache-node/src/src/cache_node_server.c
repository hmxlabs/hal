/* SPDX-License-Identifier: MIT */

#include "cache_node.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define CACHE_NODE_MAX_CMD_TOKENS 16

struct cache_node_cp_endpoint {
	char host[128];
	unsigned short port;
	char base_path[64];
};

static int cache_node_url_encode(const char *input, char *output,
					 size_t output_len)
{
	const unsigned char *cursor;
	size_t pos = 0;

	if (!input || !output || output_len == 0)
		return -1;

	for (cursor = (const unsigned char *)input; *cursor != '\0'; cursor++) {
		if (('a' <= *cursor && *cursor <= 'z') ||
		    ('A' <= *cursor && *cursor <= 'Z') ||
		    ('0' <= *cursor && *cursor <= '9') ||
		    *cursor == '-' || *cursor == '_' ||
		    *cursor == '.' || *cursor == '~') {
			if (pos + 1 >= output_len)
				return -1;
			output[pos++] = (char)*cursor;
			continue;
		}

		if (pos + 3 >= output_len)
			return -1;
		output[pos++] = '%';
		output[pos++] = "0123456789ABCDEF"[*cursor >> 4];
		output[pos++] = "0123456789ABCDEF"[*cursor & 0x0F];
	}

	output[pos] = '\0';
	return 0;
}

static int cache_node_parse_control_plane_url(const char *url,
					 struct cache_node_cp_endpoint *endpoint)
{
	const char *prefix = "http://";
	const char *start;
	const char *path;
	const char *path_end;
	char *path_copy = NULL;
	const char *events;
	char *host_port = NULL;
	char *host_port_copy = NULL;
	char *colon;

	if (!url || !endpoint)
		return -1;

	if (strncmp(url, prefix, strlen(prefix)) != 0)
		return -1;

	start = url + strlen(prefix);
	path = strchr(start, '/');
	if (path) {
		path_end = strpbrk(path, "?#");
		if (path_end)
			host_port = strndup(start, (size_t)(path - start));
		else
			host_port = strdup(start);
	} else {
		host_port = strdup(start);
	}
	if (!host_port)
		return -1;

	if (path == NULL) {
		path_copy = strdup("/");
	} else if (path_end) {
		path_copy = strndup(path, (size_t)(path_end - path));
	} else {
		path_copy = strdup(path);
	}
	if (!path_copy) {
		free(host_port);
		return -1;
	}
	path = path_copy;

	host_port_copy = strdup(host_port);
	if (!host_port_copy) {
		free(path_copy);
		free(host_port);
		return -1;
	}

	colon = strchr(host_port_copy, ':');
	if (colon) {
		*colon = '\0';
		endpoint->port = (unsigned short)atoi(colon + 1);
		if (endpoint->port == 0) {
			free(host_port_copy);
			return -1;
		}
	} else {
		endpoint->port = 80;
	}

	if (strlen(host_port_copy) >= sizeof(endpoint->host)) {
		free(host_port_copy);
		return -1;
	}
	strcpy(endpoint->host, host_port_copy);
	free(host_port_copy);

	events = strstr(path, "/events");
	if (events) {
		size_t len = (size_t)(events - path);

		if (events != path) {
			size_t copy_len = len;
			if (copy_len >= sizeof(endpoint->base_path))
				goto cleanup;
			memcpy(endpoint->base_path, path, copy_len);
			endpoint->base_path[copy_len] = '\0';
			if (copy_len > 0 &&
			    endpoint->base_path[copy_len - 1] == '/')
				endpoint->base_path[copy_len - 1] = '\0';
		} else {
			strcpy(endpoint->base_path, "/");
		}
	} else {
		if (strlen(path) >= sizeof(endpoint->base_path))
			goto cleanup;
		strcpy(endpoint->base_path, path);
	}
	if (endpoint->base_path[0] == '\0')
		strcpy(endpoint->base_path, "/");

	free(path_copy);

	return 0;
cleanup:
	free(path_copy);
	free(host_port);
	free(host_port_copy);
	return -1;
}

static int cache_node_http_request(const struct cache_node_cp_endpoint *endpoint,
					 const char *path, char **response,
					 size_t *response_len)
{
	struct addrinfo hints;
	struct addrinfo *result = NULL;
	struct addrinfo *rp;
	char request[2048];
	char port_buf[16];
	int sock = -1;
	int request_len;
	char *buffer = NULL;
	char chunk[256];
	size_t total = 0;
	size_t used = 0;
	ssize_t bytes;

	if (!endpoint || !endpoint->host[0] || !path || !response || !response_len)
		return -1;

	snprintf(port_buf, sizeof(port_buf), "%hu", endpoint->port);

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(endpoint->host, port_buf, &hints, &result) != 0)
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
				      "GET %s HTTP/1.1\r\n"
				      "Host: %s:%hu\r\n"
				      "Accept: application/json\r\n"
				      "Connection: close\r\n"
				      "\r\n",
				      path, endpoint->host, endpoint->port);
	if (request_len < 0 || (size_t)request_len >= sizeof(request)) {
		close(sock);
		return -1;
	}

	if (send(sock, request, (size_t)request_len, 0) != request_len) {
		close(sock);
		return -1;
	}

	while ((bytes = recv(sock, chunk, sizeof(chunk), 0)) > 0) {
		char *resized;

		if (used + (size_t)bytes >= total) {
			total = total > 0 ? total * 2 : 256;
			while (total <= used + (size_t)bytes)
				total *= 2;
			resized = realloc(buffer, total);
			if (!resized) {
				free(buffer);
				close(sock);
				return -1;
			}
			buffer = resized;
		}
		memcpy(buffer + used, chunk, (size_t)bytes);
		used += (size_t)bytes;
	}
	close(sock);

	if (used == 0 || !buffer) {
		free(buffer);
		return -1;
	}

	{
		char *resized = realloc(buffer, used + 1);
		if (!resized) {
			free(buffer);
			return -1;
		}
		resized[used] = '\0';
		*response = resized;
		*response_len = used;
	}

	return 0;
}

static int cache_node_http_json_to_payload(const char *response,
					 size_t response_len,
					 const char **payload,
					 size_t *payload_len)
{
	char *separator;
	if (!response || !payload)
		return -1;

	separator = strstr(response, "\r\n\r\n");
	if (!separator)
		return -1;

	*payload = separator + 4;
	if (payload_len) {
		*payload_len = response_len - (size_t)(*payload - response);
	}
	return 0;
}

static int cache_node_find_json_value_string(const char *body, const char *key,
					 char *out, size_t out_len)
{
	const char *found;
	const char *start;
	const char *cursor;
	size_t out_pos;
	char token[64];

	if (!body || !key || !out || out_len == 0)
		return -1;

	snprintf(token, sizeof(token), "\"%s\"", key);
	for (found = strstr(body, token); found; found = strstr(found + 1, token)) {
		start = found + strlen(token);
		while (*start != '\0' && isspace((unsigned char)*start))
			start++;
		if (*start != ':')
			continue;
		start++;
		while (*start != '\0' && isspace((unsigned char)*start))
			start++;
		if (*start != '"')
			continue;
		start++;
		out_pos = 0;
		for (cursor = start; *cursor != '\0'; cursor++) {
			if (*cursor == '\\') {
				cursor++;
				if (*cursor == '\0')
					return -1;
			}
			if (*cursor == '"') {
				out[out_pos] = '\0';
				return 0;
			}
			if (out_pos + 1 >= out_len)
				return -1;
			out[out_pos++] = *cursor;
		}
		return -1;
	}

	return -1;
}

static int cache_node_request_instance_address(const struct cache_node_cp_endpoint *endpoint,
							 const char *instance_id,
							 char *out_address,
							 size_t out_len)
{
	char path[256];
	char *response = NULL;
	size_t response_len;
	const char *body;
	size_t body_len;

	if (!instance_id || !out_address || out_len == 0)
		return -1;

	if (snprintf(path, sizeof(path), "%s/instances/%s",
	             endpoint->base_path, instance_id) >= (int)sizeof(path))
		return -1;
	if (cache_node_http_request(endpoint, path, &response, &response_len) != 0)
		return -1;
	if (cache_node_http_json_to_payload(response, response_len, &body, &body_len) != 0) {
		free(response);
		return -1;
	}

	(void)body_len;
	if (cache_node_find_json_value_string(body, "address", out_address, out_len) != 0) {
		free(response);
		return -1;
	}

	free(response);
	return 0;
}

static int cache_node_query_nearest_source(const struct cache_node_cp_endpoint *endpoint,
						 const char *instance_id, const char *key,
						 char *source_id,
						 size_t source_id_len)
{
	char encoded_key[256];
	char path[512];
	char *response = NULL;
	size_t response_len;
	const char *body;

	if (!instance_id || !key || !source_id || source_id_len == 0)
		return -1;
	if (cache_node_url_encode(key, encoded_key, sizeof(encoded_key)) != 0)
		return -1;

	if (snprintf(path, sizeof(path),
	             "%s/content/nearest/%s?sourceInstanceId=%s",
	             endpoint->base_path, encoded_key, instance_id) >= (int)sizeof(path))
		return -1;

	if (cache_node_http_request(endpoint, path, &response, &response_len) != 0)
		return -1;

	if (cache_node_http_json_to_payload(response, response_len, &body, NULL) != 0) {
		free(response);
		return -1;
	}

	if (cache_node_find_json_value_string(body, "instanceId", source_id,
		                         source_id_len) != 0) {
		free(response);
		return -1;
	}

	free(response);
	return 0;
}

static int cache_node_fetch_from_peer(const char *address, const char *key,
				     void **value, size_t *value_len)
{
	struct addrinfo hints;
	struct addrinfo *result = NULL;
	struct addrinfo *rp;
	char host[128];
	char *port_start;
	char cmd[256];
	char *encoded_key;
	FILE *stream = NULL;
	int sock = -1;
	char port_buf[16] = "6379";
	int cmd_len;
	long long expected;
	char *response = NULL;
	size_t response_len = 0;
	size_t bytes_to_read;
	char *line = NULL;
	size_t line_len = 0;
	char *cursor;
	char crlf[2];
	char *endp;

	if (!address || !key || !value || !value_len)
		return -1;

	encoded_key = strdup(key);
	if (!encoded_key)
		return -1;

	if (strlen(address) >= sizeof(host)) {
		free(encoded_key);
		return -1;
	}
	strcpy(host, address);
	port_start = strchr(host, ':');
	if (port_start) {
		*port_start++ = '\0';
		if (*port_start == '\0') {
			free(encoded_key);
			return -1;
		}
		strncpy(port_buf, port_start, sizeof(port_buf));
		port_buf[sizeof(port_buf) - 1] = '\0';
	}

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(host, port_buf, &hints, &result) != 0) {
		free(encoded_key);
		return -1;
	}

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
	if (sock < 0) {
		free(encoded_key);
		return -1;
	}

	cmd_len = snprintf(cmd, sizeof(cmd),
			         "*2\r\n$3\r\nGET\r\n$%zu\r\n%s\r\n",
			         strlen(encoded_key), encoded_key);
	if (cmd_len < 0 || (size_t)cmd_len >= sizeof(cmd) ||
	    send(sock, cmd, (size_t)cmd_len, 0) != cmd_len) {
		free(encoded_key);
		close(sock);
		return -1;
	}
	free(encoded_key);

	stream = fdopen(sock, "r+");
	if (!stream) {
		close(sock);
		return -1;
	}

	if (getline(&line, &line_len, stream) <= 0) {
		free(line);
		fclose(stream);
		return -1;
	}
	errno = 0;
	expected = strtoll(line + 1, &endp, 10);
	if (line[0] != '$' || endp == NULL || errno != 0) {
		free(line);
		fclose(stream);
		return -1;
	}
	while (*endp == '\r' || *endp == '\n' || *endp == ' ')
		endp++;
	if (*endp != '\0') {
		free(line);
		fclose(stream);
		return -1;
	}
	free(line);
	line = NULL;
	if (expected == -1) {
		fclose(stream);
		*value_len = 0;
		*value = NULL;
		return -ENOENT;
	}
	if (expected < 0) {
		fclose(stream);
		return -1;
	}
	if ((size_t)expected > SIZE_MAX - 1) {
		fclose(stream);
		return -1;
	}

	response_len = (size_t)expected;
	response = calloc(response_len + 1, 1);
	if (!response) {
		fclose(stream);
		return -1;
	}
	cursor = response;
	bytes_to_read = response_len;
	while (bytes_to_read > 0) {
		size_t bytes_read = (size_t)fread(cursor, 1, bytes_to_read, stream);
		if (bytes_read == 0) {
			if (ferror(stream)) {
				fclose(stream);
				free(response);
				return -1;
			}
			if (feof(stream)) {
				fclose(stream);
				free(response);
				return -1;
			}
			continue;
		}
		if (bytes_read > bytes_to_read) {
			fclose(stream);
			free(response);
			return -1;
		}
		cursor += bytes_read;
		bytes_to_read -= bytes_read;
	}
	if (fread(crlf, 1, 2, stream) != 2) {
		fclose(stream);
		free(response);
		return -1;
	}
	fclose(stream);

	if (crlf[0] != '\r' || crlf[1] != '\n') {
		free(response);
		return -1;
	}

	*value_len = (size_t)expected;
	*value = response;
	return 0;
}

static int cache_node_lookup_remote_value(struct cache_node_store *store,
					      const char *control_plane_url,
					      const char *key,
					      char **value,
					      size_t *value_len,
					      char *source_instance_id,
					      size_t source_instance_id_len)
{
	struct cache_node_cp_endpoint endpoint;
	char nearest_instance[64];
	char source_address[128];
	const char *instance_id;

	if (!store || !control_plane_url || !key || !value || !value_len)
		return -1;
	instance_id = cache_node_store_get_instance_id(store);
	if (!instance_id)
		return -1;

	if (cache_node_parse_control_plane_url(control_plane_url, &endpoint) != 0)
		return -1;

	if (cache_node_query_nearest_source(&endpoint, instance_id, key,
		                                  nearest_instance,
		                                  sizeof(nearest_instance)) != 0)
		return -1;

	if (cache_node_request_instance_address(&endpoint, nearest_instance,
		                                    source_address,
		                                    sizeof(source_address)) != 0)
		return -1;

	if (cache_node_fetch_from_peer(source_address, key, (void **)value,
		                          value_len) != 0)
		return -1;

	if (source_instance_id && source_instance_id_len > 0) {
		strncpy(source_instance_id, nearest_instance, source_instance_id_len);
		source_instance_id[source_instance_id_len - 1] = '\0';
	}

	return 0;
}

static void cache_node_trim_line(char *line)
{
	size_t len = strlen(line);

	while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
		line[--len] = '\0';
}

static int cache_node_send_response(int fd, const char *msg, size_t msg_len)
{
	ssize_t sent;
	size_t written = 0;

	if (msg == NULL)
		return -1;

	while (written < msg_len) {
		sent = send(fd, msg + written, msg_len - (size_t)written, 0);
		if (sent <= 0) {
			fprintf(stderr, "cache-node send failed fd=%d len=%zu written=%zu errno=%d\n",
				fd, msg_len, written, errno);
			return -1;
		}
		written += (size_t)sent;
	}
	fprintf(stderr, "cache-node send ok fd=%d len=%zu\n", fd, msg_len);

	return 0;
}

static int cache_node_appendf(char *resp, size_t resp_len, size_t *pos,
				 const char *fmt, ...)
{
	va_list ap;
	int n;

	if (resp == NULL || pos == NULL || *pos >= resp_len)
		return -1;

	va_start(ap, fmt);
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif
	n = vsnprintf(resp + *pos, resp_len - *pos, fmt, ap);
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
	va_end(ap);
	if (n < 0 || (size_t)n >= resp_len - *pos)
		return -1;

	*pos += (size_t)n;
	return 0;
}

static int cache_node_add_bulk(char *resp, size_t resp_len, size_t *pos,
				const char *value, size_t value_len)
{
	if (cache_node_appendf(resp, resp_len, pos, "$%zu\r\n", value_len) != 0)
		return -1;
	if (*pos + value_len + 2 > resp_len)
		return -1;

	memcpy(resp + *pos, value, value_len);
	*pos += value_len;
	if (cache_node_appendf(resp, resp_len, pos, "\r\n") != 0)
		return -1;
	return 0;
}

static int cache_node_parse_u64(const char *value, unsigned long long *out)
{
	char *endptr;
	unsigned long long parsed;

	if (value == NULL || out == NULL)
		return -1;
	errno = 0;
	parsed = strtoull(value, &endptr, 10);
	if (errno != 0 || *endptr != '\0')
		return -1;
	*out = parsed;
	return 0;
}

static int cache_node_parse_resp_integer(const char *line, char prefix,
				       long long *out)
{
	char *endptr;
	long long value;

	if (line == NULL || out == NULL || line[0] != prefix)
		return -1;

	errno = 0;
	value = strtoll(line + 1, &endptr, 10);
	if (errno != 0)
		return -1;
	while (*endptr == '\r' || *endptr == '\n' || *endptr == ' ')
		endptr++;
	if (*endptr != '\0')
		return -1;
	*out = value;
	return 0;
}

static int cache_node_parse_redis_bulk(FILE *stream, char **value,
				     size_t *value_len)
{
	char *line = NULL;
	size_t line_len = 0;
	ssize_t line_read;
	long long expected;
	size_t copied = 0;
	char crlf[2];

	line_read = getline(&line, &line_len, stream);
	if (line_read <= 0) {
		free(line);
		return -1;
	}
	if (cache_node_parse_resp_integer(line, '$', &expected) != 0 || expected < 0) {
		free(line);
		return -1;
	}
	free(line);

	*value = calloc((size_t)expected + 1, 1);
	if (*value == NULL)
		return -1;
	*value_len = (size_t)expected;

	while (copied < *value_len) {
		size_t remaining = *value_len - copied;
		size_t read = fread(*value + copied, 1, remaining, stream);
		if (read == 0) {
			free(*value);
			*value = NULL;
			*value_len = 0;
			return -1;
		}
		copied += read;
	}

	if (fread(crlf, 1, 2, stream) != 2 || crlf[0] != '\r' || crlf[1] != '\n') {
		free(*value);
		*value = NULL;
		*value_len = 0;
		return -1;
	}

	return 0;
}

static int cache_node_parse_redis_tokens(FILE *stream, char *first_line,
				 char **tokens, size_t *token_lens,
				 size_t max_tokens, int *out_count)
{
	long long arg_count;
	int i;

	if (cache_node_parse_resp_integer(first_line, '*', &arg_count) != 0)
		return -1;
	if (arg_count <= 0 || (size_t)arg_count > max_tokens)
		return -1;

	*out_count = (int)arg_count;
	for (i = 0; i < *out_count; i++) {
		char *value = NULL;
		size_t value_len = 0;

		if (cache_node_parse_redis_bulk(stream, &value, &value_len) != 0) {
			for (int j = 0; j < i; j++)
				free(tokens[j]);
			return -1;
		}
		tokens[i] = value;
		token_lens[i] = value_len;
	}

	return 0;
}

static int cache_node_parse_line_tokens(char *line, char **tokens,
				      size_t *token_lens, size_t max_tokens)
{
	char *tmp;
	char *saveptr = NULL;
	size_t token_count = 0;

	cache_node_trim_line(line);
	for (tmp = strtok_r(line, " ", &saveptr); tmp;
	     tmp = strtok_r(NULL, " ", &saveptr)) {
		if (token_count >= max_tokens)
			return -1;
		tokens[token_count] = tmp;
		token_lens[token_count] = strlen(tmp);
		token_count++;
	}

	return (int)token_count;
}

static const char *cache_node_policy_to_string(
	enum cache_node_eviction_policy policy)
{
	switch (policy) {
	case CACHE_NODE_EVICTION_POLICY_NOEVICTION:
		return "noeviction";
	case CACHE_NODE_EVICTION_POLICY_ALLKEYS_LRU:
	default:
		return "allkeys-lru";
	}
}

static int cache_node_parse_policy(const char *value,
				 enum cache_node_eviction_policy *out)
{
	if (value == NULL || out == NULL)
		return -1;
	if (strcasecmp(value, "allkeys-lru") == 0) {
		*out = CACHE_NODE_EVICTION_POLICY_ALLKEYS_LRU;
		return 0;
	}
	if (strcasecmp(value, "noeviction") == 0) {
		*out = CACHE_NODE_EVICTION_POLICY_NOEVICTION;
		return 0;
	}
	return -1;
}

static int cache_node_build_info_payload(char *resp, size_t resp_len, size_t *pos,
				    bool is_redis, struct cache_node_store *store)
{
	size_t key_count = 0;
	size_t bytes = 0;
	size_t max_bytes = 0;
	size_t max_items = 0;
	const char *policy = cache_node_policy_to_string(
		cache_node_store_get_eviction_policy(store));
	char payload[256];

	cache_node_store_stats(store, &key_count, &bytes, &max_bytes, &max_items);

	if (is_redis) {
		snprintf(payload, sizeof(payload),
			 "key_count:%zu\n"
			 "used_memory:%zu\n"
			 "maxmemory:%zu\n"
			 "max_items:%zu\n"
			 "maxmemory_policy:%s\n",
			 key_count, bytes, max_bytes, max_items, policy);
		if (cache_node_add_bulk(resp, resp_len, pos, payload, strlen(payload)) != 0)
			return -1;
		return (int)*pos;
	}

	if (cache_node_appendf(resp, resp_len, pos,
			"+INFO keys=%zu bytes=%zu max_bytes=%zu max_items=%zu"
			" maxmemory_policy=%s\r\n",
			key_count, bytes, max_bytes, max_items, policy) != 0)
		return -1;
	return (int)*pos;
}

static int cache_node_store_set_limits_from_string(struct cache_node_store *store,
					 const char *value)
{
	unsigned long long parsed;

	if (value == NULL)
		return -1;
	if (cache_node_parse_u64(value, &parsed) != 0)
		return -1;
	return cache_node_store_set_limits(store, (size_t)parsed,
			cache_node_store_get_max_items(store));
}

static int cache_node_handle_tokens(struct cache_node_store *store, char **tokens,
					 size_t *token_lens, int token_count, bool is_redis,
					 const char *control_plane_url, bool *should_close,
					 char *resp, size_t resp_len)
{
	char *cmd = tokens[0];
	char *key;
	char *source;
	char source_instance_id[CACHE_NODE_MAX_INSTANCE_ID];
	char *remote_value = NULL;
	size_t remote_value_len = 0;
	bool fetched_remote = false;
	unsigned long long retrieval_time = 0;
	void *stored_value = NULL;
	size_t value_len = 0;
	size_t payload_pos = 0;
	int status;
	int rc;

	*should_close = false;

	if (token_count <= 0)
		return snprintf(resp, resp_len, "-ERR empty command\r\n");

	if (strcasecmp(cmd, "PING") == 0)
	{
		if (cache_node_appendf(resp, resp_len, &payload_pos, "+PONG\r\n") != 0)
			return -1;
		return (int)payload_pos;
	}

	if (strcasecmp(cmd, "QUIT") == 0) {
		if (cache_node_appendf(resp, resp_len, &payload_pos, "+OK\r\n") != 0)
			return -1;
		rc = 0;
		if (rc == 0)
			*should_close = true;
		return (int)payload_pos;
	}

	if (strcasecmp(cmd, "INFO") == 0)
		return cache_node_build_info_payload(resp, resp_len, &payload_pos,
					   is_redis, store);

	if (strcasecmp(cmd, "SET") == 0) {
		if ((is_redis && token_count != 3) || (!is_redis && token_count < 3) ||
		    (is_redis == false && token_count > 5))
			return snprintf(resp, resp_len,
				"-ERR wrong number of arguments for 'SET' command\r\n");

		if (is_redis) {
			source = NULL;
		} else {
			source = (token_count >= 4) ? tokens[3] : NULL;
			if (token_count >= 5) {
				if (cache_node_parse_u64(tokens[4], &retrieval_time) != 0)
					return snprintf(resp, resp_len,
						"-ERR invalid retrieval time\r\n");
			}
		}

		key = tokens[1];
		status = cache_node_store_set(store, key, tokens[2], token_lens[2],
					     source, retrieval_time);
		if (status != 0) {
			if (status == -ENOSPC)
				return snprintf(resp, resp_len,
					"-OOM command not allowed when used memory exceeds maxmemory\r\n");
			if (status == -ENOMEM)
				return snprintf(resp, resp_len,
					"-OOM memory allocation failed\r\n");
			return snprintf(resp, resp_len, "-ERR set failed (%d)\r\n", status);
		}
		if (cache_node_appendf(resp, resp_len, &payload_pos, "+OK\r\n") != 0)
			return -1;
		return (int)payload_pos;
	}

	if (strcasecmp(cmd, "GET") == 0) {
		if ((is_redis && token_count != 2) || (!is_redis && token_count < 2))
			return snprintf(resp, resp_len,
				"-ERR wrong number of arguments for 'GET' command\r\n");

		key = tokens[1];
		status = cache_node_store_get(store, key, &stored_value, &value_len);
		if (status != 0) {
			if (control_plane_url != NULL) {
				memset(source_instance_id, 0, sizeof(source_instance_id));
				if (cache_node_lookup_remote_value(store, control_plane_url,
				                                  key, &remote_value,
				                                  &remote_value_len,
				                                  source_instance_id,
				                                  sizeof(source_instance_id)) == 0) {
					fetched_remote = true;
					cache_node_store_set(store, key, remote_value,
								     remote_value_len,
								     source_instance_id,
								     0ULL);
					stored_value = remote_value;
					value_len = remote_value_len;
					status = 0;
				}
			}
			if (status != 0 || stored_value == NULL) {
				if (cache_node_appendf(resp, resp_len, &payload_pos, "$-1\r\n") != 0)
					return -1;
				if (fetched_remote)
					free(remote_value);
				return (int)payload_pos;
			}
		}

		if (cache_node_appendf(resp, resp_len, &payload_pos, "$%zu\r\n", value_len) != 0 ||
		    payload_pos + value_len + 2 > resp_len) {
			free(stored_value);
			return snprintf(resp, resp_len, "-ERR response too large\r\n");
		}
		memcpy(resp + payload_pos, stored_value, value_len);
		payload_pos += value_len;
		free(stored_value);
		if (cache_node_appendf(resp, resp_len, &payload_pos, "\r\n") != 0)
			return snprintf(resp, resp_len, "-ERR response too large\r\n");
		return (int)payload_pos;
	}

	if (strcasecmp(cmd, "DEL") == 0) {
		if ((is_redis && token_count != 2) || (!is_redis && token_count < 2) ||
		    (!is_redis && token_count > 4))
			return snprintf(resp, resp_len,
				"-ERR wrong number of arguments for 'DEL' command\r\n");

		key = tokens[1];
		source = (is_redis || token_count < 3) ? NULL : tokens[2];
		if (!is_redis && token_count >= 4) {
			if (cache_node_parse_u64(tokens[3], &retrieval_time) != 0)
				return snprintf(resp, resp_len,
					"-ERR invalid retrieval time\r\n");
		}

		status = cache_node_store_delete(store, key, source, retrieval_time);
		if (status == -ENOENT) {
			if (cache_node_appendf(resp, resp_len, &payload_pos,
					      ":0\r\n") != 0)
				return -1;
			return (int)payload_pos;
		}
		if (status != 0)
			return snprintf(resp, resp_len,
				"-ERR del failed (%d)\r\n", status);
		if (cache_node_appendf(resp, resp_len, &payload_pos, ":1\r\n") != 0)
			return -1;
		return (int)payload_pos;
	}

	if (strcasecmp(cmd, "CONFIG") == 0) {
		if (token_count < 2)
			return snprintf(resp, resp_len,
				"-ERR wrong number of arguments for 'CONFIG' command\r\n");

		if (strcasecmp(tokens[1], "GET") == 0) {
			const char *param;
			const char *value = NULL;
			char value_buffer[64];

			if (token_count != 3)
				return snprintf(resp, resp_len,
					"-ERR wrong number of arguments for 'CONFIG GET' command\r\n");
			param = tokens[2];
			if (strcasecmp(param, "maxmemory") == 0) {
				snprintf(value_buffer, sizeof(value_buffer), "%zu",
					 cache_node_store_get_max_bytes(store));
				value = value_buffer;
			} else if (strcasecmp(param, "maxmemory-policy") == 0) {
				value = cache_node_policy_to_string(
					cache_node_store_get_eviction_policy(store));
			} else {
				if (is_redis) {
					if (cache_node_appendf(resp, resp_len, &payload_pos,
							"*0\r\n") != 0) {
						return -1;
					} else {
						return (int)payload_pos;
					}
				}
				return snprintf(resp, resp_len,
						"-ERR CONFIG GET only supports maxmemory and maxmemory-policy\r\n");
			}

			if (is_redis) {
				if (cache_node_appendf(resp, resp_len, &payload_pos, "*2\r\n") != 0)
					return -1;
				if (cache_node_add_bulk(resp, resp_len, &payload_pos, param,
					strlen(param)) != 0 ||
				    cache_node_add_bulk(resp, resp_len, &payload_pos, value,
					strlen(value)) != 0)
					return snprintf(resp, resp_len,
							"-ERR response too large\r\n");
				return (int)payload_pos;
			}

			if (cache_node_appendf(resp, resp_len, &payload_pos,
					"$%zu\r\n%s\r\n", strlen(value), value) != 0)
				return -1;
			return (int)payload_pos;
		}

		if (strcasecmp(tokens[1], "SET") == 0) {
			const char *param;
			const char *param_value;
			enum cache_node_eviction_policy policy;

			if (token_count != 4)
				return snprintf(resp, resp_len,
					"-ERR wrong number of arguments for 'CONFIG SET' command\r\n");
			param = tokens[2];
			param_value = tokens[3];

			if (strcasecmp(param, "maxmemory") == 0) {
				if (cache_node_store_set_limits_from_string(store,
						param_value) != 0)
					return snprintf(resp, resp_len,
							"-ERR maxmemory must be an unsigned integer\r\n");
				if (cache_node_appendf(resp, resp_len, &payload_pos,
						"+OK\r\n") != 0)
					return -1;
				return (int)payload_pos;
			}
			if (strcasecmp(param, "maxmemory-policy") == 0) {
				if (cache_node_parse_policy(param_value, &policy) != 0)
					return snprintf(resp, resp_len,
							"-ERR unsupported maxmemory-policy\r\n");
				if (cache_node_store_set_eviction_policy(store, policy) != 0)
					return snprintf(resp, resp_len,
							"-OOM command not allowed when used memory exceeds maxmemory\r\n");
				if (cache_node_appendf(resp, resp_len, &payload_pos,
						"+OK\r\n") != 0)
					return -1;
				return (int)payload_pos;
			}
			return snprintf(resp, resp_len,
					"-ERR CONFIG SET only supports maxmemory and maxmemory-policy\r\n");
		}

		return snprintf(resp, resp_len,
				"-ERR CONFIG syntax\r\n");
	}

	return snprintf(resp, resp_len, "-ERR unknown command\r\n");
}

static int cache_node_client_loop(int client_fd, struct cache_node_store *store,
					 bool verbose, const char *control_plane_url)
{
	FILE *stream;
	char *line = NULL;
	size_t line_len = 0;
	char resp[2048];
	ssize_t read_len;
	int token_count;

	stream = fdopen(client_fd, "r+");
	if (stream == NULL)
		return -1;

	while ((read_len = getline(&line, &line_len, stream)) > 0) {
		bool is_redis = false;
		char *tokens[CACHE_NODE_MAX_CMD_TOKENS];
		size_t token_lens[CACHE_NODE_MAX_CMD_TOKENS];
		int resp_len;
		bool should_close = false;

		if (line[0] == '*') {
			is_redis = true;
			if (cache_node_parse_redis_tokens(stream, line, tokens, token_lens,
					     CACHE_NODE_MAX_CMD_TOKENS,
					     &token_count) != 0) {
				cache_node_send_response(client_fd,
						"-ERR protocol error\r\n", 18);
				continue;
			}
		} else {
			token_count = cache_node_parse_line_tokens(line, tokens,
							 token_lens,
							 CACHE_NODE_MAX_CMD_TOKENS);
			if (token_count <= 0) {
				resp_len = snprintf(resp, sizeof(resp),
						"-ERR empty command\r\n");
			}
		}

		if (line[0] != '*' && token_count <= 0) {
			; /* already prepared */
		} else {
			resp_len = cache_node_handle_tokens(
					store, tokens, token_lens, token_count, is_redis,
					control_plane_url,
					&should_close, resp, sizeof(resp));
		}

		for (int i = 0; i < token_count; i++) {
			if (is_redis)
				free(tokens[i]);
		}

		if (cache_node_send_response(client_fd, resp, (size_t)resp_len) != 0) {
			free(line);
			fclose(stream);
			return -1;
		}
		if (verbose) {
			if (is_redis)
				fprintf(stderr, "cache-node command (redis): %s", line);
			else
				fprintf(stderr, "cache-node command: %s -> %s", line, resp);
		}
		if (should_close) {
			break;
		}
	}

	free(line);
	fclose(stream);
	return 0;
}

int cache_node_server_run(struct cache_node_store *store,
				 const struct cache_node_server_config *cfg)
{
	struct sockaddr_in listen_addr = {
		.sin_family = AF_INET,
	};
	int server_fd;
	int client_fd;
	int reuse = 1;
	int status;
	socklen_t client_len;
	struct sockaddr_in client_addr;

	if (store == NULL || cfg == NULL || cfg->listen_port == 0)
		return -1;

	listen_addr.sin_port = htons(cfg->listen_port);
	if (cfg->listen_host == NULL || strlen(cfg->listen_host) == 0) {
		listen_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	} else if (inet_pton(AF_INET, cfg->listen_host,
				 &listen_addr.sin_addr) != 1) {
		return -1;
	}

	server_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (server_fd < 0)
		return -1;
	if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse,
			       sizeof(reuse)) < 0) {
		close(server_fd);
		return -1;
	}
	if (bind(server_fd, (struct sockaddr *)&listen_addr,
			 sizeof(listen_addr)) != 0) {
		close(server_fd);
		return -1;
	}
	if (listen(server_fd, 32) != 0) {
		close(server_fd);
		return -1;
	}

	if (cfg->verbose) {
		fprintf(stderr, "cache-node listening on %s:%hu\n",
			cfg->listen_host ? cfg->listen_host : "0.0.0.0",
			cfg->listen_port);
	}

	while (1) {
		client_len = sizeof(client_addr);
		client_fd = accept(server_fd, (struct sockaddr *)&client_addr,
				  &client_len);
		if (client_fd < 0) {
			if (errno == EINTR)
				continue;
			close(server_fd);
			return -1;
		}
		status = cache_node_client_loop(client_fd, store, cfg->verbose,
					       cfg->control_plane_events_url);
		if (status != 0) {
			close(server_fd);
			return status;
		}
	}
}
