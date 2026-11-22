# Usage

**English** | [中文](usage.zh.md)

Reference for every mode, parameter and file the tool touches. For a quick start
see the [top-level README](../README.md).

## 1. Capabilities

| Capability | Notes |
|---|---|
| Full load | reads the source, writes the target; COPY protocol on PostgreSQL targets |
| In-table parallelism | single integer PK split into ranges, N connections read concurrently |
| Table policies | `copy` / `rebuild` / `skip` |
| DDL generation | type mapping + primary key + NOT NULL |
| Secondary indexes | unique and plain indexes, created after the data load |
| BLOB data | byte-exact, including NUL bytes |
| Views | MySQL `CREATE VIEW` → PostgreSQL `CREATE OR REPLACE VIEW` |
| Verification | row count + FNV-1a checksum, detects single-row tampering |
| ANALYZE | refresh statistics after the load |
| CDC | binlog ROW events, insert/update/delete |
| CDC rotation | follows binlog files across rotation |
| CDC DDL | ALTER/CREATE/DROP with basic dialect conversion |
| Dashboard | built-in HTTP panel |

## 2. Build

```bash
make                                   # macOS / Linux
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
```

Output: `build/migrator`.

## 3. Modes

| Mode | Source | Target | Status |
|---|---|---|---|
| `sim` | built-in | built-in | complete |
| `mysql2pg` | MySQL | PostgreSQL | complete |
| `mysql2og` | MySQL | openGauss | complete |
| `pg2mysql` | PostgreSQL | MySQL | complete (views unsupported) |
| `pg2og` | PostgreSQL | openGauss | complete |
| `og2pg` | openGauss | PostgreSQL | complete |
| `og2mysql` | openGauss | MySQL | complete (views unsupported) |
| `mysql2mysql` | MySQL | MySQL | complete |
| `mysql2mssql` | MySQL | SQL Server | complete (`bcp` channel) |
| `mssql2mysql` | SQL Server | MySQL | complete |
| `mssql2mssql` | SQL Server | SQL Server | complete (`bcp` channel) |
| `mysql2oracle` | MySQL | Oracle | complete (`sqlldr` channel) |
| `oracle2mysql` | Oracle | MySQL | complete |
| `oracle2oracle` | Oracle | Oracle | complete (`sqlldr` channel) |

Override the kinds with `--source-type` / `--target-type`
(`mysql|pg|og|mssql|oracle`).

### 3.1 ODBC targets and bulk loading

SQL Server and Oracle are reached through ODBC. The DSN **must** carry
`driver=`, otherwise the default driver is used and the connection fails:

```bash
--target "host=127.0.0.1;port=1433;user=sa;password=***;db=bench_dst;driver=ODBC Driver 18 for SQL Server"
--target "host=127.0.0.1;port=1521;user=bench_dst;password=***;db=FREEPDB1;driver=Oracle 23 ODBC Driver"
```

Driver names and loader paths live in `config/providers.ini`
(precedence: environment > file > auto-detect):

```ini
mssql_driver=ODBC Driver 18 for SQL Server
oracle_driver=Oracle 23 ODBC Driver
bcp_path=
sqlldr_path=
```

When `bcp` or `sqlldr` is detected the bulk channel is enabled; otherwise the
tool falls back to row-by-row SQL. Oracle `BLOB` columns use a two-stage path
(hex staging table + `TO_BLOB(HEXTORAW(...))`), converted after every batch so
the last batch is never lost.

## 4. Parameters

