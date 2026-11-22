# mini-migrator

[English](README.md) | [中文](README.zh.md) | **Русский**

[![Release](https://img.shields.io/github/v/release/bellgrell/Migrator-?display_name=tag&sort=semver&label=release)](https://github.com/bellgrell/Migrator-/releases/latest) [![Platform](https://img.shields.io/badge/platform-linux%20x86--64-blue)](https://github.com/bellgrell/Migrator-/releases/latest) [![License](https://img.shields.io/github/license/bellgrell/Migrator-)](LICENSE)

> Один бинарный файл, который переносит данные из базы A в базу B. Без Docker, без Python, без промежуточного ПО.

mini-migrator — инструмент на C++17 для миграции между **MySQL, PostgreSQL,
openGauss, SQL Server и Oracle**: 13 штатных направлений, проверка по числу строк
и контрольной сумме, параллелизм по диапазонам первичного ключа, докачка,
CDC и встроенная панель с метриками.

## Зачем он нужен

Обычно выбор невелик: построчный скрипт, который медленно убивает приёмник, или
вендорный продукт, знающий две СУБД. Этот инструмент делает одно: **переносит
схему и данные так быстро, как позволяет приёмник, и честно отчитывается** —
сколько строк, прошла ли проверка, какой коэффициент записи, какая таблица упала.

## Возможности

- **13 штатных направлений**; остальные пары — через `--source-type` / `--target-type`
- **Параллелизм по диапазонам PK**: большая таблица делится по ключу и читается N потоками
- **Адаптивная загрузка в MySQL**: `LOAD DATA` для узких строк, многострочный `INSERT` шире 512 Б
- **Проверка** по числу строк и контрольной сумме (FNV / mmv)
- **Докачка** с водяного знака PK, `--verify-async`
- **CDC** из binlog MySQL и логической репликации PostgreSQL
- **Метрики**: MB/s, строк/с, коэффициенты чтения и записи, пиковый RSS, память приёмника
- **Один бинарник**: релиз — портативный пакет без Docker

## Быстрый старт

```bash
shasum -a 256 -c SHA256SUMS
tar -xzf migrator-1.0.0-linux-x64.tar.gz
cd migrator-1.0.0-linux-x64
./run.sh doctor

./run.sh --mode mysql2pg \
  --source "db=app;host=10.0.0.5;port=3306;user=reader;password=***" \
  --target "host=10.0.0.9;port=5432;user=postgres;db=app_dst;password=***" \
  --batch 5000 --commit 10 --workers 4 --truncate --verify-async
```

Пароль можно задать как `password=env:VAR` или `password=file:PATH`. Если в DSN
есть `driver=`, ключ `password=` должен идти **последним**.

## Направления

| Режим | Источник | Приёмник |
|---|---|---|
| `mysql2pg` / `mysql2og` / `mysql2mysql` | MySQL | PG / openGauss / MySQL |
| `pg2mysql` / `pg2og` | PostgreSQL | MySQL / openGauss |
| `og2pg` / `og2mysql` | openGauss | PostgreSQL / MySQL |
| `mysql2mssql` / `mssql2mysql` / `mssql2mssql` | MySQL / SQL Server | SQL Server / MySQL |
| `mysql2oracle` / `oracle2mysql` / `oracle2oracle` | MySQL / Oracle | Oracle / MySQL |

## Производительность

Измерено на стенде (62 ГБ RAM, NVMe, 20 млн строк):

| Направление | Пропускная способность |
|---|---|
| mysql → pg | 176 MB/s |
| pg → pg | 150 MB/s |
| mssql → pg | 187 MB/s |
| mysql → mssql | 25 MB/s |
| любое → oracle | ~8 MB/s (после NOLOGGING и array binding) |

Для приёмника MySQL критичны `innodb_buffer_pool_size` по рабочему набору,
`innodb_doublewrite=OFF` (стенд), `innodb_flush_method=O_DIRECT`,
`innodb_flush_neighbors=0`, `innodb_flush_log_at_trx_commit=2` и отключённый binlog.

## Документация

| Документ | Содержание |
|---|---|
| [docs/usage.md](docs/usage.md) | параметры и рецепты |
| [docs/dependencies.md](docs/dependencies.md) | драйверы и загрузчики |
| [docs/operations.md](docs/operations.md) | панель, логи, аудит |
| [docs/adapters-mssql-oracle.md](docs/adapters-mssql-oracle.md) | особенности SQL Server / Oracle |
| [INSTALL.md](packaging/portable/INSTALL.md) | портативный пакет |
| [docs/README.md](docs/README.md) | индекс документации |

## Частые проблемы

| Симптом | Решение |
|---|---|
| `Lost connection to MySQL server during query` | увеличить `net_read_timeout`/`net_write_timeout` |
| `Can't open lib 'ODBC Driver 18 for SQL Server'` | установить `msodbcsql18` и зарегистрировать |
| `ORA-12541` | проверить порт и listener |
| `ORA-01017` | спецсимволы в пароле — `password=file:PATH` |
| `ORA-12954` | PDB достиг `MAXSIZE`, пересоздать с `STORAGE (MAXSIZE 64G)` |

## Участие

Issues: приложите версии СУБД, вывод `doctor` и строки лога с ошибкой.
Сборка пакета: `bash packaging/build-portable.sh 1.0.0`.

## Лицензия

[LICENSE](LICENSE)

## Ссылки

- Репозиторий: https://github.com/bellgrell/Migrator-
- Issues: https://github.com/bellgrell/Migrator-/issues
