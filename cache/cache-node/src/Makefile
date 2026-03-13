CC ?= cc
PKG_CONFIG ?= pkg-config

TARGET := bin/cache-node
TEST_TARGET := bin/cache-node-unit-tests

SRCS := \
	src/cache_node_main.c \
	src/cache_node_store.c \
	src/cache_node_events.c \
	src/cache_node_server.c \
	src/cache_node_notifier.c

TEST_SRCS := \
	tests/unit_cache_node_tests.c \
	src/cache_node_store.c \
	src/cache_node_events.c \
	src/cache_node_server.c \
	src/cache_node_notifier.c

CPPFLAGS += -I"$(CURDIR)/src"
CFLAGS += -std=c11 -O2 -g -Wall -Wextra -Werror -Wpedantic -Wformat=2
LDFLAGS +=

OBJECTS := $(SRCS:.c=.o)
TEST_OBJECTS := $(TEST_SRCS:.c=.o)

.PHONY: all clean test check checkpatch

all: $(TARGET)

$(TARGET): $(OBJECTS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@ $(LDFLAGS)

test: $(TEST_TARGET)
	$(TEST_TARGET)

$(TEST_TARGET): $(TEST_OBJECTS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@ $(LDFLAGS)

check:
	@echo "Running cache-node compiler quality gate"
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsyntax-only $(SRCS) $(TEST_SRCS)

clean:
	rm -f $(TARGET) $(TEST_TARGET) $(OBJECTS) $(TEST_OBJECTS)

checkpatch:
	@echo "Running Linux kernel style checkpatch (if available)"
	@if command -v checkpatch.pl >/dev/null 2>&1; then \
		checkpatch.pl --no-tree --strict --terse $(SRCS) $(TEST_SRCS); \
	elif [ -f /usr/src/linux/scripts/checkpatch.pl ]; then \
		perl /usr/src/linux/scripts/checkpatch.pl --no-tree --strict --terse \
			$(SRCS) $(TEST_SRCS); \
	else \
		echo "checkpatch.pl not found; use build-quality checks instead"; \
	fi
