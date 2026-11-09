# mini-migrator build (portable-ish)

CXX      ?= c++
CXXFLAGS  = -std=c++17 -O2 -Wall -Wextra -Isrc -MMD -MP
LDFLAGS   =
SRCS      = $(wildcard src/*.cpp)
OBJS      = $(patsubst src/%.cpp,build/%.o,$(SRCS))
DEPS      = $(OBJS:.o=.d)
BIN       = build/migrator

MYSQL_PREFIX := $(shell brew --prefix mysql-client 2>/dev/null)
PG_PREFIX    := $(shell brew --prefix libpq 2>/dev/null)

ifeq ($(wildcard $(MYSQL_PREFIX)/include/mysql/mysql.h),)
MYSQL_PREFIX :=
endif
ifeq ($(wildcard $(PG_PREFIX)/include/libpq-fe.h),)
PG_PREFIX :=
endif

ifneq ($(MYSQL_PREFIX),)
CXXFLAGS += -DHAVE_MYSQL -I$(MYSQL_PREFIX)/include
LDFLAGS  += -L$(MYSQL_PREFIX)/lib -lmysqlclient
endif

ifneq ($(PG_PREFIX),)
CXXFLAGS += -DHAVE_PG -I$(PG_PREFIX)/include
LDFLAGS  += -L$(PG_PREFIX)/lib -lpq
endif

.PHONY: all clean run

all: $(BIN)

$(BIN): $(OBJS)
	@mkdir -p build
	$(CXX) $(OBJS) -o $@ $(LDFLAGS) -lpthread

build/%.o: src/%.cpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -rf build

-include $(DEPS)

run: $(BIN)
	./$(BIN) --mode sim --rows 200000 --tables 4 --port 8123
