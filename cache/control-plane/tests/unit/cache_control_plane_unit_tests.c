#include "cache_control_plane.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct app_state *g_app;

struct test_case {
	int total;
	int failed;
};

static void test_fail(struct test_case *t, const char *name, const char *reason)
{
	t->failed++;
	fprintf(stderr, "FAILED: %s (%s)\n", name, reason);
}

#define CHECK(t, condition, name, reason)                                      \
	do {                                                                   \
		(t)->total++;                                                  \
		if (!(condition))                                              \
			test_fail((t), (name), (reason));                         \
	} while (0)

static void free_hops(char **hops, size_t count)
{
	size_t i;

	if (!hops)
		return;
	for (i = 0; i < count; i++)
		free(hops[i]);
	free(hops);
}

int main(void)
{
	struct test_case t = {0, 0};
	struct request_ctx ctx = {0};
	struct path_parts path;
	json_t *obj;
	json_t *a;
	json_t *b;
	json_t *ev;
	const char *key = NULL;
	long long size = 0;
	const char *instance = NULL;
	const char *etype = NULL;
	bool needs_size = false;
	const char *ids[4];
	const char *parents[4];
	char **route = NULL;
	size_t route_len = 0;
	char now[64];

	CHECK(&t, env_is_truthy("1"), "env_is_truthy_true", "expected 1");
	CHECK(&t, env_is_truthy("true"), "env_is_truthy_true_alpha", "expected true");
	CHECK(&t, !env_is_truthy("no"), "env_is_truthy_false", "expected false");
	CHECK(&t, str_to_int("42", 7) == 42, "str_to_int_valid",
	      "expected 42");
	CHECK(&t, str_to_int("not-a-number", 7) == 7, "str_to_int_invalid",
	      "expected default 7");

	CHECK(&t, now_unix_ms() > 0, "now_unix_ms", "timestamp must be positive");
	now_iso8601(now, sizeof(now));
	CHECK(&t, strlen(now) == 20 || strlen(now) == 21,
	      "now_iso8601_length", "unexpected timestamp length");

	CHECK(&t, parse_path("/v1/topology/proximity", &path),
	      "parse_path_absolute", "unable to parse path");
	CHECK(&t, path.count == 3, "parse_path_count", "wrong segment count");
	CHECK(&t, strcmp(path.parts[0], "v1") == 0 &&
		     strcmp(path.parts[1], "topology") == 0 &&
		     strcmp(path.parts[2], "proximity") == 0,
	      "parse_path_segments", "segments not parsed");
	free_path(&path);

	CHECK(&t, parse_path(NULL, &path), "parse_path_null", "expected null path to parse as empty");
	CHECK(&t, path.count == 0, "parse_path_null_count", "expected zero path parts");
	free_path(&path);

	append_request_body(&ctx, "{\"key\":\"alpha\"}", strlen("{\"key\":\"alpha\"}"));
	obj = parse_json_body(NULL, &ctx);
	CHECK(&t, obj != NULL, "parse_json_body_valid", "expected valid JSON");
	json_decref(obj);
	free(ctx.body);
	ctx.body = NULL;
	ctx.body_len = 0;
	obj = parse_json_body(NULL, &ctx);
	CHECK(&t, obj == NULL, "parse_json_body_empty", "expected empty input null");

	obj = json_object();
	json_object_set_new(obj, "key", json_string("alpha"));
	json_object_set_new(obj, "size", json_integer(42));
	CHECK(&t, validate_key_info(obj, &key, &size) == 0 &&
		     strcmp(key, "alpha") == 0 && size == 42,
	      "validate_key_info_ok", "valid key entry should pass");
	json_decref(obj);
	obj = json_object();
	json_object_set_new(obj, "size", json_integer(42));
	CHECK(&t, validate_key_info(obj, &key, &size) != 0,
	      "validate_key_info_missing_key", "missing key should fail");
	json_decref(obj);

	CHECK(&t, is_valid_tier("root"), "is_valid_tier_root", "root should be valid");
	CHECK(&t, !is_valid_tier("unknown"), "is_valid_tier_bad", "unknown invalid");

	ids[0] = "root";
	ids[1] = "branch-a";
	ids[2] = "branch-b";
	ids[3] = "leaf";
	parents[0] = NULL;
	parents[1] = "root";
	parents[2] = "branch-a";
	parents[3] = "branch-b";
	CHECK(&t, shortest_path_from_parents(ids, 4, parents, "root", "leaf",
					    &route, &route_len) == 0 &&
		     route_len == 4 &&
		     strcmp(route[0], "root") == 0 &&
		     strcmp(route[1], "branch-a") == 0 &&
		     strcmp(route[2], "branch-b") == 0 &&
		     strcmp(route[3], "leaf") == 0,
	      "shortest_path_from_parents", "unexpected route");
	free_hops(route, route_len);

	CHECK(&t, shortest_path_from_parents(ids, 4, parents, "leaf", "root", &route,
					    &route_len) == 0 &&
		     route_len == 4 &&
		     strcmp(route[0], "leaf") == 0 &&
		     strcmp(route[1], "branch-b") == 0 &&
		     strcmp(route[2], "branch-a") == 0 &&
		     strcmp(route[3], "root") == 0,
	      "shortest_path_from_parents_reverse", "expected reverse route");
	free_hops(route, route_len);

	obj = json_object();
	json_object_set_new(obj, "instanceId", json_string("node-1"));
	json_object_set_new(obj, "eventType", json_string("key_added"));
	json_object_set_new(obj, "timestamp", json_string("2026-03-06T00:00:00Z"));
	json_object_set_new(obj, "key", json_string("alpha"));
	json_object_set_new(obj, "size", json_integer(11));
	CHECK(&t, validate_event_payload(obj, false, &instance, &etype, &key, &size,
					&needs_size) &&
		     instance != NULL &&
		     strcmp(etype, "key_added") == 0 &&
		     needs_size &&
		     size == 11,
	      "validate_event_payload_single", "valid event should pass");
	json_decref(obj);

	ev = json_object();
	json_object_set_new(ev, "eventType", json_string("key_evicted"));
	json_object_set_new(ev, "timestamp", json_string("2026-03-06T00:00:00Z"));
	json_object_set_new(ev, "key", json_string("alpha"));
	CHECK(&t, validate_event_payload(ev, true, NULL, &etype, &key, &size,
					&needs_size) &&
		     !needs_size &&
		     strcmp(etype, "key_evicted") == 0,
	      "validate_event_payload_batch", "valid batched event should pass");
	json_decref(ev);

	a = json_object();
	b = json_object();
	json_object_set_new(a, "instanceId", json_string("a"));
	json_object_set_new(a, "_dist", json_integer(1));
	json_object_set_new(b, "instanceId", json_string("b"));
	json_object_set_new(b, "_dist", json_integer(3));
	CHECK(&t, compare_locate_items(&a, &b) < 0, "compare_locate_items_order",
	      "a should sort before b");
	json_decref(a);
	json_decref(b);

	printf("control-plane unit tests: %d total, %d failed\n", t.total, t.failed);
	return t.failed;
}
