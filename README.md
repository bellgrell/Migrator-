# mini-migrator

面向国产数据库的**异构数据迁移工具**：MySQL → PostgreSQL / openGauss。

C++17 实现，支持**全量迁移**与基于 binlog 的**实时增量同步（CDC）**。

## 亮点

| 能力 | 实测结果 |
|---|---|
| 全量迁移（COPY 协议） | 1000 万行 / 8 表 / 9.32s → **107 万 rows/s** |
| 表级策略 | copy / rebuild / skip |
| 二级索引迁移 | 唯一 / 普通索引，跨表重名自动处理 |
| BLOB 二进制 | 字节级一致（含 NUL） |
| 视图迁移 | MySQL VIEW → PostgreSQL VIEW（基础方言转换） |
| 校验 | 行数 + checksum，可检出单行篡改 |
| CDC 增量 | 增 / 改 / 删实时同步，含负数 DECIMAL |
| CDC 轮转 | binlog 跨文件自动跟随 |
| CDC DDL | ALTER / CREATE / DROP 基础转换后执行 |
| 监控 | Web 面板 + Qt 6 桌面版 |
| openGauss | `--mode mysql2og`，6.0.0 实测：511 万行 11.33s / 451k rows/s |

## 快速开始

    git clone <repo> && cd mini-migrator
    make

    # 模拟运行（不需要数据库，可看面板效果）
    ./build/migrator --mode sim --rows 600000 --tables 6 --throttle 200

    # 全量迁移 MySQL -> PostgreSQL
    ./build/migrator --mode mysql2pg \
      --source 'host=127.0.0.1;port=3306;user=root;db=demo' \
      --target 'host=127.0.0.1;port=5432;user=postgres;db=demo_dst' \
      --batch 2000 --workers 4 --with-views

    # 全量迁移 MySQL -> openGauss（目标库用 PG 兼容模式，认证见 docs/usage.md §16）
    ./build/migrator --mode mysql2og \
      --source 'host=127.0.0.1;port=3306;user=root;db=demo' \
      --target 'host=127.0.0.1;port=5432;user=gaussdb;password=***;db=og_dst' \
      --batch 2000 --workers 4

    # 全量 + 持续增量 + DDL 同步
    ./build/migrator --mode mysql2pg ... --cdc --cdc-ddl

    # 只跑增量（目标库已就绪）
    ./build/migrator --mode mysql2pg ... --cdc-only --cdc-ddl

面板：http://127.0.0.1:8123/

完整参数见 `./build/migrator --help` 或 [docs/usage.md](docs/usage.md)。

## 目录结构

    src/
      migrator.h            核心类型 / 接口 / Options / Metrics
      core.cpp              错误码、值序列化、类型映射、DDL 生成
      metrics.cpp           指标采集 + JSON 输出
      pipeline.cpp          全量迁移流水线 + 校验
      policy.cpp            表级策略 + 索引 DDL
      views.cpp             视图方言转换
      checksum.cpp          FNV-1a 校验和
      cdc.cpp / cdc.h       binlog 解析 + 增量应用
      backend_mysql.cpp     MySQL 源（libmysqlclient）
      backend_pg.cpp        PostgreSQL 目标（libpq，COPY/INSERT）
      backend_pg_source.cpp PostgreSQL 源（校验用）
      backend_sim.cpp       模拟后端
      backends.cpp          工厂
      http.cpp              内置 HTTP 服务（POSIX / Winsock）
      main.cpp              CLI
    qt/                     Qt 6 桌面版（qmake 工程）
    web/index.html          监控面板（单文件，无 CDN）
    config/                 type_map.json / tables.json / migrator.ini
    docs/                   usage.md / PROJECT-STATE.md / plan.md
    bench/stress.sh         压测脚本

## 设计要点

**1. 两级类型映射**（`config/type_map.json`）

    全局：mysql_type -> target_type（支持 {args} 参数替换）
    列级：schema.table.column 的特殊覆盖

**2. 读一批、写一批**

    游标流式读 -> FetchBatch(N) -> COPY/多行 INSERT -> 定期 COMMIT

**3. 表级策略**（`config/tables.json`）

    { "tables": { "daily_stats": "rebuild:sql/daily_stats.sql", "tmp": "skip" } }

**4. CDC**

    binlog(ROW) -> TABLE_MAP/ROWS 事件 -> 主键 UPSERT / DELETE
    位点保存在 out/cdc-position.txt，重启续传

## 构建（跨平台）

    # macOS / Linux
    make

    # 通用
    cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j

依赖：libmysqlclient、libpq（可选，缺失时对应后端不编译）。

## 已知限制

- CDC 需要主键；无主键表跳过
- 不支持 BIT / ENUM / SET / GEOMETRY 列
- DDL 方言转换有限（DATE_FORMAT / GROUP_CONCAT / LIMIT x,y / 分区需人工）
- DM8 未实现；openGauss 未实测（走 PG 协议）
- 目标表需为空或已对齐（直接重跑会撞主键）

## 相关文档

- [docs/usage.md](docs/usage.md) — 完整使用说明（15 章）
- [docs/PROJECT-STATE.md](docs/PROJECT-STATE.md) — 交接文档（架构 / 13 个坑 / 待办）
- [docs/plan.md](docs/plan.md) — 阶段计划

## 参考项目

pgloader（类型映射）、DataX/Addax（插件化）、Debezium（CDC）、sqlines（两级映射表）。
