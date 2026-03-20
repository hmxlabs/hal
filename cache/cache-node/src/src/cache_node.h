/* SPDX-License-Identifier: MIT */
/* Copyright */

#ifndef CACHE_NODE_H
#define CACHE_NODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CACHE_NODE_MAX_INSTANCE_ID 64
#define CACHE_NODE_MAX_KEY_LEN 256
#define CACHE_NODE_MAX_TIMESTAMP_LEN 64
#define CACHE_NODE_DEFAULT_BUCKETS 256

enum cache_node_event_type {
	CACHE_NODE_EVENT_KEY_ADDED = 0,
	CACHE_NODE_EVENT_KEY_UPDATED,
	CACHE_NODE_EVENT_KEY_EVICTED,
};

enum cache_node_eviction_policy {
	CACHE_NODE_EVICTION_POLICY_ALLKEYS_LRU = 0,
	CACHE_NODE_EVICTION_POLICY_NOEVICTION,
};

struct cache_node_event {
	enum cache_node_event_type type;
	char instance_id[CACHE_NODE_MAX_INSTANCE_ID];
	char key[CACHE_NODE_MAX_KEY_LEN];
	size_t key_len;
	char source_instance_id[CACHE_NODE_MAX_INSTANCE_ID];
	unsigned long long retrieval_time_ms;
	char timestamp[CACHE_NODE_MAX_TIMESTAMP_LEN];
};

struct cache_node_store;

typedef int (*cache_node_notify_fn)(const struct cache_node_event *event,
				   void *ctx);

struct cache_node_store_config {
	const char *instance_id;
	size_t max_bytes;
	size_t max_items;
	cache_node_notify_fn notify_fn;
	void *notify_ctx;
};

struct cache_node_server_config {
	const char *listen_host;
	unsigned short listen_port;
	const char *control_plane_events_url;
	bool verbose;
};

int cache_node_store_create(const struct cache_node_store_config *config,
			   struct cache_node_store **store);
void cache_node_store_destroy(struct cache_node_store *store);
int cache_node_store_set_limits(struct cache_node_store *store,
			       size_t max_bytes, size_t max_items);
int cache_node_store_set_eviction_policy(
	struct cache_node_store *store,
	enum cache_node_eviction_policy policy);
size_t cache_node_store_get_max_bytes(const struct cache_node_store *store);
size_t cache_node_store_get_max_items(const struct cache_node_store *store);
const char *cache_node_store_get_instance_id(const struct cache_node_store *store);
enum cache_node_eviction_policy cache_node_store_get_eviction_policy(
	const struct cache_node_store *store);
int cache_node_store_set(struct cache_node_store *store, const char *key,
			const void *value, size_t value_len,
			const char *source_instance_id,
			unsigned long long retrieval_time_ms);
int cache_node_store_get(const struct cache_node_store *store, const char *key,
			void **value, size_t *value_len);
int cache_node_store_delete(struct cache_node_store *store, const char *key,
			   const char *source_instance_id,
			   unsigned long long retrieval_time_ms);
void cache_node_store_stats(const struct cache_node_store *store,
			   size_t *key_count, size_t *total_bytes,
			   size_t *max_bytes, size_t *max_items);
int cache_node_server_run(struct cache_node_store *store,
			 const struct cache_node_server_config *cfg);

const char *cache_node_event_type_string(enum cache_node_event_type type);
void cache_node_event_init(struct cache_node_event *event,
			  const char *instance_id, enum cache_node_event_type type,
			  const char *key, const char *source_instance_id,
			  size_t key_len, unsigned long long retrieval_time_ms);
const char *cache_node_event_key(const struct cache_node_event *event);

struct cache_node_http_notifier;
struct cache_node_http_notifier *cache_node_http_notifier_create(
	const char *endpoint_url);
void cache_node_http_notifier_destroy(struct cache_node_http_notifier *ctx);
int cache_node_http_notifier(const struct cache_node_event *event, void *ctx);

#endif /* CACHE_NODE_H */
