#pragma once

#include <string>

#include "minidb/options.h"
#include "minidb/slice.h"
#include "minidb/status.h"
#include "minidb/storage_engine.h"

namespace minidb {

// LSM-Tree 存储引擎（第一阶段核心）。
//
// 待实现：
//   - MemTable（内存 SkipList）
//   - WAL 接入（写前日志）
//   - SSTable（有序落盘 + 索引块）
//   - Compaction（分层合并）
//   - BloomFilter（降低读放大）
class LsmEngine : public StorageEngine {
public:
    LsmEngine() = default;
    ~LsmEngine() override = default;

    Status Open(const Options& options) override;
    Status Close() override;

    Status Put(const Slice& key, const Slice& value) override;
    Status Get(const Slice& key, std::string* value) override;
    Status Delete(const Slice& key) override;
    Status Scan(const Slice& begin, const Slice& end,
                const ScanCallback& cb) override;

    std::string Name() const override { return "lsm"; }

private:
    Options options_;
    bool opened_ = false;
};

}  // namespace minidb
