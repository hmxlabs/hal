/* SPDX-License-Identifier: MIT */

#include "cache_node.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct event_capture {
	struct cache_node_event events[32];
	int count;
};

static int capture_notify(const struct cache_node_event *event, void *ctx)
{
	struct event_capture *capture = ctx;

	if (capture->count >= 32)
		return -1;
	memcpy(&capture->events[capture->count], event,
	       sizeof(*event));
	capture->count++;
	return 0;
}

struct test_case {
	int total;
	int failed;
};

static void test_fail(struct test_case *t, const char *name,
		      const char *reason)
{
	t->failed++;
	fprintf(stderr, "FAILED: %s (%s)\n", name, reason);
}

#define CHECK(t, cond, name, reason)                                  \
	do {                                                           \
		(t)->total++;                                          \
		if (!(cond))                                           \
			test_fail((t), (name), (reason));             \
	} while (0)

static int run_store_set_get_and_delete(struct test_case *t)
{
	struct cache_node_store *store;
	struct cache_node_store_config cfg = {
		.instance_id = "node-a",
		.max_bytes = 128,
		.max_items = 16,
	};
	struct event_capture capture = {0};
	void *val = NULL;
	size_t val_len = 0;
	char value1[] = "alpha";
	char value2[] = "betavalue";
	int rc;

	cfg.notify_fn = capture_notify;
	cfg.notify_ctx = &capture;
	rc = cache_node_store_create(&cfg, &store);
	CHECK(t, rc == 0, "create-store", "store creation failed");
	if (rc != 0)
		return 1;

	rc = cache_node_store_set(store, "key-1", value1, strlen(value1), "root",
				 12);
	CHECK(t, rc == 0, "set-1", "set returned error");

	rc = cache_node_store_get(store, "key-1", &val, &val_len);
	CHECK(t, rc == 0, "get-1", "expected key-1 to be present");
	if (rc == 0) {
		CHECK(t, val_len == strlen(value1), "get-1-len",
		      "unexpected length");
		CHECK(t, memcmp(val, value1, val_len) == 0, "get-1-value",
		      "value mismatch");
		free(val);
	}

	rc = cache_node_store_set(store, "key-1", value2, strlen(value2), "root",
				 7);
	CHECK(t, rc == 0, "update-1", "update failed");

	rc = cache_node_store_delete(store, "key-1", "root", 0);
	CHECK(t, rc == 0, "delete-1", "delete failed");

	CHECK(t, capture.count >= 3, "notify-event-count",
	      "expected add/update/delete notifications");

	cache_node_store_destroy(store);
	return t->failed;
}

static int run_eviction_behavior(struct test_case *t)
{
	struct cache_node_store *store;
	struct cache_node_store_config cfg = {
		.instance_id = "node-b",
		.max_bytes = 12,
		.max_items = 4,
		.notify_fn = NULL,
		.notify_ctx = NULL,
	};
	struct event_capture capture = {0};
	struct cache_node_event *ev;
	int rc;

	cfg.notify_fn = capture_notify;
	cfg.notify_ctx = &capture;
	rc = cache_node_store_create(&cfg, &store);
	CHECK(t, rc == 0, "evict-create", "failed to create store");
	if (rc != 0)
		return 1;

	/* Populate to exceed max bytes and force LRU eviction. */
	CHECK(t, cache_node_store_set(store, "a", "11111", 5, "a", 0) == 0,
	      "evict-set-a", "set failed");
	CHECK(t, cache_node_store_set(store, "b", "2222", 4, "a", 0) == 0,
	      "evict-set-b", "set failed");
	CHECK(t, cache_node_store_set(store, "c", "3333", 4, "a", 0) == 0,
	      "evict-set-c", "set should evict previous entry");

	/* Ensure that the first key was evicted and event captured. */
	{
		void *tmp = NULL;
		size_t tmp_len = 0;

		rc = cache_node_store_get(store, "a", &tmp, &tmp_len);
		free(tmp);
		CHECK(t, rc == -ENOENT, "evict-a-removed",
		      "first key should be evicted");
	}

	ev = &capture.events[0];
	CHECK(t, ev->type == CACHE_NODE_EVENT_KEY_ADDED ||
		      ev->type == CACHE_NODE_EVENT_KEY_UPDATED,
	      "evict-notify-first", "first notifications invalid");

	cache_node_store_destroy(store);
	return t->failed;
}

int main(void)
{
	struct test_case t = {0, 0};

	run_store_set_get_and_delete(&t);
	run_eviction_behavior(&t);

	printf("cache-node unit tests: %d total, %d failed\n", t.total, t.failed);
	return t.failed;
}
