# mini-migrator

**English** | [中文](README.zh.md) | [Русский](README.ru.md)

A C++17 heterogeneous data migration tool for **MySQL, PostgreSQL and openGauss**. It supports full-load migration in multiple directions, row-count and checksum verification, and CDC (change data capture) from MySQL binlog to PostgreSQL/openGauss.

The built-in web dashboard supports **Chinese / English / Russian** and can be switched in the top-right corner.

## Supported directions

| Mode | Source | Target | Status |
|---|---|---|---|
| `mysql2pg` | MySQL | PostgreSQL | Full load + verification |
| `mysql2og` | MySQL | openGauss | Full load + verification |
| `pg2mysql` | PostgreSQL | MySQL | Full load + verification |
| `pg2og` | PostgreSQL | openGauss | Full load + verification |
| `og2pg` | openGauss | PostgreSQL | Full load + verification |
| `og2mysql` | openGauss | MySQL | Full load + verification |
| `mysql2mysql` | MySQL | MySQL | Full load + verification |

CDC is currently available only with a MySQL source (MySQL binlog).

## Build

Requirements: C++17 compiler, libmysqlclient, libpq.

```bash
make
./build/migrator --help
```

On macOS:

```bash
brew install mysql-client libpq
make
```

CMake is also supported:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## Modules

Optional modules are compiled in by default and can be disabled at build time:

```bash
make WITH_PG_CDC=0 WITH_REPAIR=0
# or with CMake:
cmake -B build -DMIG_ENABLE_PG_CDC=OFF -DMIG_ENABLE_REPAIR=OFF
cmake --build build -j
```

`./build/migrator --features` lists the compiled modules: `pg_cdc`, `repair`, `metrics`, `snapshot`, `resume`.

The core binary still depends only on `libmysqlclient` and `libpq`.

For a minimal build run `make lite` (core only); `make full` restores all modules. Compiled modules can also be disabled at runtime without rebuilding: `--disable-modules pg_cdc,repair,...`. The web dashboard only displays module status; toggling there would require a job-control UI.

The web dashboard shows the compiled modules and their status. Module switches are build-time, so changing them requires a rebuild.

## Quick start

MySQL to PostgreSQL:

```bash
./build/migrator --mode mysql2pg \
  --source 'host=127.0.0.1;port=3306;user=root;db=demo' \
  --target 'host=127.0.0.1;port=5432;user=postgres;db=demo_dst' \
  --batch 2000 --workers 4 --exit
```

MySQL to openGauss:

```bash
# create the target database in PG-compatible mode first
# CREATE DATABASE demo_dst WITH ENCODING='UTF8' DBCOMPATIBILITY='PG';

./build/migrator --mode mysql2og \
  --source 'host=127.0.0.1;port=3306;user=root;db=demo' \
  --target 'host=127.0.0.1;port=15432;user=gaussdb;db=demo_dst' \
  --batch 2000 --workers 4 --exit
```

PostgreSQL to MySQL:

```bash
# target MySQL requires local_infile=ON for bulk load
# SET GLOBAL local_infile=1;

./build/migrator --mode pg2mysql \
  --source 'host=127.0.0.1;port=5432;user=postgres;db=demo' \
  --target 'host=127.0.0.1;port=3306;user=root;db=demo_mysql' \
  --batch 2000 --workers 4 --exit
```

The web dashboard starts at `http://127.0.0.1:8123/` by default. Add `--exit` to exit after migration, otherwise the process keeps serving the dashboard.

## Basic parameters

| Parameter | Default | Description |
|---|---|---|
| `--mode` | `sim` | Migration mode (see table above) |
| `--source` | — | Source DSN |
| `--target` | — | Target DSN |
| `--batch` | 1000 | Rows per batch |
| `--commit` | 10 | Commit every N batches |
| `--workers` | 1 | Parallel workers (table-level and single-table PK ranges) |
| `--port` | 8123 | Web dashboard port |
| `--no-verify` | off | Skip row-count and checksum verification |
| `--no-checksum` | off | Compare row counts only |
| `--with-views` | off | Migrate views (MySQL ↔ PG/openGauss; PG→MySQL mechanical conversion) |
| `--cdc` | off | Continue with CDC after full load |
| `--cdc-only` | off | Run CDC only |
| `--cdc-ddl` | off | Apply DDL changes from binlog |
| `--exit` | off | Exit after migration |
| `--connect-timeout` | 10 | Connect/read/write timeout in seconds |
| `--connect-retries` | 3 | Connection retry attempts |
| `--truncate` | off | Drop target tables before migration (safe rerun) |
| `--sslmode` | prefer | PostgreSQL/openGauss sslmode |
| `--mysql-ssl-mode` | PREFERRED | MySQL SSL mode: DISABLED/PREFERRED/REQUIRED |
| `--cdc-retries` | 10 | CDC reconnect attempts |
| `--cdc-slot` | migrator_slot | Logical replication slot name for PostgreSQL CDC |
| `--repair` | off | After verification, insert missing and delete extra rows by primary key |
| `--only-tables` | — | Migrate only these tables (comma-separated) |
| `--exclude-tables` | — | Exclude these tables |
| `--consistent-snapshot` | off | Consistent snapshot (MySQL: binlog position; PG/openGauss: REPEATABLE READ), forces workers=1 |
| `--resume` | off | Resume full load from PK watermark |
| `--resume-dir` | out/resume | Resume checkpoint directory |
| `--load-mode` | auto | MySQL target LOAD DATA mode: auto/insert/ignore/replace |
| `--io-scheduler` | `fair` | Batch I/O scheduling: `off` / `fair` / `scan` |
| `--io-queue-depth` | 0 | Concurrent batch reads (0 = workers) |
| `--io-prefetch` | 0 | Read-ahead batches per range (0 = off) |
| `--io-window` | 0 | SCAN range window (0 = auto) |

