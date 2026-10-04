#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "minidb/options.h"
#include "minidb/slice.h"
#include "minidb/status.h"

namespace minidb {

using SequenceNumber = std::uint64_t;

enum class RecordType : std::uint8_t {
    kPut = 1,
    kDelete = 2,
};

// WAL 里的一条记录。
struct LogRecord {
    RecordType type = RecordType::kPut;
    SequenceNumber seq = 0;
    std::string key;
    std::string value;
};

// Write-Ahead Log：改内存之前，先把操作顺序追加到日志。
//
// 写路径： Put -> WAL.Append() -> memtable
// 恢复：   Open -> Replay(fn) -> 重建 memtable
// 截断：   memtable flush 成 SSTable 后，Truncate 掉已落盘部分
class WriteAheadLog {
public:
    using ReplayFn = std::function<void(const LogRecord&)>;

    virtual ~WriteAheadLog() = default;

    virtual Status Open(const std::string& path, const Options& options) = 0;
    virtual Status Append(const LogRecord& record) = 0;
    virtual Status Sync() = 0;
    virtual Status Replay(const ReplayFn& fn) = 0;
    virtual Status Truncate(SequenceNumber up_to_seq) = 0;
    virtual Status Close() = 0;
};

std::unique_ptr<WriteAheadLog> CreateWal();

}  // namespace minidb
