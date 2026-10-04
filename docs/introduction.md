mini-redis/
├── net/            # socket、epoll/kqueue、连接管理(连接池)、会话        (M1)
├── protocol/       # RESP 解析与序列化                                  (M1)
├── commands/       # 命令分发与实现（GET/SET/DEL/SCAN/TTL/SUBSCRIBE）    (M2)
├── storage/
│   ├── storage_engine.h      # ⭐ 统一接口: put/get/del/scan/WriteBatch/snapshot
│   ├── mem_hash_engine/      # 内存哈希（现状）
│   ├── lsm_engine/           # memtable/sstable/compaction/bloom          (M4)
│   └── bplus_engine/         # 页/分裂合并/页缓存                          (M5)
├── wal/            # WAL 格式、写入、回放、LSN                            (M3)
├── cdc/            # 变更事件、订阅、offset/重放、下游 sink(可含连接池)    (M6)
├── tests/          # ★ 引擎一致性测试套件（同一批用例跑三个引擎，结果必须相同）
└── bench/          # YCSB 式 workload：读多/写多/范围扫描对比             (M7)