# mini-migrator

[English](README.md) | [中文](README.zh.md) | **Русский**

Нативный инструмент на C++17 для переноса данных между **MySQL, PostgreSQL,
openGauss, SQL Server и Oracle**: полная загрузка, проверка, докачка и CDC.
Один бинарный файл — без Docker, без Python, без внешних сервисов.

```bash
./run.sh --mode mysql2pg --source "db=app;host=10.0.0.5;user=reader;password=env:SRC_PW" \
                             --target "host=10.0.0.9;user=postgres;db=app_dst;password=env:DST_PW" \
         --batch 5000 --commit 10 --workers 4
```

## Возможности

- **13 штатных режимов** для пяти СУБД; остальные пары — через `--source-type` / `--target-type`
- **Проверка по числу строк и контрольной сумме** (FNV или mmv, где поддерживается)
- **Параллелизм по диапазонам первичного ключа**: большая таблица делится по PK
- **Адаптивная загрузка в MySQL**: `LOAD DATA` для узких строк, многострочный `INSERT` для широких
- **Докачка** с водяного знака PK, `--verify-async`
- **CDC** из binlog MySQL и логической репликации PostgreSQL
- **Встроенная веб-панель** (китайский / английский / русский), аудит и TLS
- **`doctor`** — самопроверка клиентских библиотек, ODBC-драйверов и загрузчиков
- **Без Docker**: только клиентские библиотеки

## Направления

| Режим | Источник | Приёмник |
|---|---|---|
| `mysql2pg` | MySQL | PostgreSQL |
| `mysql2og` | MySQL | openGauss |
| `pg2mysql` | PostgreSQL | MySQL |
| `pg2og` | PostgreSQL | openGauss |
| `og2pg` | openGauss | PostgreSQL |
| `og2mysql` | openGauss | MySQL |
| `mysql2mysql` | MySQL | MySQL |
| `mysql2mssql` | MySQL | SQL Server |
| `mssql2mysql` | SQL Server | MySQL |
| `mssql2mssql` | SQL Server | SQL Server |
| `mysql2oracle` | MySQL | Oracle |
| `oracle2mysql` | Oracle | MySQL |
| `oracle2oracle` | Oracle | Oracle |

## Установка

### Портативный пакет (без Docker)

```bash
tar -xzf migrator-1.0.0-linux-x64.tar.gz
cd migrator-1.0.0-linux-x64
./run.sh doctor
```

Собрать самостоятельно: `bash packaging/build-portable.sh 1.0.0`

Требования: Linux x86-64, glibc 2.39+, сетевой доступ к СУБД. ODBC-драйверы нужны
только для SQL Server (`msodbcsql18`) и Oracle (Instant Client + ODBC).

### Из исходников

```bash
sudo apt-get install -y build-essential cmake ninja-build pkg-config \
  libmysqlclient-dev libpq-dev unixodbc-dev libssl-dev
make -j4
```

## DSN

```
host=...;port=...;user=...;password=...;db=...
```

Поддерживаются `password=env:VAR` и `password=file:PATH`. Если указан `driver=`,
ключ `password=` должен идти **последним**.

## Ключевые параметры

| Параметр | По умолчанию | Описание |
|---|---|---|
| `--batch` | 1000 | строк в пакете |
| `--commit` | 10 | коммит каждые N пакетов |
| `--workers` | 1 | параллельные потоки / диапазоны PK |
| `--truncate` | выкл | очистить целевые таблицы |
| `--load-mode` | `auto` | режим загрузки в MySQL |
| `--verify-async` | выкл | проверка после загрузки |
| `--resume` | выкл | докачка по PK |
| `--cdc` | выкл | непрерывная синхронизация |
| `--tables-config` | — | политика по таблицам |

## Производительность

Для цели MySQL важнее всего настройки сервера: `innodb_buffer_pool_size` по
рабочему набору, `innodb_doublewrite=OFF` (только стенд), `innodb_flush_method=O_DIRECT`,
`innodb_flush_neighbors=0`, `innodb_flush_log_at_trx_commit=2`, отключённый binlog.

Измерено на стенде (20 млн строк): mysql→pg 176 MB/s, pg→pg 150 MB/s,
mssql→pg 187 MB/s, mysql→mssql 25 MB/s, любое→oracle 3,5–4,3 MB/s.
Oracle-приёмник загружается построчно и является узким местом.

## Проверка и CDC

```bash
./build/migrator --mode mysql2pg ... --verify-engine fnv
./build/migrator --mode mysql2pg ... --cdc --cdc-drop-slot
```

## Частые проблемы

| Симптом | Решение |
|---|---|
| `Lost connection to MySQL server during query` | увеличить `net_read_timeout`/`net_write_timeout` |
| `Can't open lib 'ODBC Driver 18 for SQL Server'` | установить `msodbcsql18` и зарегистрировать в `odbcinst.ini` |
| `ORA-12541` | проверить порт и listener |
| `ORA-01017` | пароль со спецсимволами — использовать `password=file:PATH` |
| `ORA-12954` | PDB достиг `MAXSIZE`, пересоздать с `STORAGE (MAXSIZE 64G)` |

## Лицензия

См. [LICENSE](LICENSE).
