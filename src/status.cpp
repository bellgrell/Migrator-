#include "minidb/status.h"

namespace minidb {

std::string Status::ToString() const {
    if (code_ == StatusCode::kOk) return "OK";

    const char* name = "Unknown";
    switch (code_) {
        case StatusCode::kOk:              name = "Ok"; break;
        case StatusCode::kNotFound:        name = "NotFound"; break;
        case StatusCode::kCorruption:      name = "Corruption"; break;
        case StatusCode::kIOError:         name = "IOError"; break;
        case StatusCode::kInvalidArgument: name = "InvalidArgument"; break;
        case StatusCode::kNotSupported:    name = "NotSupported"; break;
    }
    if (msg_.empty()) return name;
    return std::string(name) + ": " + msg_;
}

}  // namespace minidb
