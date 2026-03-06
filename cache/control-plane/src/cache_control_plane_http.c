#include "cache_control_plane.h"

#include <stdlib.h>
#include <string.h>

json_t *json_error(const char *code, const char *message)
{
	json_t *obj;

	obj = json_object();
	if (!obj)
		return NULL;
	json_object_set_new(obj, "code", json_string(code));
	json_object_set_new(obj, "message", json_string(message));
	return obj;
}

int send_json_response(struct MHD_Connection *connection, unsigned int status,
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

int send_empty_response(struct MHD_Connection *connection, unsigned int status)
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

int send_error_response(struct MHD_Connection *connection, unsigned int status,
		       const char *code, const char *message)
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
