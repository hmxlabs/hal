#include "cache_control_plane.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

long long now_unix_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);
	return ((long long)ts.tv_sec * 1000LL) + (ts.tv_nsec / 1000000LL);
}

void now_iso8601(char *buf, size_t size)
{
	time_t now;
	struct tm t;

	now = time(NULL);
	gmtime_r(&now, &t);
	strftime(buf, size, "%Y-%m-%dT%H:%M:%SZ", &t);
}

bool env_is_truthy(const char *value)
{
	if (!value || !*value)
		return false;
	return !strcasecmp(value, "1") || !strcasecmp(value, "true") ||
	       !strcasecmp(value, "yes") || !strcasecmp(value, "on");
}

void cp_log(const struct app_state *app, const char *fmt, ...)
{
	char timestamp[64];
	char msg[2048];
	va_list ap;

	if (!app || !app->verbose)
		return;

	now_iso8601(timestamp, sizeof(timestamp));
	va_start(ap, fmt);
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
	vsnprintf(msg, sizeof(msg), fmt, ap);
#pragma GCC diagnostic pop
#else
	vsnprintf(msg, sizeof(msg), fmt, ap);
#endif
	va_end(ap);

	fprintf(stdout, "[%s] [control-plane] %s\n", timestamp, msg);
	fflush(stdout);
}

int str_to_int(const char *value, int dflt)
{
	char *end;
	long v;

	if (!value || !*value)
		return dflt;

	errno = 0;
	v = strtol(value, &end, 10);
	if (errno || end == value || *end != '\0')
		return dflt;
	if (v > INT_MAX || v < INT_MIN)
		return dflt;
	return (int)v;
}

void sig_handler(int sig)
{
	(void)sig;
	if (g_app)
		g_app->running = 0;
}

bool append_request_body(struct request_ctx *ctx, const char *chunk,
			size_t chunk_len)
{
	char *next;

	next = realloc(ctx->body, ctx->body_len + chunk_len + 1);
	if (!next)
		return false;
	ctx->body = next;
	memcpy(ctx->body + ctx->body_len, chunk, chunk_len);
	ctx->body_len += chunk_len;
	ctx->body[ctx->body_len] = '\0';
	return true;
}

json_t *parse_json_body(const struct app_state *app, struct request_ctx *ctx)
{
	json_error_t error;

	if (!ctx->body || ctx->body_len == 0)
		return NULL;

	if (app)
		cp_log(app, "request body: %s", ctx->body);
	return json_loadb(ctx->body, ctx->body_len, 0, &error);
}

bool parse_path(const char *url, struct path_parts *path)
{
	char *save;
	char *token;

	memset(path, 0, sizeof(*path));
	path->raw = strdup(url ? url : "");
	if (!path->raw)
		return false;

	token = strtok_r(path->raw, "/", &save);
	while (token && path->count < MAX_PATH_PARTS) {
		path->parts[path->count++] = token;
		token = strtok_r(NULL, "/", &save);
	}

	return true;
}

void free_path(struct path_parts *path)
{
	free(path->raw);
}

int cmp_str_ptr(const void *a, const void *b)
{
	const char *const *sa = a;
	const char *const *sb = b;
	return strcmp(*sa, *sb);
}

int index_of_id(char **ids, size_t count, const char *id)
{
	size_t i;

	for (i = 0; i < count; i++) {
		if (strcmp(ids[i], id) == 0)
			return (int)i;
	}
	return -1;
}
