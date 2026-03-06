#include <errno.h>
#include <limits.h>
#include <microhttpd.h>
#include <hiredis/hiredis.h>
#include <jansson.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#define APP_VERSION "1.0.0"
#define DEFAULT_HTTP_PORT 8080
#define DEFAULT_REDIS_PORT 6379
#define DEFAULT_REDIS_DB 0
#define DEFAULT_REDIS_HOST "127.0.0.1"
#define DEFAULT_HEARTBEAT_MS 30000
#define MAX_PATH_PARTS 8

struct app_state {
	redisContext *redis;
	int http_port;
	char redis_host[256];
	int redis_port;
	int redis_db;
	bool verbose;
	volatile sig_atomic_t running;
};

struct request_ctx {
	char *body;
	size_t body_len;
	bool responded;
};

struct path_parts {
	char *raw;
	char *parts[MAX_PATH_PARTS];
	size_t count;
};

struct key_input {
	const char *key;
	long long size;
};

static struct app_state *g_app;

static long long now_unix_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);
	return ((long long)ts.tv_sec * 1000LL) + (ts.tv_nsec / 1000000LL);
}

static void now_iso8601(char *buf, size_t size)
{
	time_t now;
	struct tm t;

	now = time(NULL);
	gmtime_r(&now, &t);
	strftime(buf, size, "%Y-%m-%dT%H:%M:%SZ", &t);
}

static bool env_is_truthy(const char *value)
{
	if (!value || !*value)
		return false;
	return !strcasecmp(value, "1") || !strcasecmp(value, "true") ||
	       !strcasecmp(value, "yes") || !strcasecmp(value, "on");
}

static void cp_log(const struct app_state *app, const char *fmt, ...)
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

static int str_to_int(const char *value, int dflt)
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

static void sig_handler(int sig)
{
	(void)sig;
	if (g_app)
		g_app->running = 0;
}

static redisReply *redis_cmd(struct app_state *app, const char *fmt, ...)
{
	va_list ap;
	redisReply *reply;

	va_start(ap, fmt);
	reply = redisvCommand(app->redis, fmt, ap);
	va_end(ap);
	return reply;
}

static bool reply_ok_integer(redisReply *reply)
{
	return reply && reply->type == REDIS_REPLY_INTEGER;
}

static bool redis_instance_exists(struct app_state *app, const char *id)
{
	redisReply *reply;
	char key[512];
	bool exists = false;

	snprintf(key, sizeof(key), "cp:instance:%s", id);
	reply = redis_cmd(app, "EXISTS %s", key);
	if (reply_ok_integer(reply) && reply->integer == 1)
		exists = true;
	freeReplyObject(reply);
	return exists;
}

static char *redis_hget_strdup(struct app_state *app, const char *hash,
				 const char *field)
{
	redisReply *reply;
	char *result = NULL;

	reply = redis_cmd(app, "HGET %s %s", hash, field);
	if (reply && reply->type == REDIS_REPLY_STRING)
		result = strdup(reply->str);
	freeReplyObject(reply);
	return result;
}

static long long redis_hget_ll(struct app_state *app, const char *hash,
			      const char *field, long long dflt)
{
	redisReply *reply;
	char *end;
	long long val = dflt;

	reply = redis_cmd(app, "HGET %s %s", hash, field);
	if (!reply || reply->type != REDIS_REPLY_STRING)
		goto out;

	errno = 0;
	val = strtoll(reply->str, &end, 10);
	if (errno || end == reply->str || *end != '\0')
		val = dflt;
out:
	freeReplyObject(reply);
	return val;
}

static bool redis_hset_ll(struct app_state *app, const char *hash,
			 const char *field, long long value)
{
	redisReply *reply;
	bool ok = false;

	reply = redis_cmd(app, "HSET %s %s %lld", hash, field, value);
	if (reply_ok_integer(reply))
		ok = true;
	freeReplyObject(reply);
	return ok;
}

static bool redis_hset_double(struct app_state *app, const char *hash,
			     const char *field, double value)
{
	redisReply *reply;
	bool ok = false;

	reply = redis_cmd(app, "HSET %s %s %.10g", hash, field, value);
	if (reply_ok_integer(reply))
		ok = true;
	freeReplyObject(reply);
	return ok;
}

static bool redis_hset_str(struct app_state *app, const char *hash,
			  const char *field, const char *value)
{
	redisReply *reply;
	bool ok = false;

	reply = redis_cmd(app, "HSET %s %s %s", hash, field, value);
	if (reply_ok_integer(reply))
		ok = true;
	freeReplyObject(reply);
	return ok;
}

static bool redis_update_instance_counters(struct app_state *app,
					 const char *instance_id)
{
	redisReply *reply;
	char keys_hash[512];
	char inst_hash[512];
	long long total_size = 0;
	long long key_count = 0;
	size_t i;

	snprintf(keys_hash, sizeof(keys_hash), "cp:instance:%s:keys", instance_id);
	snprintf(inst_hash, sizeof(inst_hash), "cp:instance:%s", instance_id);

	reply = redis_cmd(app, "HGETALL %s", keys_hash);
	if (!reply || reply->type != REDIS_REPLY_ARRAY) {
		freeReplyObject(reply);
		return false;
	}

	for (i = 0; i + 1 < reply->elements; i += 2) {
		redisReply *v = reply->element[i + 1];
		char *end;
		long long sz;

		if (v->type != REDIS_REPLY_STRING)
			continue;
		errno = 0;
		sz = strtoll(v->str, &end, 10);
		if (errno || end == v->str)
			sz = 0;
		total_size += sz;
		key_count++;
	}
	freeReplyObject(reply);

	return redis_hset_ll(app, inst_hash, "keyCount", key_count) &&
	       redis_hset_ll(app, inst_hash, "totalSize", total_size);
}

static bool redis_remove_holder(struct app_state *app, const char *instance_id,
			       const char *key)
{
	redisReply *reply;
	char inst_keys_hash[512];
	char key_holders_set[512];
	char key_size_hash[512];
	long long remaining = 0;

	snprintf(inst_keys_hash, sizeof(inst_keys_hash), "cp:instance:%s:keys",
		 instance_id);
	snprintf(key_holders_set, sizeof(key_holders_set), "cp:key:%s:instances",
		 key);
	snprintf(key_size_hash, sizeof(key_size_hash), "cp:key:%s:sizes", key);

	reply = redis_cmd(app, "HDEL %s %s", inst_keys_hash, key);
	freeReplyObject(reply);

	reply = redis_cmd(app, "SREM %s %s", key_holders_set, instance_id);
	freeReplyObject(reply);

	reply = redis_cmd(app, "HDEL %s %s", key_size_hash, instance_id);
	freeReplyObject(reply);

	reply = redis_cmd(app, "SCARD %s", key_holders_set);
	if (reply_ok_integer(reply))
		remaining = reply->integer;
	freeReplyObject(reply);

	if (remaining == 0) {
		reply = redis_cmd(app, "DEL %s %s", key_holders_set, key_size_hash);
		freeReplyObject(reply);
	}

	return redis_update_instance_counters(app, instance_id);
}

static bool redis_upsert_holder(struct app_state *app, const char *instance_id,
			       const char *key, long long size,
			       bool *was_added)
{
	redisReply *reply;
	char inst_keys_hash[512];
	char key_holders_set[512];
	char key_size_hash[512];
	bool existed = false;

	snprintf(inst_keys_hash, sizeof(inst_keys_hash), "cp:instance:%s:keys",
		 instance_id);
	snprintf(key_holders_set, sizeof(key_holders_set), "cp:key:%s:instances",
		 key);
	snprintf(key_size_hash, sizeof(key_size_hash), "cp:key:%s:sizes", key);

	reply = redis_cmd(app, "HEXISTS %s %s", inst_keys_hash, key);
	if (reply_ok_integer(reply) && reply->integer == 1)
		existed = true;
	freeReplyObject(reply);

	reply = redis_cmd(app, "HSET %s %s %lld", inst_keys_hash, key, size);
	freeReplyObject(reply);
	reply = redis_cmd(app, "SADD %s %s", key_holders_set, instance_id);
	freeReplyObject(reply);
	reply = redis_cmd(app, "HSET %s %s %lld", key_size_hash, instance_id,
			  size);
	freeReplyObject(reply);

	if (was_added)
		*was_added = !existed;
	return redis_update_instance_counters(app, instance_id);
}

static json_t *json_error(const char *code, const char *message)
{
	json_t *obj;

	obj = json_object();
	if (!obj)
		return NULL;
	json_object_set_new(obj, "code", json_string(code));
	json_object_set_new(obj, "message", json_string(message));
	return obj;
}

