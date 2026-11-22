# migrator 1.0.0 - Linux x86-64 portable package

No Docker required. The tool is a native binary that talks to databases over TCP.

## Layout

    bin/migrator      the migration binary
    lib/              bundled client libraries (MySQL, PostgreSQL, unixODBC)
    config/           type maps, table policies, module/providers config
    run.sh            launcher that sets LD_LIBRARY_PATH

## Requirements on the target host

- Linux x86-64, glibc 2.39+ (Ubuntu 24.04+ / Debian 13+)
- Network access to the source and target databases
- ODBC drivers only if you migrate to/from SQL Server or Oracle:
  - SQL Server: msodbcsql18
  - Oracle: Oracle Instant Client + ODBC driver

Everything else is bundled. If the host has newer system versions you may delete
the corresponding file from lib/ and the loader will fall back to the system copy.

## Quick start

    ./run.sh --help

    ./run.sh --mode mysql2pg \
      --source "db=app;host=10.0.0.5;port=3306;user=reader;password=env:SRC_PW" \
      --target "host=10.0.0.9;port=5432;user=postgres;db=app_dst;password=env:DST_PW" \
      --workers 4 --batch 5000 --commit 10

Credentials can be given as password=..., password=env:VAR or password=file:PATH.

## Verify

    ./run.sh --mode sim --rows 200000 --tables 1 --exit

## Common problems

| Symptom | Cause |
|---|---|
| error while loading shared libraries | glibc older than the build host |
| Can't open lib 'ODBC Driver 18 for SQL Server' | driver not installed or not in odbcinst.ini |
| ORA-12541: no listener | wrong port / listener not running |
| Lost connection to MySQL server during query | raise net_read_timeout / net_write_timeout on the server |