| Parameter | Default | Purpose |
|---|---|---|
| `--mode` | `sim` | migration mode |
| `--source` | empty | source DSN |
| `--target` | empty | target DSN |
| `--rows` | 200000 | simulated rows (sim only) |
| `--tables` | 4 | simulated tables (sim only) |
| `--batch` | 1000 | rows per batch |
| `--commit` | 10 | commit every N batches |
| `--source-type` | from mode | `mysql/pg/og/mssql/oracle` |
| `--target-type` | from mode | `mysql/pg/og/mssql/oracle` |
| `--workers` | 1 | parallel workers; large single tables use PK ranges |
| `--pk-min-rows` | 1000000 | minimum rows to trigger PK-range parallelism |
| `--no-pk-parallel` | off | disable in-table parallelism |
| `--log-every` | 10 | log every N batches |
| `--out-dir` | out | log directory |
| `--throttle` | 0 | sleep milliseconds per batch |
| `--io-scheduler` | fair | batch I/O policy: `off/fair/scan` |
| `--io-queue-depth` | 0 | concurrent read batches (0 = workers) |
| `--io-prefetch` | 0 | prefetch batches per range (0 = off) |
| `--io-window` | 0 | range window for `scan` (0 = auto) |
| `--load-mode` | auto | MySQL target: `auto/insert/ignore/replace` |
| `--port` | 8123 | dashboard port |
| `--type-map` | config/type_map.json | type mapping |
| `--tables-config` | config/tables.json | per-table policy |
| `--no-index` | off | skip secondary indexes |
| `--no-analyze` | off | skip ANALYZE |
| `--no-copy` | off | fall back to INSERT |
| `--no-checksum` | off | compare row counts only |
| `--no-verify` | off | skip verification |
| `--verify-only` | off | verify without migrating |
| `--with-views` | off | migrate views |
| `--cdc` | off | keep streaming after the load |
| `--cdc-only` | off | streaming only |
| `--cdc-ddl` | off | replicate DDL |
| `--cdc-start` | empty | starting position `file:pos` |
| `--cdc-server-id` | 987654321 | replication client `server_id` |
| `--cdc-max-events` | 0 | exit after N events (0 = unlimited) |
| `--exit` | off | exit when the migration finishes |

The I/O scheduler only applies to in-table PK-range parallelism: `fair`
(default) dispatches reads round-robin and caps in-flight batches; `scan`
admits ranges inside the PK window, which suits clustered sources; `off`
disables it. It does not control the database server's physical disk schedule.

## 5. Examples

```bash
# simulation, no database required
./build/migrator --mode sim --rows 600000 --tables 6 --throttle 200

# full load
./build/migrator --mode mysql2pg \
  --source "host=127.0.0.1;port=3306;user=root;db=demo" \
  --target "host=127.0.0.1;port=5432;user=postgres;db=demo_dst" \
  --batch 2000 --workers 4 --with-views

# full load + CDC + DDL
./build/migrator --mode mysql2pg ... --cdc --cdc-ddl

# streaming only
./build/migrator --mode mysql2pg ... --cdc-only --cdc-ddl

# verify an existing target
./build/migrator --mode mysql2pg ... --verify-only
```

## 6. Table policies

`config/tables.json`:

```json
{
  "tables": {
    "daily_stats": "rebuild:sql/daily_stats.sql",
    "tmp_cache": "skip"
  }
}
```

| Policy | Behaviour |
|---|---|
| `copy` (default) | create table, load data, build indexes |
| `rebuild` | skip data, run the given SQL after the load |
| `skip` | ignore the table |

## 7. Type mapping

Selected automatically by direction:

| Direction | File |
|---|---|
| MySQL → PG/openGauss | `config/type_map.json` |
| MySQL → MySQL | `config/type_map.mysql.json` |
| PG/openGauss → PG/openGauss | `config/type_map.pg.json` |
| PG/openGauss → MySQL | `config/type_map.pg2mysql.json` |

`--type-map <path>` overrides the choice.

```json
{
  "global":  { "varchar": "varchar{args}", "datetime": "timestamp" },
  "columns": { "users.bio": "text" }
}
```

Two levels: global types plus per-column overrides; `{args}` is replaced by the
source type arguments.

## 8. Index migration