static int send_json_response(struct MHD_Connection *connection, unsigned int status,
			     json_t *payload)
{
	struct MHD_Response *response;
	char *body;
	int ret;

	body = json_dumps(payload, JSON_COMPACT);
	if (!body)
		return MHD_NO;

	response = MHD_create_response_from_buffer(strlen(body), body,
					      MHD_RESPMEM_MUST_FREE);
	if (!response) {
		free(body);
		return MHD_NO;
	}

	MHD_add_response_header(response, "Content-Type", "application/json");
	ret = MHD_queue_response(connection, status, response);
	MHD_destroy_response(response);
	return ret;
}

static int send_empty_response(struct MHD_Connection *connection,
			      unsigned int status)
{
	struct MHD_Response *response;
	int ret;

	response = MHD_create_response_from_buffer(0, NULL, MHD_RESPMEM_PERSISTENT);
	if (!response)
		return MHD_NO;
	ret = MHD_queue_response(connection, status, response);
	MHD_destroy_response(response);
	return ret;
}

static int send_error_response(struct MHD_Connection *connection,
			       unsigned int status,
			       const char *code,
			       const char *message)
{
	json_t *obj;
	int ret;

	obj = json_error(code, message);
	if (!obj)
		return MHD_NO;
	ret = send_json_response(connection, status, obj);
	json_decref(obj);
	return ret;
}

