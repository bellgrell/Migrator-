# mini-migrator

**English** | [中文](README.zh.md) | [Русский](README.ru.md)

A native C++17 tool for migrating data between **MySQL, PostgreSQL, openGauss,
SQL Server and Oracle** — full loads, verification, resume, and CDC. It is a
single binary: no Docker, no Python, no external services.

```bash
./run.sh --mode mysql2pg --source "db=app;host=10.0.0.5;user=reader;password=env:SRC_PW" \
                             --target "host=10.0.0.9;user=postgres;db=app_dst;password=env:DST_PW" \
         --batch 5000 --commit 10 --workers 4
```

## Why it exists

Moving data between heterogeneous databases is usually a choice between a slow
row-by-row script and a vendor tool that only understands two databases. This
tool does one job: copy a schema and its data as fast as the target can absorb
it, verify the result, and tell you honestly what happened.

## Highlights

- **13 native modes** across five databases; any other pair via `--source-type` / `--target-type`
- **Row-count + checksum verification** (FNV, or the mmv digest where the dialect supports it)
- **Primary-key range parallelism**: one large table is split by PK bounds and copied by N workers
- **Adaptive MySQL target loading**: `LOAD DATA` for narrow rows, multi-row `INSERT` for wide rows
- **Resume** from a primary-key watermark; `--verify-async` keeps verification off the critical path
- **CDC** from MySQL binlog and PostgreSQL logical replication, optional DDL sync
- **Built-in web dashboard** (Chinese / English / Russian) with audit log and TLS
- **`doctor`** self-check for client libraries, ODBC drivers and bulk loaders
- **No runtime dependencies beyond client libraries** — no Docker required

## Supported directions

| Mode | Source | Target | Notes |
|---|---|---|---|
| `mysql2pg` | MySQL | PostgreSQL | full load + verification |
| `mysql2og` | MySQL | openGauss | full load + verification |
| `pg2mysql` | PostgreSQL | MySQL | full load + verification |
| `pg2og` | PostgreSQL | openGauss | full load + verification |
| `og2pg` | openGauss | PostgreSQL | full load + verification |
| `og2mysql` | openGauss | MySQL | full load + verification |
| `mysql2mysql` | MySQL | MySQL | full load + verification |
| `mysql2mssql` | MySQL | SQL Server | `bcp` channel, INSERT fallback |
| `mssql2mysql` | SQL Server | MySQL | full load |
| `mssql2mssql` | SQL Server | SQL Server | `bcp` channel |
| `mysql2oracle` | MySQL | Oracle | `sqlldr` channel |
| `oracle2mysql` | Oracle | MySQL | full load |
| `oracle2oracle` | Oracle | Oracle | `sqlldr` channel |

Any other pair can be expressed by overriding the kinds:

```bash
./build/migrator --mode mysql2pg --source-type oracle --target-type pg ...
```

## Install

### Portable package (recommended, no Docker)

```bash
tar -xzf migrator-1.0.0-linux-x64.tar.gz
cd migrator-1.0.0-linux-x64
./run.sh doctor          # environment self-check
./run.sh --help
```

The archive contains the binary, bundled client libraries (`libmysqlclient`,
`libpq`, `unixODBC`) and the type-map configuration. Build it yourself with:

```bash
bash packaging/build-portable.sh 1.0.0
```

Requirements on the target host: Linux x86-64 with glibc 2.39+ and TCP access to
the databases. ODBC drivers are needed only for SQL Server (`msodbcsql18`) and
Oracle (Instant Client + ODBC).

### From source

```bash
sudo apt-get install -y build-essential cmake ninja-build pkg-config \
  libmysqlclient-dev libpq-dev unixodbc-dev libssl-dev
make -j4            # or: cmake -S . -B build -G Ninja && cmake --build build -j
```

macOS:

```bash
brew install mysql-client libpq unixodbc openssl@3
make -j4
```

### Debian package / container

```bash
bash packaging/build-deb.sh
sudo dpkg -i mini-migrator_0.9.0_amd64.deb
```

A Docker image exists for CI convenience only; the binary itself does not need it.

## Usage

### DSN

```
key=value;key=value;...
```

| Key | Meaning |
|---|---|
| `host`, `port` | server address |
| `user`, `password` | credentials; `password=env:VAR` and `password=file:PATH` are supported |
| `db` | database / service name |
| `driver` | ODBC driver name, required for SQL Server and Oracle |

Put `password=` **last** when a `driver=` key is present — the parser consumes
the rest of the string as the password value.

### A typical run

```bash
./build/migrator --mode mysql2oracle \
  --source "db=erp;host=10.0.0.5;port=3306;user=reader;password=env:SRC_PW" \
  --target "host=10.0.0.9;port=1521;user=ERP;db=FREEPDB1;driver=Oracle 23 ODBC Driver;password=env:DST_PW" \
  --batch 5000 --commit 10 --workers 4 --truncate --verify-async
```

