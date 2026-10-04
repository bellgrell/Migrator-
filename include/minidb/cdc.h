#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "minidb/status.h"
#include "minidb/wal.h"

namespace minidb {

// 对外暴露的变更事件。
struct ChangeEvent {
    RecordType type = RecordType::kPut;
    std::string key;
    std::string value;
    SequenceNumber seq = 0;
    std::uint64_t timestamp_ms = 0;
};

// 一个订阅者。offset 记录“消费到哪了”，持久化后可断点续传。
class Subscription {
public:
    using Callback = std::function<void(const ChangeEvent&)>;

    virtual ~Subscription() = default;
    virtual std::uint64_t offset() const = 0;
    virtual void Commit(std::uint64_t offset) = 0;
};

// Change Data Capture：把引擎的变更变成有序事件流，供外部订阅。
// 最省事的实现方式：tail WAL（参考 MySQL binlog）。
class ChangeCapture {
public:
    virtual ~ChangeCapture() = default;

    virtual Status Start() = 0;
    virtual Status Stop() = 0;

    virtual std::shared_ptr<Subscription> Subscribe(const Subscription::Callback& cb) = 0;
    virtual Status ReplayFrom(SequenceNumber seq) = 0;
};

std::unique_ptr<ChangeCapture> CreateCdc();

}  // namespace minidb
