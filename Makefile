# mini-migrator build (portable-ish)

CXX      ?= c++
CXXFLAGS  = -std=c++17 -O2 -Wall -Wextra -Isrc -MMD -MP
LDFLAGS   =

# 可选模块（1=编译进来，0=不编译）
WITH_PG_CDC   ?= 1
WITH_REPAIR   ?= 1
WITH_METRICS  ?= 1
WITH_SNAPSHOT ?= 1
WITH_RESUME   ?= 1

SRCS      = $(wildcard src/*.cpp)
SRCS     += $(if $(filter 1,$(WITH_PG_CDC)),$(wildcard src/ext/*.cpp))
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

ifeq ($(WITH_PG_CDC),1)
CXXFLAGS += -DMIG_ENABLE_PG_CDC
endif
ifeq ($(WITH_REPAIR),1)
CXXFLAGS += -DMIG_ENABLE_REPAIR
endif
ifeq ($(WITH_METRICS),1)
CXXFLAGS += -DMIG_ENABLE_METRICS
endif
ifeq ($(WITH_SNAPSHOT),1)
CXXFLAGS += -DMIG_ENABLE_SNAPSHOT
endif
ifeq ($(WITH_RESUME),1)
CXXFLAGS += -DMIG_ENABLE_RESUME
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
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -rf build

-include $(DEPS)

run: $(BIN)
	./$(BIN) --mode sim --rows 200000 --tables 4 --port 8123
