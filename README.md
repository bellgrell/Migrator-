# mini-migrator

**English** | [中文](README.zh.md) | [Русский](README.ru.md)

[![Release](https://img.shields.io/github/v/release/bellgrell/Migrator-?display_name=tag&sort=semver&label=release)](https://github.com/bellgrell/Migrator-/releases/latest) [![Platform](https://img.shields.io/badge/platform-linux%20x86--64-blue)](https://github.com/bellgrell/Migrator-/releases/latest) [![License](https://img.shields.io/github/license/bellgrell/Migrator-)](LICENSE)

> One binary that moves data from database A to database B. No Docker, no Python, no middleware.

mini-migrator is a C++17 migration tool for **MySQL, PostgreSQL, openGauss,
SQL Server and Oracle**. It runs 13 native directions, verifies row counts and
checksums, parallelises large tables by primary-key ranges, resumes from a
watermark, streams changes via CDC, and reports throughput and write
amplification from a built-in read-only dashboard.

## Why it exists

Cross-database migration usually offers two choices: a row-by-row script that is
slow and hammers the target, or a vendor tool that understands two databases and
costs a licence. This project does one job — **copy the schema and the data as
fast as the target can absorb it, then tell you honestly what happened**: rows
moved, verification result, write amplification, which table failed.

## Features

- **13 native directions** across five databases; any other pair via `--source-type` / `--target-type`
- **Primary-key range parallelism** — split a large table by PK bounds, read with N workers
- **Adaptive MySQL loading** — `LOAD DATA` for narrow rows, multi-row `INSERT` above 512 B/row
- **Row-count + checksum verification** — FNV or the mmv digest, detects single-row tampering
- **Resume** from a PK watermark; `--verify-async` keeps verification off the critical path
- **CDC** from MySQL binlog and PostgreSQL logical replication, optional DDL sync
- **Observability** — throughput (MB/s), rows/s, read/write amplification, peak RSS, target memory
- **Single-binary deployment** — the release is a Docker-free portable package

## Quick start

### Download

Grab `migrator-1.0.0-linux-x64.tar.gz` from
[Releases](https://github.com/bellgrell/Migrator-/releases/latest), verify, unpack:

```bash
shasum -a 256 -c SHA256SUMS
tar -xzf migrator-1.0.0-linux-x64.tar.gz
cd migrator-1.0.0-linux-x64
./run.sh doctor        # environment self-check
```

### Run a migration

```bash
./run.sh --mode mysql2pg \
  --source "db=app;host=10.0.0.5;port=3306;user=reader;password=***" \
  --target "host=10.0.0.9;port=5432;user=postgres;db=app_dst;password=***" \
  --batch 5000 --commit 10 --workers 4 --truncate --verify-async
```

Credentials can be `password=env:VAR` or `password=file:PATH`. When the DSN
contains `driver=`, the `password=` key must come **last**.

### Build from source

```bash
sudo apt-get install -y build-essential cmake ninja-build pkg-config \
  libmysqlclient-dev libpq-dev unixodbc-dev libssl-dev
make -j4
```

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

Any other pair:

```bash
./run.sh --mode mysql2pg --source-type oracle --target-type pg ...
```

## Performance

Two things dominate throughput: **how the target commits** and **how rows are inserted**.

The settings that matter most on a MySQL target:

```ini
[mysqld]
innodb_buffer_pool_size=16G
innodb_doublewrite=OFF
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

Tool side: raise `--commit` to cut fsyncs, keep `--workers` at 4–8, leave
`--load-mode auto` on.

Measured on a 62 GB / NVMe host with a 20M-row dataset:

| Direction | Throughput |
|---|---|
| mysql → pg | 176 MB/s |
| pg → pg | 150 MB/s |
| mssql → pg | 187 MB/s |
| mysql → mssql | 25 MB/s |
| \* → oracle | ~8 MB/s (NOLOGGING + array binding) |

Oracle targets load row by row and are the bottleneck of the matrix; budget time
or enable `sqlldr`.

## Documentation

| Document | Contents |
|---|---|
| [docs/usage.md](docs/usage.md) | full parameter reference and recipes |
| [docs/dependencies.md](docs/dependencies.md) | drivers, bulk loaders, installation |
| [docs/operations.md](docs/operations.md) | dashboard, logging, audit |
| [docs/adapters-mssql-oracle.md](docs/adapters-mssql-oracle.md) | SQL Server / Oracle specifics |
| [INSTALL.md](packaging/portable/INSTALL.md) | portable package and troubleshooting |
| [docs/README.md](docs/README.md) | documentation index |

## Troubleshooting

| Symptom | Fix |
|---|---|
| `Lost connection to MySQL server during query` | raise `net_read_timeout`/`net_write_timeout`, keep `--load-mode auto` |
| `Can't open lib 'ODBC Driver 18 for SQL Server'` | install `msodbcsql18`, register it in `odbcinst.ini` |
| `ORA-12541` | check port and listener |
| `ORA-01017` | password with special characters — use `password=file:PATH` |
| `ORA-12954` | PDB hit `MAXSIZE`; recreate with `STORAGE (MAXSIZE 64G)` |
| verification timeout on huge tables | use `--verify-async` |
| `error while loading shared libraries` | host glibc older than the build host |

## Contributing

- Issues: include database versions, `doctor` output and the failing log lines
- Code changes: `bash -n` the scripts, smoke-test one direction, then run the matrix
- Packaging: `bash packaging/build-portable.sh 1.0.0`

## License

[LICENSE](LICENSE)

## Links

- Repository: https://github.com/bellgrell/Migrator-
- Issues: https://github.com/bellgrell/Migrator-/issues
