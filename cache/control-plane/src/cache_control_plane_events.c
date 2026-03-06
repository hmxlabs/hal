#include "cache_control_plane.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool validate_event_payload(json_t *event, bool batched, const char **instance_id,
			   const char **event_type, const char **key,
			   long long *size, bool *needs_size)
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

int handle_single_event(struct app_state *app, struct MHD_Connection *connection,
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
					  "bad_request", "Invalid JSON body");

	if (!validate_event_payload(body, false, &instance_id, &event_type, &key,
				   &size, &needs_size)) {
		json_decref(body);
		return send_error_response(connection, MHD_HTTP_BAD_REQUEST,
					  "bad_request", "Invalid event payload");
	}
	cp_log(app, "single_event validated instance=%s type=%s key=%s", instance_id,
	       event_type, key);

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

int handle_batch_event(struct app_state *app, struct MHD_Connection *connection,
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
					  "bad_request", "Invalid JSON body");

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
	cp_log(app, "batch_event processing instance=%s event_count=%zu", instance_id,
	       json_array_size(events_j));
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
	cp_log(app, "batch_event complete instance=%s count=%zu", instance_id,
	       json_array_size(events_j));
	ret = send_json_response(connection, MHD_HTTP_ACCEPTED, resp);
	json_decref(resp);
	json_decref(body);
	return ret;
}