static bool append_request_body(struct request_ctx *ctx, const char *chunk,
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

static json_t *parse_json_body(const struct app_state *app, struct request_ctx *ctx)
{
	json_error_t error;

	if (!ctx->body || ctx->body_len == 0)
		return NULL;

	if (app)
		cp_log(app, "request body: %s", ctx->body);
	return json_loadb(ctx->body, ctx->body_len, 0, &error);
}

static bool parse_path(const char *url, struct path_parts *path)
{
	char *save;
	char *token;

	memset(path, 0, sizeof(*path));
	path->raw = strdup(url);
	if (!path->raw)
		return false;

	token = strtok_r(path->raw, "/", &save);
	while (token && path->count < MAX_PATH_PARTS) {
		path->parts[path->count++] = token;
		token = strtok_r(NULL, "/", &save);
	}

	return true;
}

static void free_path(struct path_parts *path)
{
	free(path->raw);
}

static int cmp_str_ptr(const void *a, const void *b)
{
	const char *const *sa = a;
	const char *const *sb = b;
	return strcmp(*sa, *sb);
}

static int get_all_instance_ids(struct app_state *app, char ***ids_out,
				size_t *count_out)
{
	redisReply *reply;
	char **ids;
	size_t i;

	reply = redis_cmd(app, "SMEMBERS cp:instances");
	if (!reply || reply->type != REDIS_REPLY_ARRAY) {
		freeReplyObject(reply);
		return -1;
	}

	ids = calloc(reply->elements, sizeof(*ids));
	if (!ids && reply->elements > 0) {
		freeReplyObject(reply);
		return -1;
	}

	for (i = 0; i < reply->elements; i++) {
		redisReply *item = reply->element[i];

		if (item->type != REDIS_REPLY_STRING) {
			ids[i] = strdup("");
			continue;
		}
		ids[i] = strdup(item->str);
		if (!ids[i]) {
			size_t j;

			for (j = 0; j < i; j++)
				free(ids[j]);
			free(ids);
			freeReplyObject(reply);
			return -1;
		}
	}

	qsort(ids, reply->elements, sizeof(*ids), cmp_str_ptr);
	*ids_out = ids;
	*count_out = reply->elements;
	freeReplyObject(reply);
	return 0;
}

static void free_ids(char **ids, size_t count)
{
	size_t i;

	if (!ids)
		return;
	for (i = 0; i < count; i++)
		free(ids[i]);
	free(ids);
}

static int index_of_id(char **ids, size_t count, const char *id)
{
	size_t i;

	for (i = 0; i < count; i++) {
		if (strcmp(ids[i], id) == 0)
			return (int)i;
	}
	return -1;
}

static int shortest_path(struct app_state *app, const char *from, const char *to,
			 char ***hops_out, size_t *hop_count_out)
{
	char **ids = NULL;
	size_t count = 0;
	char *adj = NULL;
	int *queue = NULL;
	int *prev = NULL;
	int from_idx;
	int to_idx;
	int q_head = 0;
	int q_tail = 0;
	int i;
	int rc = -1;

	if (get_all_instance_ids(app, &ids, &count) != 0)
		return -1;
	if (count == 0)
		goto out;

	from_idx = index_of_id(ids, count, from);
	to_idx = index_of_id(ids, count, to);
	if (from_idx < 0 || to_idx < 0) {
		rc = -2;
		goto out;
	}

	adj = calloc(count * count, sizeof(*adj));
	queue = malloc(count * sizeof(*queue));
	prev = malloc(count * sizeof(*prev));
	if (!adj || !queue || !prev)
		goto out;

	for (i = 0; i < (int)count; i++) {
		char inst_hash[512];
		char *parent;
		int pidx;

		prev[i] = -1;
		snprintf(inst_hash, sizeof(inst_hash), "cp:instance:%s", ids[i]);
		parent = redis_hget_strdup(app, inst_hash, "parentId");
		if (!parent || parent[0] == '\0') {
			free(parent);
			continue;
		}
		pidx = index_of_id(ids, count, parent);
		free(parent);
		if (pidx < 0)
			continue;

		adj[(i * (int)count) + pidx] = 1;
		adj[(pidx * (int)count) + i] = 1;
	}

	queue[q_tail++] = from_idx;
	prev[from_idx] = from_idx;

	while (q_head < q_tail) {
		int cur = queue[q_head++];

		if (cur == to_idx)
			break;
		for (i = 0; i < (int)count; i++) {
			if (!adj[(cur * (int)count) + i])
				continue;
			if (prev[i] != -1)
				continue;
			prev[i] = cur;
			queue[q_tail++] = i;
		}
	}

	if (prev[to_idx] == -1) {
		rc = -3;
		goto out;
	}

	{
		int node = to_idx;
		size_t path_len = 1;
		char **hops;
		size_t pos;

		while (node != from_idx) {
			node = prev[node];
			path_len++;
		}

		hops = calloc(path_len, sizeof(*hops));
		if (!hops)
			goto out;

		node = to_idx;
		for (pos = path_len; pos > 0; pos--) {
			hops[pos - 1] = strdup(ids[node]);
			if (!hops[pos - 1]) {
				size_t j;

				for (j = pos; j < path_len; j++)
					free(hops[j]);
				free(hops);
				goto out;
			}
			if (node == from_idx)
				break;
			node = prev[node];
		}

		*hops_out = hops;
		*hop_count_out = path_len;
		rc = 0;
	}

out:
	free(adj);
	free(queue);
	free(prev);
	free_ids(ids, count);
	return rc;
}

static long long shortest_distance(struct app_state *app, const char *from,
				 const char *to)
{
	char **hops = NULL;
	size_t hop_count = 0;
	long long dist;
	size_t i;
	int rc;

	if (strcmp(from, to) == 0)
		return 0;

	rc = shortest_path(app, from, to, &hops, &hop_count);
	if (rc != 0)
		return LLONG_MAX / 4;

	dist = (hop_count > 0) ? (long long)(hop_count - 1) : 0;
	for (i = 0; i < hop_count; i++)
		free(hops[i]);
	free(hops);
	return dist;
}

static json_t *make_proximity_metrics(struct app_state *app, const char *from,
				     const char *to)
{
	json_t *obj;
	long long dist;
	double latency;
	double bandwidth;
	double cost;

	dist = shortest_distance(app, from, to);
	if (dist >= (LLONG_MAX / 8)) {
		latency = 1000000.0;
		bandwidth = 1.0;
		cost = 1000000.0;
	} else {
		latency = (double)dist * 1.5;
		bandwidth = 1000.0 / ((double)dist + 1.0);
		cost = (double)dist;
	}

	obj = json_object();
	if (!obj)
		return NULL;
	json_object_set_new(obj, "latencyMs", json_real(latency));
	json_object_set_new(obj, "bandwidthMbps", json_real(bandwidth));
	json_object_set_new(obj, "cost", json_real(cost));
	return obj;
}

static int handle_list_instances(struct app_state *app,
				 struct MHD_Connection *connection)
{
	const char *status_filter;
	const char *region_filter;
	char **ids = NULL;
	size_t count = 0;
	size_t i;
	json_t *resp;
	json_t *arr;
	int ret;

	status_filter = MHD_lookup_connection_value(connection,
					    MHD_GET_ARGUMENT_KIND,
					    "status");
	region_filter = MHD_lookup_connection_value(connection,
					    MHD_GET_ARGUMENT_KIND,
					    "region");

	resp = json_object();
	arr = json_array();
	if (!resp || !arr) {
		json_decref(resp);
		json_decref(arr);
		return send_error_response(connection, MHD_HTTP_INTERNAL_SERVER_ERROR,
					   "internal_error",
					   "Failed to allocate response");
	}

	cp_log(app, "request list_instances status_filter=%s region_filter=%s",
	       status_filter ? status_filter : "-",
	       region_filter ? region_filter : "-");

	if (get_all_instance_ids(app, &ids, &count) != 0)
		count = 0;

	for (i = 0; i < count; i++) {
		char inst_hash[512];
		char *status;
		char *region;
		char *address;
		char *tier;
		char *parent;
		char *last;
		json_t *inst;

		snprintf(inst_hash, sizeof(inst_hash), "cp:instance:%s", ids[i]);
		status = redis_hget_strdup(app, inst_hash, "status");
		region = redis_hget_strdup(app, inst_hash, "region");
		if (status_filter && (!status || strcmp(status_filter, status) != 0)) {
			free(status);
			free(region);
			continue;
		}
		if (region_filter) {
			if (!region || strcmp(region_filter, region) != 0) {
				free(status);
				free(region);
				continue;
			}
		}

		address = redis_hget_strdup(app, inst_hash, "address");
		tier = redis_hget_strdup(app, inst_hash, "tier");
		parent = redis_hget_strdup(app, inst_hash, "parentId");
		last = redis_hget_strdup(app, inst_hash, "lastHeartbeat");

		inst = json_object();
		if (!inst)
			goto free_fields;
		json_object_set_new(inst, "id", json_string(ids[i]));
		json_object_set_new(inst, "status", json_string(status ? status :
							   "unverified"));
		json_object_set_new(inst, "address", json_string(address ? address :
							    ""));
		if (region)
			json_object_set_new(inst, "region", json_string(region));
		if (tier)
			json_object_set_new(inst, "tier", json_string(tier));
		if (parent && parent[0] != '\0')
			json_object_set_new(inst, "parentId", json_string(parent));
		if (last)
			json_object_set_new(inst, "lastHeartbeat", json_string(last));
		json_array_append_new(arr, inst);

free_fields:
		free(status);
		free(region);
		free(address);
		free(tier);
		free(parent);
		free(last);
	}

	json_object_set_new(resp, "instances", arr);
	json_object_set_new(resp, "total", json_integer((json_int_t)
							      json_array_size(arr)));
	ret = send_json_response(connection, MHD_HTTP_OK, resp);
	json_decref(resp);
	free_ids(ids, count);
	cp_log(app, "request list_instances returned=%zu instances", count);
	return ret;
}

static int handle_get_instance(struct app_state *app,
			       struct MHD_Connection *connection,
			       const char *id)
{
	char inst_hash[512];
	char child_key[512];
	char *address;
	char *status;
	char *region;
	char *tier;
	char *parent;
	char *last;
	char *registered;
	json_t *obj;
	json_t *children;
	redisReply *reply;
	size_t i;
	int ret;

	if (!redis_instance_exists(app, id))
		return send_error_response(connection, MHD_HTTP_NOT_FOUND,
					   "not_found",
					   "Instance not found");
	cp_log(app, "get_instance requested id=%s", id);

	snprintf(inst_hash, sizeof(inst_hash), "cp:instance:%s", id);
	snprintf(child_key, sizeof(child_key), "cp:children:%s", id);

	address = redis_hget_strdup(app, inst_hash, "address");
	status = redis_hget_strdup(app, inst_hash, "status");
	region = redis_hget_strdup(app, inst_hash, "region");
	tier = redis_hget_strdup(app, inst_hash, "tier");
	parent = redis_hget_strdup(app, inst_hash, "parentId");
	last = redis_hget_strdup(app, inst_hash, "lastHeartbeat");
	registered = redis_hget_strdup(app, inst_hash, "registeredAt");

	obj = json_object();
	children = json_array();
	if (!obj || !children) {
		json_decref(obj);
		json_decref(children);
		ret = send_error_response(connection, MHD_HTTP_INTERNAL_SERVER_ERROR,
					  "internal_error",
					  "Failed to allocate response");
		goto out;
	}

	reply = redis_cmd(app, "SMEMBERS %s", child_key);
	if (reply && reply->type == REDIS_REPLY_ARRAY) {
		for (i = 0; i < reply->elements; i++) {
			if (reply->element[i]->type == REDIS_REPLY_STRING)
				json_array_append_new(children,
					json_string(reply->element[i]->str));
		}
	}
	freeReplyObject(reply);

	json_object_set_new(obj, "id", json_string(id));
	json_object_set_new(obj, "status", json_string(status ? status :
							     "unverified"));
	json_object_set_new(obj, "address", json_string(address ? address : ""));
	if (region)
		json_object_set_new(obj, "region", json_string(region));
	if (tier)
		json_object_set_new(obj, "tier", json_string(tier));
	if (parent && parent[0] != '\0')
		json_object_set_new(obj, "parentId", json_string(parent));
	if (last)
		json_object_set_new(obj, "lastHeartbeat", json_string(last));
	if (registered)
		json_object_set_new(obj, "registeredAt", json_string(registered));
	json_object_set_new(obj, "keyCount",
			json_integer(redis_hget_ll(app, inst_hash, "keyCount", 0)));
	json_object_set_new(obj, "totalSize",
			json_integer(redis_hget_ll(app, inst_hash, "totalSize", 0)));
	json_object_set_new(obj, "maxSize",
			json_integer(redis_hget_ll(app, inst_hash, "maxSize", 0)));
	json_object_set_new(obj, "hitRate",
			json_real((double)redis_hget_ll(app, inst_hash, "hitRateMilli", 0) /
				 1000.0));
	json_object_set_new(obj, "children", children);

	ret = send_json_response(connection, MHD_HTTP_OK, obj);
	json_decref(obj);
out:
	free(address);
	free(status);
	free(region);
	free(tier);
	free(parent);
	free(last);
	free(registered);
	return ret;
}

static bool is_valid_tier(const char *tier)
{
	return tier && (!strcmp(tier, "root") || !strcmp(tier, "branch") ||
			!strcmp(tier, "leaf"));
}

static int handle_register_instance(struct app_state *app,
				    struct MHD_Connection *connection,
				    const char *id,
				    struct request_ctx *ctx)
{
	json_t *body;
	json_t *address_j;
	json_t *tier_j;
	json_t *region_j;
	json_t *parent_j;
	json_t *max_size_j;
	json_t *metadata_j;
	const char *address;
	const char *tier;
	const char *region = "";
	const char *parent = "";
	long long max_size = 0;
	char now[64];
	char inst_hash[512];
	char region_set[512];
	char status_set[128];
	char children_set[512];
	char *metadata_dump = NULL;
	redisReply *reply;
	json_t *resp;
	int ret;

	if (redis_instance_exists(app, id))
		return send_error_response(connection, MHD_HTTP_CONFLICT,
					   "already_exists",
					   "Instance already registered");

	cp_log(app, "register_instance requested id=%s", id);
	body = parse_json_body(app, ctx);
	if (!body)
		return send_error_response(connection, MHD_HTTP_BAD_REQUEST,
					   "bad_request",
					   "Invalid JSON body");

	address_j = json_object_get(body, "address");
	tier_j = json_object_get(body, "tier");
	region_j = json_object_get(body, "region");
	parent_j = json_object_get(body, "parentId");
	max_size_j = json_object_get(body, "maxSize");
	metadata_j = json_object_get(body, "metadata");

	if (!json_is_string(address_j) || !json_is_string(tier_j)) {
		json_decref(body);
		return send_error_response(connection, MHD_HTTP_BAD_REQUEST,
					   "bad_request",
					   "address and tier are required");
	}

	address = json_string_value(address_j);
	tier = json_string_value(tier_j);
	if (!is_valid_tier(tier)) {
		json_decref(body);
		return send_error_response(connection, MHD_HTTP_BAD_REQUEST,
					   "bad_request",
					   "tier must be root, branch, or leaf");
	}

	if (strcmp(tier, "root") != 0) {
		if (!json_is_string(parent_j) ||
		    strlen(json_string_value(parent_j)) == 0) {
			json_decref(body);
			return send_error_response(connection, MHD_HTTP_BAD_REQUEST,
						   "bad_request",
						   "parentId required for non-root tier");
		}
	}

	if (json_is_string(region_j))
		region = json_string_value(region_j);
	if (json_is_string(parent_j))
		parent = json_string_value(parent_j);
	if (json_is_integer(max_size_j))
		max_size = (long long)json_integer_value(max_size_j);

	if (metadata_j && json_is_object(metadata_j))
		metadata_dump = json_dumps(metadata_j, JSON_COMPACT);
	if (!metadata_dump)
		metadata_dump = strdup("{}");
	cp_log(app, "register_instance id=%s tier=%s region=%s parent=%s max_size=%lld",
	       id, tier, region, parent, max_size);

	now_iso8601(now, sizeof(now));
	snprintf(inst_hash, sizeof(inst_hash), "cp:instance:%s", id);
	snprintf(region_set, sizeof(region_set), "cp:region:%s", region);
	snprintf(status_set, sizeof(status_set), "cp:status:active");
	snprintf(children_set, sizeof(children_set), "cp:children:%s", parent);

	reply = redis_cmd(app,
		"HSET %s id %s address %s region %s tier %s parentId %s "
		"status active keyCount 0 totalSize 0 maxSize %lld "
		"registeredAt %s lastHeartbeat %s metadata %s hitRateMilli 0",
		inst_hash, id, address, region, tier, parent, max_size, now, now,
		metadata_dump);
	freeReplyObject(reply);

	reply = redis_cmd(app, "SADD cp:instances %s", id);
	freeReplyObject(reply);
	reply = redis_cmd(app, "SADD %s %s", status_set, id);
	freeReplyObject(reply);
	if (region[0] != '\0') {
		reply = redis_cmd(app, "SADD %s %s", region_set, id);
		freeReplyObject(reply);
	}
	if (parent[0] != '\0') {
		reply = redis_cmd(app, "SADD %s %s", children_set, id);
		freeReplyObject(reply);
	}

	resp = json_object();
	if (!resp) {
		ret = send_error_response(connection, MHD_HTTP_INTERNAL_SERVER_ERROR,
					  "internal_error",
					  "Failed to allocate response");
	} else {
		json_object_set_new(resp, "id", json_string(id));
		json_object_set_new(resp, "registered", json_true());
		json_object_set_new(resp, "controlPlaneVersion",
				    json_string(APP_VERSION));
		ret = send_json_response(connection, MHD_HTTP_CREATED, resp);
		json_decref(resp);
		cp_log(app, "register_instance complete id=%s", id);
	}

	free(metadata_dump);
	json_decref(body);
	return ret;
}

static int handle_heartbeat(struct app_state *app,
			    struct MHD_Connection *connection,
			    const char *id,
			    struct request_ctx *ctx)
{
	json_t *body;
	char inst_hash[512];
	char now[64];
	json_t *j;

	if (!redis_instance_exists(app, id))
		return send_error_response(connection, MHD_HTTP_NOT_FOUND,
					   "not_found",
					   "Instance not found");

	cp_log(app, "heartbeat requested id=%s", id);
	body = parse_json_body(app, ctx);
	if (!body)
		return send_error_response(connection, MHD_HTTP_BAD_REQUEST,
					   "bad_request",
					   "Invalid JSON body");

	snprintf(inst_hash, sizeof(inst_hash), "cp:instance:%s", id);

	j = json_object_get(body, "keyCount");
	if (json_is_integer(j))
		redis_hset_ll(app, inst_hash, "keyCount", json_integer_value(j));
	j = json_object_get(body, "totalSize");
	if (json_is_integer(j))
		redis_hset_ll(app, inst_hash, "totalSize", json_integer_value(j));
	j = json_object_get(body, "hitRate");
	if (json_is_real(j) || json_is_integer(j)) {
		double rate = json_number_value(j);
		redis_hset_ll(app, inst_hash, "hitRateMilli", (long long)(rate * 1000));
	}
	j = json_object_get(body, "cpuUsage");
	if (json_is_real(j) || json_is_integer(j))
		redis_hset_double(app, inst_hash, "cpuUsage", json_number_value(j));
	j = json_object_get(body, "memoryUsage");
	if (json_is_real(j) || json_is_integer(j))
		redis_hset_double(app, inst_hash, "memoryUsage", json_number_value(j));
	j = json_object_get(body, "healthy");
	if (json_is_boolean(j))
		redis_hset_str(app, inst_hash, "healthy",
			      json_boolean_value(j) ? "true" : "false");

	now_iso8601(now, sizeof(now));
	redis_hset_str(app, inst_hash, "lastHeartbeat", now);
	redis_hset_str(app, inst_hash, "status", "active");
	cp_log(app, "heartbeat complete id=%s", id);

	json_decref(body);
	{
		json_t *resp = json_object();
		int ret;

		if (!resp)
			return send_error_response(connection,
				MHD_HTTP_INTERNAL_SERVER_ERROR,
				"internal_error",
				"Failed to allocate response");
		json_object_set_new(resp, "acknowledged", json_true());
		json_object_set_new(resp, "nextHeartbeatMs",
				    json_integer(DEFAULT_HEARTBEAT_MS));
		ret = send_json_response(connection, MHD_HTTP_OK, resp);
		json_decref(resp);
		return ret;
	}
}

static int handle_deregister(struct app_state *app,
			     struct MHD_Connection *connection,
			     const char *id)
{
	char inst_hash[512];
	char inst_keys_hash[512];
	char *region;
	char *status;
	char *parent;
	char region_set[512];
	char status_set[512];
	char children_set[512];
	redisReply *reply;
	size_t i;

	if (!redis_instance_exists(app, id))
		return send_error_response(connection, MHD_HTTP_NOT_FOUND,
					   "not_found",
					   "Instance not found");
	cp_log(app, "deregister requested id=%s", id);

	snprintf(inst_hash, sizeof(inst_hash), "cp:instance:%s", id);
	snprintf(inst_keys_hash, sizeof(inst_keys_hash), "cp:instance:%s:keys", id);

	region = redis_hget_strdup(app, inst_hash, "region");
	status = redis_hget_strdup(app, inst_hash, "status");
	parent = redis_hget_strdup(app, inst_hash, "parentId");

	reply = redis_cmd(app, "HGETALL %s", inst_keys_hash);
	if (reply && reply->type == REDIS_REPLY_ARRAY) {
		for (i = 0; i + 1 < reply->elements; i += 2) {
			if (reply->element[i]->type != REDIS_REPLY_STRING)
				continue;
			redis_remove_holder(app, id, reply->element[i]->str);
		}
	}
	freeReplyObject(reply);

	reply = redis_cmd(app, "SREM cp:instances %s", id);
	freeReplyObject(reply);

	if (status && status[0] != '\0') {
		snprintf(status_set, sizeof(status_set), "cp:status:%s", status);
		reply = redis_cmd(app, "SREM %s %s", status_set, id);
		freeReplyObject(reply);
	}
	if (region && region[0] != '\0') {
		snprintf(region_set, sizeof(region_set), "cp:region:%s", region);
		reply = redis_cmd(app, "SREM %s %s", region_set, id);
		freeReplyObject(reply);
	}
	if (parent && parent[0] != '\0') {
		snprintf(children_set, sizeof(children_set), "cp:children:%s", parent);
		reply = redis_cmd(app, "SREM %s %s", children_set, id);
		freeReplyObject(reply);
	}

	reply = redis_cmd(app, "DEL %s %s cp:children:%s", inst_hash,
			  inst_keys_hash, id);
	freeReplyObject(reply);

	free(region);
	free(status);
	free(parent);
	cp_log(app, "deregister complete id=%s", id);
	return send_empty_response(connection, MHD_HTTP_NO_CONTENT);
}

static int compare_locate_items(const void *a, const void *b)
{
	const json_t *ia = *(const json_t *const *)a;
	const json_t *ib = *(const json_t *const *)b;
	json_t *da = json_object_get((json_t *)ia, "_dist");
	json_t *db = json_object_get((json_t *)ib, "_dist");
	json_t *ida = json_object_get((json_t *)ia, "instanceId");
	json_t *idb = json_object_get((json_t *)ib, "instanceId");
	long long va = LLONG_MAX / 4;
	long long vb = LLONG_MAX / 4;

	if (json_is_integer(da))
		va = json_integer_value(da);
	if (json_is_integer(db))
		vb = json_integer_value(db);
	if (va < vb)
		return -1;
	if (va > vb)
		return 1;
	if (json_is_string(ida) && json_is_string(idb))
		return strcmp(json_string_value(ida), json_string_value(idb));
	return 0;
}

static int build_locate_instances(struct app_state *app,
				  const char *key,
				  const char *source_id,
				  json_t **instances_out)
{
	redisReply *holders;
	char holder_set[512];
	char size_hash[512];
	json_t **items = NULL;
	json_t *instances;
	size_t i;
	int ret = -1;

	snprintf(holder_set, sizeof(holder_set), "cp:key:%s:instances", key);
	snprintf(size_hash, sizeof(size_hash), "cp:key:%s:sizes", key);

	holders = redis_cmd(app, "SMEMBERS %s", holder_set);
	if (!holders || holders->type != REDIS_REPLY_ARRAY)
		goto out;

	items = calloc(holders->elements, sizeof(*items));
	if (!items && holders->elements > 0)
		goto out;

	for (i = 0; i < holders->elements; i++) {
		char inst_hash[512];
		char *holder_id;
		redisReply *size_reply;
		long long size = 0;
		long long dist = LLONG_MAX / 4;
		json_t *obj;
		json_t *prox;

		if (holders->element[i]->type != REDIS_REPLY_STRING)
			continue;
		holder_id = holders->element[i]->str;

		size_reply = redis_cmd(app, "HGET %s %s", size_hash, holder_id);
		if (size_reply && size_reply->type == REDIS_REPLY_STRING)
			size = strtoll(size_reply->str, NULL, 10);
		freeReplyObject(size_reply);

		if (source_id)
			dist = shortest_distance(app, source_id, holder_id);

		prox = source_id ? make_proximity_metrics(app, source_id, holder_id) :
			       NULL;
		obj = json_object();
		if (!obj) {
			json_decref(prox);
			continue;
		}
		json_object_set_new(obj, "instanceId", json_string(holder_id));
		json_object_set_new(obj, "size", json_integer(size));
		json_object_set_new(obj, "_dist", json_integer(dist));
		if (prox)
			json_object_set_new(obj, "proximity", prox);

		snprintf(inst_hash, sizeof(inst_hash), "cp:instance:%s", holder_id);
		if (!redis_instance_exists(app, holder_id)) {
			json_decref(obj);
			continue;
		}

		items[i] = obj;
	}

	qsort(items, holders->elements, sizeof(*items), compare_locate_items);

	instances = json_array();
	if (!instances)
		goto out;
	for (i = 0; i < holders->elements; i++) {
		if (!items[i])
			continue;
		json_object_del(items[i], "_dist");
		json_array_append_new(instances, items[i]);
		items[i] = NULL;
	}

	*instances_out = instances;
	ret = 0;
out:
	if (items) {
		for (i = 0; holders && i < holders->elements; i++)
			json_decref(items[i]);
		free(items);
	}
	freeReplyObject(holders);
	return ret;
}

static int handle_locate_key(struct app_state *app,
			     struct MHD_Connection *connection,
			     const char *key)
{
	const char *source;
	json_t *resp;
	json_t *instances;
	int ret;

	source = MHD_lookup_connection_value(connection,
					 MHD_GET_ARGUMENT_KIND,
					 "sourceInstanceId");
	if (source && !redis_instance_exists(app, source))
		source = NULL;

	resp = json_object();
	if (!resp)
		return send_error_response(connection, MHD_HTTP_INTERNAL_SERVER_ERROR,
					   "internal_error",
					   "Failed to allocate response");

	if (build_locate_instances(app, key, source, &instances) != 0)
		instances = json_array();

	json_object_set_new(resp, "key", json_string(key));
	json_object_set_new(resp, "instances", instances);
	ret = send_json_response(connection, MHD_HTTP_OK, resp);
	json_decref(resp);
	return ret;
}

static int handle_nearest_key(struct app_state *app,
			      struct MHD_Connection *connection,
			      const char *key)
{
	const char *source;
	json_t *instances = NULL;
	json_t *best;
	json_t *resp;
	char inst_hash[512];
	char *address;
	json_t *inst_id;
	int ret;

	source = MHD_lookup_connection_value(connection,
					 MHD_GET_ARGUMENT_KIND,
					 "sourceInstanceId");
	if (!source || !redis_instance_exists(app, source))
		return send_error_response(connection, MHD_HTTP_BAD_REQUEST,
					   "bad_request",
					   "sourceInstanceId is required");

	if (build_locate_instances(app, key, source, &instances) != 0)
		instances = json_array();
	if (!instances)
		return send_error_response(connection, MHD_HTTP_INTERNAL_SERVER_ERROR,
					   "internal_error",
					   "Failed to allocate response");

	if (json_array_size(instances) == 0) {
		json_decref(instances);
		return send_error_response(connection, MHD_HTTP_NOT_FOUND,
					   "not_found",
					   "Key not found");
	}

	best = json_array_get(instances, 0);
	inst_id = json_object_get(best, "instanceId");
	if (!json_is_string(inst_id)) {
		json_decref(instances);
		return send_error_response(connection, MHD_HTTP_INTERNAL_SERVER_ERROR,
					   "internal_error",
					   "Invalid state");
	}

	snprintf(inst_hash, sizeof(inst_hash), "cp:instance:%s",
		 json_string_value(inst_id));
	address = redis_hget_strdup(app, inst_hash, "address");

	resp = json_object();
	if (!resp) {
		free(address);
		json_decref(instances);
		return send_error_response(connection, MHD_HTTP_INTERNAL_SERVER_ERROR,
					   "internal_error",
					   "Failed to allocate response");
	}
	json_object_set_new(resp, "key", json_string(key));
	json_object_set(resp, "instanceId", inst_id);
	json_object_set(resp, "size", json_object_get(best, "size"));
	if (address)
		json_object_set_new(resp, "address", json_string(address));
	if (json_object_get(best, "proximity"))
		json_object_set(resp, "proximity", json_object_get(best, "proximity"));

	ret = send_json_response(connection, MHD_HTTP_OK, resp);
	json_decref(resp);
	json_decref(instances);
	free(address);
	return ret;
}

static int handle_list_instance_keys(struct app_state *app,
				     struct MHD_Connection *connection,
				     const char *id)
{
	const char *limit_s;
	const char *cursor_s;
	long long limit;
	long long cursor;
	char keys_hash[512];
	redisReply *reply;
	json_t *resp;
	json_t *keys;
	long long total = 0;
	long long idx;
	size_t i;
	int ret;

	if (!redis_instance_exists(app, id))
		return send_error_response(connection, MHD_HTTP_NOT_FOUND,
					   "not_found",
					   "Instance not found");

	limit_s = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND,
				      "limit");
	cursor_s = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND,
				       "cursor");
	limit = str_to_int(limit_s, 1000);
	if (limit <= 0)
		limit = 1000;
	cursor = str_to_int(cursor_s, 0);
	if (cursor < 0)
		cursor = 0;

	snprintf(keys_hash, sizeof(keys_hash), "cp:instance:%s:keys", id);
	reply = redis_cmd(app, "HGETALL %s", keys_hash);
	if (!reply || reply->type != REDIS_REPLY_ARRAY) {
		freeReplyObject(reply);
		return send_error_response(connection, MHD_HTTP_INTERNAL_SERVER_ERROR,
					   "internal_error",
					   "Failed to query key inventory");
	}

	total = (long long)(reply->elements / 2);
	resp = json_object();
	keys = json_array();
	if (!resp || !keys) {
		json_decref(resp);
		json_decref(keys);
		freeReplyObject(reply);
		return send_error_response(connection, MHD_HTTP_INTERNAL_SERVER_ERROR,
					   "internal_error",
					   "Failed to allocate response");
	}

	idx = 0;
	for (i = 0; i + 1 < reply->elements; i += 2) {
		redisReply *k = reply->element[i];
		redisReply *v = reply->element[i + 1];
		json_t *item;
		long long size;

		if (k->type != REDIS_REPLY_STRING || v->type != REDIS_REPLY_STRING)
			continue;
		if (idx++ < cursor)
			continue;
		if ((long long)json_array_size(keys) >= limit)
			break;
		size = strtoll(v->str, NULL, 10);
		item = json_object();
		if (!item)
			continue;
		json_object_set_new(item, "key", json_string(k->str));
		json_object_set_new(item, "size", json_integer(size));
		json_array_append_new(keys, item);
	}
	freeReplyObject(reply);

	json_object_set_new(resp, "instanceId", json_string(id));
	json_object_set_new(resp, "keys", keys);
	json_object_set_new(resp, "total", json_integer(total));
	if ((cursor + (long long)json_array_size(keys)) < total) {
		char buf[64];
		snprintf(buf, sizeof(buf), "%lld",
			 cursor + (long long)json_array_size(keys));
		json_object_set_new(resp, "cursor", json_string(buf));
	} else {
		json_object_set_new(resp, "cursor", json_null());
	}

	ret = send_json_response(connection, MHD_HTTP_OK, resp);
	json_decref(resp);
	return ret;
}

