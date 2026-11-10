# mini-migrator

**中文** | [English](README.md) | [Русский](README.ru.md)

一个 C++17 实现的异构数据迁移工具，支持 **MySQL、PostgreSQL、openGauss** 多方向全量迁移、行数与 checksum 校验，以及从 MySQL binlog 到 PostgreSQL/openGauss 的 CDC 增量同步。

内置 Web 面板支持 **中文 / English / Русский**，可在右上角切换。

## 支持方向

| 模式 | 源端 | 目标端 | 状态 |
|---|---|---|---|
| `mysql2pg` | MySQL | PostgreSQL | 全量迁移 + 校验 |
| `mysql2og` | MySQL | openGauss | 全量迁移 + 校验 |
| `pg2mysql` | PostgreSQL | MySQL | 全量迁移 + 校验 |
| `pg2og` | PostgreSQL | openGauss | 全量迁移 + 校验 |
| `og2pg` | openGauss | PostgreSQL | 全量迁移 + 校验 |
| `og2mysql` | openGauss | MySQL | 全量迁移 + 校验 |
| `mysql2mysql` | MySQL | MySQL | 全量迁移 + 校验 |

CDC 目前仅支持 MySQL 源（MySQL binlog）。

## 构建

依赖：C++17 编译器、libmysqlclient、libpq。

```bash
make
./build/migrator --help
```

macOS：

```bash
brew install mysql-client libpq
make
```

也支持 CMake：

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## 模块

可选模块默认编译进来，也可以在构建时关闭：

```bash
make WITH_PG_CDC=0 WITH_REPAIR=0
# 或者用 CMake：
cmake -B build -DMIG_ENABLE_PG_CDC=OFF -DMIG_ENABLE_REPAIR=OFF
cmake --build build -j
```

`./build/migrator --features` 会列出编译进来的模块：`pg_cdc`、`repair`、`metrics`、`snapshot`、`resume`。

核心二进制仍然只依赖 `libmysqlclient` 和 `libpq`。

最小构建可以直接 `make lite`（只保留核心），`make full` 恢复全部模块。已编译进来的模块还可以在运行时禁用，无需重新编译：`--disable-modules pg_cdc,repair,...`。Web 面板只显示模块状态；要在界面里切换需要任务控制面。

Web 面板会显示已编译的模块及状态；模块是编译期开关，修改需要重新编译。

## 快速开始

MySQL 到 PostgreSQL：

```bash
./build/migrator --mode mysql2pg \
  --source 'host=127.0.0.1;port=3306;user=root;db=demo' \
  --target 'host=127.0.0.1;port=5432;user=postgres;db=demo_dst' \
  --batch 2000 --workers 4 --exit
```

MySQL 到 openGauss：

```bash
# 先创建 PG 兼容模式的目标库
# CREATE DATABASE demo_dst WITH ENCODING='UTF8' DBCOMPATIBILITY='PG';

./build/migrator --mode mysql2og \
  --source 'host=127.0.0.1;port=3306;user=root;db=demo' \
  --target 'host=127.0.0.1;port=15432;user=gaussdb;db=demo_dst' \
  --batch 2000 --workers 4 --exit
```

PostgreSQL 到 MySQL：

```bash
# 目标 MySQL 需要开启 local_infile=ON
# SET GLOBAL local_infile=1;

./build/migrator --mode pg2mysql \
  --source 'host=127.0.0.1;port=5432;user=postgres;db=demo' \
  --target 'host=127.0.0.1;port=3306;user=root;db=demo_mysql' \
  --batch 2000 --workers 4 --exit
```

Web 面板默认在 `http://127.0.0.1:8123/` 启动。加 `--exit` 表示迁移后退出，否则进程会继续提供面板。

## 基本参数

| 参数 | 默认值 | 说明 |
|---|---|---|
| `--mode` | `sim` | 迁移模式（见上表） |
| `--source` | — | 源端 DSN |
| `--target` | — | 目标端 DSN |
| `--batch` | 1000 | 每批行数 |
| `--commit` | 10 | 每 N 批提交一次 |
| `--workers` | 1 | 并行线程数（表级并行与单表主键区间并行） |
| `--port` | 8123 | Web 面板端口 |
| `--no-verify` | 关 | 跳过行数与 checksum 校验 |
| `--no-checksum` | 关 | 只比较行数 |
| `--with-views` | 关 | 迁移视图（MySQL 到 PG/openGauss） |
| `--cdc` | 关 | 全量迁移后继续 CDC |
| `--cdc-only` | 关 | 只运行 CDC |
| `--cdc-ddl` | 关 | 应用 binlog 中的 DDL 变更 |
| `--exit` | 关 | 迁移后退出 |
| `--connect-timeout` | 10 | 连接/读写超时（秒） |
| `--connect-retries` | 3 | 连接失败重试次数 |
| `--truncate` | 关 | 迁移前 DROP 目标表，便于重跑 |
| `--sslmode` | prefer | PostgreSQL/openGauss sslmode |
| `--mysql-ssl-mode` | PREFERRED | MySQL SSL 模式：DISABLED/PREFERRED/REQUIRED |
| `--cdc-retries` | 10 | CDC 断线重连次数 |
| `--cdc-slot` | migrator_slot | PostgreSQL CDC 逻辑复制槽名 |
| `--repair` | 关 | 校验后按主键补/删差异 |
| `--only-tables` | — | 只迁移指定表（逗号分隔） |
| `--exclude-tables` | — | 排除指定表 |
| `--consistent-snapshot` | 关 | 一致性快照（MySQL 用 binlog 位点；PG/openGauss 用 REPEATABLE READ），自动 workers=1 |
| `--resume` | 关 | 按主键水位断点续传 |
| `--resume-dir` | out/resume | 断点文件目录 |

