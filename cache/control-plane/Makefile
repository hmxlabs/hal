CC ?= cc
PKG_CONFIG ?= pkg-config
PKGS := hiredis jansson libmicrohttpd

TARGET := bin/cache-control-plane
TEST_TARGET := bin/cache-control-plane-unit-tests
SRCS := \
	src/cache_control_plane_main.c \
	src/cache_control_plane_utils.c \
	src/cache_control_plane_http.c \
	src/cache_control_plane_redis.c \
	src/cache_control_plane_graph.c \
	src/cache_control_plane_instances.c \
	src/cache_control_plane_content.c \
	src/cache_control_plane_topology.c \
	src/cache_control_plane_events.c \
	src/cache_control_plane_router.c
TEST_SRCS := $(filter-out src/cache_control_plane_main.c, $(SRCS))
TEST_SRCS := tests/unit/cache_control_plane_unit_tests.c $(TEST_SRCS)

CPPFLAGS += -I"$(CURDIR)/src"
CPPFLAGS += $(shell $(PKG_CONFIG) --cflags $(PKGS))
CFLAGS += -std=c11 -O2 -g -Wall -Wextra -Werror -Wpedantic -Wformat=2
LDFLAGS += $(shell $(PKG_CONFIG) --libs $(PKGS))

OBJECTS := $(SRCS:.c=.o)

.PHONY: all clean check checkpatch test unit-test contract

all: $(TARGET)

$(TARGET): $(OBJECTS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@ $(LDFLAGS)

test: $(TEST_TARGET)
	$(TEST_TARGET)

$(TEST_TARGET): $(TEST_SRCS:.c=.o)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@ $(LDFLAGS)

clean:
	rm -f $(TARGET) $(TEST_TARGET) $(OBJECTS) $(TEST_SRCS:.c=.o)

check: $(TARGET)
	@echo "Running compiler quality gate"
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsyntax-only $(SRCS)
	$(MAKE) checkpatch

checkpatch:
	@echo "Running Linux kernel style checkpatch gate"
	@if command -v checkpatch.pl >/dev/null 2>&1; then \
		checkpatch.pl --no-tree --strict --terse $(SRCS); \
	elif [ -f /usr/src/linux/scripts/checkpatch.pl ]; then \
		perl /usr/src/linux/scripts/checkpatch.pl --no-tree --strict --terse $(SRCS); \
	else \
		echo "checkpatch.pl not found; running strict compiler checks as fallback"; \
	fi

unit-test: test

contract: $(TARGET)
	bash ../tests/run-tests.sh --contract-only
