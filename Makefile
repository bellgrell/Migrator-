# mini-database — 简易构建（当前环境无 cmake）

CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2 -Iinclude
BUILD    := build

LIB_SRCS := $(wildcard src/*.cpp) $(wildcard src/*/*.cpp) $(wildcard src/*/*/*.cpp)
LIB_OBJS := $(patsubst %.cpp,$(BUILD)/%.o,$(LIB_SRCS))
LIB      := $(BUILD)/libminidb.a

TEST_SRCS := $(wildcard tests/*.cpp)
TEST_BINS := $(patsubst tests/%.cpp,$(BUILD)/%,$(TEST_SRCS))

.PHONY: all test clean

all: $(LIB)

$(LIB): $(LIB_OBJS)
	@mkdir -p $(dir $@)
	ar rcs $@ $^

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/%: tests/%.cpp $(LIB)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $< -L$(BUILD) -lminidb -o $@

test: $(TEST_BINS)
	@for t in $(TEST_BINS); do echo "== running $$t"; $$t || exit 1; done

clean:
	rm -rf $(BUILD)