### Common parameters

| Parameter | Default | Description |
|---|---|---|
| `--mode` | `sim` | migration mode |
| `--source` / `--target` | — | DSN strings |
| `--batch` | 1000 | rows per batch |
| `--commit` | 10 | commit every N batches |
| `--workers` | 1 | parallel workers / PK ranges |
| `--pk-min-rows` | 1000000 | table size that triggers PK-range parallelism |
| `--truncate` | off | drop target tables before loading |
| `--load-mode` | `auto` | MySQL target: `auto` / `insert` / `ignore` / `replace` |
| `--io-scheduler` | `fair` | `off` / `fair` / `scan` batch I/O policy |
| `--no-verify` | off | skip row-count and checksum verification |
| `--verify-async` | off | verify after the load finishes |
| `--resume` | off | continue from a PK watermark |
| `--cdc` | off | keep applying changes after the full load |
| `--tables-config` | — | per-table policy: `copy` / `rebuild:file.sql` / `skip` |
| `--type-map` | `config/type_map.json` | DDL type mapping |
| `--port` | 8123 | dashboard port |

## Performance

Two things dominate throughput: how the target commits, and how the data is
inserted.

**Server-side settings that matter most** (MySQL target):

```ini
[mysqld]
innodb_buffer_pool_size=16G     # size to the working set, not the row count
innodb_doublewrite=OFF          # benchmark/lab only
innodb_flush_method=O_DIRECT
innodb_flush_neighbors=0
innodb_io_capacity=2000
innodb_io_capacity_max=4000
innodb_flush_log_at_trx_commit=2
skip-log-bin
max_allowed_packet=64M
net_read_timeout=3600
net_write_timeout=3600
```

On the tool side: raise `--commit` (fewer fsyncs), keep `--workers` at 4–8, and
leave `--load-mode auto` — it switches wide rows to multi-row INSERT, which
avoids the long single-statement `LOAD DATA` that servers kill mid-flight.

Measured on a 62 GB / NVMe lab host (Docker-hosted databases, 20M-row dataset):

| Direction | Throughput |
|---|---|
| mysql → pg | 176 MB/s |
| pg → pg | 150 MB/s |
| mssql → pg | 187 MB/s |
| mysql → mssql | 25 MB/s |
| * → oracle | 3.5–4.3 MB/s |

The Oracle target is the outlier: it loads row by row. If Oracle is your target,
budget accordingly or enable `sqlldr`.

### Metrics the dashboard reports

- throughput (MB/s) and rows/s
- read and write amplification (container block-device bytes ÷ payload)
- peak RSS (VmHWM) and target database memory
- per-table progress, error counters, ETA

## Verification

```bash
./build/migrator --mode mysql2pg ... --verify-engine fnv        # checksum
./build/migrator --mode mysql2pg ... --verify-async             # after load
```

Verification compares row counts and a normalized digest of every column, so
type differences (DECIMAL vs NUMERIC, DATETIME vs TIMESTAMP) do not produce
false mismatches.

## CDC

```bash
./build/migrator --mode mysql2pg ... --cdc --cdc-drop-slot
```

MySQL requires `binlog_format=ROW`; PostgreSQL needs `wal_level=logical`.
`--cdc-drop-slot` removes the replication slot when the tool stops.

## Troubleshooting

| Symptom | Fix |
|---|---|
| `Lost connection to MySQL server during query` | raise `net_read_timeout`/`net_write_timeout`, keep `--load-mode auto` |
| `Can't open lib 'ODBC Driver 18 for SQL Server'` | install `msodbcsql18`, register it in `odbcinst.ini` |
| `ORA-12541: no listener` | check port and listener |
| `ORA-01017: invalid credential` | password with special characters — use `password=file:PATH` |
| `ORA-12954: exceeds the maximum allowed database size` | PDB hit its `MAXSIZE`; recreate with `STORAGE (MAXSIZE 64G)` |
| verification timeout on huge tables | use `--verify-async` |
| `error while loading shared libraries` | host glibc older than the build host |

`./build/migrator doctor` prints a machine-readable environment check.

## Limitations

- Oracle `BLOB` tables use a two-stage hex path and are slower than plain tables
- `bcp`/`sqlldr` receive credentials through the command line or a temporary file
- CDC needs server-side replication configured in advance
- the dashboard is read-only; module switches are build-time
- MySQL targets default to `LOAD DATA`; very wide rows switch to INSERT automatically

## Documentation

- [docs/usage.md](docs/usage.md) — full parameter reference
- [docs/dependencies.md](docs/dependencies.md) — drivers, loaders, troubleshooting

## License

See [LICENSE](LICENSE).
