QT += core gui widgets
CONFIG += c++17
TEMPLATE = app
TARGET = migrator_gui

INCLUDEPATH += ../src

SOURCES += \
    main.cpp \
    mainwindow.cpp \
    ../src/core.cpp \
    ../src/metrics.cpp \
    ../src/pipeline.cpp \
    ../src/backend_sim.cpp \
    ../src/backend_mysql.cpp \
    ../src/backend_pg.cpp \
    ../src/backend_pg_source.cpp \
    ../src/backend_mysql_target.cpp \
    ../src/checksum.cpp \
    ../src/policy.cpp \
    ../src/views.cpp \
    ../src/backends.cpp \
    ../src/job_scheduler.cpp \
    ../src/ddl_dialect.cpp \
    ../src/local_file_loader.cpp \
    ../src/logger.cpp \
    ../src/module.cpp \
    ../src/pg_dialect.cpp \
    ../src/checksum_normalizer.cpp \
    ../src/config_validator.cpp

HEADERS += \
    mainwindow.h \
    ../src/migrator.h \
    ../src/backends.h \
    ../src/db_base.h \
    ../src/io_scheduler.h \
    ../src/job_scheduler.h \
    ../src/local_file_loader.h \
    ../src/logger.h \
    ../src/module.h \
    ../src/pg_dialect.h \
    ../src/checksum_normalizer.h \
    ../src/config_validator.h \
    ../src/ddl_dialect.h

MYSQL_PREFIX = /opt/homebrew/opt/mysql-client
PG_PREFIX = /opt/homebrew/opt/libpq
INCLUDEPATH += $$MYSQL_PREFIX/include $$PG_PREFIX/include
LIBS += -L$$MYSQL_PREFIX/lib -lmysqlclient -L$$PG_PREFIX/lib -lpq
DEFINES += HAVE_MYSQL HAVE_PG MIG_ENABLE_REPAIR MIG_ENABLE_METRICS MIG_ENABLE_SNAPSHOT MIG_ENABLE_RESUME
