/* SPDX-License-Identifier: MIT */

#include "cache_node.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct cache_node_entry {
	char *key;
	char *value;
	size_t value_len;
	unsigned long long last_access_ms;
	struct cache_node_entry *bucket_next;
	struct cache_node_entry *lru_prev;
	struct cache_node_entry *lru_next;
};

struct cache_node_store {
	char instance_id[CACHE_NODE_MAX_INSTANCE_ID];
	size_t max_bytes;
	size_t max_items;
	size_t used_bytes;
	size_t key_count;
	size_t bucket_count;
	struct cache_node_entry **buckets;
	struct cache_node_entry *lru_head;
	struct cache_node_entry *lru_tail;
	cache_node_notify_fn notify_fn;
	void *notify_ctx;
};

static unsigned int cache_node_hash(const char *key)
{
	unsigned int hash = 5381;
	size_t i;

	for (i = 0; i < strlen(key); i++)
		hash = ((hash << 5) + hash) + (unsigned char)key[i];
	return hash;
}

static unsigned long long cache_node_time_now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned long long)ts.tv_sec * 1000ULL +
	       (unsigned long long)(ts.tv_nsec / 1000000ULL);
}

static void cache_node_event_emit(struct cache_node_store *store,
				 const struct cache_node_event *event)
{
	if (store->notify_fn)
		store->notify_fn(event, store->notify_ctx);
}

static int cache_node_relink_lru(struct cache_node_store *store,
				struct cache_node_entry *entry)
{
	if (entry == store->lru_head)
		return 0;

	if (entry->lru_prev)
		entry->lru_prev->lru_next = entry->lru_next;
	if (entry->lru_next)
		entry->lru_next->lru_prev = entry->lru_prev;
	if (store->lru_tail == entry)
		store->lru_tail = entry->lru_prev;

	entry->lru_prev = NULL;
	entry->lru_next = store->lru_head;
	if (store->lru_head)
		store->lru_head->lru_prev = entry;
	store->lru_head = entry;
	if (!store->lru_tail)
		store->lru_tail = entry;

	return 0;
}

static void cache_node_lru_remove(struct cache_node_store *store,
				 struct cache_node_entry *entry)
{
	if (!entry->lru_prev)
		store->lru_head = entry->lru_next;
	else
		entry->lru_prev->lru_next = entry->lru_next;

	if (!entry->lru_next)
		store->lru_tail = entry->lru_prev;
	else
		entry->lru_next->lru_prev = entry->lru_prev;

	entry->lru_prev = NULL;
	entry->lru_next = NULL;
}

static void cache_node_lru_push_front(struct cache_node_store *store,
				     struct cache_node_entry *entry)
{
	entry->lru_prev = NULL;
	entry->lru_next = store->lru_head;
	if (store->lru_head)
		store->lru_head->lru_prev = entry;
	store->lru_head = entry;
	if (!store->lru_tail)
		store->lru_tail = entry;
}

static int cache_node_find_entry(struct cache_node_store *store, const char *key,
					struct cache_node_entry **out_entry)
{
	struct cache_node_entry *entry;
	unsigned int bucket;

	if (!store || !key || !out_entry)
		return -1;

	if (strlen(key) == 0 || strlen(key) > CACHE_NODE_MAX_KEY_LEN - 1) {
		*out_entry = NULL;
		return -1;
	}

	bucket = cache_node_hash(key) % store->bucket_count;
	entry = store->buckets[bucket];
	for (; entry; entry = entry->bucket_next) {
		if (strcmp(entry->key, key) == 0) {
			*out_entry = entry;
			return 0;
		}
	}

	*out_entry = NULL;
	return -1;
}

static void cache_node_free_entry(struct cache_node_entry *entry)
{
	free(entry->key);
	free(entry->value);
	free(entry);
}

