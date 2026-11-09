# 项目交接文档（PROJECT-STATE）

> 目的：上下文压缩或新会话时，读这一份就能接着干。

## 1. 项目是什么

C++17 写的**异构数据迁移工具**：从 MySQL 全量迁移到 PostgreSQL/openGauss，
并支持基于 binlog 的实时增量同步（CDC）。

## 2. 目录结构

    src/
      migrator.h         核心类型 / 接口 / Metrics / Options
      core.cpp           错误码、值序列化、类型映射、DDL 生成
      metrics.cpp        指标 + JSON 输出
      pipeline.cpp       全量迁移流水线 + 校验（含 checksum）
      policy.cpp         表级策略加载 + 索引 DDL
      views.cpp          视图方言转换
      checksum.cpp       FNV-1a 校验和
      cdc.cpp            binlog 解析 + 增量应用（最复杂）
      cdc.h
      backend_mysql.cpp  MySQL 源（libmysqlclient）
      backend_mysql_target.cpp  MySQL 目标（INSERT 批量写）
      backend_pg.cpp     PostgreSQL 目标（libpq，COPY/INSERT）
      backend_pg_source.cpp  PostgreSQL 源（校验用，服务端游标）
      backend_sim.cpp    模拟后端
      backends.cpp       工厂
      http.cpp           内置 HTTP 服务（POSIX / Winsock）
      main.cpp           CLI
    qt/                  Qt 6 桌面版（qmake 工程）
    web/index.html       监控面板（单文件，无 CDN）
    config/              type_map.json / tables.json / migrator.ini
    docs/                usage.md / plan.md / file-manifest.md
    bench/stress.sh      压测脚本

## 3. 构建与运行

    cd ~/Documents/mini-migrator
    make
    ./build/migrator --help

    ./build/migrator --mode mysql2pg \
      --source 'host=127.0.0.1;port=3306;user=root;db=demo' \
      --target 'host=127.0.0.1;port=5432;user=bellgrell;db=demo_dst' \
      --batch 2000 --workers 4 --with-views

    ./build/migrator ... --cdc --cdc-ddl
    ./build/migrator ... --cdc-only --cdc-ddl

    # MySQL -> openGauss
    ./build/migrator --mode mysql2og --source '...' --target '...' \
      --batch 2000 --workers 4
    bench/opengauss-smoke.sh   # 端到端冒烟（需 OG_DSN 等环境变量）

## 4. 实测数据

| 场景 | 结果 |
|---|---|
| 1000 万行 / 8 表 / 4 线程 / COPY | 9.32s → **1,072,691 rows/s**，verify PASS |
| 同数据 INSERT 模式 | 18.84s → 530,747 rows/s |
| 单表 10 万行 | 0.42s → 240k rows/s |
| 单表 500 万行（顺序） | 迁移 4.88s → 1.05M rows/s，verify PASS |
| 单表 500 万行（主键 8 区间） | 迁移 4.06s → **1.38M rows/s**，verify PASS |
| openGauss 6.0.0：demo 库 | 100,010 行 / 0.53s，checksum PASS |
| openGauss 6.0.0：511 万行 | 11.33s → 451k rows/s（4 区间并行），checksum PASS |
| openGauss 6.0.0：视图 | `v_user_stats` 迁移成功，verify PASS |
| openGauss 6.0.0：CDC | INSERT/UPDATE/DELETE + DDL（ADD COLUMN）5 事件，0 错误，目标与源一致 |
| PG → openGauss | demo 库 100,010 行 / 0.59s，checksum PASS |
| openGauss → PG | 5,110,000 行 / 15.71s，checksum PASS |
| PG → MySQL | demo 库 100,010 行 / 1.30s，checksum PASS |
| MySQL → MySQL | demo 库 100,013 行 / 1.00s，checksum PASS |

### 多方向 500 万行压力测试（bench/stress-matrix.sh）

同一份 5,110,000 行数据（big 500 万 + comp 10 万 + nopk 1 万），4 workers / batch 2000，时间为迁移+校验：

| 路径 | 耗时 | 吞吐 | 校验 |
|---|---|---|---|
| MySQL → openGauss | 10.62s | 481k rows/s | PASS |
| MySQL → PostgreSQL | 12.42s | 411k rows/s | PASS |
| MySQL → MySQL | 13.31s | 384k rows/s | PASS |
| openGauss → PostgreSQL | 15.94s | 320k rows/s | PASS |
| PostgreSQL → openGauss | 19.16s | 267k rows/s | PASS |
| PostgreSQL → MySQL | 27.52s | 186k rows/s | PASS |
| openGauss → MySQL | 22.71s | 225k rows/s | PASS |

