#include "minidb/wal.h"

namespace minidb {

namespace {

// 占位实现：先跑通接口，M2 再补真正的日志格式 / 回放 / 截断。
class StubWal : public WriteAheadLog {
public:
    Status Open(const std::string& /*path*/, const Options& /*options*/) override {
        return Status::NotSupported("WAL 尚未实现 (M2)");
    }
    Status Append(const LogRecord& /*record*/) override {
        return Status::NotSupported("WAL 尚未实现 (M2)");
    }
    Status Sync() override { return Status::OK(); }
    Status Replay(const ReplayFn& /*fn*/) override {
        return Status::NotSupported("WAL 尚未实现 (M2)");
    }
    Status Truncate(SequenceNumber /*up_to_seq*/) override {
        return Status::NotSupported("WAL 尚未实现 (M2)");
    }
    Status Close() override { return Status::OK(); }
};

}  // namespace

std::unique_ptr<WriteAheadLog> CreateWal() {
    return std::unique_ptr<WriteAheadLog>(new StubWal());
}

}  // namespace minidb
