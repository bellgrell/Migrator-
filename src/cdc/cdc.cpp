#include "minidb/cdc.h"

namespace minidb {

namespace {

class StubSubscription : public Subscription {
public:
    std::uint64_t offset() const override { return offset_; }
    void Commit(std::uint64_t offset) override { offset_ = offset; }

private:
    std::uint64_t offset_ = 0;
};

// 占位实现：M4 再基于 WAL tail 实现真正的变更流。
class StubCdc : public ChangeCapture {
public:
    Status Start() override { return Status::NotSupported("CDC 尚未实现 (M4)"); }
    Status Stop() override { return Status::OK(); }
    std::shared_ptr<Subscription> Subscribe(const Subscription::Callback& /*cb*/) override {
        return std::make_shared<StubSubscription>();
    }
    Status ReplayFrom(SequenceNumber /*seq*/) override {
        return Status::NotSupported("CDC 尚未实现 (M4)");
    }
};

}  // namespace

std::unique_ptr<ChangeCapture> CreateCdc() {
    return std::unique_ptr<ChangeCapture>(new StubCdc());
}

}  // namespace minidb
