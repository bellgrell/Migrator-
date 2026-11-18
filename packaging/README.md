# Packaging and operations

This directory contains service definitions and install notes. The binary
itself is built with `make` or CMake; packaging does not vendor third-party
libraries.

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

The binary reads `config/` and `web/` relative to the working directory, so
run it from a directory that contains those paths (or copy them next to the
service working directory).

## Dashboard token

Never pass the token on the command line in production; generate a file:

```bash
sudo install -d -m 0750 /etc/mini-migrator
openssl rand -hex 32 | sudo tee /etc/mini-migrator/dashboard.token >/dev/null
sudo chmod 0600 /etc/mini-migrator/dashboard.token
```

Then start with `--http-token-file /etc/mini-migrator/dashboard.token`.
The dashboard prompts for the token once per browser session.

## TLS

The server refuses a non-loopback bind without TLS unless
`--http-allow-insecure` is passed explicitly. For a private CA:

```bash
openssl req -x509 -newkey rsa:3072 -nodes -days 365 \
  -keyout /etc/mini-migrator/tls.key \
  -out /etc/mini-migrator/tls.crt \
  -subj "/CN=migrator.internal" \
  -addext "subjectAltName=DNS:migrator.internal,IP:10.0.0.10"
```

Requires a build with OpenSSL (`http_tls_available()` and `--version` output
report it; `/api/security` reports `tls_available`).

## systemd

```bash
sudo install -d -m 0750 /var/lib/migrator /var/log/mini-migrator
sudo useradd --system --home /var/lib/migrator --shell /usr/sbin/nologin migrator
sudo install -m 0644 packaging/migrator.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now migrator
journalctl -u migrator -f
```

The unit sets `ProtectSystem=strict`, `NoNewPrivileges`, a private `/tmp`,
and only allows writes under `/var/lib/migrator` and `/var/log/mini-migrator`.

## launchd (macOS)

```bash
sudo install -d -m 0755 /usr/local/etc/mini-migrator /usr/local/var/log/mini-migrator
sudo install -m 0644 packaging/com.bellgrell.migrator.plist /Library/LaunchDaemons/
sudo launchctl load -w /Library/LaunchDaemons/com.bellgrell.migrator.plist
```

## Docker

```bash
docker build -f packaging/Dockerfile -t mini-migrator:0.9.0 .
docker run --rm -p 8123:8123 \
  -v "$PWD/secrets/dashboard_token:/run/secrets/dashboard_token:ro" \
  -v "$PWD/secrets/tls:/run/tls:ro" \
  -v migrator-state:/var/lib/migrator \
  mini-migrator:0.9.0
```

## Release tarball

```bash
make dist            # dist/mini-migrator-<version>.tar.gz
tar tzf dist/mini-migrator-0.9.0.tar.gz | head
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
3. Install the new binary with `make install`.
4. Run `migrator --prove-type-rules` and `--check-type-rules` before start.
5. Start the unit and check `/api/health` plus the audit log.
