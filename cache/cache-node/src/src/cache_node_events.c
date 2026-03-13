/* SPDX-License-Identifier: MIT */

#include "cache_node.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *cache_node_event_type_to_string(
	enum cache_node_event_type type)
{
	switch (type) {
	case CACHE_NODE_EVENT_KEY_ADDED:
		return "key_added";
	case CACHE_NODE_EVENT_KEY_UPDATED:
		return "key_updated";
	case CACHE_NODE_EVENT_KEY_EVICTED:
		return "key_evicted";
	default:
		return "unknown";
	}
}

const char *cache_node_event_type_string(enum cache_node_event_type type)
{
	return cache_node_event_type_to_string(type);
}

void cache_node_event_init(struct cache_node_event *event,
			  const char *instance_id, enum cache_node_event_type type,
			  const char *key, const char *source_instance_id,
			  size_t key_len, unsigned long long retrieval_time_ms)
{
	struct timespec ts;
	struct tm tm;
	time_t sec;

	memset(event, 0, sizeof(*event));
	event->type = type;
	event->key_len = key_len;
	event->retrieval_time_ms = retrieval_time_ms;
	if (instance_id)
		snprintf(event->instance_id, sizeof(event->instance_id), "%s",
			 instance_id);
	else
		event->instance_id[0] = '\0';
	if (key)
		snprintf(event->key, sizeof(event->key), "%s", key);
	else
		event->key[0] = '\0';
	if (source_instance_id)
		snprintf(event->source_instance_id,
			 sizeof(event->source_instance_id), "%s",
			 source_instance_id);
	else
		event->source_instance_id[0] = '\0';

	clock_gettime(CLOCK_REALTIME, &ts);
	sec = (time_t)ts.tv_sec;
	gmtime_r(&sec, &tm);
	snprintf(event->timestamp, sizeof(event->timestamp),
		 "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900,
		 tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
		 (int)(ts.tv_nsec / 1000000));
}

const char *cache_node_event_key(const struct cache_node_event *event)
{
	return event ? event->key : "";
}
