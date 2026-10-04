#pragma once

#include <functional>
#include <memory>
#include <string>

#include "minidb/options.h"
#include "minidb/slice.h"
#include "minidb/status.h"

namespace minidb {

// 统一存储引擎接口。
//
// 约定：所有引擎（lsm / bplus / ...）都必须实现它。
// 上层模块（CDC / tests / bench）只依赖本接口，绝不依赖具体引擎实现。
// 这样新增一个引擎时，上层代码零改动 —— 这就是“给 B+Tree 留空间”的做法。
class StorageEngine {
public:
    using ScanCallback = std::function<bool(const Slice& key, const Slice& value)>;

    virtual ~StorageEngine() = default;

    virtual Status Open(const Options& options) = 0;
    virtual Status Close() = 0;

    virtual Status Put(const Slice& key, const Slice& value) = 0;
    virtual Status Get(const Slice& key, std::string* value) = 0;
    virtual Status Delete(const Slice& key) = 0;

    // 范围扫描 [begin, end)。回调返回 false 表示提前结束。
    virtual Status Scan(const Slice& begin, const Slice& end,
                        const ScanCallback& cb) = 0;

    // 引擎名字，例如 "lsm" / "bplus"
    virtual std::string Name() const = 0;

    // 调试用统计信息
    virtual std::string Stats() const { return std::string(); }
};

// 工厂：按名字创建引擎（"lsm" / "bplus"）。未实现的引擎返回 nullptr。
std::unique_ptr<StorageEngine> CreateEngine(const std::string& name);

}  // namespace minidb
