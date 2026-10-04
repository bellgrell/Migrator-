#pragma once

#include <cstddef>
#include <string>

namespace minidb {

struct Options {
    std::string db_path = "./data";
    size_t memtable_size_bytes = 4 * 1024 * 1024;  // memtable 超过此大小后 flush
    size_t wal_block_size = 32 * 1024;             // WAL 分块大小
    bool wal_sync_on_write = true;                 // 每次写是否 fsync（可靠性 vs 性能取舍）
    int bloom_bits_per_key = 10;                   // Bloom filter 每位 key 的位数
    int level0_compaction_trigger = 4;             // L0 文件数达到多少触发 compaction
    size_t max_file_size = 2 * 1024 * 1024;        // 单个 SSTable 最大尺寸
};

}  // namespace minidb