DSN 格式：`host=...;port=...;user=...;password=...;db=...`。

## 性能

测试数据集：三张表共 5,110,000 行（500 万行整数主键表、10 万行复合主键表、1 万行无主键表）。batch 2000，4 线程，开启行数与 checksum 校验。耗时包含校验。

| 方向 | 行数 | 耗时 | 吞吐 |
|---|---:|---:|---:|
| MySQL → openGauss | 7,110,000 | 15.43 s | 460,836 rows/s |
| MySQL → PostgreSQL | 5,110,000 | 12.42 s | 411,287 rows/s |
| MySQL → MySQL | 5,110,000 | 13.31 s | 384,006 rows/s |
| openGauss → PostgreSQL | 5,110,000 | 17.39 s | 293,923 rows/s |
| PostgreSQL → openGauss | 5,110,000 | 19.32 s | 264,441 rows/s |
| openGauss → MySQL | 5,110,000 | 23.57 s | 216,776 rows/s |
| PostgreSQL → MySQL | 5,110,000 | 27.52 s | 185,680 rows/s |

MySQL → openGauss 的源库还包含两张 100 万行的临时基准表，因此是 7,110,000 行。

单表主键区间并行（`--workers 4`）实测：PostgreSQL → MySQL 125 万行 6.23 s / 200,517 rows/s（顺序读取 9.11 s / 137,168 rows/s，提升约 46%）；openGauss → MySQL 500 万行 15.12 s / 330,621 rows/s。两项均 checksum PASS。

同一数据集下的典型表级结果：

| 表 | 行数 | 说明 |
|---|---:|---|
| `big` | 5,000,000 | 整数主键，区间并行 |
| `comp` | 100,000 | 复合主键 |
| `nopk` | 10,000 | 无主键 |
| `users` | 100,000 | 二级索引、文本和 decimal 列 |

## 限制

- CDC 需要主键；无主键表会被跳过。
- CDC 支持 MySQL 源（binlog ROW 事件）和 PostgreSQL 源（wal2json 逻辑解码）；PG CDC 目标必须是 PG/openGauss，不支持 MySQL。
- PostgreSQL CDC 需要 `wal_level=logical`、在 `output_plugin_libraries` 中允许 `wal2json`，并创建复制槽；全量迁移期间的变化只有槽提前存在才能捕获。
- `--repair` 需要单列主键，会在内存中保存主键集合，补缺失行、删多余行，然后重新校验。
- 不支持的列类型：BIT、ENUM、SET、GEOMETRY。
- DDL 方言转换有限；复杂的 MySQL 专用 DDL 可能需要手工改写。
- DM8 尚未实现。
- PostgreSQL → MySQL 的视图迁移尚未实现。
- 目标表应为空或已对齐；可用 `--truncate` 先 DROP 再重建，便于安全重跑。
- checksum 读取失败会让校验直接 FAIL，不再静默跳过。
- 全量连接失败会按退避重试；CDC 断线会从最后提交的 binlog 位点重连。
- 远程数据库需要放行端口、在账号/防火墙中允许客户端 IP；必要时用 SSH 隧道或 VPN。
- `--resume` 已支持 MySQL、PostgreSQL、openGauss 源，需要单列主键。
- `--consistent-snapshot` 使用单条源连接（MySQL 用一致性快照 + binlog 位点；PG/openGauss 用 REPEATABLE READ），并强制 `workers=1`。
- `--resume` 与 `--truncate` 互斥。
- PostgreSQL/openGauss 源已支持单表主键区间并行（`--workers`）；`--resume` 与 `--consistent-snapshot` 仍使用单游标顺序读取。
- MySQL 目标批量导入使用 `LOAD DATA LOCAL INFILE`，服务端需要开启 `local_infile=ON`；含二进制列的表会自动回退为批量 INSERT。
- openGauss 目标库应使用 `DBCOMPATIBILITY='PG'` 创建；如果账号使用 sha256 认证，原生 libpq 可能需要把迁移账号切换为 md5。

## Web 面板

面板展示进度、吞吐、表级状态、校验结果和事件流，支持三种界面语言：

- 中文
- English
- Русский

语言选择会保存在浏览器 localStorage 中。

## 指标

Prometheus 端点：`http://127.0.0.1:8123/metrics`。

暴露的指标包括 `migrator_rows_done`、`migrator_rows_total`、`migrator_errors_total`、`migrator_batches_total`、`migrator_bytes_written_total`、`migrator_verify_mismatch`、`migrator_running`、`migrator_verified`，以及按表的 `migrator_table_rows_done{table="..."}`。

## 许可证

Apache License 2.0，见 [LICENSE](LICENSE)。
