# mini-migrator 使用说明

> 面向国产数据库的异构数据迁移工具（MySQL → PostgreSQL / openGauss）

## 1. 能力总览

| 能力 | 说明 |
|---|---|
| 全量迁移 | MySQL 读 → PostgreSQL/openGauss 写，COPY 协议，实测 **107 万 rows/s** |
| 表内并行 | 单表整数主键按区间切分，多连接并发读写（大表单表也能吃满 `--workers`） |
| 表级策略 | `copy` / `rebuild` / `skip` |
| 自动建表 | 类型映射 + 主键 + NOT NULL |
| 二级索引迁移 | 唯一/普通索引，跨表重名自动加表名后缀 |
| BLOB 二进制 | 字节级一致（含 NUL） |
| 视图迁移 | MySQL CREATE VIEW → PostgreSQL CREATE OR REPLACE VIEW |
| 校验 | 行数 + checksum（FNV-1a，可检出单行篡改） |
| ANALYZE | 迁移后更新统计信息 |
| CDC 增量 | binlog ROW 事件，增/改/删实时同步 |
| CDC 轮转 | binlog 跨文件自动跟随 |
| CDC DDL | ALTER/CREATE/DROP 基础方言转换后执行 |
| 监控面板 | Web + Qt 桌面版 |

## 2. 构建

    # macOS / Linux（Makefile）
    make

    # 跨平台（CMake）
    cmake -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build -j

产物：build/migrator

## 3. 运行模式

| mode | 源端 | 目标端 | 可用性 |
|---|---|---|---|
| sim | 内置模拟 | 内置模拟 | 完整 |
| mysql2pg | MySQL | PostgreSQL | 完整（推荐） |
| mysql2og | MySQL | openGauss | 完整 |
| pg2mysql | PostgreSQL | MySQL | 完整（视图暂不支持） |
| pg2og | PostgreSQL | openGauss | 完整 |
| og2pg | openGauss | PostgreSQL | 完整 |
| og2mysql | openGauss | MySQL | 完整（视图暂不支持） |
| mysql2mysql | MySQL | MySQL | 完整 |
| mysql2opengauss | MySQL | openGauss | mysql2og 的别名 |

也可用 `--source-type` / `--target-type` 显式指定 `mysql|pg|og`，覆盖 mode。

## 4. 参数总表

| 参数 | 默认 | 作用 |
|---|---|---|
| --mode | sim | 运行模式 |
| --source | 空 | 源库 DSN |
| --target | 空 | 目标库 DSN |
| --rows | 200000 | 模拟总行数（仅 sim） |
| --tables | 4 | 模拟表数（仅 sim） |
| --batch | 1000 | 每批行数 |
| --commit | 10 | 每 N 批提交 |
| --source-type | 按 mode | 源类型：mysql/pg/og |
| --target-type | 按 mode | 目标类型：mysql/pg/og |
| --workers | 1 | 并行线程数（按表并行；大单表自动按主键区间并行） |
| --pk-min-rows | 1000000 | 触发表内主键区间并行的最小行数 |
| --no-pk-parallel | 关 | 关闭单表主键区间并行 |
| --log-every | 10 | 每 N 批记一条日志 |
| --out-dir | out | 日志导出目录 |
| --throttle | 0 | 每批休眠毫秒 |
| --port | 8123 | 面板端口 |
| --type-map | config/type_map.json | 类型映射 |
| --tables-config | config/tables.json | 表级策略 |
| --no-index | 关 | 不迁二级索引 |
| --no-analyze | 关 | 不执行 ANALYZE |
| --no-copy | 关 | 回退 INSERT 模式 |
| --no-checksum | 关 | 只比行数 |
| --no-verify | 关 | 跳过校验 |
| --verify-only | 关 | 只校验不迁移 |
| --with-views | 关 | 迁移视图 |
| --cdc | 关 | 迁移后持续增量 |
| --cdc-only | 关 | 只跑增量 |
| --cdc-ddl | 关 | 同步 DDL |
| --cdc-start | 空 | 起始位点 file:pos |
| --cdc-server-id | 987654321 | 复制客户端 server_id |
| --cdc-max-events | 0 | 应用 N 个事件后退出（0=不限） |
| --exit | 关 | 迁移完退出 |

