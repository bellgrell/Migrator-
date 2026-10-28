#ifndef STORAGE_MINIDA_INCLUDE_OPTIONS_H_
#define STORAGE_MINIDA_INCLUDE_OPTIONS_H_

#include <cassert>
#include <cstddef>
#include <cstring>
#include <string>

#include ""

namespace miniDA{
    class miniDA_export{
    public:
    //create a empty slice
        Slice() : data_(""),size_(0) {}
        //create a slice refers to d[0,n-1]
        Slice(const char* d, size_t n) : data_(d), size_(n){}
        //create a slice that refers to the contents of "s"
        Slice(const std::string& s): data_(s.data()), size_t(s.size()){}
        //create a slice that refers to s[0, strlen(s)-1]
        Slice(const char* s) : data_(s), size_(strlen(s)){}
        //intentionnally copyable
        Slice(const Slice&) = default;
        Slice& operator = (const Slice&) = default;

        //return a pointer to the beginning of the referencd data
        const char* data() const {return data_;}

        //return the length (in bytes) of the referenced data
        size_t size() const {return size_;}

        //return true iff the lenth of the referenced data is zero
        bool empty() const {return size_ == 0;}

        const char* begin() const {return data();}
        const char* end() const {return data() + size();}

        //return the ith byte in the referenced data (n<size())
        char operator[](size_t n) const {
            assert (n < size());
            return data_[n];
        }

        //change this slice to refer to an empty array
        void clear(){
            data_ = "";
            size_ = 0;

        }
        //Drop the first "n" bytes from this slice
        void  remove_prefix(size_t n){
            assert(n <= size());
            data_ += n;
            size_ -= n;  
        }
        //return a string that contains the copy of the referenced data
        std::string ToString()const {return std::string(data_, size_);}

        //three-way comparison return value
        int compare(const Slice& b) const;

        //return true iff "x" is a prefix of "this"
        bool starts_with(const Slice& x) const {
            return ((size_ >= x.size_) && (memcmp(data_, x.data_, x.size_) == 0));
        }
    private:
        const char* data_;
        size_t size_;

}
    inline bool operator == (const Slice& x, const Slice& y){
        return((x.size() == y.size()) && (memcmp(x.data(), y.data(), x.size()) == 0));
    }
    inline bool operator != (const Slice& x, const Slice& y){return !(x == y);}

    inline int Slice::compare(const Slice& b) const {
        const size_t min_len = (size_ < b.size_) ? size_ : b.size_;
        int r = memcmp(data_, b.data_, min_len);
        if(r == 0){
            if(size_ < b.size_)
            r = -1;
            else if(size_ > b.size_)
            r = +1;
        }
        return r;
    }
}

#endif //STORAGE_MINIDA_INCLUDE_OPTIONS_H_