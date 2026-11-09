# 文件清单（迁移工具版）

> 按里程碑排好顺序。总共约 **71 个文件**，但**不用一次全建**——按阶段来。
> 「参考」列是**别人仓库里的文件**，格式 `仓库: 路径`。

# 📦 第 0 层 · 地基（先写，M1 之前）

| 文件 | 放哪 | 干嘛用 | 参考 | 估计行数 |
|---|---|---|---|---|
| `slice.h` | include/migrator/ | 字节视图 | leveldb: `include/leveldb/slice.h` | ~60 |
| `status.h` + `status.cpp` | include/migrator/ + src/util/ | 错误码 | leveldb: `include/leveldb/status.h` + `util/status.cc` | ~90 |
| `options.h` | include/migrator/ | 配置项 | leveldb: `include/leveldb/options.h` | ~80 |
| `types.h` + `types.cpp` | include/migrator/ + src/common/ | ⭐ 数据模型 Value/Row/ColumnDef/TableSchema | sqlines: `sqldata/sqlapibase.h`（SqlCol/SqlColMeta） | ~250 |
| `coding.h` + `coding.cpp` | src/util/ | Varint 变长编码 | leveldb: `util/coding.h` + `util/coding.cc` | ~150 |
| `crc32.h` + `crc32.cpp` | src/util/ | 校验和（M6 要用） | leveldb: `util/crc32c.cc` | ~100 |
| `config.h` + `config.cpp` | src/config/ | 读配置文件 | sqlines: `sqldata/parameters.h` + `parameters.cpp` | ~250 |
| `logger.h` + `logger.cpp` | src/common/ | 日志 + 进度 | sqlines: `sqldata/applog.h` + `applog.cpp` | ~150 |
| `retry.h` + `retry.cpp` | src/common/ | 重试（网络断/死锁） | 无 | ~100 |

> ✅ **这 16 个文件写完，地基就有了。**

---

# 🔌 M1 · 连接层

| 文件 | 放哪 | 干嘛用 | 参考 | 行数 |
|---|---|---|---|---|
| `pool.h` + `pool.cpp` | src/db/ | ⭐ 通用连接池 | **本项目** `src/db/mysql/pool/`（你原来的代码） | ~300 |
| `connector.h` + `connector.cpp` | src/db/ | 工厂：按 type 建连接 | sqlines: `sqldata/sqlapibase.h`（Init/SetDriverType） | ~150 |
| `mysql_conn.h` + `.cpp` | src/db/mysql/ | MySQL 连接封装 | sqlines: `sqldata/sqlmysqlapi.h` + `sqlmysqlapi.cpp` | ~200 |
| `pg_conn.h` + `.cpp` | src/db/postgres/ | PG 连接封装 | sqlines: `sqldata/sqlpgapi.h` + `sqlpgapi.cpp` | ~150 |

> M1 合计 **8 个文件、约 800 行**（验收：两边 SELECT 1 都通）

---

# 📤 M2 · 源端（读）

| 文件 | 放哪 | 干嘛用 | 参考 | 行数 |
|---|---|---|---|---|
| `source.h` | include/migrator/ | ⭐ ISource 抽象（6 方法） | sqlines: `sqldata/sqlapibase.h` | ~80 |
| `mysql_source.h` + `.cpp` | src/db/mysql/ | Connect/ListTables/ReadSchema/OpenCursor/FetchBatch/CloseCursor | sqlines: `sqldata/sqlmysqlapi.cpp` | ~450 |

> M2 合计 **3 个文件、约 530 行**（验收：能分批读出 100 万行）

---

# 📥 M3 · 目标端（写）

| 文件 | 放哪 | 干嘛用 | 参考 | 行数 |
|---|---|---|---|---|
| `target.h` | include/migrator/ | ⭐ ITarget 抽象（6 方法） | sqlines: `sqldata/sqlapibase.h` | ~80 |
| `pg_target.h` + `.cpp` | src/db/postgres/ | Connect/CreateTable/WriteBatch/Commit/Rollback/Close | sqlines: `sqldata/sqlpgapi.cpp` | ~350 |
| `opengauss_target.h` + `.cpp` | src/db/opengauss/ | 继承 pg_target，只处理差异 | 无 | ~120 |

