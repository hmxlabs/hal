#include "cache_control_plane.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

redisReply *redis_cmd(struct app_state *app, const char *fmt, ...)
{
	va_list ap;
	redisReply *reply;

	va_start(ap, fmt);
	reply = redisvCommand(app->redis, fmt, ap);
	va_end(ap);
	return reply;
}

bool reply_ok_integer(redisReply *reply)
{
	return reply && reply->type == REDIS_REPLY_INTEGER;
}

bool redis_instance_exists(struct app_state *app, const char *id)
{
	redisReply *reply;
	char key[512];
	bool exists = false;

	snprintf(key, sizeof(key), "cp:instance:%s", id);
	reply = redis_cmd(app, "EXISTS %s", key);
	if (reply_ok_integer(reply) && reply->integer == 1)
		exists = true;
	freeReplyObject(reply);
	return exists;
}

char *redis_hget_strdup(struct app_state *app, const char *hash,
		       const char *field)
{
	redisReply *reply;
	char *result = NULL;

	reply = redis_cmd(app, "HGET %s %s", hash, field);
	if (reply && reply->type == REDIS_REPLY_STRING)
		result = strdup(reply->str);
	freeReplyObject(reply);
	return result;
}

long long redis_hget_ll(struct app_state *app, const char *hash,
		       const char *field, long long dflt)
{
	redisReply *reply;
	char *end;
	long long val = dflt;

	reply = redis_cmd(app, "HGET %s %s", hash, field);
	if (!reply || reply->type != REDIS_REPLY_STRING)
		goto out;

	errno = 0;
	val = strtoll(reply->str, &end, 10);
	if (errno || end == reply->str || *end != '\0')
		val = dflt;
out:
	freeReplyObject(reply);
	return val;
}

bool redis_hset_ll(struct app_state *app, const char *hash, const char *field,
		  long long value)
{
	redisReply *reply;
	bool ok = false;

	reply = redis_cmd(app, "HSET %s %s %lld", hash, field, value);
	if (reply_ok_integer(reply))
		ok = true;
	freeReplyObject(reply);
	return ok;
}

bool redis_hset_double(struct app_state *app, const char *hash, const char *field,
		      double value)
{
	redisReply *reply;
	bool ok = false;

	reply = redis_cmd(app, "HSET %s %s %.10g", hash, field, value);
	if (reply_ok_integer(reply))
		ok = true;
	freeReplyObject(reply);
	return ok;
}

bool redis_hset_str(struct app_state *app, const char *hash, const char *field,
		   const char *value)
{
	redisReply *reply;
	bool ok = false;

	reply = redis_cmd(app, "HSET %s %s %s", hash, field, value);
	if (reply_ok_integer(reply))
		ok = true;
	freeReplyObject(reply);
	return ok;
}

bool redis_update_instance_counters(struct app_state *app,
				   const char *instance_id)
{
	redisReply *reply;
	char keys_hash[512];
	char inst_hash[512];
	long long total_size = 0;
	long long key_count = 0;
	size_t i;

	snprintf(keys_hash, sizeof(keys_hash), "cp:instance:%s:keys", instance_id);
	snprintf(inst_hash, sizeof(inst_hash), "cp:instance:%s", instance_id);

	reply = redis_cmd(app, "HGETALL %s", keys_hash);
	if (!reply || reply->type != REDIS_REPLY_ARRAY) {
		freeReplyObject(reply);
		return false;
	}

	for (i = 0; i + 1 < reply->elements; i += 2) {
		redisReply *v = reply->element[i + 1];
		char *end;
		long long sz;

		if (v->type != REDIS_REPLY_STRING)
			continue;
		errno = 0;
		sz = strtoll(v->str, &end, 10);
		if (errno || end == v->str)
			sz = 0;
		total_size += sz;
		key_count++;
	}
	freeReplyObject(reply);

	return redis_hset_ll(app, inst_hash, "keyCount", key_count) &&
	       redis_hset_ll(app, inst_hash, "totalSize", total_size);
}

