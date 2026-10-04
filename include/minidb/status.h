#pragma once

#include <string>
#include <utility>

namespace minidb {

enum class StatusCode {
    kOk = 0,
    kNotFound,
    kCorruption,
    kIOError,
    kInvalidArgument,
    kNotSupported,
};

class Status {
public:
    Status() : code_(StatusCode::kOk) {}
    Status(StatusCode code, std::string msg) : code_(code), msg_(std::move(msg)) {}

    static Status OK() { return Status(); }
    static Status NotFound(std::string m = "") { return Status(StatusCode::kNotFound, std::move(m)); }
    static Status Corruption(std::string m) { return Status(StatusCode::kCorruption, std::move(m)); }
    static Status IOError(std::string m) { return Status(StatusCode::kIOError, std::move(m)); }
    static Status InvalidArgument(std::string m) { return Status(StatusCode::kInvalidArgument, std::move(m)); }
    static Status NotSupported(std::string m) { return Status(StatusCode::kNotSupported, std::move(m)); }

    bool ok() const { return code_ == StatusCode::kOk; }
    bool IsNotFound() const { return code_ == StatusCode::kNotFound; }
    StatusCode code() const { return code_; }
    const std::string& message() const { return msg_; }

    std::string ToString() const;

private:
    StatusCode code_;
    std::string msg_;
};

}  // namespace minidb
