#include <cassert>
#include <iostream>

#include "minidb/cdc.h"
#include "minidb/storage_engine.h"
#include "minidb/wal.h"

int main() {
    using namespace minidb;

    // 1) 工厂能创建引擎，接口可多态调用
    std::unique_ptr<StorageEngine> engine = CreateEngine("lsm");
    assert(engine != nullptr);
    assert(engine->Name() == "lsm");

    Options opts;
    assert(engine->Open(opts).ok());

    // 2) 未实现的接口应返回 NotSupported，而不是崩溃
    Status put = engine->Put(Slice("k"), Slice("v"));
    std::cout << "Put -> " << put.ToString() << std::endl;
    assert(!put.ok() && put.code() == StatusCode::kNotSupported);

    // 3) WAL / CDC 的占位实现都能拿到
    auto wal = CreateWal();
    auto cdc = CreateCdc();
    assert(wal != nullptr);
    assert(cdc != nullptr);

    assert(engine->Close().ok());
    std::cout << "smoke test passed" << std::endl;
    return 0;
}
