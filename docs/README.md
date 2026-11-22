# Documentation

**English** | [中文](README.zh.md)

Entry point for the published documentation. Internal working notes, benchmark
raw data and session logs are kept out of the repository on purpose.

## User guides

| Document | What it covers |
|---|---|
| [usage.md](usage.md) | every CLI parameter, DSN syntax, common recipes ([中文](usage.zh.md)) |
| [dependencies.md](dependencies.md) | client libraries, ODBC drivers, bcp/sqlldr, installation ([中文](dependencies.zh.md)) |
| [../packaging/portable/INSTALL.md](../packaging/portable/INSTALL.md) | portable package: layout, requirements, troubleshooting ([中文](../packaging/portable/INSTALL.zh.md)) |

## Release artifacts

| Document | What it covers |
|---|---|
| [../release/README.md](../release/README.md) | where official binaries live, checksum policy |
| [../release/SHA256SUMS](../release/SHA256SUMS) | checksum of the v1.0.0 asset |

## Language entry points

| Document | Language |
|---|---|
| [../README.md](../README.md) | English |
| [../README.zh.md](../README.zh.md) | 中文 |
| [../README.ru.md](../README.ru.md) | Русский |

## Not published here

Benchmark runs, session notes and design drafts (`bench-*.md`, `data-test-*.md`,
`session-*.md`, `plan.md`, `refactor-plan.md`) stay in the working copy and are
excluded by `.gitignore`.
