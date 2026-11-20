# Packaging and operations

This directory holds the packaging scripts, service definitions and install notes.
The binary is built with `make` or CMake; third-party client libraries are not
vendored - see [../docs/dependencies.md](../docs/dependencies.md).

## Install from source

```bash
make -j
sudo make install PREFIX=/usr/local
```

Installed layout:

| Path | Contents |
|---|---|
| `/usr/local/bin/migrator` | binary |
| `/usr/local/etc/mini-migrator/` | config JSON + `migrator.ini` |
| `/usr/local/share/mini-migrator/web/` | dashboard `index.html` |

The binary reads `config/` and `web/` relative to the working directory, so run it
from a directory that contains those paths (or copy them next to the service
working directory).

## Debian package

```bash
bash packaging/build-deb.sh
sudo dpkg -i mini-migrator_0.9.0_amd64.deb
```

The script assembles `packaging/debian/` from `build/migrator`,
`config/providers.ini` and `docs/dependencies.md`, then calls `dpkg-deb`. The
resulting `*.deb` and the generated tree are build artifacts and are ignored by
git.

Installed layout:

| Path | Contents |
|---|---|
| `/usr/bin/mini-migrator` | binary |
| `/etc/mini-migrator/providers.ini` | driver names and bulk-loader paths |
| `/usr/share/doc/mini-migrator/dependencies.md` | dependency and troubleshooting guide |

Declared dependencies (Ubuntu 24.04):

```
libmysqlclient21, libpq5, libodbc2, libodbcinst2, unixodbc-common, libssl3t64
```

Vendor ODBC drivers (`msodbcsql18`, Oracle Instant Client) are intentionally not
declared: they are optional per target and Oracle does not allow redistribution.
Install them per [../docs/dependencies.md](../docs/dependencies.md).

Verify after install:

```bash
mini-migrator doctor
```

`providers.ini` is resolved in this order: `MIG_PROVIDERS` -> `config/providers.ini`
-> `/etc/mini-migrator/providers.ini`.

## Docker image

```bash
docker build -t mini-migrator:full -f packaging/Dockerfile .
docker run --rm mini-migrator:full doctor
```

The image is multi-stage: the builder compiles the binary, the runtime image
ships `libmysqlclient21`, `libpq5`, unixODBC, `msodbcsql18`, `mssql-tools18`
(`bcp`) and Oracle Instant Client Basic/ODBC/Tools (`sqlldr`).

Environment baked into the image:

| Variable | Value |
|---|---|
| `MIG_PROVIDERS` | `/etc/mini-migrator/providers.ini` |
| `MIG_BCP` | `/opt/mssql-tools18/bin/bcp` |
| `MIG_SQLLDR` | `/opt/oracle/instantclient/sqlldr` |
| `LD_LIBRARY_PATH` | `/opt/oracle/instantclient` |
| `ODBCSYSINI` | `/etc` |

Run a migration:

```bash
docker run --rm -p 8123:8123 \
  -e MIG_PROVIDERS=/etc/mini-migrator/providers.ini \
  mini-migrator:full --mode mysql2pg \
  --source "host=10.0.0.5;port=3306;user=root;password=env:MYSQL_PW" \
  --target "host=10.0.0.6;port=5432;user=postgres;password=env:PG_PW" \
  --batch 5000 --workers 4 --exit
```

Mount a dashboard token and TLS material when binding a non-loopback address:

```bash
docker run --rm -p 8123:8123 \
  -v "$PWD/secrets/dashboard_token:/run/secrets/dashboard_token:ro" \
  -v "$PWD/secrets/tls:/run/tls:ro" \
  -v migrator-state:/var/lib/migrator \
  mini-migrator:full --http-bind 0.0.0.0 \
  --http-token-file /run/secrets/dashboard_token \
  --tls-cert /run/tls/tls.crt --tls-key /run/tls/tls.key
```

## Release tarball

```bash
make dist            # dist/mini-migrator-<version>.tar.gz
tar tzf dist/mini-migrator-0.9.0.tar.gz | head
```

## Dashboard token

Never pass the token on the command line in production; generate a file:

```bash
sudo install -d -m 0750 /etc/mini-migrator
openssl rand -hex 32 | sudo tee /etc/mini-migrator/dashboard.token >/dev/null
sudo chmod 0600 /etc/mini-migrator/dashboard.token
```

Then start with `--http-token-file /etc/mini-migrator/dashboard.token`. The
dashboard prompts for the token once per browser session.

## TLS

The server refuses a non-loopback bind without TLS unless `--http-allow-insecure`
is passed explicitly. For a private CA:

```bash
openssl req -x509 -newkey rsa:3072 -nodes -days 365 \
  -keyout /etc/mini-migrator/tls.key \
  -out /etc/mini-migrator/tls.crt \
  -subj "/CN=migrator.internal" \
  -addext "subjectAltName=DNS:migrator.internal,IP:10.0.0.10"
```

Requires a build with OpenSSL (`http_tls_available()` and `--version` output report
it; `/api/security` reports `tls_available`).

## systemd

```bash
sudo install -d -m 0750 /var/lib/migrator /var/log/mini-migrator
sudo useradd --system --home /var/lib/migrator --shell /usr/sbin/nologin migrator
sudo install -m 0644 packaging/migrator.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now migrator
journalctl -u migrator -f
```

The unit sets `ProtectSystem=strict`, `NoNewPrivileges`, a private `/tmp`, and
only allows writes under `/var/lib/migrator` and `/var/log/mini-migrator`.

## launchd (macOS)

```bash
sudo install -d -m 0755 /usr/local/etc/mini-migrator /usr/local/var/log/mini-migrator
sudo install -m 0644 packaging/com.bellgrell.migrator.plist /Library/LaunchDaemons/
sudo launchctl load -w /Library/LaunchDaemons/com.bellgrell.migrator.plist
```

## Health and monitoring

* `GET /api/health` is unauthenticated and returns `{"ok":true}`.
* `GET /metrics` requires the bearer token; scrape with
  `authorization: Bearer <token>` in the Prometheus config.
* `GET /api/security` reports bind, TLS, auth and audit state.
* `GET /api/audit/tail?n=50` returns the last audit lines (token required).

## Backup and upgrade

1. Stop the unit (or let the instance lock hand over).
2. Back up `/var/lib/migrator` (resume/CDC positions, audit, locks).
3. Install the new binary with `make install`, `dpkg -i` or a new image tag.
4. Run `mini-migrator doctor` and `--prove-type-rules` before start.
5. Start the unit and check `/api/health` plus the audit log.
