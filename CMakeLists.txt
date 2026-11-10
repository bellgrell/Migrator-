cmake_minimum_required(VERSION 3.16)
project(mini_migrator LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

option(MIG_ENABLE_PG_CDC   "PostgreSQL CDC (wal2json)" ON)
option(MIG_ENABLE_REPAIR   "Primary-key diff repair" ON)
option(MIG_ENABLE_METRICS  "Prometheus /metrics" ON)
option(MIG_ENABLE_SNAPSHOT "Consistent snapshot" ON)
option(MIG_ENABLE_RESUME   "Primary-key resume" ON)

add_library(migrator_core STATIC
  src/core.cpp
  src/checksum_normalizer.cpp
  src/config_validator.cpp
  src/ddl_dialect.cpp
  src/job_scheduler.cpp
  src/metrics.cpp
  src/pipeline.cpp
  src/backend_sim.cpp
  src/backends.cpp
  src/http.cpp
)
target_include_directories(migrator_core PUBLIC src)

find_package(Threads REQUIRED)
target_link_libraries(migrator_core PUBLIC Threads::Threads)

if(MIG_ENABLE_PG_CDC)
  target_sources(migrator_core PRIVATE src/ext/pg_cdc.cpp)
  target_compile_definitions(migrator_core PRIVATE MIG_ENABLE_PG_CDC)
endif()
if(MIG_ENABLE_REPAIR)
  target_compile_definitions(migrator_core PRIVATE MIG_ENABLE_REPAIR)
endif()
if(MIG_ENABLE_METRICS)
  target_compile_definitions(migrator_core PRIVATE MIG_ENABLE_METRICS)
endif()
if(MIG_ENABLE_SNAPSHOT)
  target_compile_definitions(migrator_core PRIVATE MIG_ENABLE_SNAPSHOT)
endif()
if(MIG_ENABLE_RESUME)
  target_compile_definitions(migrator_core PRIVATE MIG_ENABLE_RESUME)
endif()

# ---- MySQL (optional) ----
find_path(MYSQL_INCLUDE_DIR NAMES mysql/mysql.h
  HINTS /opt/homebrew/opt/mysql-client/include /usr/include/mysql
        $ENV{MYSQL_INCLUDE_DIR} C:/Program\ Files/MySQL/*/include)
find_library(MYSQL_LIBRARY NAMES mysqlclient libmysql
  HINTS /opt/homebrew/opt/mysql-client/lib /usr/lib/x86_64-linux-gnu
        $ENV{MYSQL_LIB_DIR} C:/Program\ Files/MySQL/*/lib)
if(MYSQL_INCLUDE_DIR AND MYSQL_LIBRARY)
  target_sources(migrator_core PRIVATE src/backend_mysql.cpp)
  target_include_directories(migrator_core PRIVATE ${MYSQL_INCLUDE_DIR})
  target_compile_definitions(migrator_core PRIVATE HAVE_MYSQL)
  target_link_libraries(migrator_core PUBLIC ${MYSQL_LIBRARY})
  message(STATUS "MySQL support: ON")
else()
  message(STATUS "MySQL support: OFF")
endif()

# ---- PostgreSQL / openGauss (optional) ----
find_path(PG_INCLUDE_DIR NAMES libpq-fe.h
  HINTS /opt/homebrew/opt/libpq/include /usr/include/postgresql
        $ENV{PG_INCLUDE_DIR} C:/Program\ Files/PostgreSQL/*/include)
find_library(PG_LIBRARY NAMES pq
  HINTS /opt/homebrew/opt/libpq/lib /usr/lib/x86_64-linux-gnu
        $ENV{PG_LIB_DIR} C:/Program\ Files/PostgreSQL/*/lib)
if(PG_INCLUDE_DIR AND PG_LIBRARY)
  target_sources(migrator_core PRIVATE src/backend_pg.cpp)
  target_include_directories(migrator_core PRIVATE ${PG_INCLUDE_DIR})
  target_compile_definitions(migrator_core PRIVATE HAVE_PG)
  target_link_libraries(migrator_core PUBLIC ${PG_LIBRARY})
  message(STATUS "PostgreSQL support: ON")
else()
  message(STATUS "PostgreSQL support: OFF")
endif()

if(WIN32)
  target_link_libraries(migrator_core PUBLIC ws2_32)
endif()

add_executable(migrator src/main.cpp)
target_link_libraries(migrator PRIVATE migrator_core)

option(BUILD_QT_GUI "Build the Qt desktop front-end" OFF)
if(BUILD_QT_GUI)
  find_package(Qt6 COMPONENTS Widgets REQUIRED)
  add_executable(migrator_gui qt/main.cpp qt/mainwindow.cpp qt/mainwindow.h)
  target_link_libraries(migrator_gui PRIVATE migrator_core Qt6::Widgets)
endif()
