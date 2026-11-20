# mini-migrator build (portable-ish)

VERSION   ?= 0.9.0

CXX      ?= c++
CXXFLAGS  = -std=c++17 -O2 -Wall -Wextra -Isrc -MMD -MP -DMIG_VERSION=\"$(VERSION)\"
LDFLAGS   =

# ASan/UBSan build: make asan / make ubsan
ifeq ($(SANITIZE),1)
CXXFLAGS += -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined
LDFLAGS  += -fsanitize=address,undefined
endif

# 可选模块（1=编译进来，0=不编译）
WITH_PG_CDC   ?= 1
WITH_REPAIR   ?= 1
WITH_METRICS  ?= 1
WITH_SNAPSHOT ?= 1
WITH_RESUME   ?= 1

SRCS      = $(wildcard src/*.cpp)
SRCS     += $(if $(filter 1,$(WITH_PG_CDC)),$(wildcard src/ext/*.cpp))
VERIFY_SRCS = $(wildcard verify/mmv/*.cpp)
VERIFY_OBJS = $(patsubst verify/mmv/%.cpp,build/verify/%.o,$(VERIFY_SRCS))
OBJS      = $(patsubst src/%.cpp,build/%.o,$(SRCS)) $(VERIFY_OBJS)
DEPS      = $(OBJS:.o=.d)
BIN       = build/migrator

MYSQL_PREFIX := $(shell brew --prefix mysql-client 2>/dev/null)
PG_PREFIX    := $(shell brew --prefix libpq 2>/dev/null)
OPENSSL_PREFIX := $(shell brew --prefix openssl@3 2>/dev/null)
ifeq ($(OPENSSL_PREFIX),)
OPENSSL_PREFIX := $(shell brew --prefix openssl 2>/dev/null)
endif

ifeq ($(wildcard $(MYSQL_PREFIX)/include/mysql/mysql.h),)
MYSQL_PREFIX :=
endif
ifeq ($(wildcard $(PG_PREFIX)/include/libpq-fe.h),)
PG_PREFIX :=
endif

# Linux distribution fallbacks (no Homebrew)
ifeq ($(MYSQL_PREFIX),)
  ifneq ($(wildcard /usr/include/mysql/mysql.h),)
    MYSQL_PREFIX := /usr
  endif
endif
ifeq ($(PG_PREFIX),)
  ifneq ($(wildcard /usr/include/postgresql/libpq-fe.h),)
    PG_INCLUDE_DIR := /usr/include/postgresql
    PG_LIB_DIR := /usr/lib/x86_64-linux-gnu
  endif
endif
ifeq ($(wildcard $(OPENSSL_PREFIX)/include/openssl/ssl.h),)
OPENSSL_PREFIX :=
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
else ifneq ($(PG_INCLUDE_DIR),)
CXXFLAGS += -DHAVE_PG -I$(PG_INCLUDE_DIR)
LDFLAGS  += -L$(PG_LIB_DIR) -lpq
endif

# mm-verify digest engine: ICU is optional (accent/collation degrade without it).
ICU_PREFIX := $(shell brew --prefix icu4c@79 2>/dev/null)
ifeq ($(ICU_PREFIX),)
ICU_PREFIX := $(shell brew --prefix icu4c 2>/dev/null)
endif
ifneq ($(wildcard $(ICU_PREFIX)/include/unicode/unorm2.h),)
CXXFLAGS += -DHAVE_ICU -I$(ICU_PREFIX)/include
LDFLAGS  += -L$(ICU_PREFIX)/lib -licui18n -licuuc -licudata
endif
CXXFLAGS += -Iverify/mmv

# ODBC is optional; SQL Server / Oracle backends need unixODBC + drivers.
ODBC_PREFIX ?= $(if $(wildcard /usr/include/sql.h),/usr,/opt/homebrew)
ifneq ($(wildcard $(ODBC_PREFIX)/include/sql.h),)
CXXFLAGS += -DHAVE_ODBC -I$(ODBC_PREFIX)/include
LDFLAGS  += -L$(ODBC_PREFIX)/lib -lodbc
endif

# Dashboard TLS is optional; without OpenSSL the server stays plaintext-only.
ifneq ($(OPENSSL_PREFIX),)
CXXFLAGS += -DMIG_HAVE_OPENSSL -I$(OPENSSL_PREFIX)/include
LDFLAGS  += -L$(OPENSSL_PREFIX)/lib -lssl -lcrypto
endif

PREFIX      ?= /usr/local
BINDIR      ?= $(PREFIX)/bin
SYSCONFDIR  ?= $(PREFIX)/etc/mini-migrator
SHAREDIR    ?= $(PREFIX)/share/mini-migrator
DESTDIR     ?=

.PHONY: all clean run lite full doctor check prove asan ubsan install uninstall dist

all: $(BIN)

$(BIN): $(OBJS)
	@mkdir -p build
	$(CXX) $(OBJS) -o $@ $(LDFLAGS) -lpthread

build/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/verify/%.o: verify/mmv/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# 环境自检工具（独立二进制）
doctor:
	@mkdir -p build
	$(CXX) $(CXXFLAGS) tools/doctor.cpp -o build/mm-doctor $(LDFLAGS) -lpthread

clean:
	rm -rf build

-include $(DEPS)

run: $(BIN)
	./$(BIN) --mode sim --rows 200000 --tables 4 --port 8123

# 只保留核心，不编译任何可选模块
lite: clean
	$(MAKE) WITH_PG_CDC=0 WITH_REPAIR=0 WITH_METRICS=0 WITH_SNAPSHOT=0 WITH_RESUME=0

# 全部模块
full: clean
	$(MAKE) WITH_PG_CDC=1 WITH_REPAIR=1 WITH_METRICS=1 WITH_SNAPSHOT=1 WITH_RESUME=1

# 快速自检：类型规则 + 模拟迁移冒烟
check: $(BIN)
	./$(BIN) --check-type-rules
	./$(BIN) --prove-type-rules
	./$(BIN) --mode sim --rows 2000 --tables 2 --exit --no-verify

prove: $(BIN)
	./$(BIN) --prove-type-rules

asan: clean
	$(MAKE) SANITIZE=1

ubsan: clean
	$(MAKE) SANITIZE=1

install: $(BIN)
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(BIN) $(DESTDIR)$(BINDIR)/migrator
	install -d $(DESTDIR)$(SYSCONFDIR) $(DESTDIR)$(SHAREDIR)/web
	install -m 0644 config/*.json config/migrator.ini $(DESTDIR)$(SYSCONFDIR)/
	install -m 0644 web/index.html $(DESTDIR)$(SHAREDIR)/web/index.html

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/migrator
	rm -rf $(DESTDIR)$(SYSCONFDIR) $(DESTDIR)$(SHAREDIR)

dist:
	@mkdir -p dist
	tar --exclude ./.git --exclude ./build --exclude ./out --exclude ./dist \
	    --exclude ./.DS_Store -czf dist/mini-migrator-$(VERSION).tar.gz .
	@echo "dist/mini-migrator-$(VERSION).tar.gz"