static int validate_key_info(json_t *entry, const char **key_out,
			     long long *size_out)
{
	json_t *key_j;
	json_t *size_j;

	key_j = json_object_get(entry, "key");
	size_j = json_object_get(entry, "size");
	if (!json_is_string(key_j) ||
	    !(json_is_integer(size_j) || json_is_real(size_j)))
		return -1;

	*key_out = json_string_value(key_j);
	*size_out = json_is_integer(size_j) ?
		(long long)json_integer_value(size_j) :
		(long long)json_number_value(size_j);
	return 0;
}

static int handle_update_instance_keys(struct app_state *app,
				       struct MHD_Connection *connection,
				       const char *id,
				       struct request_ctx *ctx)
{
	json_t *body;
	json_t *mode_j;
	json_t *keys_j;
	const char *mode = "partial";
	int added = 0;
	int removed = 0;
	int updated = 0;
	json_t *resp;
	size_t i;
	int ret;

	if (!redis_instance_exists(app, id))
		return send_error_response(connection, MHD_HTTP_NOT_FOUND,
					   "not_found",
					   "Instance not found");

	cp_log(app, "update_instance_keys requested id=%s mode=%s", id, mode);
	body = parse_json_body(app, ctx);
	if (!body)
		return send_error_response(connection, MHD_HTTP_BAD_REQUEST,
					   "bad_request",
					   "Invalid JSON body");