DSN format: `host=...;port=...;user=...;password=...;db=...`.

### I/O scheduling

`--io-scheduler fair` (default) hands batch reads to ranges in FIFO turns and bounds the number of in-flight reads, so one range cannot monopolize the source; `scan` only admits ranges inside a moving primary-key window, which suits tables clustered by primary key; `off` disables scheduling. `--io-prefetch` adds a read-ahead buffer per range (reader and writer overlap); it is off by default because it usually does not help with remote databases. The scheduler only controls client-side batch reads; physical disk scheduling remains with the OS/database engine.

## Performance

Dataset used for the measurements below: 5,110,000 rows in three tables (5,000,000 with an integer primary key, 100,000 with a composite key, 10,000 without a key). Batch size 2000, 4 workers, row-count and checksum verification enabled. Timing includes verification.

| Direction | Rows | Time | Throughput |
|---|---:|---:|---:|
| MySQL → openGauss | 7,110,000 | 15.43 s | 460,836 rows/s |
| MySQL → PostgreSQL | 5,110,000 | 12.42 s | 411,287 rows/s |
| MySQL → MySQL | 5,110,000 | 13.31 s | 384,006 rows/s |
| openGauss → PostgreSQL | 5,110,000 | 17.39 s | 293,923 rows/s |
| PostgreSQL → openGauss | 5,110,000 | 19.32 s | 264,441 rows/s |
| openGauss → MySQL | 5,110,000 | 23.57 s | 216,776 rows/s |
| PostgreSQL → MySQL | 5,110,000 | 27.52 s | 185,680 rows/s |

The MySQL → openGauss run used a source database that also contained two temporary 1,000,000-row benchmark tables, hence 7,110,000 rows.

Single-table primary-key range parallelism (`--workers 4`) measured: PostgreSQL → MySQL, 1.25M rows in 6.23 s / 200,517 rows/s (sequential 9.11 s / 137,168 rows/s, about 46% faster); openGauss → MySQL, 5M rows in 15.12 s / 330,621 rows/s. Both passed checksum verification.

Batch I/O scheduling (`--io-scheduler fair --io-prefetch 0`) versus `off`: PostgreSQL, 1.25M rows in 5.18 s / 241,276 rows/s (off 5.80 s / 215,442); openGauss, 5M rows in 14.30 s / 349,703 rows/s (off 15.07 s / 331,851). Both passed checksum verification.

Typical table-level results with the same dataset:

| Table | Rows | Notes |
|---|---:|---|
| `big` | 5,000,000 | Integer primary key, range-parallel |
| `comp` | 100,000 | Composite primary key |
| `nopk` | 10,000 | No primary key |
| `users` | 100,000 | Secondary indexes, text and decimal columns |

## Limitations

- CDC requires a primary key; tables without a primary key are skipped.
- CDC supports MySQL sources (binlog ROW events) and PostgreSQL sources (wal2json logical decoding); the PostgreSQL CDC target must be PG/openGauss, not MySQL.
- PostgreSQL CDC requires `wal_level=logical`, the `wal2json` output plugin allowed in `output_plugin_libraries`, and a replication slot. Changes made during a full load are not captured unless the slot existed before the load.
- `--repair` requires a single-column primary key; integer keys use a streaming merge (O(batch) memory), non-integer keys fall back to in-memory sets.
- Unsupported column types: BIT, ENUM, SET, GEOMETRY.
- DDL dialect conversion is limited; complex MySQL-specific DDL may need manual rewriting.
- DM8 is not implemented.
- PostgreSQL → MySQL view migration uses mechanical conversion (quotes/functions/simple `::type` casts); complex expressions still need review.
- Target tables should be empty or already aligned; use `--truncate` to drop and recreate them for a safe rerun.
- Checksum read failures make verification FAIL instead of being silently skipped.
- Full-load connections retry with backoff; a mid-stream read failure reconnects and resumes from the last written primary key; CDC reconnects from the last committed binlog position.
- For remote databases, open the DB port, allow the client host in grants/firewall, and use SSH tunnel/VPN if needed.
- `--resume` is implemented for MySQL, PostgreSQL and openGauss sources; it requires a single-column primary key.
- `--consistent-snapshot` uses a single source connection (MySQL: consistent snapshot + binlog position; PG/openGauss: REPEATABLE READ) and forces `workers=1`.
- `--resume` and `--truncate` are mutually exclusive.
- PostgreSQL/openGauss sources now support single-table primary-key range parallelism (`--workers`); `--resume` and `--consistent-snapshot` still use a single sequential cursor.
- MySQL target bulk load uses `LOAD DATA LOCAL INFILE`; the server must have `local_infile=ON`. Tables with binary columns automatically fall back to batched INSERT.
- openGauss target databases should be created with `DBCOMPATIBILITY='PG'`; if the account uses sha256 authentication, native libpq may require switching the migration account to md5.

## Web dashboard

The dashboard shows progress, throughput, per-table status, verification result and the event stream. It supports three UI languages:

- 中文
- English
- Русский

The language selection is stored in the browser (localStorage).

## Metrics

Prometheus endpoint: `http://127.0.0.1:8123/metrics`.

Exposed metrics: `migrator_rows_done`, `migrator_rows_total`, `migrator_errors_total`, `migrator_batches_total`, `migrator_bytes_written_total`, `migrator_verify_mismatch`, `migrator_running`, `migrator_verified`, plus per-table `migrator_table_rows_done{table="..."}`.

## License

Apache License 2.0. See [LICENSE](LICENSE).
