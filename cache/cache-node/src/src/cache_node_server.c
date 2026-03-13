/* SPDX-License-Identifier: MIT */

#include "cache_node.h"

#include <errno.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

static void cache_node_trim_line(char *line)
{
	size_t len = strlen(line);

	while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
		line[--len] = '\0';
}

static int cache_node_send_response(int fd, const char *msg)
{
	size_t len = strlen(msg);
	ssize_t sent;
	size_t written = 0;

	while (written < len) {
		sent = send(fd, msg + written, len - written, 0);
		if (sent <= 0)
			return -1;
		written += (size_t)sent;
	}

	return 0;
}

static int cache_node_parse_u64(const char *value, unsigned long long *out)
{
	char *endptr;
	unsigned long long parsed;

	if (!value || !out)
		return -1;
	errno = 0;
	parsed = strtoull(value, &endptr, 10);
	if (errno || *endptr != '\0')
		return -1;
	*out = parsed;
	return 0;
}

static int cache_node_handle_command(struct cache_node_store *store,
					 const char *line,
					 char *resp,
					 size_t resp_len)
{
	char *tokens[6];
	char *cmd;
	char *saveptr = NULL;
	char *tmp;
	char *line_copy;
	char *key;
	char *value;
	char *source;
	unsigned long long retrieval_time = 0;
	void *stored_value = NULL;
	size_t value_len = 0;
	int token_count;
	int status = 0;
	size_t i;

	line_copy = strdup(line);
	if (!line_copy)
		return snprintf(resp, resp_len, "-ERR internal error\r\n");
	cache_node_trim_line(line_copy);
	cmd = strtok_r(line_copy, " ", &saveptr);
	if (!cmd) {
		free(line_copy);
		return snprintf(resp, resp_len, "-ERR empty command\r\n");
	}

	for (i = 0; i < 6; i++)
		tokens[i] = NULL;
	token_count = 0;
	tokens[token_count++] = cmd;
	while (token_count < 6 && (tmp = strtok_r(NULL, " ", &saveptr)))
		tokens[token_count++] = tmp;

	if (!strcasecmp(cmd, "PING")) {
		free(line_copy);
		return snprintf(resp, resp_len, "+PONG\r\n");
	}

	if (!strcasecmp(cmd, "INFO")) {
		size_t key_count = 0;
		size_t bytes = 0;
		size_t max_bytes = 0;
		size_t max_items = 0;

		cache_node_store_stats(store, &key_count, &bytes, &max_bytes,
				      &max_items);
		free(line_copy);
		return snprintf(resp, resp_len,
					"+INFO keys=%zu bytes=%zu max_bytes=%zu "
					"max_items=%zu\r\n",
					 key_count, bytes, max_bytes, max_items);
	}

	if (!strcasecmp(cmd, "QUIT")) {
		free(line_copy);
		return snprintf(resp, resp_len, "+BYE\r\n");
	}

	if (!strcasecmp(cmd, "SET")) {
		if (token_count < 3) {
			free(line_copy);
			return snprintf(resp, resp_len,
					"-ERR SET requires key and value\r\n");
		}
		key = tokens[1];
		value = tokens[2];
		source = (token_count >= 4) ? tokens[3] : NULL;
		if (token_count >= 5) {
			if (cache_node_parse_u64(tokens[4], &retrieval_time) != 0) {
				free(line_copy);
				return snprintf(resp, resp_len,
						"-ERR invalid retrieval time\r\n");
			}
		}
		status = cache_node_store_set(store, key, value, strlen(value),
					 source, retrieval_time);
		if (status != 0) {
			free(line_copy);
			return snprintf(resp, resp_len, "-ERR set failed (%d)\r\n", status);
		}
		free(line_copy);
		return snprintf(resp, resp_len, "+OK\r\n");
	}

	if (!strcasecmp(cmd, "GET")) {
		if (token_count < 2) {
			free(line_copy);
			return snprintf(resp, resp_len,
					"-ERR GET requires key\r\n");
		}
		key = tokens[1];
		status = cache_node_store_get(store, key, &stored_value, &value_len);
		if (status != 0) {
			free(line_copy);
			return snprintf(resp, resp_len, "$-1\r\n");
		}
		snprintf(resp, resp_len, "$%zu\r\n", value_len);
		i = strlen(resp);
		if (i + value_len + 2 >= resp_len) {
			free(stored_value);
			free(line_copy);
			return snprintf(resp, resp_len,
					"-ERR response too large\r\n");
		}
		memcpy(resp + i, stored_value, value_len);
		memcpy(resp + i + value_len, "\r\n", 2);
		resp[i + value_len + 2] = '\0';
		free(stored_value);
		free(line_copy);
		return (int)i + (int)value_len + 2;
	}

	if (!strcasecmp(cmd, "DEL")) {
		if (token_count < 2) {
			free(line_copy);
			return snprintf(resp, resp_len, "-ERR DEL requires key\r\n");
		}
		key = tokens[1];
		source = (token_count >= 3) ? tokens[2] : NULL;
		if (token_count >= 4) {
			if (cache_node_parse_u64(tokens[3], &retrieval_time) != 0) {
				free(line_copy);
				return snprintf(resp, resp_len,
						"-ERR invalid retrieval time\r\n");
			}
		}
		status = cache_node_store_delete(store, key, source, retrieval_time);
		if (status == -ENOENT) {
			free(line_copy);
			return snprintf(resp, resp_len, ":0\r\n");
		}
		if (status != 0) {
			free(line_copy);
			return snprintf(resp, resp_len,
					"-ERR del failed (%d)\r\n", status);
		}
		free(line_copy);
		return snprintf(resp, resp_len, ":1\r\n");
	}

	free(line_copy);
	return snprintf(resp, resp_len, "-ERR unknown command\r\n");
}

static int cache_node_client_loop(int client_fd, struct cache_node_store *store,
					 bool verbose)
{
	FILE *stream;
	char *line = NULL;
	size_t line_len = 0;
	char resp[1024];
	ssize_t read_len;

	stream = fdopen(client_fd, "r+");
	if (!stream)
		return -1;

	while ((read_len = getline(&line, &line_len, stream)) > 0) {
		int resp_len;

		resp_len = cache_node_handle_command(store, line, resp,
						 sizeof(resp));
		if (resp_len <= 0)
			continue;
		if (cache_node_send_response(client_fd, resp) != 0) {
			free(line);
			fclose(stream);
			return -1;
		}
		if (verbose)
			fprintf(stderr, "cache-node command: %s -> %s", line, resp);
		if (strncmp(resp, "+BYE", 4) == 0)
			break;
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

	if (!store || !cfg || cfg->listen_port == 0)
		return -1;

	listen_addr.sin_port = htons(cfg->listen_port);
	if (!cfg->listen_host || strlen(cfg->listen_host) == 0) {
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
		status = cache_node_client_loop(client_fd, store, cfg->verbose);
		if (status != 0) {
			close(server_fd);
			return status;
		}
	}
}
