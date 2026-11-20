# mini-migrator

[English](README.md) | [中文](README.zh.md) | **Русский**

Инструмент гетерогенной миграции баз данных на C++17 для **MySQL, PostgreSQL,
openGauss, SQL Server и Oracle**. Поддерживает полную миграцию в нескольких
направлениях, сверку количества строк и контрольных сумм, CDC; SQL Server и
Oracle подключаются через ODBC и могут использовать штатные загрузчики
(`bcp`, `sqlldr`).

## Возможности

- 13 штатных направлений миграции по пяти СУБД; остальные пары через `--source-type` / `--target-type`
- Сверка количества строк и контрольных сумм (FNV или дайджест mmv)
- Массовая загрузка: `bcp` для SQL Server, `sqlldr` для Oracle (при отсутствии - обычные INSERT)
- Двухэтапная загрузка таблиц Oracle с `BLOB` (промежуточная таблица + `TO_BLOB`)
- CDC из binlog MySQL и логической репликации PostgreSQL, опциональная синхронизация DDL
- Докачка по водяному знаку первичного ключа, параллелизм по диапазонам ключа
- Веб-панель (китайский / английский / русский), журнал аудита, TLS
- Самопроверка `doctor`: клиентские библиотеки, драйверы ODBC, загрузчики
- Пакет Debian и самодостаточный Docker-образ

## Поддерживаемые направления

| Режим | Источник | Приёмник | Примечание |
|---|---|---|---|
| `mysql2pg` | MySQL | PostgreSQL | полная загрузка + сверка |
| `mysql2og` | MySQL | openGauss | полная загрузка + сверка |
| `pg2mysql` | PostgreSQL | MySQL | полная загрузка + сверка |
| `pg2og` | PostgreSQL | openGauss | полная загрузка + сверка |
| `og2pg` | openGauss | PostgreSQL | полная загрузка + сверка |
| `og2mysql` | openGauss | MySQL | полная загрузка + сверка |
| `mysql2mysql` | MySQL | MySQL | полная загрузка + сверка |
| `mysql2mssql` | MySQL | SQL Server | канал `bcp` |
| `mssql2mysql` | SQL Server | MySQL | полная загрузка |
| `mssql2mssql` | SQL Server | SQL Server | канал `bcp` |
| `mysql2oracle` | MySQL | Oracle | канал `sqlldr` |
| `oracle2mysql` | Oracle | MySQL | полная загрузка |
| `oracle2oracle` | Oracle | Oracle | канал `sqlldr` |

Другие пары задаются переопределением типов, например:

```bash
./build/migrator --mode mysql2pg --source-type oracle --target-type pg ...
```

## Быстрый старт

### Сборка

```bash
sudo apt-get install -y build-essential cmake ninja-build pkg-config \
  libmysqlclient-dev libpq-dev unixodbc-dev libssl-dev
make -j4            # или: cmake -S . -B build -G Ninja && cmake --build build -j
```

macOS:

```bash
brew install mysql-client libpq unixodbc openssl@3
make -j4
```

### Проверка окружения

```bash
./build/migrator doctor
```

`doctor` проверяет клиентские библиотеки, зарегистрированные драйверы ODBC,
`bcp` и `sqlldr`; ненулевой код возврата только при серьёзных ошибках. Имена
драйверов и пути задаются в `config/providers.ini` (или `MIG_MSSQL_DRIVER`,
`MIG_ORACLE_DRIVER`, `MIG_BCP`, `MIG_SQLLDR`). Подробнее:
[docs/dependencies.md](docs/dependencies.md).

### Запуск миграции

```bash
./build/migrator --mode mysql2pg \
  --source "host=127.0.0.1;port=3306;user=root;password=...;db=demo" \
  --target "host=127.0.0.1;port=5432;user=postgres;password=...;db=demo_dst" \
  --batch 5000 --workers 4 --truncate --exit
```

Для SQL Server драйвер указывается в DSN:

```bash
--target "host=127.0.0.1;port=1433;user=sa;password=...;db=demo_dst;driver=ODBC Driver 18 for SQL Server"
```

## Каналы массовой загрузки

Если доступны `bcp` или `sqlldr`, пакет целиком передаётся штатному загрузчику;
иначе выполняется обычная вставка SQL.

Замер на 5,05 млн строк / 7 таблиц (Ubuntu, базы в Docker):

| Приёмник | Канал | Время | Пропускная способность |
|---|---|---|---|
| SQL Server | `bcp` | 35,8 с | 8,8 МБ/с |
| SQL Server | INSERT | 233 с | 1,3 МБ/с |
| Oracle | `sqlldr` | 133,7 с | 2,4 МБ/с |
| Oracle | INSERT ALL | 1285 с | 0,2 МБ/с |

Колонки `BLOB` в Oracle загружаются в два этапа: шестнадцатеричный текст во
промежуточную таблицу, затем `TO_BLOB(HEXTORAW(...))` в целевую таблицу.

## Сборка пакетов

```bash
# Пакет Debian/Ubuntu
bash packaging/build-deb.sh
sudo dpkg -i mini-migrator_0.9.0_amd64.deb

# Самодостаточный образ (msodbcsql18 + Instant Client + bcp/sqlldr)
docker build -t mini-migrator:full -f packaging/Dockerfile .
docker run --rm mini-migrator:full doctor
```

Пакет устанавливает `/usr/bin/mini-migrator` и
`/etc/mini-migrator/providers.ini`.

## Основные параметры

| Параметр | По умолчанию | Описание |
|---|---|---|
| `--mode` | `sim` | режим миграции (таблица выше) |
| `--source` / `--target` | - | DSN: `host=;port=;user=;password=;db=` |
| `--batch` | 1000 | строк в пакете |
| `--commit` | 10 | коммит каждые N пакетов |
| `--workers` | 1 | число потоков и параллелизм по диапазонам ключа |
| `--truncate` | выкл | очистить целевые таблицы перед загрузкой |
| `--no-verify` | выкл | пропустить сверку строк и контрольных сумм |
| `--resume` / `--resume-dir` | выкл | докачка по водяному знаку ключа |
| `--cdc` | выкл | применять изменения после полной загрузки |
| `--with-views` | выкл | переносить представления (где поддерживается) |
| `--port` | 8123 | порт веб-панели |

## Документация

- [docs/usage.md](docs/usage.md) - полный справочник параметров
- [docs/dependencies.md](docs/dependencies.md) - драйверы, загрузчики, устранение неполадок
- `migrator doctor` - проверка окружения

## Ограничения

- Таблицы Oracle с `BLOB` идут по двухэтапному пути и медленнее обычных
- `bcp` / `sqlldr` получают пароль через командную строку или временный файл;
  ограничьте доступ к хосту
- CDC требует binlog (MySQL) или логической репликации (PostgreSQL)
- Веб-панель только для чтения; модули переключаются при сборке

## Лицензия

См. [LICENSE](LICENSE).
