# 全部工作清单（WBS）

> 用法：做完一项在前面打 [x]。一次只做一件事。
> 总计约 59 个源文件、约 8760 行；MVP（P0–P7）约 6–8 周。

## P0 · 准备环境（3–5 天）

- [ ] 装 Docker Desktop（macOS）
- [ ] 起 MySQL 8：docker run -d --name src-mysql -p 3306:3306 -e MYSQL_ROOT_PASSWORD=123456 mysql:8
- [ ] 起 PostgreSQL 16：docker run -d --name dst-pg -p 5432:5432 -e POSTGRES_PASSWORD=123456 postgres:16
- [ ] 建测试表 users（id/int、name/varchar、created/datetime、score/decimal、bio/text、active/tinyint(1)）
- [ ] 建测试表 orders（含外键，用来测依赖顺序）
- [ ] 灌 100 万行假数据
- [ ] 记录基线：两张表行数、抽样 100 行内容
- [ ] 决定：openGauss 用 PostgreSQL 代替（同源）
- [ ] 验证：mysql 客户端和 psql 都能连上

## P1 · M0 地基（1 周）

- [ ] 修 3 个头文件重复的 include guard
- [ ] 修 slice.h 的 4 处语法错误
- [ ] 修 status.h 的类名
- [ ] make 能通过（第一个正反馈）
- [ ] 设计 types.h：Value / Row / ColumnDef / TableSchema / IndexDef（先画结构图）
- [ ] 实现 types.h + types.cpp
- [ ] 实现 config.h/.cpp（读 YAML 或 ini）
- [ ] 实现 logger.h/.cpp
- [ ] 实现 retry.h/.cpp
- [ ] 验收：能读配置、打印日志、编译通过

## P2 · M1 连接层（1 周）

- [ ] 修连接池 6 处编译错误（clock_T、缺分号、p ! =、-conn、atomic、类名拼写）
- [ ] 把连接池从 MySQL 专用改成通用（模板或接口）
- [ ] connector.h/.cpp：按 type 创建连接
- [ ] mysql_conn.h/.cpp
- [ ] pg_conn.h/.cpp
- [ ] 验收：MySQL 和 PostgreSQL 都能 SELECT 1

## P3 · M2 源端（1 周）

- [ ] 设计 source.h（Connect/ListTables/ReadSchema/OpenCursor/FetchBatch/CloseCursor）
- [ ] mysql_source：Connect
- [ ] mysql_source：ListTables
- [ ] mysql_source：ReadSchema（information_schema）
- [ ] mysql_source：OpenCursor
- [ ] mysql_source：FetchBatch
- [ ] mysql_source：CloseCursor
- [ ] 验收：能列出表、能分批读出 100 万行

## P4 · M3 目标端（1 周）

- [ ] 设计 target.h（Connect/CreateTable/WriteBatch/Commit/Rollback/Close）
- [ ] pg_target：Connect
- [ ] pg_target：CreateTable
- [ ] pg_target：WriteBatch
- [ ] pg_target：Commit / Rollback
- [ ] opengauss_target（继承 pg_target，只处理差异）
- [ ] 验收：能建表、能批量写 10 万行

## P5 · M4 类型映射 + DDL（2 周）⭐

- [ ] 写 config/type_map.yaml（先 20 条常见映射）
- [ ] 写 config/column_override.yaml（先空，留格式）
- [ ] type_map.h/.cpp：加载 + 两级查询
- [ ] extractor.h/.cpp：读 information_schema 抽元数据
- [ ] identifier.h/.cpp：大小写 + 加引号
- [ ] ddl_builder.h/.cpp：生成 CREATE TABLE + 索引 + 注释
- [ ] 验收：生成的 DDL 在目标库能执行，字段类型正确

## P6 · M5 全量搬运（2 周）⭐

- [ ] copier.h/.cpp：主流程（建表 → 抽 → 写 → 提交）
- [ ] batch_writer.h/.cpp：批量 INSERT + 事务
- [ ] progress.h/.cpp：进度 + 耗时
- [ ] 调优 batch size（试 500 / 1000 / 5000）
- [ ] 验收：100 万行迁移完成，行数一致

## P7 · M6 校验（1 周）

- [ ] verifier：行数校验
- [ ] checksum：分块校验和
- [ ] verifier：抽样逐字段比对
- [ ] 验收：人为篡改 1 行能检出

## P8 · M7 增量 CDC（3–4 周）

- [ ] 读 MySQL binlog 格式文档，手写解析一个事件
- [ ] change_event.h
- [ ] binlog_reader.h/.cpp：ROW 格式解析
- [ ] change_applier.h/.cpp：应用到目标库（幂等）
- [ ] offset_store.h/.cpp：位点持久化
- [ ] 实现全量→增量位点衔接
- [ ] 验收：迁移中 insert/update/delete，目标库 5 秒内同步

## P9 · M8 工程化（1–2 周）

- [ ] main.cpp + cli：inspect / schema / copy / verify / sync
- [ ] 配置文件模板（example）
- [ ] dead_letter：坏行落盘
- [ ] 进度条
- [ ] 验收：一条命令跑完整个流程

## P10 · M9 DM8（可选）

- [ ] config/type_map_dm8.yaml
- [ ] dm8_target.h/.cpp
- [ ] 验收（有环境时）：能连上、能建表、能写

## P11 · 测试与压测

- [ ] test_type_map.cpp
- [ ] test_identifier.cpp
- [ ] test_ddl_builder.cpp
- [ ] test_batch_writer.cpp
- [ ] test_verifier.cpp
- [ ] test_end_to_end.cpp（建表→灌 10 万→迁移→校验）
- [ ] bench/copy_bench.cpp（batch size vs 吞吐）
- [ ] 验收：全部通过，且能重复运行

## P12 · 文档与发布

- [ ] README：架构图 + 快速开始
- [ ] docs/usage.md：使用说明
- [ ] 演示脚本：一条命令跑完整迁移
- [ ] 记录性能数字（吞吐 / P99 / 耗时）
- [ ] GitHub：建私有仓库 → push
- [ ] 写简历条目（数字 + 难点 + 取舍）

## 时间表

| 阶段 | 工期 | 累计 |
|---|---|---|
| P0 准备 | 3–5 天 | 1 周 |
| P1 地基 | 1 周 | 2 周 |
| P2 连接层 | 1 周 | 3 周 |
| P3 源端 | 1 周 | 4 周 |
| P4 目标端 | 1 周 | 5 周 |
| P5 类型映射 | 2 周 | 7 周 |
| P6 全量搬运 | 2 周 | 9 周 |
| P7 校验 | 1 周 | 10 周 |
| **MVP 完成** | | **约 10 周** |
| P8 增量 CDC | 3–4 周 | 13–14 周 |
| P9 工程化 | 1–2 周 | 15–16 周 |
| P10 DM8 | 可选 | |
| P11 测试压测 | 2 周 | 17–18 周 |
| P12 文档发布 | 1 周 | 18–19 周 |

## 如果时间不够，砍的顺序

1. 先砍 P10（DM8）
2. 再砍 P8（增量 CDC）
3. 再砍 P9 的 CLI
4. P0–P7 一个都不能砍
