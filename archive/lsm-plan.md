# mini-database 开发计划

> 项目定位：**嵌入式 KV 存储引擎库**（对标 LevelDB / RocksDB），不是数据库服务器。
> 对外产出一个可链接的库；核心是 LSM + WAL + CDC；B+Tree 作为第二阶段引擎。

## 0. 明确不做（Out of Scope）

- 网络层（net / RESP / commands）→ 不做服务器
- MySQL 连接池（backend/）→ 除非 CDC 需要把变更同步到 MySQL
- 复制/集群/鉴权 → 后期再说

## 1. 架构契约（第一步就要定死）

storage/storage_engine.h —— 所有引擎必须实现同一接口：

    put / get / del / scan / WriteBatch / snapshot

规则：上层（CDC、tests、bench）**只依赖这个接口**，绝不依赖具体引擎。
这是“给 B+Tree 留空间”的唯一实质做法。

## 2. 里程碑

### M1 · LSM 核心
目标：内存 + 磁盘的 LSM，能存能读
文件：storage/lsm_engine/{memtable, sstable, compaction, bloom}.{h,cpp}
要点：
- memtable 用 SkipList
- SSTable 有序落盘 + 索引块
- Compaction（先 size-tiered，再考虑 level-tiered）
- Bloom Filter 降低读放大
验收：单线程 put/get/del 正确；落盘后重启能读回。

### M2 · WAL + 崩溃恢复
目标：写入先落 WAL，崩溃不丢数据
文件：wal/{wal, replay, lsn}.{h,cpp}
验收（硬指标）：
- kill -9 随机 100+ 次，已确认的写操作零丢失
- 日志能正确重放与截断

### M3 · 正确性测试
文件：tests/
- engine_consistency_test：同一批用例跑引擎，结果必须一致
- crash_test：反复 kill -9 后校验数据
验收：全部通过，且可重复运行。

### M4 · CDC
目标：捕获 put/del 变更，提供订阅 / 拉取接口
文件：cdc/{change_event, subscription, offset}.{h,cpp}
验收：程序能订阅变更；顺序正确、不丢不重（至少 at-least-once）。

### M5 · 压测与调优
文件：bench/
- YCSB 式 workload：读多 / 写多 / 范围扫描
- 与 LevelDB 对比
验收（硬指标）：
- 给出吞吐 + P50 / P99 延迟数字
- 连续跑 1 小时无内存泄漏、无性能衰减

### M6 · B+Tree 第二引擎（前提：M1–M5 全通过）
文件：storage/bplus_engine/
验收：实现同一 StorageEngine 接口；bench 给出 LSM vs B+Tree 对比数据。

## 3. “留空间”现在就要做的 4 件事

1. 写 storage/storage_engine.h（接口）
2. lsm_engine 实现它
3. 建空目录 storage/bplus_engine/ 占位
4. tests / bench / cdc 一律只依赖接口

→ 将来加 B+Tree，上层代码零改动。

## 4. 每个里程碑的通用验收动作

- 编译通过（CMake）
- 单元测试通过
- 更新 docs/ 里的进度记录
- 打一个 git tag（建议 M0 就先 git init）
