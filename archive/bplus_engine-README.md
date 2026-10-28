# bplus_engine（占位）

第二阶段（M6）实现，前提是 M1–M5 全部通过。

要求：
- 实现同一套 minidb::StorageEngine 接口（见 include/minidb/storage_engine.h）
- 完成后在 src/storage/engine_factory.cpp 里注册 "bplus"
- bench 需给出 LSM vs B+Tree 对比数据（写密集 / 读密集 / 范围扫描）

为什么要两个引擎：
- 写密集：LSM 吞吐更高
- 读密集 / 范围扫描：B+Tree 延迟更低
- 用数据证明，而不是“因为想写两个”
