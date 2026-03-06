#ifndef CACHE_CONTROL_PLANE_H
#define CACHE_CONTROL_PLANE_H

#include <errno.h>
#include <limits.h>
#include <microhttpd.h>
#include <hiredis/hiredis.h>
#include <jansson.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <signal.h>
#include <time.h>

#define APP_VERSION "1.0.0"
#define DEFAULT_HTTP_PORT 8080
#define DEFAULT_REDIS_PORT 6379
#define DEFAULT_REDIS_DB 0
#define DEFAULT_REDIS_HOST "127.0.0.1"
#define DEFAULT_HEARTBEAT_MS 30000
#define MAX_PATH_PARTS 8

struct app_state {
	redisContext *redis;
	int http_port;
	char redis_host[256];
	int redis_port;
	int redis_db;
	bool verbose;
	volatile sig_atomic_t running;
};

struct request_ctx {
	char *body;
	size_t body_len;
	bool responded;
};

struct path_parts {
	char *raw;
	char *parts[MAX_PATH_PARTS];
	size_t count;
};

struct key_input {
	const char *key;
	long long size;
};

extern struct app_state *g_app;

long long now_unix_ms(void);
void now_iso8601(char *buf, size_t size);
bool env_is_truthy(const char *value);
void cp_log(const struct app_state *app, const char *fmt, ...);
int str_to_int(const char *value, int dflt);
void sig_handler(int sig);

bool append_request_body(struct request_ctx *ctx, const char *chunk, size_t chunk_len);
json_t *parse_json_body(const struct app_state *app, struct request_ctx *ctx);
bool parse_path(const char *url, struct path_parts *path);
void free_path(struct path_parts *path);
int index_of_id(char **ids, size_t count, const char *id);
int cmp_str_ptr(const void *a, const void *b);

json_t *json_error(const char *code, const char *message);
int send_json_response(struct MHD_Connection *connection, unsigned int status,
			      json_t *payload);
int send_empty_response(struct MHD_Connection *connection, unsigned int status);
int send_error_response(struct MHD_Connection *connection, unsigned int status,
		       const char *code, const char *message);

redisReply *redis_cmd(struct app_state *app, const char *fmt, ...);
bool reply_ok_integer(redisReply *reply);
bool redis_instance_exists(struct app_state *app, const char *id);
char *redis_hget_strdup(struct app_state *app, const char *hash,
		       const char *field);
long long redis_hget_ll(struct app_state *app, const char *hash,
		       const char *field, long long dflt);
bool redis_hset_ll(struct app_state *app, const char *hash, const char *field,
		  long long value);
bool redis_hset_double(struct app_state *app, const char *hash, const char *field,
		      double value);
bool redis_hset_str(struct app_state *app, const char *hash, const char *field,
		   const char *value);
bool redis_update_instance_counters(struct app_state *app,
				  const char *instance_id);
bool redis_remove_holder(struct app_state *app, const char *instance_id,
			const char *key);
bool redis_upsert_holder(struct app_state *app, const char *instance_id,
			const char *key, long long size, bool *was_added);
int get_all_instance_ids(struct app_state *app, char ***ids_out, size_t *count_out);
void free_ids(char **ids, size_t count);

int shortest_path_from_parents(const char *const *ids, size_t count,
			      const char *const *parents, const char *from,
			      const char *to, char ***hops_out,
			      size_t *hop_count_out);
int shortest_path(struct app_state *app, const char *from, const char *to,
		 char ***hops_out, size_t *hop_count_out);
long long shortest_distance(struct app_state *app, const char *from,
			   const char *to);
json_t *make_proximity_metrics(struct app_state *app, const char *from,
			       const char *to);
int compare_locate_items(const void *a, const void *b);

bool is_valid_tier(const char *tier);
int handle_list_instances(struct app_state *app,
			 struct MHD_Connection *connection);
int handle_get_instance(struct app_state *app,
		       struct MHD_Connection *connection, const char *id);
int handle_register_instance(struct app_state *app,
			    struct MHD_Connection *connection,
			    const char *id, struct request_ctx *ctx);
int handle_heartbeat(struct app_state *app,
		     struct MHD_Connection *connection, const char *id,
		     struct request_ctx *ctx);
int handle_deregister(struct app_state *app,
		      struct MHD_Connection *connection, const char *id);

int validate_key_info(json_t *entry, const char **key_out, long long *size_out);
int handle_locate_key(struct app_state *app, struct MHD_Connection *connection,
		     const char *key);
int handle_nearest_key(struct app_state *app, struct MHD_Connection *connection,
		      const char *key);
int handle_list_instance_keys(struct app_state *app,
			     struct MHD_Connection *connection,
			     const char *id);
int handle_update_instance_keys(struct app_state *app,
			       struct MHD_Connection *connection,
			       const char *id, struct request_ctx *ctx);

int parse_instance_filter(struct app_state *app, const char *value, char ***ids_out,
			 size_t *count_out);
int handle_topology(struct app_state *app, struct MHD_Connection *connection);
int handle_proximity_matrix(struct app_state *app,
			    struct MHD_Connection *connection);
int handle_route(struct app_state *app, struct MHD_Connection *connection,
		 const char *from, const char *to);

bool validate_event_payload(json_t *event, bool batched, const char **instance_id,
			   const char **event_type, const char **key,
			   long long *size, bool *needs_size);
int handle_single_event(struct app_state *app, struct MHD_Connection *connection,
		       struct request_ctx *ctx);
int handle_batch_event(struct app_state *app, struct MHD_Connection *connection,
		      struct request_ctx *ctx);

int dispatch_request(struct app_state *app, struct MHD_Connection *connection,
		    const char *method, const struct path_parts *path,
		    struct request_ctx *ctx);
void request_completed(void *cls, struct MHD_Connection *connection,
		      void **con_cls,
		      enum MHD_RequestTerminationCode toe);
enum MHD_Result access_handler(void *cls, struct MHD_Connection *connection,
			      const char *url, const char *method,
			      const char *version, const char *upload_data,
			      size_t *upload_data_size, void **con_cls);

void usage(const char *prog);
int parse_args(struct app_state *app, int argc, char **argv);
int connect_redis(struct app_state *app);

#endif /* CACHE_CONTROL_PLANE_H */
