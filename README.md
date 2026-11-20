# mini-migrator

**English** | [中文](README.zh.md) | [Русский](README.ru.md)

A C++17 heterogeneous data migration tool for **MySQL, PostgreSQL, openGauss,
SQL Server and Oracle**. It migrates full loads in multiple directions, verifies
row counts and checksums, and can stream changes via CDC. SQL Server and Oracle
are reached through ODBC and can use their native bulk loaders (`bcp`, `sqlldr`).

## Features

- 13 native migration modes across five databases; other pairs via `--source-type` / `--target-type`
- Row-count plus checksum verification (FNV, or the mmv digest where supported)
- Bulk-load channels: `bcp` for SQL Server, `sqlldr` for Oracle (automatic fallback to SQL inserts)
- Two-stage load for Oracle `BLOB` tables (hex staging + `TO_BLOB`)
- CDC from MySQL binlog and PostgreSQL logical replication, optional DDL sync
- Resume from a primary-key watermark, primary-key range parallelism
- Web dashboard (Chinese / English / Russian) with audit logging and TLS support
- `doctor` self-check for drivers, clients and bulk loaders
- Debian package and a self-contained Docker image

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
| `mysql2mssql` | MySQL | SQL Server | `bcp` channel |
| `mssql2mysql` | SQL Server | MySQL | full load |
| `mssql2mssql` | SQL Server | SQL Server | `bcp` channel |
| `mysql2oracle` | MySQL | Oracle | `sqlldr` channel |
| `oracle2mysql` | Oracle | MySQL | full load |
| `oracle2oracle` | Oracle | Oracle | `sqlldr` channel |

Any other pair can be expressed by overriding the kinds, for example:

```bash
./build/migrator --mode mysql2pg --source-type oracle --target-type pg ...
```

## Quick start

### Build

```bash
sudo apt-get install -y build-essential cmake ninja-build pkg-config \
  libmysqlclient-dev libpq-dev unixodbc-dev libssl-dev
make -j4            # or: cmake -S . -B build -G Ninja && cmake --build build -j
```

On macOS:

```bash
brew install mysql-client libpq unixodbc openssl@3
make -j4
```

### Check the environment

```bash
./build/migrator doctor
```

`doctor` checks the client libraries, registered ODBC drivers, `bcp` and `sqlldr`,
and exits non-zero only on hard failures. Driver names and tool paths come from
`config/providers.ini` (or `MIG_MSSQL_DRIVER`, `MIG_ORACLE_DRIVER`, `MIG_BCP`,
`MIG_SQLLDR`). See [docs/dependencies.md](docs/dependencies.md).

### Run a migration

```bash
./build/migrator --mode mysql2pg \
  --source "host=127.0.0.1;port=3306;user=root;password=...;db=demo" \
  --target "host=127.0.0.1;port=5432;user=postgres;password=...;db=demo_dst" \
  --batch 5000 --workers 4 --truncate --exit
```

For SQL Server add the driver to the DSN:

```bash
--target "host=127.0.0.1;port=1433;user=sa;password=...;db=demo_dst;driver=ODBC Driver 18 for SQL Server"
```

## Bulk-load channels

When `bcp` or `sqlldr` is available the target can hand a whole batch to the
vendor loader. Both are optional; without them the tool falls back to SQL inserts.

Measured on 5.05M rows / 7 tables (Ubuntu, Docker databases):

| Target | Channel | Time | Throughput |
|---|---|---|---|
| SQL Server | `bcp` | 35.8 s | 8.8 MB/s |
| SQL Server | INSERT fallback | 233 s | 1.3 MB/s |
| Oracle | `sqlldr` | 133.7 s | 2.4 MB/s |
| Oracle | INSERT ALL fallback | 1285 s | 0.2 MB/s |

Oracle `BLOB` columns are loaded in two stages: hex text into a staging table,
then `TO_BLOB(HEXTORAW(...))` into the target table.

## Packaging

```bash
# Debian/Ubuntu package
bash packaging/build-deb.sh
sudo dpkg -i mini-migrator_0.9.0_amd64.deb

# Self-contained image (msodbcsql18 + Instant Client + bcp/sqlldr)
docker build -t mini-migrator:full -f packaging/Dockerfile .
docker run --rm mini-migrator:full doctor
```

The package installs `/usr/bin/mini-migrator` and
`/etc/mini-migrator/providers.ini`.

## Common parameters

| Parameter | Default | Description |
|---|---|---|
| `--mode` | `sim` | migration mode (table above) |
| `--source` / `--target` | — | DSN: `host=;port=;user=;password=;db=` |
| `--batch` | 1000 | rows per batch |
| `--commit` | 10 | commit every N batches |
| `--workers` | 1 | parallel workers and PK-range parallelism |
| `--truncate` | off | drop target tables before loading |
| `--no-verify` | off | skip row-count and checksum verification |
| `--resume` / `--resume-dir` | off | continue from a primary-key watermark |
| `--cdc` | off | keep applying changes after the full load |
| `--with-views` | off | migrate views where supported |
| `--port` | 8123 | web dashboard port |

## Documentation

- [docs/usage.md](docs/usage.md) - full parameter reference and recipes
- [docs/dependencies.md](docs/dependencies.md) - drivers, bulk loaders, troubleshooting
- `migrator doctor` - machine-readable environment check

## Limitations

- Oracle `BLOB` tables use a two-stage path and are slower than plain tables
- `bcp`/`sqlldr` receive the password on the command line or through a temporary file;
  restrict access to the host
- CDC needs binlog enabled (MySQL) or logical replication (PostgreSQL)
- The web dashboard is read-only; module switches are build-time

## License

See [LICENSE](LICENSE).