> M3 合计 **5 个文件、约 550 行**（验收：能建表、能批量写 10 万行）

---

# 🧠 M4 · 类型映射 + DDL（⭐ 项目灵魂）

| 文件 | 放哪 | 干嘛用 | 参考 | 行数 |
|---|---|---|---|---|
| `type_map.h` + `.cpp` | src/config/ | ⭐ 两级映射加载 + 查询 | sqlines: `sqldata/sqldb.cpp`（搜类型映射逻辑） | ~200 |
| `type_map.yaml` | config/ | ⭐ 全局类型映射表 | sqlines: `sqldata/sqlines_dtmap.txt`（164 字节） | ~80 行配置 |
| `column_override.yaml` | config/ | ⭐ 列级覆盖（特例） | sqlines: `sqldata/sqlines_cmap.txt`（277 字节） | ~30 行配置 |
| `extractor.h` + `.cpp` | src/schema/ | 读 information_schema 抽元数据 | 声明：sqlines `sqldata/sqlapibase.h`（ReadSchema）<br>实现：sqlines `sqldata/sqlmysqlapi.cpp` | ~350 |
| `identifier.h` + `.cpp` | src/schema/ | 大小写 + 加引号 | 无 | ~120 |
| `ddl_builder.h` + `.cpp` | src/schema/ | 生成 CREATE TABLE | sqlines: `sqlines/sqlines.h` + `sqlines/sqlines.cpp` | ~400 |

> M4 合计 **8 个源文件 + 2 个配置文件、约 1070 行**（验收：DDL 能在目标库执行）

---

# 🚚 M5 · 全量搬运（⭐ 核心）

| 文件 | 放哪 | 干嘛用 | 参考 | 行数 |
|---|---|---|---|---|
| `copier.h` + `.cpp` | src/data/ | ⭐ 主流程：建表→抽→写→提交 | sqlines: `sqldata/sqldata.cpp` | ~400 |
| `batch_writer.h` + `.cpp` | src/data/ | ⭐ 批量写 + 事务控制 | 声明：sqlines `sqldata/sqlapibase.h`（TransferRows）<br>实现：sqlines `sqldata/sqlmysqlapi.cpp` | ~300 |
| `progress.h` + `.cpp` | src/data/ | 进度 + 限流 + 耗时 | sqlines: `sqldata/applog.cpp` | ~150 |

> M5 合计 **6 个文件、约 850 行**（验收：100 万行迁移完成）

---

# ✅ M6 · 校验（⭐ 简历加分）

| 文件 | 放哪 | 干嘛用 | 参考 | 行数 |
|---|---|---|---|---|
| `verifier.h` + `.cpp` | src/verify/ | 行数校验 + 抽样逐字段比对 | 无 | ~250 |
| `checksum.h` + `.cpp` | src/verify/ | 分块 checksum | leveldb: `util/crc32c.cc` | ~150 |

> M6 合计 **4 个文件、约 400 行**（验收：人为改 1 行能检出）

---

# 🔄 M7 · 增量 CDC（⚠️ 最难）

| 文件 | 放哪 | 干嘛用 | 参考 | 行数 |
|---|---|---|---|---|
| `change_event.h` | src/cdc/ | 变更事件结构 | debezium: `debezium-connector-mysql/`（模块级） | ~80 |
| `binlog_reader.h` + `.cpp` | src/cdc/ | ⭐ MySQL binlog 解析（ROW） | canal: `parse/`（模块级）、canal: `server/` | ~600 |
| `change_applier.h` + `.cpp` | src/cdc/ | 应用到目标库（幂等） | canal: `sink/`（模块级） | ~300 |
| `offset_store.h` + `.cpp` | src/cdc/ | ⭐ 位点持久化（断点续传） | canal: `meta/`（模块级） | ~150 |

> M7 合计 **7 个文件、约 1130 行**（验收：迁移中变更，5 秒内同步）

---

# 🛠️ M8 · 工程化

| 文件 | 放哪 | 干嘛用 | 参考 | 行数 |
|---|---|---|---|---|
| `main.cpp` | src/ | CLI 入口 | sqlines: `sqlines/main.cpp` | ~150 |
| `cli.h` + `cli.cpp` | src/ | 子命令解析 | sqlines: `sqlines/parameters.h` + `parameters.cpp` | ~250 |
| `dead_letter.h` + `.cpp` | src/data/ | 坏行落盘 | 无 | ~120 |