> MySQL 目标已改用 `LOAD DATA LOCAL INFILE`；上表为 4 workers。
> 需要服务端 `local_infile=ON`；二进制列的表自动回退 INSERT。

### 线程扩展性（LOAD DATA，5,110,000 行）

| 模式 | 1 线程 | 2 线程 | 4 线程 | 8 线程 |
|---|---|---|---|---|
| MySQL → MySQL | 266k | 298k (+12%) | 384k (+29%) | 410k (+7%) |
| MySQL → openGauss | 356k | 423k (+19%) | 485k (+15%) | 485k (0%) |
| PostgreSQL → MySQL | 187k | 174k | 186k | 184k |
| openGauss → MySQL | 231k | 230k | 225k | 227k |

结论：MySQL 源在 4 线程后增长率明显下降（4→8 仅 +7%）；
PG/openGauss 源没有表内主键区间并行，线程数基本无收益。
| CDC 增删改 | 实时同步，含负数 DECIMAL |
| CDC DDL | ADD COLUMN 自动同步 |

## 5. 踩过的坑（重要，别重犯）

1. **Makefile 必须带 -MMD -MP**：改 Options 字段后旧 .o 不重编会导致 ABI 错位（rows 变成 1.99 亿）。
2. **所有 DDL/COPY 后必须 COMMIT**：PG 事务不提交，日志显示成功但库里没有。
3. **PG 一条语句失败会 abort 整个事务**：exec_sql 用 SAVEPOINT 隔离；commit 失败要 ROLLBACK+BEGIN。
4. **PG 索引名全库唯一**（MySQL 每表唯一）：跨表重名要加表名后缀。
5. **MySQL TEXT 的类型码也是 MYSQL_TYPE_BLOB**：只有 charsetnr==63 才是二进制。
6. **DATETIME 的 charsetnr 也是 63**：不能据此判断二进制。
7. **binlog 客户端要先 SET @master_binlog_checksum='NONE'**，否则服务端拒绝发事件。
8. **libmysqlclient 的 binlog 包前多 1 字节状态位**：用 event_size 自检纠正偏移。
9. **ROWS v2 的 extra_data_length 包含自身 2 字节**：p = 8 + extra。
10. **ROTATE 事件尾部带 CRC**：文件名只取合法字符。
11. **QUERY 事件提前 return 前必须设 has_event**，否则事件被丢弃。
12. **工具写入含敏感词（password/token）的代码时可能被脱敏成星号**：改用变量拼接。
13. **字符串字面量 + char 是指针运算**：必须 std::string(...) + ch。
14. **区间并行建表后必须 COMMIT**：子线程用各自新连接，未提交的 CREATE TABLE 它们看不见。
15. **小表别开区间并行**：1M 行以下线程/连接开销大于收益，默认阈值 1,000,000 行；
    2M 行基本持平，5M 行约快 17%。
16. **区间边界用闭区间 + 数值均分**：`min + (max-min)*i/parts`，空区间直接跳过；
    只对单列整数主键生效，复合主键/无主键自动回退顺序迁移。
17. **openGauss 不支持 ON CONFLICT**：CDC 更新要用 DELETE+INSERT；
    binlog QUERY 事件尾部有 CRC32，解析要扣 4 字节；DDL 后必须清空 schema 缓存。

## 6. 已知限制

- CDC 需要主键；无主键表跳过
- 不支持 BIT / ENUM / SET / GEOMETRY 列
- DDL 方言转换有限（DATE_FORMAT / GROUP_CONCAT / LIMIT x,y / 分区需人工）
- DM8 未实现
- 目标端已支持 MySQL（backend_mysql_target）+ PG/openGauss；源端支持 MySQL / PG / openGauss
- PG→MySQL 暂不支持视图；CDC 仍只支持 MySQL binlog 源
- openGauss 走 PG 协议：mysql2og 模式，6.0.0 实测通过；
  库需 DBCOMPATIBILITY='PG'；numeric 需 display_leading_zero；
  若账号是 sha256 认证，原生 libpq 会报错，改用 md5
- 目标表需为空或已对齐（重跑会撞主键）
- 表内并行仅支持单列整数主键（tinyint/smallint/mediumint/int/bigint）；
  复合主键、无主键、非整数主键回退表级/顺序并行

## 7. 待办（按优先级）

1. DM8 适配（类型映射 + dmfldr）
2. ~~openGauss 实测~~（已完成：Docker 6.0.0，全量/区间/视图/checksum 均 PASS）
3. 根目录 README 更新（现有版本过时）
4. ~~单表主键区间并行~~（已完成：`--workers` + 单列整数主键，5M 行 1.05M → 1.38M rows/s）
5. CDC 事件批量应用（现在逐条 SQL，高写入量会延迟；openGauss 增删改 + DDL 已实测）
