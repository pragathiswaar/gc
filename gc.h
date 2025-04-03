#ifndef __GC_H__
#define __GC_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct AllocationMap;

typedef struct GarbageCollector{
    struct AllocationMap* allocs;
    bool paused;
    void* bos; 
    size_t min_size;
}GarbageCollector;

extern GarbageCollector gc;

#endif
