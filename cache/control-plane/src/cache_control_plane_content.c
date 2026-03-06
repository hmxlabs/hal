#include "cache_control_plane.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int validate_key_info(json_t *entry, const char **key_out, long long *size_out)
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

static int build_locate_instances(struct app_state *app, const char *key,
				 const char *source_id, json_t **instances_out)
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
		for (i = 0; i < (holders ? holders->elements : 0); i++)
			json_decref(items[i]);
		free(items);
	}
	freeReplyObject(holders);
	return ret;
}

int handle_locate_key(struct app_state *app, struct MHD_Connection *connection,
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

int handle_nearest_key(struct app_state *app, struct MHD_Connection *connection,
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
					  "not_found", "Key not found");
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

int handle_list_instance_keys(struct app_state *app,
			     struct MHD_Connection *connection, const char *id)
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
					  "not_found", "Instance not found");

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

int handle_update_instance_keys(struct app_state *app,
			       struct MHD_Connection *connection,
			       const char *id, struct request_ctx *ctx)
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
					  "not_found", "Instance not found");

	cp_log(app, "update_instance_keys requested id=%s mode=%s", id, mode);
	body = parse_json_body(app, ctx);
	if (!body)
		return send_error_response(connection, MHD_HTTP_BAD_REQUEST,
					  "bad_request", "Invalid JSON body");

	mode_j = json_object_get(body, "mode");
	keys_j = json_object_get(body, "keys");
	if (!json_is_array(keys_j)) {
		json_decref(body);
		return send_error_response(connection, MHD_HTTP_BAD_REQUEST,
					  "bad_request", "keys array is required");
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
		size_t j;
		size_t k;

		snprintf(inst_keys_hash, sizeof(inst_keys_hash), "cp:instance:%s:keys",
			 id);
		old = redis_cmd(app, "HGETALL %s", inst_keys_hash);
		if (old && old->type == REDIS_REPLY_ARRAY) {
			for (j = 0; j + 1 < old->elements; j += 2) {
				const char *old_key;
				bool still_present = false;

				if (old->element[j]->type != REDIS_REPLY_STRING)
					continue;
				old_key = old->element[j]->str;
				for (k = 0; k < json_array_size(keys_j); k++) {
					json_t *entry = json_array_get(keys_j, k);
					const char *key_str;
					long long sz;

					if (validate_key_info(entry, &key_str, &sz) != 0)
						continue;
					if (strcmp(old_key, key_str) == 0) {
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
