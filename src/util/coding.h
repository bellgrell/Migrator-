#pragma once

#include <sctdint>
#include <cstring>
#include <string>

#include "minidb/slice.h"

namespace minidb{

    void PutFixed32(std::string* dst, uint32_t value);
    void PutFixed64(std::string* dst, uint64_t value);
    void PutVarint32(std::string* dst, uint32_t value);
    void PutVarint64(std::string* dst, uint64_t value);
    void PutLenthPrefixedSlice(const std::string dst, const Slice& value);

    bool GetVarint32(Slice* input, uint32_t* value);
    bool GetVarint64(Slice* input, uint64_t* value);
    bool GetLenthPrefixedSlice(Slice* input, Slice* result);

    const char* GetVarint32Ptr(const char* p, const char* limit, uint32_t* value);
    const char* GetVarint64Ptr(const char* p, const char* limit, uint64_t* value);
}