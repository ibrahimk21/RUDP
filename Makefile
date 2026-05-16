CC ?= cc
AR ?= ar
CPPFLAGS += -Iinclude
CFLAGS ?= -std=c11 -D_POSIX_C_SOURCE=200809L
CFLAGS += -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wformat=2 -Werror
LDFLAGS ?=
LDLIBS ?=

BUILD_DIR ?= build
LIBRARY := $(BUILD_DIR)/librudp.a
CORE_SOURCES := $(wildcard src/rudp/*.c src/common/*.c)
CORE_OBJECTS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(CORE_SOURCES))
CLI := $(BUILD_DIR)/rudp
TCP_REF := $(BUILD_DIR)/tcp_ref
UDP_REF := $(BUILD_DIR)/udp_ref
TEST_SOURCES := $(wildcard tests/**/*.c tests/*.c)
TEST_OBJECTS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(TEST_SOURCES))
TEST_SUPPORT_SOURCES := $(wildcard tests/support/*.c)
TEST_SUPPORT_OBJECTS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(TEST_SUPPORT_SOURCES))
UNIT_TEST_SOURCES := $(wildcard tests/unit/test_*.c)
UNIT_TEST_BINS := $(patsubst %.c,$(BUILD_DIR)/%,$(UNIT_TEST_SOURCES))
INTEGRATION_TEST_SOURCES := $(wildcard tests/integration/test_*.c)
INTEGRATION_TEST_BINS := $(patsubst %.c,$(BUILD_DIR)/%,$(INTEGRATION_TEST_SOURCES))
ALL_OBJECTS := $(CORE_OBJECTS) $(TEST_OBJECTS) $(TEST_SUPPORT_OBJECTS)

.PHONY: all configure lib cli tcp_ref udp_ref build test integration sanitize format format-check lint clean help

all: lib cli tcp_ref udp_ref

help:
	@printf '%s\n' \
	  'make configure   - verify project prerequisites' \
	  'make all         - build the library and command-line stubs' \
	  'make lib         - build the RUDP static library' \
	  'make cli         - build the RUDP command-line stub' \
	  'make tcp_ref     - build the TCP reference stub' \
	  'make udp_ref     - build the UDP reference stub' \
	  'make test        - run deterministic unit tests' \
	  'make integration - run bounded live-socket tests' \
	  'make sanitize    - run sanitizer tests' \
	  'make format      - format C sources' \
	  'make lint        - run static-analysis checks'

configure:
	@./tools/bootstrap_ubuntu.sh --check

build: all

lib: $(LIBRARY)

cli: $(CLI)

tcp_ref: $(TCP_REF)

udp_ref: $(UDP_REF)

$(LIBRARY): $(CORE_OBJECTS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(CLI): src/cli/rudp.c $(LIBRARY)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LIBRARY) $(LDFLAGS) $(LDLIBS) -o $@

$(TCP_REF): src/tcp_ref/tcp_ref.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LDFLAGS) $(LDLIBS) -o $@

$(UDP_REF): src/udp_ref/udp_ref.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD_DIR)/tests/unit/test_%: tests/unit/test_%.c $(LIBRARY) $(TEST_SUPPORT_OBJECTS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Itests/support $< $(TEST_SUPPORT_OBJECTS) $(LIBRARY) $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD_DIR)/tests/integration/test_%: tests/integration/test_%.c $(LIBRARY)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LIBRARY) $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

-include $(ALL_OBJECTS:.o=.d)

test: all $(UNIT_TEST_BINS)
	@BUILD_DIR=$(BUILD_DIR) ./tools/run_tests.sh unit

integration: all $(INTEGRATION_TEST_BINS)
	@BUILD_DIR=$(BUILD_DIR) ./tools/run_tests.sh integration

sanitize:
	@$(MAKE) BUILD_DIR=$(BUILD_DIR)/sanitize \
		CFLAGS='$(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer' \
		LDFLAGS='$(LDFLAGS) -fsanitize=address,undefined' test

format:
	@command -v clang-format >/dev/null || { echo 'clang-format is required; run tools/bootstrap_ubuntu.sh'; exit 1; }
	@files=$$(find include src tests -type f \( -name '*.c' -o -name '*.h' \) -print 2>/dev/null); if [ -n "$$files" ]; then clang-format -i $$files; fi

format-check:
	@command -v clang-format >/dev/null || { echo 'clang-format is required; run tools/bootstrap_ubuntu.sh'; exit 1; }
	@files=$$(find include src tests -type f \( -name '*.c' -o -name '*.h' \) -print 2>/dev/null); if [ -n "$$files" ]; then clang-format --dry-run --Werror $$files; fi

lint: all
	@command -v cppcheck >/dev/null || { echo 'cppcheck is required; run tools/bootstrap_ubuntu.sh'; exit 1; }
	@files=$$(find src tests -type f \( -name '*.c' -o -name '*.h' \) -print 2>/dev/null); \
	if [ -n "$$files" ]; then cppcheck --enable=warning,style,performance,portability --error-exitcode=1 $$files; else echo 'No C sources exist yet; static-analysis scaffold check passed.'; fi

clean:
	rm -rf $(BUILD_DIR)