static int cache_node_remove_entry(struct cache_node_store *store,
				  const char *key,
				  const char *source_instance_id,
				  unsigned long long retrieval_ms)
{
	struct cache_node_entry *entry;
	unsigned int bucket;
	struct cache_node_entry **prev;
	struct cache_node_entry *cursor;
	struct cache_node_event event;

	if (!store || !key)
		return -1;

	if (cache_node_find_entry(store, key, &entry) != 0 || !entry)
		return -ENOENT;

	bucket = cache_node_hash(key) % store->bucket_count;
	prev = &store->buckets[bucket];
	for (cursor = *prev; cursor; prev = &cursor->bucket_next, cursor = cursor->bucket_next) {
		if (cursor == entry) {
			*prev = cursor->bucket_next;
			break;
		}
	}

	cache_node_lru_remove(store, entry);
	store->used_bytes -= entry->value_len;
	store->key_count--;
	cache_node_event_init(&event, store->instance_id,
			     CACHE_NODE_EVENT_KEY_EVICTED, key,
			     source_instance_id, 0, retrieval_ms);
	cache_node_event_emit(store, &event);
	cache_node_free_entry(entry);
	return 0;
}

static int cache_node_delete_entry_internal(struct cache_node_store *store,
                                          const char *key,
                                          const char *source_instance_id,
                                          unsigned long long retrieval_ms)
{
	struct cache_node_entry *entry;

	if (!store || !key)
		return -1;

	if (cache_node_find_entry(store, key, &entry) != 0)
		return -ENOENT;

	return cache_node_remove_entry(store, key, source_instance_id, retrieval_ms);
}

static int cache_node_evict_if_needed(struct cache_node_store *store)
{
	while ((store->max_bytes && store->used_bytes > store->max_bytes) ||
	       (store->max_items && store->key_count > store->max_items)) {
		if (!store->lru_tail)
			return -1;
		if (cache_node_remove_entry(store, store->lru_tail->key,
					   store->instance_id, 0ULL) != 0)
			return -1;
	}
	return 0;
}

static int cache_node_create_bucket_store(struct cache_node_store *store,
					 size_t bucket_count)
{
	store->buckets = calloc(bucket_count, sizeof(struct cache_node_entry *));
	if (!store->buckets)
		return -1;
	store->bucket_count = bucket_count;
	return 0;
}

int cache_node_store_create(const struct cache_node_store_config *config,
			   struct cache_node_store **out_store)
{
	struct cache_node_store *store;

	if (!config || !out_store || !config->instance_id ||
	    *config->instance_id == '\0')
		return -EINVAL;

	store = calloc(1, sizeof(*store));
	if (!store)
		return -ENOMEM;

	if (cache_node_create_bucket_store(store, CACHE_NODE_DEFAULT_BUCKETS) !=
	    0) {
		free(store);
		return -ENOMEM;
	}

	snprintf(store->instance_id, sizeof(store->instance_id), "%s",
		 config->instance_id);
	store->max_bytes = config->max_bytes;
	store->max_items = config->max_items;
	store->notify_fn = config->notify_fn;
	store->notify_ctx = config->notify_ctx;

	*out_store = store;
	return 0;
}

void cache_node_store_destroy(struct cache_node_store *store)
{
	size_t b;

	if (!store)
		return;

	for (b = 0; b < store->bucket_count; b++) {
		struct cache_node_entry *entry = store->buckets[b];
		while (entry) {
			struct cache_node_entry *next = entry->bucket_next;
			cache_node_free_entry(entry);
			entry = next;
		}
	}
	free(store->buckets);
	free(store);
}

