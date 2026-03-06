#include "cache_control_plane.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int handle_topology(struct app_state *app, struct MHD_Connection *connection)
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

int parse_instance_filter(struct app_state *app, const char *value, char ***ids_out,
			 size_t *count_out)
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

int handle_proximity_matrix(struct app_state *app,
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

int handle_route(struct app_state *app, struct MHD_Connection *connection,
		const char *from, const char *to)
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
					  "not_found", "No route found");

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