	mode_j = json_object_get(body, "mode");
	keys_j = json_object_get(body, "keys");
	if (!json_is_array(keys_j)) {
		json_decref(body);
		return send_error_response(connection, MHD_HTTP_BAD_REQUEST,
					   "bad_request",
					   "keys array is required");
	}

	if (json_is_string(mode_j))
		mode = json_string_value(mode_j);
	if (strcmp(mode, "full") != 0 && strcmp(mode, "partial") != 0) {
		json_decref(body);
		return send_error_response(connection, MHD_HTTP_BAD_REQUEST,
					   "bad_request",
					   "mode must be full or partial");
	}
	cp_log(app, "update_instance_keys processing id=%s mode=%s key_count=%zu",
	       id, mode, json_array_size(keys_j));

	if (strcmp(mode, "full") == 0) {
		char inst_keys_hash[512];
		redisReply *old;

		snprintf(inst_keys_hash, sizeof(inst_keys_hash), "cp:instance:%s:keys", id);
		old = redis_cmd(app, "HGETALL %s", inst_keys_hash);
		if (old && old->type == REDIS_REPLY_ARRAY) {
			for (i = 0; i + 1 < old->elements; i += 2) {
				const char *old_key;
				bool still_present = false;
				size_t j;

				if (old->element[i]->type != REDIS_REPLY_STRING)
					continue;
				old_key = old->element[i]->str;
				for (j = 0; j < json_array_size(keys_j); j++) {
					json_t *entry = json_array_get(keys_j, j);
					const char *k;
					long long sz;

					if (validate_key_info(entry, &k, &sz) != 0)
						continue;
					if (strcmp(old_key, k) == 0) {
						still_present = true;
						break;
					}
				}
				if (!still_present) {
					redis_remove_holder(app, id, old_key);
					removed++;
				}
			}
		}
		freeReplyObject(old);
	}

