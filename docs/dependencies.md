# Dependencies

**English** | [中文](dependencies.zh.md)

The tool does not bundle vendor components such as Oracle Instant Client or the
Microsoft ODBC Driver. It discovers them, is configured for them, and checks
them — `migrator doctor` is the verification step.

## 1. Components

| Component | Purpose | Required |
|---|---|---|
| libmysqlclient (MySQL 8.0 client) | MySQL source/target | only for MySQL directions |
| libpq (PostgreSQL client) | PostgreSQL / openGauss | only for PG/OG directions |
| unixODBC (libodbc2 + odbcinst) | ODBC driver management | only for MSSQL/Oracle |
| ODBC Driver 18 for SQL Server | MSSQL source/target | only for MSSQL |
| Oracle Instant Client (Basic + ODBC + Tools) | Oracle source/target | only for Oracle |
| bcp (mssql-tools18) | bulk load into MSSQL | optional, falls back to INSERT |
| sqlldr (Instant Client Tools) | bulk load into Oracle | optional, falls back to INSERT ALL |

## 2. Ubuntu / Debian

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build pkg-config \
  libmysqlclient-dev libpq-dev unixodbc unixodbc-dev libssl-dev

# SQL Server ODBC driver (Microsoft repository)
curl -fsSL https://packages.microsoft.com/keys/microsoft.asc | sudo gpg --dearmor -o /usr/share/keyrings/microsoft-prod.gpg
echo "deb [arch=amd64 signed-by=/usr/share/keyrings/microsoft-prod.gpg] https://packages.microsoft.com/ubuntu/24.04/prod noble main" | sudo tee /etc/apt/sources.list.d/mssql-release.list
sudo apt-get update && sudo ACCEPT_EULA=Y apt-get install -y msodbcsql18 mssql-tools18
```

Oracle Instant Client: accept the licence on
<https://www.oracle.com/database/technologies/instant-client.html>, download the
`basic`, `odbc` and `tools` zips and unpack them into one directory. No
installation step is required.

## 3. RHEL / CentOS / openEuler

```bash
sudo dnf install -y gcc-c++ cmake ninja-build pkgconfig \
  mysql-devel postgresql-devel unixODBC unixODBC-devel openssl-devel
# msodbcsql18 and mssql-tools18 come from the Microsoft RHEL repository
```

## 4. macOS (development)

```bash
brew install mysql-client libpq unixodbc
brew install --cask oracle-instantclient   # or unpack the zip manually
```

## 5. Rootless user-level install (restricted hosts)

```bash
mkdir -p ~/msodbc && cd ~/msodbc
curl -fsSLO https://packages.microsoft.com/ubuntu/24.04/prod/pool/main/m/msodbcsql18/msodbcsql18_18.6.2.1-1_amd64.deb
dpkg-deb -x msodbcsql18_18.6.2.1-1_amd64.deb root
# driver: ~/msodbc/root/opt/microsoft/msodbcsql18/lib64/libmsodbcsql-18.6.so.2.1
```

Register it in `~/.odbc/odbcinst.ini` and export:

```bash
export ODBCSYSINI=$HOME/.odbc
export ODBCINI=$HOME/.odbc/odbc.ini
```

## 6. Tool configuration (config/providers.ini)

```ini
mssql_driver=ODBC Driver 18 for SQL Server
oracle_driver=Oracle 23 ODBC Driver
bcp_path=
sqlldr_path=
```

Precedence: environment (`MIG_MSSQL_DRIVER`, `MIG_ORACLE_DRIVER`, `MIG_BCP`,
`MIG_SQLLDR`) > `config/providers.ini` > auto-detection (`PATH`, `~/msodbc`,
`~/oracle-ic`).

## 7. Self-check

```bash
./build/migrator doctor
```

Sample output:

```
[OK  ] providers.ini          config/providers.ini
[OK  ] mssql driver (cfg)     ODBC Driver 18 for SQL Server
[OK  ] oracle driver (cfg)    Oracle 23 ODBC Driver
[OK  ] mysql client           8.0.46
[OK  ] libpq                  160015
[OK  ] odbc drivers           Oracle 23 ODBC Driver, ODBC Driver 18 for SQL Server
[OK  ] bcp                    .../bcp | BCP - Bulk Copy Program for Microsoft SQL Server.
[OK  ] sqlldr                 .../sqlldr | SQL*Loader: Release 23.26.2.0.0
13 checks: 13 ok, 0 warn, 0 fail
```

## 8. Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `Can't open lib 'FreeTDS'` | no `driver=` in the DSN, fell back to FreeTDS | add `driver=ODBC Driver 18 for SQL Server` or set providers.ini |
| `ORA-01017 invalid credential` | wrong password / service name | check `user/password/db=FREEPDB1` |
| `bcp: Unable to locate driver dynamic library` | missing `ODBCSYSINI` or driver not registered | `export ODBCSYSINI=~/.odbc` |
| `sqlldr: libclntsh.so.23.1: cannot open` | missing `LD_LIBRARY_PATH` | `export LD_LIBRARY_PATH=~/oracle-ic/instantclient_23_26` |
| `resume module not built` | binary lacks the build flag | fixed in CMake; rebuild older packages |
| `migration finished: 0 rows` with exit 0 | old build without row validation | upgrade to a build with `doctor` and strict checks |
