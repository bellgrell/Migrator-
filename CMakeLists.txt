cmake_minimum_required(VERSION 3.16)
project(mini_migrator VERSION 0.9.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

option(MIG_SANITIZE "Build with ASan+UBSan" OFF)
if(MIG_SANITIZE)
  add_compile_options(-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined)
  add_link_options(-fsanitize=address,undefined)
endif()

add_compile_definitions(MIG_VERSION="${PROJECT_VERSION}")

option(MIG_ENABLE_PG_CDC   "PostgreSQL CDC (wal2json)" ON)
option(MIG_ENABLE_REPAIR   "Primary-key diff repair" ON)
option(MIG_ENABLE_METRICS  "Prometheus /metrics" ON)
option(MIG_ENABLE_SNAPSHOT "Consistent snapshot" ON)
option(MIG_ENABLE_RESUME   "Primary-key resume" ON)

file(GLOB VERIFY_SOURCES CONFIGURE_DEPENDS verify/mmv/*.cpp)

add_library(migrator_core STATIC
  ${VERIFY_SOURCES}
  src/core.cpp
  src/checksum.cpp
  src/checksum_normalizer.cpp
  src/policy.cpp
  src/views.cpp
  src/cdc.cpp
  src/odbc_cdc.cpp
  src/config_validator.cpp
  src/ddl_dialect.cpp
  src/job_scheduler.cpp
  src/local_file_loader.cpp
  src/logger.cpp
  src/metrics.cpp
  src/mmv_verify.cpp
  src/module.cpp
  src/pg_dialect.cpp
  src/pipeline.cpp
  src/type_system.cpp
  src/backend_sim.cpp
  src/backends.cpp
  src/http.cpp
)
target_include_directories(migrator_core PUBLIC src)
target_include_directories(migrator_core PRIVATE verify/mmv)

find_package(Threads REQUIRED)
target_link_libraries(migrator_core PUBLIC Threads::Threads)

# ---- ODBC (optional; SQL Server / Oracle) ----
find_path(ODBC_INCLUDE_DIR NAMES sql.h HINTS /opt/homebrew/include /usr/include)
find_library(ODBC_LIBRARY NAMES odbc HINTS /opt/homebrew/lib /usr/lib)
if(ODBC_INCLUDE_DIR AND ODBC_LIBRARY)
  target_sources(migrator_core PRIVATE src/backend_odbc.cpp)
  target_compile_definitions(migrator_core PRIVATE HAVE_ODBC)
  target_include_directories(migrator_core PRIVATE ${ODBC_INCLUDE_DIR})
  target_link_libraries(migrator_core PUBLIC ${ODBC_LIBRARY})
  message(STATUS "ODBC support: ON")
else()
  message(STATUS "ODBC support: OFF (SQL Server/Oracle disabled)")
endif()

# ---- OpenSSL (optional; dashboard TLS) ----
find_package(OpenSSL QUIET)
if(OpenSSL_FOUND)
  target_compile_definitions(migrator_core PRIVATE MIG_HAVE_OPENSSL)
  target_link_libraries(migrator_core PUBLIC OpenSSL::SSL OpenSSL::Crypto)
  message(STATUS "OpenSSL support: ON")
else()
  message(STATUS "OpenSSL support: OFF (dashboard TLS disabled)")
endif()

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
  target_sources(migrator_core PRIVATE src/backend_mysql.cpp src/backend_mysql_target.cpp)
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
  target_sources(migrator_core PRIVATE src/backend_pg.cpp src/backend_pg_source.cpp)
  target_include_directories(migrator_core PRIVATE ${PG_INCLUDE_DIR})
  target_compile_definitions(migrator_core PRIVATE HAVE_PG)
  target_link_libraries(migrator_core PUBLIC ${PG_LIBRARY})
  message(STATUS "PostgreSQL support: ON")
else()
  message(STATUS "PostgreSQL support: OFF")
endif()

# ---- ICU (optional; accent/collation degrade without it) ----
find_package(ICU COMPONENTS uc i18n data QUIET)
if(ICU_FOUND)
  target_compile_definitions(migrator_core PRIVATE HAVE_ICU)
  target_include_directories(migrator_core PRIVATE ${ICU_INCLUDE_DIRS})
  target_link_libraries(migrator_core PUBLIC ${ICU_LIBRARIES})
  message(STATUS "ICU support: ON")
else()
  message(STATUS "ICU support: OFF (mmv collation profiles degrade)")
endif()

if(WIN32)
  target_link_libraries(migrator_core PUBLIC ws2_32)
endif()

add_executable(migrator src/main.cpp)
target_link_libraries(migrator PRIVATE migrator_core)

include(GNUInstallDirs)
install(TARGETS migrator RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY config/ DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/mini-migrator
        FILES_MATCHING PATTERN "*.json" PATTERN "*.ini")
install(DIRECTORY web/ DESTINATION ${CMAKE_INSTALL_DATADIR}/mini-migrator/web)

enable_testing()
add_test(NAME type_rules COMMAND migrator --prove-type-rules
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_test(NAME type_rules_conflicts COMMAND migrator --check-type-rules
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

option(BUILD_QT_GUI "Build the Qt desktop front-end" OFF)
if(BUILD_QT_GUI)
  find_package(Qt6 COMPONENTS Widgets REQUIRED)
  add_executable(migrator_gui qt/main.cpp qt/mainwindow.cpp qt/mainwindow.h)
  target_link_libraries(migrator_gui PRIVATE migrator_core Qt6::Widgets)
endif()