	for (i = 0; i < json_array_size(keys_j); i++) {
		json_t *entry = json_array_get(keys_j, i);
		const char *key;
		long long size;
		bool is_new;

		if (validate_key_info(entry, &key, &size) != 0) {
			json_decref(body);
			return send_error_response(connection, MHD_HTTP_BAD_REQUEST,
						   "bad_request",
						   "Each key entry must include key and size");
		}

		if (redis_upsert_holder(app, id, key, size, &is_new)) {
			if (is_new)
				added++;
			updated++;
		}
	}

	resp = json_object();
	if (!resp) {
		json_decref(body);
		return send_error_response(connection, MHD_HTTP_INTERNAL_SERVER_ERROR,
					   "internal_error",
					   "Failed to allocate response");
	}
	json_object_set_new(resp, "updated", json_integer(updated));
	json_object_set_new(resp, "added", json_integer(added));
	json_object_set_new(resp, "removed", json_integer(removed));
	cp_log(app, "update_instance_keys complete id=%s added=%d removed=%d updated=%d",
	       id, added, removed, updated);
	ret = send_json_response(connection, MHD_HTTP_OK, resp);
	json_decref(resp);
	json_decref(body);
	return ret;
}

static int handle_topology(struct app_state *app,
			   struct MHD_Connection *connection)
{
	char **ids = NULL;
	size_t count = 0;
	size_t i;
	json_t *resp;
	json_t *nodes;
	json_t *edges;
	char *root_id = NULL;
	int ret;

	resp = json_object();
	nodes = json_array();
	edges = json_array();
	if (!resp || !nodes || !edges) {
		json_decref(resp);
		json_decref(nodes);
		json_decref(edges);
		return send_error_response(connection, MHD_HTTP_INTERNAL_SERVER_ERROR,
					   "internal_error",
					   "Failed to allocate response");
	}

	if (get_all_instance_ids(app, &ids, &count) != 0)
		count = 0;

	for (i = 0; i < count; i++) {
		char inst_hash[512];
		char *tier;
		char *status;
		char *region;
		char *parent;
		long long key_count;
		json_t *node;

		snprintf(inst_hash, sizeof(inst_hash), "cp:instance:%s", ids[i]);
		tier = redis_hget_strdup(app, inst_hash, "tier");
		status = redis_hget_strdup(app, inst_hash, "status");
		region = redis_hget_strdup(app, inst_hash, "region");
		parent = redis_hget_strdup(app, inst_hash, "parentId");
		key_count = redis_hget_ll(app, inst_hash, "keyCount", 0);

		node = json_object();
		if (!node)
			goto free_local;
		json_object_set_new(node, "id", json_string(ids[i]));
		json_object_set_new(node, "tier", json_string(tier ? tier : "leaf"));
		json_object_set_new(node, "status",
				    json_string(status ? status : "unverified"));
		if (region)
			json_object_set_new(node, "region", json_string(region));
		json_object_set_new(node, "keyCount", json_integer(key_count));
		json_array_append_new(nodes, node);

		if (!root_id && tier && strcmp(tier, "root") == 0)
			root_id = strdup(ids[i]);

		if (parent && parent[0] != '\0') {
			json_t *edge = json_object();
			json_t *prox;

			if (edge) {
				prox = make_proximity_metrics(app, parent, ids[i]);
				json_object_set_new(edge, "from",
						    json_string(parent));
				json_object_set_new(edge, "to", json_string(ids[i]));
				json_object_set_new(edge, "relationship",
						    json_string("parent-child"));
				if (prox)
					json_object_set_new(edge, "proximity", prox);
				json_array_append_new(edges, edge);
			}
		}

free_local:
		free(tier);
		free(status);
		free(region);
		free(parent);
	}

	json_object_set_new(resp, "nodes", nodes);
	json_object_set_new(resp, "edges", edges);
	if (root_id) {
		json_object_set_new(resp, "rootId", json_string(root_id));
	} else if (count > 0) {
		json_object_set_new(resp, "rootId", json_string(ids[0]));
	}

	ret = send_json_response(connection, MHD_HTTP_OK, resp);
	json_decref(resp);
	free(root_id);
	free_ids(ids, count);
	return ret;
}

static int parse_instance_filter(struct app_state *app, const char *value,
				 char ***ids_out, size_t *count_out)
{
	char *dup;
	char *save;
	char *tok;
	char **ids = NULL;
	size_t count = 0;

	if (!value || !*value)
		return get_all_instance_ids(app, ids_out, count_out);

	dup = strdup(value);
	if (!dup)
		return -1;

	tok = strtok_r(dup, ",", &save);
	while (tok) {
		if (redis_instance_exists(app, tok)) {
			char **next = realloc(ids, (count + 1) * sizeof(*ids));

			if (!next)
				goto err;
			ids = next;
			ids[count] = strdup(tok);
			if (!ids[count])
				goto err;
			count++;
		}
		tok = strtok_r(NULL, ",", &save);
	}

	free(dup);
	*ids_out = ids;
	*count_out = count;
	return 0;
err:
	free(dup);
	free_ids(ids, count);
	return -1;
}

static int handle_proximity_matrix(struct app_state *app,
				   struct MHD_Connection *connection)
{
	const char *filter;
	char **ids = NULL;
	size_t count = 0;
	size_t i;
	size_t j;
	json_t *resp;
	json_t *inst;
	json_t *matrix;
	int ret;

	filter = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND,
				     "instanceIds");
	if (parse_instance_filter(app, filter, &ids, &count) != 0)
		return send_error_response(connection, MHD_HTTP_INTERNAL_SERVER_ERROR,
					   "internal_error",
					   "Failed to build proximity matrix");

	resp = json_object();
	inst = json_array();
	matrix = json_array();
	if (!resp || !inst || !matrix) {
		json_decref(resp);
		json_decref(inst);
		json_decref(matrix);
		free_ids(ids, count);
		return send_error_response(connection, MHD_HTTP_INTERNAL_SERVER_ERROR,
					   "internal_error",
					   "Failed to allocate response");
	}

	for (i = 0; i < count; i++)
		json_array_append_new(inst, json_string(ids[i]));

	for (i = 0; i < count; i++) {
		json_t *row = json_array();

		if (!row)
			continue;
		for (j = 0; j < count; j++) {
			json_t *p = make_proximity_metrics(app, ids[i], ids[j]);

			if (!p)
				p = json_object();
			json_array_append_new(row, p);
		}
		json_array_append_new(matrix, row);
	}

	json_object_set_new(resp, "instances", inst);
	json_object_set_new(resp, "matrix", matrix);
	ret = send_json_response(connection, MHD_HTTP_OK, resp);
	json_decref(resp);
	free_ids(ids, count);
	return ret;
}

