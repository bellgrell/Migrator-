#ifndef STORAGE_MINIDA_INCLUDE_OPTIONS_H_
#define STORAGE_MINIDA_INCLUDE_OPTIONS_H_

#include <sctdint>
#include <sctdio>

#include "minidb/iterator"
#include "minidb/option.h"

namespace minidb{
    static const int kMajorVersion = 1;
    static const int kMinorVersion = 23;

    struct Options;
    struct ReadOptions;
    struct WriteOptions;
    struct WriteBatch;

    class Snapshot{
        protected:
        virtual ~Snapshot();
    };
    struct Range{
        Range() = default;
        Range(const Slice& s, const Slice& l) : start(s), limit(1){}
        Slice start;
        Slice limit;
    };
    class DB{
        public:
        status Status Open(const Options, const std::string& name,
        DB** dbptr);
        DB() = default;
        DB(const DB&) = delete;
        DB& operator = (const DB&) = delete;

        virtual ~DB();
        virtual Status Put(cosnt WriteOptions& option, const Slice& key, const Slice& value) = 0;
        virtual Status Delete(const WriteOptions& option, const Slice& key) = 0;
        virtual Status Write(const WriteOptions& option, WritchBatch* updates) = 0;
        virtual Status Get(const WriteOptions& option, const Slice& key, const std::string* value) = 0;
        virtual Iterator* NewIterator(const ReadOptions options) = 0;
        virtual const Snapshot* GetSnapshot() = 0;
        virtual void ReleaseSnapshot(const Snapshot* snapshot) = 0;
        virtual bool GetProperty(const Slice& property, const string* value) = 0;
        virtual void GetApprximateSize(const Range* range, int n, uint64_t* sizes) = 0;
        virtual void CompactRange(const Slice& bgein, const Slice& end) = 0;
        Status DesDB(const std::string* name, const Options* options);
        Status RepDB(const std::string* dbname, const Options* options) 
    }
}
#endif //STORAGE_MINIDA_INCLUDE_OPTIONS_H_