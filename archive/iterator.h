#ifndef STORAGE_MINIDA_INCLUDE_OPTIONS_H_
#define STORAGE_MINIDA_INCLUDE_OPTIONS_H_

#include "slice.h"
#include "status.h"

namespace miniDA{
    class Iterator{
        public:
            Iterator();
            Iterator(const Iterator&) = delete;
            Iterator& operator = (const Iterator) = delete;
            virtual ~Iterator();
            virtual bool Valid() const =  0;
            virtual void SeekToFirst() = 0;
            virtual void SeekTolast() = 0;
            virtual void Seek() = 0;
            virtual void Next() = 0;
            virtual void Prev() = 0;
            virtual Slice key() const = 0;
            virtual Slice value() const = 0;
            virtual Status status() const = 0;
            using CleanFunction = void (*)(void* arg1, void* arg2);
            void RegisterCleanup(CleanFunction function, void*arg1, void* arg2);

            private:
            struct CleanNode{
                bool IsEmpty() const {return function == nullptr;}

                void Run(){
                    assert(function != nullptr);
                    (*function)(arg1, arg2);
                }
                CleanFunction function;
                void* arg1;
                void* arg2;
                CleanNode* next;
            }
            CleanNode clean_head;
    }
    Iterator* NewEmptyIterator();
    Iterator* NewErrorIterator(const Status& status);
}

#endif //STORAGE_MINIDA_INCLUDE_OPTIONS_H_