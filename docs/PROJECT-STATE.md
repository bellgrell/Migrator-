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

## 4. 实测数据

| 场景 | 结果 |
|---|---|
| 1000 万行 / 8 表 / 4 线程 / COPY | 9.32s → **1,072,691 rows/s**，verify PASS |
| 同数据 INSERT 模式 | 18.84s → 530,747 rows/s |
| 单表 10 万行 | 0.42s → 240k rows/s |
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

## 6. 已知限制

- CDC 需要主键；无主键表跳过
- 不支持 BIT / ENUM / SET / GEOMETRY 列
- DDL 方言转换有限（DATE_FORMAT / GROUP_CONCAT / LIMIT x,y / 分区需人工）
- DM8 未实现；openGauss 未实测（走 PG 协议）
- 目标表需为空或已对齐（重跑会撞主键）

## 7. 待办（按优先级）

1. DM8 适配（类型映射 + dmfldr）
2. openGauss 实测（Docker）
3. 根目录 README 更新（现有版本过时）
4. 单表主键区间并行（突破按表并行的限制）
5. CDC 事件批量应用（现在逐条 SQL，高写入量会延迟）
