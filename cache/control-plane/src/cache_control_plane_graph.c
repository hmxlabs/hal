#include "cache_control_plane.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int shortest_path_from_parents(const char *const *ids, size_t count,
			      const char *const *parents, const char *from,
			      const char *to, char ***hops_out,
			      size_t *hop_count_out)
{
	char *adj = NULL;
	int *queue = NULL;
	int *prev = NULL;
	int from_idx;
	int to_idx;
	int q_head = 0;
	int q_tail = 0;
	int i;
	size_t pos;
	char **hops = NULL;
	size_t hop_count = 0;
	int node;
	int rc = -1;

	if (!ids || !parents || !from || !to || !hops_out || !hop_count_out)
		return -1;

	from_idx = index_of_id((char **)ids, count, from);
	to_idx = index_of_id((char **)ids, count, to);
	if (from_idx < 0 || to_idx < 0) {
		return -2;
	}

	adj = calloc(count * count, sizeof(*adj));
	queue = malloc(count * sizeof(*queue));
	prev = malloc(count * sizeof(*prev));
	if (!adj || !queue || !prev)
		goto out;

	for (i = 0; i < (int)count; i++) {
		const char *parent = parents[i];
		int pidx;

		prev[i] = -1;
		if (!parent || parent[0] == '\0')
			continue;
		pidx = index_of_id((char **)ids, count, parent);
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

	if (prev[to_idx] == -1)
		goto out;

	node = to_idx;
	hop_count = 0;
	while (1) {
		hop_count++;
		if (node == from_idx) {
			break;
		}
		node = prev[node];
		if (node < 0 || (size_t)node >= count) {
			rc = -4;
			goto out;
		}
	}

	hops = calloc(hop_count, sizeof(*hops));
	if (!hops)
		goto out;

	node = to_idx;
	for (pos = hop_count; pos > 0; pos--) {
		hops[pos - 1] = strdup(ids[node]);
		if (!hops[pos - 1]) {
			size_t j;

			for (j = pos; j < hop_count; j++)
				free(hops[j]);
			free(hops);
			goto out;
		}
		if (node == from_idx)
			break;
		node = prev[node];
	}

	*hops_out = hops;
	*hop_count_out = hop_count;
	rc = 0;
	goto out;

out:
	free(adj);
	free(queue);
	free(prev);
	return rc;
}

int shortest_path(struct app_state *app, const char *from, const char *to,
		 char ***hops_out, size_t *hop_count_out)
{
	char **ids = NULL;
	size_t count = 0;
	char **parents = NULL;
	size_t i;
	char inst_hash[512];
	int rc = -1;

	if (get_all_instance_ids(app, &ids, &count) != 0)
		return -1;
	if (count == 0)
		goto out;

	parents = calloc(count, sizeof(*parents));
	if (!parents)
		goto out;

	for (i = 0; i < count; i++) {
		char *parent;

		snprintf(inst_hash, sizeof(inst_hash), "cp:instance:%s", ids[i]);
		parent = redis_hget_strdup(app, inst_hash, "parentId");
		if (!parent || parent[0] == '\0') {
			free(parent);
			continue;
		}
		parents[i] = parent;
	}

	rc = shortest_path_from_parents((const char *const *)ids, count,
				       (const char *const *)parents, from, to,
				       hops_out, hop_count_out);

	for (i = 0; i < count; i++)
		free(parents[i]);
out:
	free(parents);
	free_ids(ids, count);
	return rc;
}

long long shortest_distance(struct app_state *app, const char *from, const char *to)
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

json_t *make_proximity_metrics(struct app_state *app, const char *from,
			      const char *to)
{
	json_t *obj;
	long long dist;
	double latency;
	double bandwidth;
	double cost;

	(void)app;

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

int compare_locate_items(const void *a, const void *b)
{
	const json_t *ia = *(const json_t *const *)a;
	const json_t *ib = *(const json_t *const *)b;
	if (!ia && !ib)
		return 0;
	if (!ia)
		return 1;
	if (!ib)
		return -1;

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
