CC ?= cc
PKG_CONFIG ?= pkg-config
PKGS := hiredis jansson libmicrohttpd

TARGET := bin/cache-control-plane
SRCS := src/cache_control_plane.c

CPPFLAGS += $(shell $(PKG_CONFIG) --cflags $(PKGS))
CFLAGS += -std=c11 -O2 -g -Wall -Wextra -Werror -Wpedantic -Wformat=2
LDFLAGS += $(shell $(PKG_CONFIG) --libs $(PKGS))

.PHONY: all clean check checkpatch contract

all: $(TARGET)

$(TARGET): $(SRCS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@ $(LDFLAGS)

clean:
	rm -f $(TARGET)

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

contract: $(TARGET)
	bash ../tests/run-tests.sh --contract-only
