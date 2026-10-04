#pragma once

#include <cstddef>
#include <cstring>
#include <string>

namespace minidb {

// 对一段只读字节的轻量视图（不拥有内存）。
class Slice {
public:
    Slice() : data_(""), size_(0) {}
    Slice(const char* d, size_t n) : data_(d), size_(n) {}
    Slice(const std::string& s) : data_(s.data()), size_(s.size()) {}
    Slice(const char* s) : data_(s), size_(std::strlen(s)) {}

    const char* data() const { return data_; }
    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    char operator[](size_t i) const { return data_[i]; }

    std::string ToString() const { return std::string(data_, size_); }

    int compare(const Slice& other) const {
        const size_t min_len = size_ < other.size_ ? size_ : other.size_;
        int r = std::memcmp(data_, other.data_, min_len);
        if (r == 0) {
            if (size_ < other.size_) r = -1;
            else if (size_ > other.size_) r = 1;
        }
        return r;
    }

    bool operator==(const Slice& o) const { return compare(o) == 0; }
    bool operator!=(const Slice& o) const { return compare(o) != 0; }
    bool operator<(const Slice& o) const { return compare(o) < 0; }

private:
    const char* data_;
    size_t size_;
};

}  // namespace minidb