int cache_node_store_set(struct cache_node_store *store, const char *key,
			const void *value, size_t value_len,
			const char *source_instance_id,
			unsigned long long retrieval_time_ms)
{
	struct cache_node_entry *entry;
	struct cache_node_entry *new_entry;
	char *value_copy;
	struct cache_node_event event;
	int exists = 0;

	if (!store || !key || !value || value_len == 0)
		return -EINVAL;
	if (strlen(key) == 0 || strlen(key) >= CACHE_NODE_MAX_KEY_LEN)
		return -EINVAL;
	if (source_instance_id && strlen(source_instance_id) >=
	    CACHE_NODE_MAX_INSTANCE_ID)
		return -EINVAL;
	if (store->max_bytes > 0 && value_len > store->max_bytes)
		return -ENOSPC;

	if (cache_node_find_entry(store, key, &entry) == 0 && entry)
		exists = 1;

	value_copy = malloc(value_len + 1);
	if (!value_copy)
		return -ENOMEM;
	memcpy(value_copy, value, value_len);
	value_copy[value_len] = '\0';

	if (exists) {
		size_t new_used = store->used_bytes -
				  entry->value_len + value_len;

		free(entry->value);
		entry->value = value_copy;
		store->used_bytes = new_used;
		entry->value_len = value_len;
		entry->last_access_ms = cache_node_time_now_ms();
		cache_node_relink_lru(store, entry);
		cache_node_event_init(&event, store->instance_id,
				     CACHE_NODE_EVENT_KEY_UPDATED, key,
					     source_instance_id ? source_instance_id : "",
				     value_len, retrieval_time_ms);
		cache_node_event_emit(store, &event);
		return cache_node_evict_if_needed(store);
	}

	new_entry = calloc(1, sizeof(*new_entry));
	if (!new_entry) {
		free(value_copy);
		return -ENOMEM;
	}
	new_entry->key = strdup(key);
	if (!new_entry->key) {
		free(value_copy);
		free(new_entry);
		return -ENOMEM;
	}
	new_entry->value = value_copy;
	new_entry->value_len = value_len;
	new_entry->last_access_ms = cache_node_time_now_ms();
	cache_node_lru_push_front(store, new_entry);

	{
		unsigned int bucket = cache_node_hash(key) %
				     store->bucket_count;
		new_entry->bucket_next = store->buckets[bucket];
		store->buckets[bucket] = new_entry;
	}

	store->used_bytes += value_len;
	store->key_count++;

	cache_node_event_init(&event, store->instance_id,
			     CACHE_NODE_EVENT_KEY_ADDED, key, source_instance_id,
			     value_len, retrieval_time_ms);
	cache_node_event_emit(store, &event);
	return cache_node_evict_if_needed(store);
}

int cache_node_store_get(const struct cache_node_store *cache,
			const char *key, void **value, size_t *value_len)
{
	struct cache_node_store *store = (struct cache_node_store *)cache;
	struct cache_node_entry *entry;
	void *value_copy;

	if (!store || !key || !value || !value_len)
		return -EINVAL;
	if (cache_node_find_entry(store, key, &entry) != 0 || !entry)
		return -ENOENT;

	value_copy = malloc(entry->value_len);
	if (!value_copy)
		return -ENOMEM;
	memcpy(value_copy, entry->value, entry->value_len);
	*value = value_copy;
	*value_len = entry->value_len;
	entry->last_access_ms = cache_node_time_now_ms();
	cache_node_relink_lru(store, entry);
	return 0;
}

int cache_node_store_delete(struct cache_node_store *store, const char *key,
			   const char *source_instance_id,
			   unsigned long long retrieval_time_ms)
{
	return cache_node_delete_entry_internal(store, key, source_instance_id,
					       retrieval_time_ms);
}

void cache_node_store_stats(const struct cache_node_store *store,
			   size_t *key_count, size_t *total_bytes,
			   size_t *max_bytes, size_t *max_items)
{
	if (!store)
		return;
	if (key_count)
		*key_count = store->key_count;
	if (total_bytes)
		*total_bytes = store->used_bytes;
	if (max_bytes)
		*max_bytes = store->max_bytes;
	if (max_items)
		*max_items = store->max_items;
}
