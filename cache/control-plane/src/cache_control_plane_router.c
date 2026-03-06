#include "cache_control_plane.h"

#include <stdio.h>
#include <string.h>

int dispatch_request(struct app_state *app, struct MHD_Connection *connection,
		    const char *method, const struct path_parts *path,
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

void request_completed(void *cls, struct MHD_Connection *connection,
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

enum MHD_Result access_handler(void *cls, struct MHD_Connection *connection,
			      const char *url, const char *method,
			      const char *version, const char *upload_data,
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
			if (!append_request_body(ctx, upload_data,
						*upload_data_size))
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
