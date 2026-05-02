CC ?= cc
CPPFLAGS += -Iinclude
CFLAGS ?= -std=c11 -D_POSIX_C_SOURCE=200809L
CFLAGS += -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wformat=2 -Werror
LDFLAGS ?=
LDLIBS ?=

BUILD_DIR ?= build
CORE_SOURCES := $(wildcard src/**/*.c src/*.c)
TEST_SOURCES := $(wildcard tests/**/*.c tests/*.c)
ALL_OBJECTS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(CORE_SOURCES) $(TEST_SOURCES))

.PHONY: all configure build test integration sanitize format format-check lint clean help

all: build

help:
	@printf '%s\n' \
	  'make configure   - verify project prerequisites' \
	  'make build       - build available C sources' \
	  'make test        - run deterministic unit tests' \
	  'make integration - run bounded live-socket tests' \
	  'make sanitize    - run sanitizer tests' \
	  'make format      - format C sources' \
	  'make lint        - run static-analysis checks'

configure:
	@./tools/bootstrap_ubuntu.sh --check

build: $(ALL_OBJECTS)
	@mkdir -p $(BUILD_DIR)
	@echo 'Build scaffold ready; no product sources have been added yet.'

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

-include $(ALL_OBJECTS:.o=.d)

test: build
	@./tools/run_tests.sh unit

integration: build
	@./tools/run_tests.sh integration

sanitize:
	@./tools/run_tests.sh sanitize

format:
	@command -v clang-format >/dev/null || { echo 'clang-format is required; run tools/bootstrap_ubuntu.sh'; exit 1; }
	@files=$$(find src tests -type f \( -name '*.c' -o -name '*.h' \) -print); if [ -n "$$files" ]; then clang-format -i $$files; fi

format-check:
	@command -v clang-format >/dev/null || { echo 'clang-format is required; run tools/bootstrap_ubuntu.sh'; exit 1; }
	@files=$$(find src tests -type f \( -name '*.c' -o -name '*.h' \) -print); if [ -n "$$files" ]; then clang-format --dry-run --Werror $$files; fi

lint: build
	@command -v cppcheck >/dev/null || { echo 'cppcheck is required; run tools/bootstrap_ubuntu.sh'; exit 1; }
	@files=$$(find src tests -type f \( -name '*.c' -o -name '*.h' \) -print); \
	if [ -n "$$files" ]; then cppcheck --enable=warning,style,performance,portability --error-exitcode=1 $$files; else echo 'No C sources exist yet; static-analysis scaffold check passed.'; fi

clean:
	rm -rf $(BUILD_DIR)