> M8 合计 **5 个文件、约 520 行**

---

# 🇨🇳 M9 · DM8 适配（可选）

| 文件 | 放哪 | 干嘛用 | 参考 | 行数 |
|---|---|---|---|---|
| `dm8_target.h` + `.cpp` | src/db/dm8/ | DM8 实现（dmfldr 批量装载） | 无（按 DM8 文档） | ~300 |
| `type_map_dm8.yaml` | config/ | DM8 类型映射 | 无 | ~60 行配置 |

> M9 合计 **2 个源文件 + 1 个配置、约 360 行**

---

# 🧪 测试

| 文件 | 放哪 | 测什么 | 行数 |
|---|---|---|---|
| `test_type_map.cpp` | tests/ | 映射表全覆盖 | ~200 |
| `test_identifier.cpp` | tests/ | 大小写/引号 | ~120 |
| `test_ddl_builder.cpp` | tests/ | 生成的 DDL 对不对 | ~250 |
| `test_batch_writer.cpp` | tests/ | 批量写 + 事务 | ~200 |
| `test_verifier.cpp` | tests/ | 能检出篡改 | ~150 |
| `test_end_to_end.cpp` | tests/ | ⭐ 建表→灌 10 万→迁移→校验 | ~300 |

> **6 个文件、约 1220 行**。`test_end_to_end` 是简历上最值钱的。

---

# ⚡ 压测

| 文件 | 放哪 | 干嘛用 | 行数 |
|---|---|---|---|
| `copy_bench.cpp` | bench/ | batch size vs 吞吐曲线，产出 P50/P99 | ~400 |

---

# 🔧 附加（非代码）

| 文件 | 说明 |
|---|---|
| `Makefile` | 构建（已有） |
| `README.md` | 项目说明（已有） |
| `docs/plan.md` | 阶段计划（已有） |
| `docs/file-manifest.md` | 本清单 |
| `docs/work-breakdown.md` | 可勾选任务表 |
| `.gitignore` | 已有 |

---

# 📊 汇总

| 阶段 | 文件数 | 估计行数 | 能不能跳过 |
|---|---|---|---|
| 第 0 层 地基 | 16 | ~1200 | ❌ 必须 |
| M1 连接层 | 8 | ~800 | ❌ 必须 |
| M2 源端 | 3 | ~530 | ❌ 必须 |
| M3 目标端 | 5 | ~550 | ❌ 必须 |
| M4 类型映射 + DDL | 8 + 2 配置 | ~1070 | ❌ **必须（灵魂）** |
| M5 全量搬运 | 6 | ~850 | ❌ **必须（核心）** |
| M6 校验 | 4 | ~400 | ⚠️ 强烈建议 |
| M7 增量 CDC | 7 | ~1130 | ✅ 差异化亮点 |
| M8 工程化 | 5 | ~520 | ⚠️ 建议 |
| M9 DM8 | 2 + 1 配置 | ~360 | ✅ 可选 |
| 测试 | 6 | ~1220 | ⚠️ **别跳** |
| 压测 | 1 | ~400 | ⚠️ 强烈建议 |
| **合计** | **~71** | **~9000 行** | |

---

# 🎯 三条重要提醒

**① 别一次建 71 个文件！**

每个里程碑**只建"能编译通过的最小集合"**：

    第 0 层写完 → make 通过 → 进 M1
    M1 写完    → SELECT 1 两边通 → 进 M2
    M2+M3      → 能读 100 行、写 100 行 → 进 M4
    M4 写完    → DDL 能在目标库执行 → 进 M5
    M5 写完    → 100 万行搬完 → 进 M6

**② 哪些能砍**

    先砍 M9（DM8）→ 再砍 M7（增量 CDC）→ 再砍 M8 的 CLI
    但 第0层 + M1 + M2 + M3 + M4 + M5 + M6 绝对不能砍 ← 这是能演示的最小闭环

**③ 第一个文件不是 slice.h**

而是 **`config/type_map.yaml`** —— 它不用编译，却能逼你想清楚"MySQL 的类型怎么映射"。
这一步想清楚了，M4 就成了一半。