bool redis_remove_holder(struct app_state *app, const char *instance_id,
			const char *key)
{
	redisReply *reply;
	char inst_keys_hash[512];
	char key_holders_set[512];
	char key_size_hash[512];
	long long remaining = 0;

	snprintf(inst_keys_hash, sizeof(inst_keys_hash), "cp:instance:%s:keys",
		 instance_id);
	snprintf(key_holders_set, sizeof(key_holders_set), "cp:key:%s:instances",
		 key);
	snprintf(key_size_hash, sizeof(key_size_hash), "cp:key:%s:sizes", key);

	reply = redis_cmd(app, "HDEL %s %s", inst_keys_hash, key);
	freeReplyObject(reply);

	reply = redis_cmd(app, "SREM %s %s", key_holders_set, instance_id);
	freeReplyObject(reply);

	reply = redis_cmd(app, "HDEL %s %s", key_size_hash, instance_id);
	freeReplyObject(reply);

	reply = redis_cmd(app, "SCARD %s", key_holders_set);
	if (reply_ok_integer(reply))
		remaining = reply->integer;
	freeReplyObject(reply);

	if (remaining == 0) {
		reply = redis_cmd(app, "DEL %s %s", key_holders_set, key_size_hash);
		freeReplyObject(reply);
	}

	return redis_update_instance_counters(app, instance_id);
}

bool redis_upsert_holder(struct app_state *app, const char *instance_id,
			const char *key, long long size, bool *was_added)
{
	redisReply *reply;
	char inst_keys_hash[512];
	char key_holders_set[512];
	char key_size_hash[512];
	bool existed = false;

	snprintf(inst_keys_hash, sizeof(inst_keys_hash), "cp:instance:%s:keys",
		 instance_id);
	snprintf(key_holders_set, sizeof(key_holders_set), "cp:key:%s:instances",
		 key);
	snprintf(key_size_hash, sizeof(key_size_hash), "cp:key:%s:sizes", key);

	reply = redis_cmd(app, "HEXISTS %s %s", inst_keys_hash, key);
	if (reply_ok_integer(reply) && reply->integer == 1)
		existed = true;
	freeReplyObject(reply);

	reply = redis_cmd(app, "HSET %s %s %lld", inst_keys_hash, key, size);
	freeReplyObject(reply);
	reply = redis_cmd(app, "SADD %s %s", key_holders_set, instance_id);
	freeReplyObject(reply);
	reply = redis_cmd(app, "HSET %s %s %lld", key_size_hash, instance_id, size);
	freeReplyObject(reply);

	if (was_added)
		*was_added = !existed;
	return redis_update_instance_counters(app, instance_id);
}

int get_all_instance_ids(struct app_state *app, char ***ids_out,
			size_t *count_out)
{
	redisReply *reply;
	char **ids;
	size_t i;

	reply = redis_cmd(app, "SMEMBERS cp:instances");
	if (!reply || reply->type != REDIS_REPLY_ARRAY) {
		freeReplyObject(reply);
		return -1;
	}

	ids = calloc(reply->elements, sizeof(*ids));
	if (!ids && reply->elements > 0) {
		freeReplyObject(reply);
		return -1;
	}

	for (i = 0; i < reply->elements; i++) {
		redisReply *item = reply->element[i];

		if (item->type != REDIS_REPLY_STRING) {
			ids[i] = strdup("");
			continue;
		}
		ids[i] = strdup(item->str);
		if (!ids[i]) {
			size_t j;

			for (j = 0; j < i; j++)
				free(ids[j]);
			free(ids);
			freeReplyObject(reply);
			return -1;
		}
	}

	qsort(ids, reply->elements, sizeof(*ids), cmp_str_ptr);
	*ids_out = ids;
	*count_out = reply->elements;
	freeReplyObject(reply);
	return 0;
}

void free_ids(char **ids, size_t count)
{
	size_t i;

	if (!ids)
		return;
	for (i = 0; i < count; i++)
		free(ids[i]);
	free(ids);
}
