# mini-migrator 开发计划

## M0 · 地基（1 周）

| 文件 | 位置 | 用途 |
|---|---|---|
| status.h / .cpp | include/migrator, src/util | 错误码（已有，待修） |
| slice.h | include/migrator | 字节视图（已有，待修） |
| options.h | include/migrator | 配置结构（已有，待改） |
| types.h / .cpp | include/migrator, src/common | Value / Row / ColumnDef / TableSchema |
| config.h / .cpp | src/config | 读 YAML |
| logger.h / .cpp | src/common | 日志 + 进度 |
| retry.h / .cpp | src/common | 重试 |

验收：能读配置文件、打印日志。

## M1 · 连接层（1 周）

| 文件 | 位置 |
|---|---|
| pool.h / .cpp | src/db（改造已有连接池） |
| connector.h / .cpp | src/db |
| mysql_conn.h / .cpp | src/db/mysql |
| pg_conn.h / .cpp | src/db/postgres |

验收：MySQL 和 PostgreSQL 都能 SELECT 1。

## M2 · 源端（1 周）

| 文件 | 位置 |
|---|---|
| source.h | include/migrator |
| mysql_source.h / .cpp | src/db/mysql |

ISource 六个方法：Connect / ListTables / ReadSchema / OpenCursor / FetchBatch / CloseCursor。

验收：能列出表、能分批读出 100 万行。

## M3 · 目标端（1 周）

| 文件 | 位置 |
|---|---|
| target.h | include/migrator |
| pg_target.h / .cpp | src/db/postgres |
| opengauss_target.h / .cpp | src/db/opengauss |

ITarget 六个方法：Connect / CreateTable / WriteBatch / Commit / Rollback / Close。

验收：能建表、能批量写 10 万行。

## M4 · 类型映射 + DDL（2 周）⭐

| 文件 | 位置 |
|---|---|
| type_map.h / .cpp | src/config |
| type_map.yaml | config |
| column_override.yaml | config |
| extractor.h / .cpp | src/schema |
| ddl_builder.h / .cpp | src/schema |
| identifier.h / .cpp | src/schema |

验收：生成的 DDL 能在目标库执行，类型正确。

## M5 · 全量搬运（2 周）⭐

| 文件 | 位置 |
|---|---|
| copier.h / .cpp | src/data |
| batch_writer.h / .cpp | src/data |
| progress.h / .cpp | src/data |

验收：100 万行迁移完成，行数一致，耗时达标。

## M6 · 校验（1 周）

| 文件 | 位置 |
|---|---|
| verifier.h / .cpp | src/verify |
| checksum.h / .cpp | src/verify |

验收：人为篡改目标库 1 行，能检出。

## M7 · 增量 CDC（3-4 周）

| 文件 | 位置 |
|---|---|
| change_event.h | src/cdc |
| binlog_reader.h / .cpp | src/cdc |
| change_applier.h / .cpp | src/cdc |
| offset_store.h / .cpp | src/cdc |

验收：迁移中 insert/update/delete，目标库 5 秒内同步。

## M8 · 工程化（1-2 周）

CLI 子命令：inspect / schema / copy / verify / sync

## M9 · DM8（可选）

dm8_target.h / .cpp + config/type_map_dm8.yaml
