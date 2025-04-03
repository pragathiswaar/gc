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

void gc_start(GarbageCollector* gc,void* bos);
void gc_start_ext(GarbageCollector* gc,void* bos,size_t initial_size,size_t min_size,double downsize_load_factor,double upsize_load_factor,double sweep_factor);
size_t gc_stop(GarbageCollector* gc);
void gc_pause(GarbageCollector* gc);
void gc_resume(GarbageCollector* gc);
size_t gc_run(GarbageCollector* gc);

#endif
