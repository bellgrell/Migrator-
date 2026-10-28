# mini-database

一个用 C++17 从零实现的 **嵌入式 KV 存储引擎**（学习 / 作品项目）。

定位：**库**，不是数据库服务器。对标 LevelDB / RocksDB。

## 路线

    M1  LSM 核心      memtable / sstable / compaction / bloom
    M2  WAL           日志格式 / append / replay / truncate / fsync
    M3  正确性测试    引擎一致性 + kill -9 崩溃测试
    M4  CDC           tail WAL -> 变更事件流 -> 订阅
    M5  压测          YCSB 式 + 与 LevelDB 对比
    M6  B+Tree 引擎   第二引擎（前提：M1-M5 全通过）

详见 docs/plan.md。

## 目录

    include/minidb/        公共接口（对外）
    src/storage/           存储引擎
      lsm_engine/          LSM 引擎（第一阶段）
      bplus_engine/        B+Tree 引擎（第二阶段，占位）
    src/wal/               预写日志
    src/cdc/               变更数据捕获
    tests/                 单元测试 + 崩溃测试
    bench/                 压测
    server/                （暂缓）Redis 协议 / 网络层
    sink/mysql_pool/       （暂缓）CDC 下游连接池
    docs/                  设计文档

## 核心契约

所有引擎实现同一个接口 include/minidb/storage_engine.h：

    Open / Close / Put / Get / Delete / Scan / Name / Stats

上层（CDC / tests / bench）只依赖该接口，不依赖具体引擎。
这样新增引擎时上层零改动。

## 构建

    make            # 生成 build/libminidb.a
    make test       # 编译并运行 tests/
    make clean

## WAL 与 CDC 的关系

- WAL  = 给引擎自己用的，为了崩溃恢复（内部）
- CDC  = 给外部系统订阅的，变更事件流（对外）
- CDC 最省事的实现：tail WAL（一份日志，两种用途，参考 MySQL binlog）


//学习笔记：
B树因为可能存在的SMO操作（分裂合并操作）导致必须在可能发生该操作的节点加锁，从而影响到他本身的并发性能

然后对于这个锁，这里存在两种锁一种是lock逻辑上的锁，主要是面对用户的事物，另一种是latch这个是线程锁，主要是数据加锁（比如我要修改内存中的一块数据，那么为了防止在我修改数据时，这块数据被其他人访问，那么我给他上个latch锁，后面我修改完数据了，但是还没有提交。这时候需要添加一个lock来锁住这被我修改的数据块，当我提交之后lock解除就可以正常访问）

B+树，在非叶子节点仅保留指针，数据都存放在叶子节点（减少了，数的高度，并且区分开了索引段和数据段）
LSM树，就是分层结构一层一层向下追加写入数据，当内存满了之后，会开始层级合并去重整理后都放在下层（Compaction，同时这也是一种标准的SMO操作，会占用大量资源，还可能伴随着缓冲丢失，写停顿从而影响到并发性能，所以LSM树最大的问题就是提高Compaction的性能

BLOOM过滤器误报率跟哈希数量之间的取舍是提高LSM树的关键点之一

Compaction还会造成Cache丢失，可以通过在结束后主动更新Cache活着机器学习的方法避免