# Cache Node (ValKey-derived Component)

This directory contains the cache node implementation for HAL. The node is a
small, self-contained in-memory cache service that follows the same operational
model as the control plane design: local cache instances cache keys and notify the
control plane when key state changes.

This implementation is written in C and structured to be maintainable:

- core key/value store with LRU-based eviction
- explicit key lifecycle events (`key_added`, `key_updated`, `key_evicted`)
- pluggable notifier callback for control-plane publishing
- built-in HTTP notifier that posts events to `/v1/events`
- single-threaded TCP command listener for operational testing

## Build

```bash
cd cache/cache-node/src
make all
```

This generates `bin/cache-node`.

Run build + tests + quality check:

```bash
bash cache/cache-node/src/scripts/build.sh
```

## Run

```bash
cd cache/cache-node/src
./bin/cache-node --instance-id node-1 --listen-host 127.0.0.1 --listen-port 6379
```

Enable control-plane reporting:

```bash
./bin/cache-node \
  --instance-id leaf-1 \
  --listen-port 6380 \
  --control-plane-events-url "http://127.0.0.1:8080/v1/events"
```

### Supported Commands (line-based)

Commands are sent as plain text over TCP.

- `PING`
- `SET <key> <value> [source-instance-id] [retrieval-ms]`
- `GET <key>`
- `DEL <key> [source-instance-id] [retrieval-ms]`
- `INFO`
- `QUIT`

Examples:

```bash
printf 'PING\r\n' | nc 127.0.0.1 6379
printf 'SET model:v1 weights\r\n' | nc 127.0.0.1 6379
printf 'GET model:v1\r\n' | nc 127.0.0.1 6379
printf 'DEL model:v1\r\n' | nc 127.0.0.1 6379
```

## Dependencies

### Build dependencies

- C compiler (`cc`)
- `make`

### Runtime dependencies

- POSIX-compliant OS sockets (used by TCP server and notifier)
- A reachable HTTP control-plane endpoint if `--control-plane-events-url` is used

No additional third-party runtime libraries are required.

## Notes

- Key length is constrained to `CACHE_NODE_MAX_KEY_LEN - 1` bytes.
- Values are stored in memory and copied on write.
- Eviction is LRU-based and emits `key_evicted` events.
- Notifications are sent synchronously inside mutating operations.
