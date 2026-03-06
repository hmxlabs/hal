#include "cache_control_plane.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

struct app_state *g_app;

void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [--port <port>] [--redis-host <host>] "
		"[--redis-port <port>] [--redis-db <db>] [--verbose]\n",
		prog);
}

int parse_args(struct app_state *app, int argc, char **argv)
{
	int i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--port") && i + 1 < argc) {
			app->http_port = str_to_int(argv[++i], app->http_port);
			continue;
		}
		if (!strcmp(argv[i], "--redis-host") && i + 1 < argc) {
			strncpy(app->redis_host, argv[++i],
				sizeof(app->redis_host) - 1);
			app->redis_host[sizeof(app->redis_host) - 1] = '\0';
			continue;
		}
		if (!strcmp(argv[i], "--redis-port") && i + 1 < argc) {
			app->redis_port = str_to_int(argv[++i], app->redis_port);
			continue;
		}
		if (!strcmp(argv[i], "--redis-db") && i + 1 < argc) {
			app->redis_db = str_to_int(argv[++i], app->redis_db);
			continue;
		}
		if (!strcmp(argv[i], "--verbose")) {
			app->verbose = true;
			continue;
		}
		usage(argv[0]);
		return -1;
	}
	return 0;
}

int connect_redis(struct app_state *app)
{
	redisReply *reply;

	app->redis = redisConnect(app->redis_host, app->redis_port);
	if (!app->redis || app->redis->err) {
		if (app->redis)
			fprintf(stderr, "redis connection error: %s\n",
				app->redis->errstr);
		else
			fprintf(stderr, "redis connection error: allocation failed\n");
		return -1;
	}

	reply = redis_cmd(app, "SELECT %d", app->redis_db);
	if (!reply || reply->type == REDIS_REPLY_ERROR) {
		fprintf(stderr, "redis SELECT failed\n");
		freeReplyObject(reply);
		return -1;
	}
	freeReplyObject(reply);
	return 0;
}

int main(int argc, char **argv)
{
	struct MHD_Daemon *daemon;
	struct app_state app;
	const char *env;

	memset(&app, 0, sizeof(app));
	app.http_port = str_to_int(getenv("HAL_CACHE_CONTROL_PLANE_PORT"),
				   DEFAULT_HTTP_PORT);
	strncpy(app.redis_host,
		getenv("HAL_CACHE_REDIS_HOST") ? getenv("HAL_CACHE_REDIS_HOST") :
		DEFAULT_REDIS_HOST,
		sizeof(app.redis_host) - 1);
	app.redis_host[sizeof(app.redis_host) - 1] = '\0';
	app.redis_port = str_to_int(getenv("HAL_CACHE_REDIS_PORT"),
				    DEFAULT_REDIS_PORT);
	app.redis_db = str_to_int(getenv("HAL_CACHE_REDIS_DB"), DEFAULT_REDIS_DB);
	app.verbose = env_is_truthy(getenv("HAL_CACHE_CONTROL_PLANE_VERBOSE"));
	env = getenv("HAL_CACHE_REDIS_URL");
	if (env)
		fprintf(stderr,
			"warning: HAL_CACHE_REDIS_URL is not parsed directly; use "
			"HAL_CACHE_REDIS_HOST/HAL_CACHE_REDIS_PORT/HAL_CACHE_REDIS_DB\n");

	if (parse_args(&app, argc, argv) != 0)
		return 2;

	if (connect_redis(&app) != 0)
		return 1;

	srand((unsigned)time(NULL));
	app.running = 1;
	g_app = &app;
	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);

	daemon = MHD_start_daemon(MHD_USE_INTERNAL_POLLING_THREAD,
				 app.http_port,
				 NULL,
				 NULL,
				 &access_handler,
				 &app,
				 MHD_OPTION_NOTIFY_COMPLETED,
				 &request_completed,
				 NULL,
				 MHD_OPTION_END);
	if (!daemon) {
		fprintf(stderr, "failed to start HTTP daemon on port %d\n",
			app.http_port);
		redisFree(app.redis);
		return 1;
	}

	fprintf(stdout,
		"HAL cache control plane listening on :%d (redis %s:%d db %d)\n",
		app.http_port, app.redis_host, app.redis_port, app.redis_db);
	fflush(stdout);

	while (app.running)
		sleep(1);

	MHD_stop_daemon(daemon);
	redisFree(app.redis);
	return 0;
}