static int handle_route(struct app_state *app,
			struct MHD_Connection *connection,
			const char *from,
			const char *to)
{
	char **hops = NULL;
	size_t hop_count = 0;
	json_t *resp;
	json_t *arr;
	size_t i;
	int rc;
	int ret;

	if (!redis_instance_exists(app, from) || !redis_instance_exists(app, to))
		return send_error_response(connection, MHD_HTTP_NOT_FOUND,
					   "not_found",
					   "One or both instances not found");

	rc = shortest_path(app, from, to, &hops, &hop_count);
	if (rc != 0)
		return send_error_response(connection, MHD_HTTP_NOT_FOUND,
					   "not_found",
					   "No route found");

	resp = json_object();
	arr = json_array();
	if (!resp || !arr) {
		json_decref(resp);
		json_decref(arr);
		ret = send_error_response(connection, MHD_HTTP_INTERNAL_SERVER_ERROR,
					  "internal_error",
					  "Failed to allocate response");
		goto out;
	}

	for (i = 0; i < hop_count; i++)
		json_array_append_new(arr, json_string(hops[i]));

	json_object_set_new(resp, "from", json_string(from));
	json_object_set_new(resp, "to", json_string(to));
	json_object_set_new(resp, "hops", arr);
	json_object_set_new(resp, "totalLatencyMs",
			json_real((double)(hop_count - 1) * 1.5));
	json_object_set_new(resp, "totalCost", json_real((double)(hop_count - 1)));

	ret = send_json_response(connection, MHD_HTTP_OK, resp);
	json_decref(resp);
out:
	for (i = 0; i < hop_count; i++)
		free(hops[i]);
	free(hops);
	return ret;
}

static bool validate_event_payload(json_t *event, bool batched,
				   const char **instance_id,
				   const char **event_type,
				   const char **key,
				   long long *size,
				   bool *needs_size)
{
	json_t *instance_j;
	json_t *type_j;
	json_t *timestamp_j;
	json_t *key_j;
	json_t *size_j;
	const char *type;

	instance_j = json_object_get(event, "instanceId");
	type_j = json_object_get(event, "eventType");
	timestamp_j = json_object_get(event, "timestamp");
	key_j = json_object_get(event, "key");
	size_j = json_object_get(event, "size");

	if (!batched && !json_is_string(instance_j))
		return false;
	if (!json_is_string(type_j) || !json_is_string(timestamp_j) ||
	    !json_is_string(key_j))
		return false;

	type = json_string_value(type_j);
	if (strcmp(type, "key_added") && strcmp(type, "key_updated") &&
	    strcmp(type, "key_evicted"))
		return false;

	*needs_size = (!strcmp(type, "key_added") || !strcmp(type, "key_updated"));
	if (*needs_size && !(json_is_integer(size_j) || json_is_real(size_j)))
		return false;

	if (!batched)
		*instance_id = json_string_value(instance_j);
	*event_type = type;
	*key = json_string_value(key_j);
	*size = *needs_size ?
		(json_is_integer(size_j) ? (long long)json_integer_value(size_j) :
		 (long long)json_number_value(size_j)) : 0;
	return true;
}

static int apply_event(struct app_state *app, const char *instance_id,
		       const char *event_type, const char *key, long long size)
{
	bool dummy;

	if (!redis_instance_exists(app, instance_id))
		return -1;

	if (!strcmp(event_type, "key_evicted")) {
		if (!redis_remove_holder(app, instance_id, key))
			return -1;
		return 0;
	}

	if (!redis_upsert_holder(app, instance_id, key, size, &dummy))
		return -1;
	return 0;
}

static int handle_single_event(struct app_state *app,
			       struct MHD_Connection *connection,
			       struct request_ctx *ctx)
{
	json_t *body;
	const char *instance_id;
	const char *event_type;
	const char *key;
	long long size;
	bool needs_size;
	char event_id[128];
	json_t *resp;
	int ret;

	cp_log(app, "single_event received");
	body = parse_json_body(app, ctx);
	if (!body)
		return send_error_response(connection, MHD_HTTP_BAD_REQUEST,
					   "bad_request",
					   "Invalid JSON body");

	if (!validate_event_payload(body, false, &instance_id, &event_type, &key,
				    &size, &needs_size)) {
		json_decref(body);
		return send_error_response(connection, MHD_HTTP_BAD_REQUEST,
					   "bad_request",
					   "Invalid event payload");
	}
	cp_log(app, "single_event validated instance=%s type=%s key=%s",
	       instance_id, event_type, key);

	if (apply_event(app, instance_id, event_type, key, size) != 0) {
		json_decref(body);
		return send_error_response(connection, MHD_HTTP_BAD_REQUEST,
					   "bad_request",
					   "Event references unknown instance");
	}
	cp_log(app, "single_event accepted instance=%s type=%s key=%s", instance_id,
	       event_type, key);

	snprintf(event_id, sizeof(event_id), "evt-%lld-%u", now_unix_ms(),
		 (unsigned)rand());

	resp = json_object();
	if (!resp) {
		json_decref(body);
		return send_error_response(connection, MHD_HTTP_INTERNAL_SERVER_ERROR,
					   "internal_error",
					   "Failed to allocate response");
	}
	json_object_set_new(resp, "accepted", json_true());
	json_object_set_new(resp, "eventId", json_string(event_id));
	ret = send_json_response(connection, MHD_HTTP_ACCEPTED, resp);
	json_decref(resp);
	json_decref(body);
	return ret;
}

static int handle_batch_event(struct app_state *app,
			      struct MHD_Connection *connection,
			      struct request_ctx *ctx)
{
	json_t *body;
	json_t *instance_j;
	json_t *events_j;
	const char *instance_id;
	size_t i;
	char batch_id[128];
	json_t *resp;
	int ret;

	cp_log(app, "batch_event received");
	body = parse_json_body(app, ctx);
	if (!body)
		return send_error_response(connection, MHD_HTTP_BAD_REQUEST,
					   "bad_request",
					   "Invalid JSON body");

	instance_j = json_object_get(body, "instanceId");
	events_j = json_object_get(body, "events");
	if (!json_is_string(instance_j) || !json_is_array(events_j) ||
	    json_array_size(events_j) == 0) {
		json_decref(body);
		return send_error_response(connection, MHD_HTTP_BAD_REQUEST,
					   "bad_request",
					   "instanceId and non-empty events are required");
	}

	instance_id = json_string_value(instance_j);
	cp_log(app, "batch_event processing instance=%s event_count=%zu",
	       instance_id, json_array_size(events_j));
	if (!redis_instance_exists(app, instance_id)) {
		json_decref(body);
		return send_error_response(connection, MHD_HTTP_BAD_REQUEST,
					   "bad_request",
					   "Batch references unknown instance");
	}

	for (i = 0; i < json_array_size(events_j); i++) {
		json_t *event = json_array_get(events_j, i);
		const char *event_type;
		const char *key;
		long long size;
		bool needs_size;

		if (!json_is_object(event) ||
		    !validate_event_payload(event, true, NULL, &event_type, &key,
					    &size, &needs_size)) {
			json_decref(body);
			return send_error_response(connection, MHD_HTTP_BAD_REQUEST,
						   "bad_request",
						   "Invalid batch event payload");
		}
		if (apply_event(app, instance_id, event_type, key, size) != 0) {
			json_decref(body);
			return send_error_response(connection, MHD_HTTP_BAD_REQUEST,
						   "bad_request",
						   "Invalid batch event payload");
		}
		cp_log(app, "batch_event accepted index=%zu instance=%s type=%s key=%s",
		       i, instance_id, event_type, key);
	}

	snprintf(batch_id, sizeof(batch_id), "batch-%lld-%u", now_unix_ms(),
		 (unsigned)rand());
	resp = json_object();
	if (!resp) {
		json_decref(body);
		return send_error_response(connection, MHD_HTTP_INTERNAL_SERVER_ERROR,
					   "internal_error",
					   "Failed to allocate response");
	}
	json_object_set_new(resp, "accepted", json_true());
	json_object_set_new(resp, "count",
			    json_integer((json_int_t)json_array_size(events_j)));
	json_object_set_new(resp, "batchId", json_string(batch_id));
	cp_log(app, "batch_event complete instance=%s count=%zu",
	       instance_id, json_array_size(events_j));
	ret = send_json_response(connection, MHD_HTTP_ACCEPTED, resp);
	json_decref(resp);
	json_decref(body);
	return ret;
}

static int dispatch_request(struct app_state *app,
			    struct MHD_Connection *connection,
			    const char *method,
			    const struct path_parts *path,
			    struct request_ctx *ctx)
{
	if (path->count == 2 &&
	    !strcmp(path->parts[0], "v1") &&
	    !strcmp(path->parts[1], "instances") &&
	    !strcmp(method, "GET"))
		return handle_list_instances(app, connection);

	if (path->count == 3 &&
	    !strcmp(path->parts[0], "v1") &&
	    !strcmp(path->parts[1], "instances") &&
	    !strcmp(method, "GET"))
		return handle_get_instance(app, connection, path->parts[2]);

	if (path->count == 4 &&
	    !strcmp(path->parts[0], "v1") &&
	    !strcmp(path->parts[1], "instances") &&
	    !strcmp(path->parts[3], "register") &&
	    !strcmp(method, "POST"))
		return handle_register_instance(app, connection, path->parts[2], ctx);