## 5. 典型用法

    # 模拟（无需数据库）
    ./build/migrator --mode sim --rows 600000 --tables 6 --throttle 200

    # 全量迁移
    ./build/migrator --mode mysql2pg \
      --source "host=127.0.0.1;port=3306;user=root;db=demo" \
      --target "host=127.0.0.1;port=5432;user=postgres;db=demo_dst" \
      --batch 2000 --workers 4 --with-views

    # 全量 + 持续增量 + DDL
    ./build/migrator --mode mysql2pg ... --cdc --cdc-ddl

    # 只跑增量
    ./build/migrator --mode mysql2pg ... --cdc-only --cdc-ddl

    # 事后核查
    ./build/migrator --mode mysql2pg ... --verify-only

## 6. 表级策略（config/tables.json）

    {
      "tables": {
        "daily_stats": "rebuild:sql/daily_stats.sql",
        "tmp_cache": "skip"
      }
    }

| 策略 | 行为 |
|---|---|
| copy（默认） | 建表 + 迁数据 + 建索引 |
| rebuild | 不迁数据，迁移后执行指定 SQL 重建 |
| skip | 完全跳过 |

## 7. 类型映射（config/type_map*.json）

按方向自动选择：

| 方向 | 文件 |
|---|---|
| MySQL → PG/openGauss | config/type_map.json（默认） |
| MySQL → MySQL | config/type_map.mysql.json |
| PG/openGauss → PG/openGauss | config/type_map.pg.json |
| PG/openGauss → MySQL | config/type_map.pg2mysql.json |

`--type-map <path>` 可显式覆盖。

    {
      "global":  { "varchar": "varchar{args}", "datetime": "timestamp" },
      "columns": { "users.bio": "text" }
    }

两级：全局类型 + 列级覆盖；{args} 由源类型参数替换。

## 8. 索引迁移

读取 information_schema.STATISTICS，数据导入完成后批量 CREATE INDEX。
PostgreSQL 索引名在 schema 内唯一，跨表重名自动改为 <name>_<table>。

## 9. 视图迁移（--with-views）

抽取 SHOW CREATE VIEW，做基础转换：

- 剥离 ALGORITHM / DEFINER / SQL SECURITY
- 反引号 → 双引号
- IFNULL → COALESCE、NOW()/CURDATE()/LENGTH() 归一
- 单条失败用 SAVEPOINT 隔离，不影响其他对象

## 10. CDC 增量同步

基于 binlog（要求 log_bin=ON、binlog_format=ROW）。

事件流：GTID → QUERY → TABLE_MAP → WRITE/UPDATE/DELETE_ROWS → XID

应用方式：

- INSERT / UPDATE → INSERT ... ON CONFLICT (pk) DO UPDATE
- DELETE → DELETE WHERE pk = ...

支持列类型：TINY/SHORT/LONG/INT24/LONGLONG/YEAR/FLOAT/DOUBLE/NEWDECIMAL/
VARCHAR/VAR_STRING/STRING/BLOB/TEXT/JSON/DATE/DATETIME2/TIMESTAMP2。

位点保存在 out/cdc-position.txt，重启自动续传；自动跟随 binlog 轮转。

## 11. 校验

- 行数：源/目标逐表比对
- checksum：逐行规范化后 FNV-1a，聚合 sum + xor（顺序无关）
- 可检出“行数一致但内容被改”的情况

## 12. Web 面板 API

| 端点 | 内容 |
|---|---|
| / | 面板 |
| /api/status | 进度/吞吐/ETA/错误/校验 |
| /api/tables | 每表明细 |
| /api/series | 吞吐序列 |
| /api/events | 事件日志 |
| /api/health | 健康检查 |

