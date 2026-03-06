#include "cache_control_plane.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool is_valid_tier(const char *tier)
{
	return tier && (!strcmp(tier, "root") || !strcmp(tier, "branch") ||
			!strcmp(tier, "leaf"));
}

int handle_list_instances(struct app_state *app,
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
	       status_filter ? status_filter : "-", region_filter ? region_filter : "-");

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

int handle_get_instance(struct app_state *app,
		       struct MHD_Connection *connection, const char *id)
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
					  "not_found", "Instance not found");
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
			    json_real((double)redis_hget_ll(app, inst_hash,
							   "hitRateMilli", 0) /
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

int handle_register_instance(struct app_state *app,
			    struct MHD_Connection *connection, const char *id,
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
					   "bad_request", "Invalid JSON body");

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
			return send_error_response(connection,
						  MHD_HTTP_BAD_REQUEST,
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

	reply = redis_cmd(
		app,
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

int handle_heartbeat(struct app_state *app, struct MHD_Connection *connection,
		     const char *id, struct request_ctx *ctx)
{
	json_t *body;
	char inst_hash[512];
	char now[64];
	json_t *j;

	if (!redis_instance_exists(app, id))
		return send_error_response(connection, MHD_HTTP_NOT_FOUND,
					  "not_found", "Instance not found");

	cp_log(app, "heartbeat requested id=%s", id);
	body = parse_json_body(app, ctx);
	if (!body)
		return send_error_response(connection, MHD_HTTP_BAD_REQUEST,
					   "bad_request", "Invalid JSON body");

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
		redis_hset_ll(app, inst_hash, "hitRateMilli",
			      (long long)(rate * 1000));
	}
	j = json_object_get(body, "cpuUsage");
	if (json_is_real(j) || json_is_integer(j))
		redis_hset_double(app, inst_hash, "cpuUsage",
				  json_number_value(j));
	j = json_object_get(body, "memoryUsage");
	if (json_is_real(j) || json_is_integer(j))
		redis_hset_double(app, inst_hash, "memoryUsage",
				  json_number_value(j));
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

int handle_deregister(struct app_state *app, struct MHD_Connection *connection,
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
					  "not_found", "Instance not found");
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
