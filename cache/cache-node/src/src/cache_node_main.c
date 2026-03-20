/* SPDX-License-Identifier: MIT */

#include "cache_node.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void cache_node_usage(const char *progname)
{
	fprintf(stderr,
		"Usage: %s --instance-id <id> [options]\n"
		"Options:\n"
		"  --instance-id <id>            Required identifier\n"
		"  --listen-host <addr>          Listen host (default 0.0.0.0)\n"
		"  --listen-port <port>          Listen port (default 6379)\n"
		"  --max-bytes <bytes>           Max total in-memory bytes\n"
		"  --max-items <count>           Max cached key count\n"
		"  --control-plane-events-url     Full CP endpoint URL\n"
		"                                default: disabled\n"
		"  --verbose                     Enable verbose server logs\n",
		progname);
}

static int cache_node_parse_u64_arg(const char *arg, unsigned long long *out)
{
	char *endptr;
	unsigned long long parsed;

	if (!arg || !out)
		return -1;
	parsed = strtoull(arg, &endptr, 10);
	if (errno != 0 || *endptr != '\0')
		return -1;
	*out = parsed;
	return 0;
}

int main(int argc, char **argv)
{
	struct cache_node_http_notifier *notifier = NULL;
	struct cache_node_store_config cfg = {
		.instance_id = NULL,
		.max_bytes = 0,
		.max_items = 0,
		.notify_fn = NULL,
		.notify_ctx = NULL,
	};
	struct cache_node_store *store = NULL;
	struct cache_node_server_config server_cfg = {
		.listen_host = "0.0.0.0",
		.listen_port = 6379,
		.control_plane_events_url = NULL,
		.verbose = false,
	};
	int i;
	const char *cp_url = NULL;
	int rc;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--instance-id")) {
			if (i + 1 >= argc) {
				cache_node_usage(argv[0]);
				return 2;
			}
			cfg.instance_id = argv[++i];
			continue;
		}
		if (!strcmp(argv[i], "--listen-host")) {
			if (i + 1 >= argc) {
				cache_node_usage(argv[0]);
				return 2;
			}
			server_cfg.listen_host = argv[++i];
			continue;
		}
		if (!strcmp(argv[i], "--listen-port")) {
			unsigned long long parsed;

			if (i + 1 >= argc) {
				cache_node_usage(argv[0]);
				return 2;
			}
			if (cache_node_parse_u64_arg(argv[++i], &parsed) != 0 ||
			    parsed > USHRT_MAX)
				return 2;
			server_cfg.listen_port = (unsigned short)parsed;
			continue;
		}
		if (!strcmp(argv[i], "--max-bytes")) {
			unsigned long long parsed;

			if (i + 1 >= argc) {
				cache_node_usage(argv[0]);
				return 2;
			}
			if (cache_node_parse_u64_arg(argv[++i], &parsed) != 0)
				return 2;
			cfg.max_bytes = (size_t)parsed;
			continue;
		}
		if (!strcmp(argv[i], "--max-items")) {
			unsigned long long parsed;

			if (i + 1 >= argc) {
				cache_node_usage(argv[0]);
				return 2;
			}
			if (cache_node_parse_u64_arg(argv[++i], &parsed) != 0)
				return 2;
			cfg.max_items = (size_t)parsed;
			continue;
		}
		if (!strcmp(argv[i], "--control-plane-events-url")) {
			if (i + 1 >= argc) {
				cache_node_usage(argv[0]);
				return 2;
			}
			cp_url = argv[++i];
			continue;
		}
		if (!strcmp(argv[i], "--verbose")) {
			server_cfg.verbose = true;
			continue;
		}
		cache_node_usage(argv[0]);
		return 2;
	}

	if (!cfg.instance_id) {
		cache_node_usage(argv[0]);
		return 2;
	}

	if (cp_url) {
		notifier = cache_node_http_notifier_create(cp_url);
		if (!notifier) {
			fprintf(stderr, "Invalid control plane URL: %s\n", cp_url);
			return 1;
		}
		server_cfg.control_plane_events_url = cp_url;
		cfg.notify_fn = cache_node_http_notifier;
		cfg.notify_ctx = notifier;
	}

	rc = cache_node_store_create(&cfg, &store);
	if (rc != 0) {
		fprintf(stderr, "Failed to create cache store: %d\n", rc);
		cache_node_http_notifier_destroy(notifier);
		return 1;
	}

	rc = cache_node_server_run(store, &server_cfg);
	cache_node_store_destroy(store);
	cache_node_http_notifier_destroy(notifier);
	return rc == 0 ? 0 : 1;
}