## 13. Qt 桌面版

    cd qt && mkdir -p build && cd build
    /Users/bellgrell/Qt/6.11.1/macos/bin/qmake ../migrator_gui.pro
    make -j4

从项目根目录启动（需读取 config/ 与 web/）：

    ./qt/build/migrator_gui.app/Contents/MacOS/migrator_gui

## 14. 输出产物

| 产物 | 位置 |
|---|---|
| 迁移日志 | out/migration-YYYYmmdd-HHMMSS.log |
| CDC 位点 | out/cdc-position.txt |
| 面板 | http://127.0.0.1:<port>/ |

## 15. 已知限制

| 限制 | 说明 |
|---|---|
| CDC 需要主键 | 无主键表跳过 |
| 部分列类型不支持 | BIT / ENUM / SET / GEOMETRY |
| DDL 方言转换有限 | DATE_FORMAT/GROUP_CONCAT/LIMIT x,y/分区语法需人工改写 |
| DM8 未实现 | 保留接口 |
| openGauss 需 md5 认证 | 默认 sha256 不被原生 libpq 支持，见第 16 节 |
| 目标表需为空或已对齐 | 直接重跑会撞主键 |

## 16. openGauss 对接

> 已在 openGauss 6.0.0（Docker `enmotech/opengauss:latest`，aarch64）实测：
> 10 万行迁移 + checksum PASS，511 万行 11.33s（451k rows/s）+ 区间并行 PASS，视图迁移 PASS，
> CDC 增/删/改 + DDL 同步 PASS（5 事件、0 错误）。

模式：`--mode mysql2og`（MySQL → openGauss），`mysql2opengauss` 为别名；
`--mode opengauss` 仅目标端。

    ./build/migrator --mode mysql2og \
      --source "host=127.0.0.1;port=3306;user=root;db=demo" \
      --target "host=127.0.0.1;port=5432;user=gaussdb;password=...;db=og_dst" \
      --batch 2000 --workers 4

创建目标库时建议显式用 PG 兼容模式（A 模式会把 `''` 当 NULL）：

    CREATE DATABASE og_dst WITH ENCODING='UTF8' DBCOMPATIBILITY='PG';

认证注意事项：

- 原生 libpq 支持 md5 / scram；部分 openGauss 版本默认 `sha256`，会报
  `unsupported authentication method`。遇到时把迁移账号改为 md5（6.0.0 容器镜像
  实测可直接用密码连接，无需改）：

      ALTER SYSTEM SET password_encryption_type = 0;
      -- 重新设置一次密码，才会以 md5 存储
      ALTER USER migrator IDENTIFIED BY '...';
      -- pg_hba.conf: host all all 0.0.0.0/0 md5
      SELECT pg_reload_conf();

- 或者改用 openGauss 自带的 libpq 重新链接本工具（默认用系统 libpq）。

已做的兼容处理：

- 连接后执行 `SELECT version()`，日志会标明实际是 openGauss 还是 PostgreSQL；
- 会话固定 `client_encoding=UTF8`、`standard_conforming_strings=on`；
- checksum 校验的游标优先 `NO SCROLL`，失败自动降级；
- COPY / SAVEPOINT / ANALYZE / 索引迁移与 PostgreSQL 路径一致；
- 布尔值 COPY 输出改为 `true`/`false`，两边都兼容；
- openGauss 的 numeric 默认输出 `.17`（省略前导 0），连接时设
  `behavior_compat_options='display_leading_zero'`，checksum 才不会假阳性。

CDC 差异处理：

- openGauss 不支持 `INSERT ... ON CONFLICT`，增量更新改为同一事务内
  `DELETE` + `INSERT`（幂等）；
- binlog QUERY 事件尾部带 CRC32，解析时已扣除，否则 DDL 末尾会多出垃圾字符；
- DDL 执行后清空 schema 缓存，下个事件重新读列，避免新增列被静默丢弃。

已知限制：openGauss 需要在目标库开启 PG 兼容模式；JDBC 驱动/ssl 认证等
高级选项不在本工具范围内（只使用 libpq 基础能力）。