Reads `information_schema.STATISTICS` and issues `CREATE INDEX` after the data
load. PostgreSQL index names are schema-unique, so duplicates get a
`_<table>` suffix.

## 9. Views

`--with-views` extracts `SHOW CREATE VIEW` and converts:

- strips `ALGORITHM` / `DEFINER` / `SQL SECURITY`
- backticks → double quotes
- `IFNULL` → `COALESCE`, normalises `NOW()` / `CURDATE()` / `LENGTH()`
- a failing view is isolated with `SAVEPOINT`

## 10. CDC

Requires `log_bin=ON` and `binlog_format=ROW`.

Event flow: GTID → QUERY → TABLE_MAP → WRITE/UPDATE/DELETE_ROWS → XID

Application:

- INSERT / UPDATE → `INSERT ... ON CONFLICT (pk) DO UPDATE`
- DELETE → `DELETE WHERE pk = ...`

Supported column types: TINY/SHORT/LONG/INT24/LONGLONG/YEAR/FLOAT/DOUBLE/
NEWDECIMAL/VARCHAR/VAR_STRING/STRING/BLOB/TEXT/JSON/DATE/DATETIME2/TIMESTAMP2.

The position is stored in `out/cdc-position.txt`; restart resumes automatically
and follows binlog rotation.

## 11. Verification

- row counts compared table by table
- checksum: per-row normalisation then FNV-1a, aggregated with sum + xor
  (order-independent)
- detects "same row count, modified content"

## 12. Dashboard API

| Endpoint | Content |
|---|---|
| `/` | dashboard |
| `/api/status` | progress, throughput, ETA, errors, verification |
| `/api/tables` | per-table details |
| `/api/series` | throughput series |
| `/api/events` | event log |
| `/api/health` | health check |

## 13. Output artifacts

| Artifact | Location |
|---|---|
| migration log | `out/migration-YYYYmmdd-HHMMSS.log` |
| CDC position | `out/cdc-position.txt` |
| dashboard | `http://127.0.0.1:<port>/` |

## 14. Known limitations

| Limitation | Detail |
|---|---|
| CDC needs a primary key | tables without a PK are skipped |
| Unsupported column types | BIT / ENUM / SET / GEOMETRY |
| Limited DDL conversion | `DATE_FORMAT`/`GROUP_CONCAT`/`LIMIT x,y`/partition syntax need manual edits |
| openGauss authentication | default `sha256` is not supported by native libpq, see section 15 |
| Target must be empty or aligned | re-running against partially loaded tables hits primary keys |
| Verification memory | integer PKs stream and merge in O(batch); other PK types use set comparison |
| I/O scheduler is client-side only | physical disk scheduling stays with the OS / engine |
| Range parallelism needs a single integer PK | supported for MySQL / PostgreSQL / openGauss; `--resume` and `--consistent-snapshot` fall back to sequential reads |
| Oracle BLOB tables | two-stage load, slower than plain tables |
| bcp / sqlldr credentials | passed via command line or a temporary file; restrict host access |

## 15. openGauss notes

Use `--mode mysql2og` (alias `mysql2opengauss`). Create the target database in
PG compatibility mode:

```sql
CREATE DATABASE og_dst WITH ENCODING='UTF8' DBCOMPATIBILITY='PG';
```

If the server only allows `sha256` authentication, switch the migration account
to md5:

```sql
ALTER SYSTEM SET password_encryption_type = 0;
ALTER USER migrator IDENTIFIED BY '...';
-- pg_hba.conf: host all all 0.0.0.0/0 md5
SELECT pg_reload_conf();
```

Compatibility handled by the tool: `client_encoding=UTF8`,
`standard_conforming_strings=on`, boolean COPY output as `true`/`false`,
`behavior_compat_options='display_leading_zero'` so numerics compare correctly,
`NO SCROLL` cursors with fallback, and DELETE+INSERT instead of
`ON CONFLICT` for CDC updates.
