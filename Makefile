# Build modes live in separate directories so that sanitized objects can never
# reach a timed binary. Run `make help` for the targets.
CXX := clang++
MODE ?= release
BUILD := build/$(MODE)

CXXFLAGS_BASE := -std=c++20 -Iinclude -Itests -Wall -Wextra -Wpedantic -Wconversion -Wshadow -MMD -MP
CXXFLAGS_release := -O3 -DNDEBUG -mcpu=native
CXXFLAGS_debug := -O0 -g
CXXFLAGS_asan := -O1 -g -fsanitize=address -fno-omit-frame-pointer
CXXFLAGS_ubsan := -O1 -g -fsanitize=undefined -fno-sanitize-recover=undefined
CXXFLAGS := $(CXXFLAGS_BASE) $(CXXFLAGS_$(MODE))

TEST_SOURCES := $(wildcard tests/test_*.cpp)
TEST_BINS := $(patsubst tests/%.cpp,$(BUILD)/%,$(TEST_SOURCES))
TOOL_BINS := $(patsubst tools/%.cpp,$(BUILD)/%,$(wildcard tools/*.cpp))

.PHONY: all test tools sanitize smoke clean help

all: tools $(TEST_BINS)

tools: $(TOOL_BINS)

test: $(TEST_BINS) $(TOOL_BINS)
	@for t in $(TEST_BINS); do echo "== $$t"; $$t || exit 1; done

sanitize:
	$(MAKE) MODE=asan test
	$(MAKE) MODE=ubsan test

smoke:
	./scripts/smoke.sh

clean:
	rm -rf build

help:
	@echo "make test      build and run the correctness suite"
	@echo "make sanitize  run the suite under ASan, then UBSan"
	@echo "make smoke     one-command end-to-end check"
	@echo "make clean     remove build output"

$(BUILD)/test_%: tests/test_%.cpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $< -o $@

$(BUILD)/%: tools/%.cpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $< -o $@

-include $(wildcard $(BUILD)/*.d)
