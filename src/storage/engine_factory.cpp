#include "minidb/storage_engine.h"

#include "lsm_engine/lsm_engine.h"

namespace minidb {

std::unique_ptr<StorageEngine> CreateEngine(const std::string& name) {
    if (name == "lsm") {
        return std::unique_ptr<StorageEngine>(new LsmEngine());
    }
    // TODO(M6): name == "bplus" -> BPlusEngine
    return nullptr;
}

}  // namespace minidb
