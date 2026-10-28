#ifndef STORAGE_MINIDA_INCLUDE_OPTIONS_H_
#define STORAGE_MINIDA_INCLUDE_OPTIONS_H_

#include <algorithm>
#include <string>

#include "slice.h"

namespace miniDA{
class MINIDB_EXPORT{
    public:
    Status() noexcept : state_(nullptr) {}
    ~Status() {delete[] state_;}

    Status(const Status& rhs);
    Status& operator = (const Status& rhs);

    Status(Status&& rhs) noexcept : state_(rhs.state_) {rhs.state_ = nullptr;}
    Status& operator = (Status&& rhs) noexcept;

    //return a success status.
    static Status OK() { return Status();}

    //return an error status of an appropriate type
    static Status NotFound(const Slice& msg, const Slice& msg2 = Slice()){
        return Status(kNotFound, msg, msg2);
    }
    static Status Corruption(const Slice& msg, const Slice& msg2 = Slice()){
        return Status(kCorruption, msg, msg2);
    }
    static Status NotSupported(const Slice& msg, const Slice& msg2 = Slice()){
        return Status(kNotSupported, msg, msg2);
    }
    static Status InvalidArgument(const Slice& msg, const Slice& msg2 = Slice()){
        return Status(kInvalidArgument, msg, msg2);
    }
    static Status IOError(const Slice& msg, const Slice& msg2 = Slice()){
        return Status(kIOError, msg, msg2);
    }
    bool ok() const { return (state_ == nullptr);}

    bool IsNotFound() const {return code() == kNotFound;}
    bool IsCorruption() const {return code() == kCorruption;}
    bool IsNotSupported() const {return code() == kNotSupported;}
    bool IsInvalidArgument() const {return code() == kInvalidArgument;}
    bool IsIOError() const {return code() == kIOError;}

    std::string ToString() const;

    private:
    enum Code{
        kOk = 0,
        kNotFound = 1,
        kCorruption = 2,
        kNotSupported = 3,
        kInvalidArgument = 4,
        kIOError = 5
    };
    Code code() const{
        return(state_ == nullptr) ? kOk :static_cast<Code>(state_[4]);
    }
    Status (Code code, const Slice& msg, const Slice& msg2);
    static const char* CopyState(const char* s);
    const char* state_;
}
    inline Status::Status(const Status& rhs){
        state_ = (rhs.state_ == nullptr) ? nullptr : CopyState(rhs.state_);
    }

    inline Status& Status::operator = (cosnt Status& rhs){
        if(state_ != rhs.state_){
            delete[] state_;
            state_ = (rhs.state_ == nullptr) ? nullptr : CopyState(rhs.state_);
        }
        return *this;
    }

    inline Status& Status::operator = (Status&& rhs) noexcept {
        std::swap(state_, rhs.state_);
        return *this;
    }

}
#endif //STORAGE_MINIDA_INCLUDE_OPTIONS_H_