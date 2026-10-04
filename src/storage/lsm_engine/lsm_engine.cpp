#include "lsm_engine.h"

namespace minidb {

Status LsmEngine::Open(const Options& options) {
    options_ = options;
    opened_ = true;
    return Status::OK();
}

Status LsmEngine::Close() {
    opened_ = false;
    return Status::OK();
}

Status LsmEngine::Put(const Slice& /*key*/, const Slice& /*value*/) {
    // TODO(M1): 先写 WAL，再写 memtable
    return Status::NotSupported("LsmEngine::Put 尚未实现");
}

Status LsmEngine::Get(const Slice& /*key*/, std::string* /*value*/) {
    // TODO(M1): memtable -> L0 SSTable -> ... -> Bloom 过滤
    return Status::NotSupported("LsmEngine::Get 尚未实现");
}

Status LsmEngine::Delete(const Slice& /*key*/) {
    // TODO(M1): 写入墓碑记录（tombstone）
    return Status::NotSupported("LsmEngine::Delete 尚未实现");
}

Status LsmEngine::Scan(const Slice& /*begin*/, const Slice& /*end*/,
                       const ScanCallback& /*cb*/) {
    // TODO(M1): 多路归并 memtable + 各层 SSTable
    return Status::NotSupported("LsmEngine::Scan 尚未实现");
}

}  // namespace minidb