	if (path->count == 4 &&
	    !strcmp(path->parts[0], "v1") &&
	    !strcmp(path->parts[1], "instances") &&
	    !strcmp(path->parts[3], "heartbeat") &&
	    !strcmp(method, "POST"))
		return handle_heartbeat(app, connection, path->parts[2], ctx);

	if (path->count == 4 &&
	    !strcmp(path->parts[0], "v1") &&
	    !strcmp(path->parts[1], "instances") &&
	    !strcmp(path->parts[3], "deregister") &&
	    !strcmp(method, "DELETE"))
		return handle_deregister(app, connection, path->parts[2]);

	if (path->count == 4 &&
	    !strcmp(path->parts[0], "v1") &&
	    !strcmp(path->parts[1], "content") &&
	    !strcmp(path->parts[2], "locate") &&
	    !strcmp(method, "GET"))
		return handle_locate_key(app, connection, path->parts[3]);

	if (path->count == 4 &&
	    !strcmp(path->parts[0], "v1") &&
	    !strcmp(path->parts[1], "content") &&
	    !strcmp(path->parts[2], "nearest") &&
	    !strcmp(method, "GET"))
		return handle_nearest_key(app, connection, path->parts[3]);

	if (path->count == 5 &&
	    !strcmp(path->parts[0], "v1") &&
	    !strcmp(path->parts[1], "content") &&
	    !strcmp(path->parts[2], "instances") &&
	    !strcmp(path->parts[4], "keys") &&
	    !strcmp(method, "GET"))
		return handle_list_instance_keys(app, connection, path->parts[3]);

	if (path->count == 5 &&
	    !strcmp(path->parts[0], "v1") &&
	    !strcmp(path->parts[1], "content") &&
	    !strcmp(path->parts[2], "instances") &&
	    !strcmp(path->parts[4], "keys") &&
	    !strcmp(method, "POST"))
		return handle_update_instance_keys(app, connection, path->parts[3],
						   ctx);

	if (path->count == 2 &&
	    !strcmp(path->parts[0], "v1") &&
	    !strcmp(path->parts[1], "topology") &&
	    !strcmp(method, "GET"))
		return handle_topology(app, connection);

	if (path->count == 3 &&
	    !strcmp(path->parts[0], "v1") &&
	    !strcmp(path->parts[1], "topology") &&
	    !strcmp(path->parts[2], "proximity") &&
	    !strcmp(method, "GET"))
		return handle_proximity_matrix(app, connection);

	if (path->count == 5 &&
	    !strcmp(path->parts[0], "v1") &&
	    !strcmp(path->parts[1], "topology") &&
	    !strcmp(path->parts[2], "routes") &&
	    !strcmp(method, "GET"))
		return handle_route(app, connection, path->parts[3], path->parts[4]);

	if (path->count == 2 &&
	    !strcmp(path->parts[0], "v1") &&
	    !strcmp(path->parts[1], "events") &&
	    !strcmp(method, "POST"))
		return handle_single_event(app, connection, ctx);

	if (path->count == 3 &&
	    !strcmp(path->parts[0], "v1") &&
	    !strcmp(path->parts[1], "events") &&
	    !strcmp(path->parts[2], "batch") &&
	    !strcmp(method, "POST"))
		return handle_batch_event(app, connection, ctx);

	return send_error_response(connection, MHD_HTTP_NOT_FOUND, "not_found",
				   "Endpoint not found");
}

static void request_completed(void *cls, struct MHD_Connection *connection,
			      void **con_cls,
			      enum MHD_RequestTerminationCode toe)
{
	struct request_ctx *ctx = *con_cls;

	(void)cls;
	(void)connection;
	(void)toe;
	if (!ctx)
		return;
	free(ctx->body);
	free(ctx);
	*con_cls = NULL;
}

static enum MHD_Result access_handler(void *cls,
				      struct MHD_Connection *connection,
				      const char *url, const char *method,
				      const char *version,
				      const char *upload_data,
				      size_t *upload_data_size, void **con_cls)
{
	struct app_state *app = cls;
	struct request_ctx *ctx = *con_cls;
	struct path_parts path;
	int ret;

	(void)version;

	if (!ctx) {
		ctx = calloc(1, sizeof(*ctx));
		if (!ctx)
			return MHD_NO;
		*con_cls = ctx;
		return MHD_YES;
	}

	if (!strcmp(method, "POST")) {
		if (*upload_data_size > 0) {
			if (!append_request_body(ctx, upload_data, *upload_data_size))
				return MHD_NO;
			*upload_data_size = 0;
			return MHD_YES;
		}
	}

	if (ctx->responded)
		return MHD_YES;

	cp_log(app, "request_start method=%s url=%s", method, url);

	if (!parse_path(url, &path))
		return MHD_NO;

	ret = dispatch_request(app, connection, method, &path, ctx);
	cp_log(app, "request_complete method=%s url=%s result=%d", method, url,
	       ret);
	free_path(&path);
	ctx->responded = true;
	return ret;
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [--port <port>] [--redis-host <host>] "
		"[--redis-port <port>] [--redis-db <db>] [--verbose]\\n",
		prog);
}

static int parse_args(struct app_state *app, int argc, char **argv)
{
	int i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--port") && i + 1 < argc) {
			app->http_port = str_to_int(argv[++i], app->http_port);
			continue;
		}
		if (!strcmp(argv[i], "--redis-host") && i + 1 < argc) {
			strncpy(app->redis_host, argv[++i],
				sizeof(app->redis_host) - 1);
			app->redis_host[sizeof(app->redis_host) - 1] = '\0';
			continue;
		}
		if (!strcmp(argv[i], "--redis-port") && i + 1 < argc) {
			app->redis_port = str_to_int(argv[++i], app->redis_port);
			continue;
		}
		if (!strcmp(argv[i], "--redis-db") && i + 1 < argc) {
			app->redis_db = str_to_int(argv[++i], app->redis_db);
			continue;
		}
		if (!strcmp(argv[i], "--verbose")) {
			app->verbose = true;
			continue;
		}
		usage(argv[0]);
		return -1;
	}
	return 0;
}

static int connect_redis(struct app_state *app)
{
	redisReply *reply;

	app->redis = redisConnect(app->redis_host, app->redis_port);
	if (!app->redis || app->redis->err) {
		if (app->redis)
			fprintf(stderr, "redis connection error: %s\n",
				app->redis->errstr);
		else
			fprintf(stderr, "redis connection error: allocation failed\n");
		return -1;
	}

	reply = redis_cmd(app, "SELECT %d", app->redis_db);
	if (!reply || reply->type == REDIS_REPLY_ERROR) {
		fprintf(stderr, "redis SELECT failed\n");
		freeReplyObject(reply);
		return -1;
	}
	freeReplyObject(reply);
	return 0;
}

int main(int argc, char **argv)
{
	struct MHD_Daemon *daemon;
	struct app_state app;
	const char *env;

	memset(&app, 0, sizeof(app));
	app.http_port = str_to_int(getenv("HAL_CACHE_CONTROL_PLANE_PORT"),
				   DEFAULT_HTTP_PORT);
	strncpy(app.redis_host,
		getenv("HAL_CACHE_REDIS_HOST") ? getenv("HAL_CACHE_REDIS_HOST") :
		DEFAULT_REDIS_HOST,
		sizeof(app.redis_host) - 1);
	app.redis_port = str_to_int(getenv("HAL_CACHE_REDIS_PORT"),
				    DEFAULT_REDIS_PORT);
	app.redis_db = str_to_int(getenv("HAL_CACHE_REDIS_DB"), DEFAULT_REDIS_DB);
	app.verbose = env_is_truthy(getenv("HAL_CACHE_CONTROL_PLANE_VERBOSE"));
	env = getenv("HAL_CACHE_REDIS_URL");
	if (env)
		fprintf(stderr,
			"warning: HAL_CACHE_REDIS_URL is not parsed directly; use "
			"HAL_CACHE_REDIS_HOST/HAL_CACHE_REDIS_PORT/HAL_CACHE_REDIS_DB\n");

	if (parse_args(&app, argc, argv) != 0)
		return 2;

	if (connect_redis(&app) != 0)
		return 1;

	srand((unsigned)time(NULL));
	app.running = 1;
	g_app = &app;
	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);

	daemon = MHD_start_daemon(MHD_USE_INTERNAL_POLLING_THREAD,
				 app.http_port,
				 NULL,
				 NULL,
				 &access_handler,
				 &app,
				 MHD_OPTION_NOTIFY_COMPLETED,
				 &request_completed,
				 NULL,
				 MHD_OPTION_END);
	if (!daemon) {
		fprintf(stderr, "failed to start HTTP daemon on port %d\n",
			app.http_port);
		redisFree(app.redis);
		return 1;
	}

	fprintf(stdout,
		"HAL cache control plane listening on :%d (redis %s:%d db %d)\n",
		app.http_port, app.redis_host, app.redis_port, app.redis_db);
	fflush(stdout);

	while (app.running)
		sleep(1);

	MHD_stop_daemon(daemon);
	redisFree(app.redis);
	return 0;
}
