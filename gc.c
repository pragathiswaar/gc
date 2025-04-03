#include "gc.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <setjmp.h>

#define GC_TAG_NONE 0x0
#define GC_TAG_ROOT 0x1
#define GC_TAG_MARK 0x2

#define PTRSIZE sizeof(char*)

#ifndef GC_NO_GLOBAL_GC
GarbageCollector gc; 
#endif


typedef struct Allocation{
    void* ptr;
    size_t size;
    char tag; 
    void (*dtor)(void*); 
    struct Allocation* next;
}Allocation;

static Allocation* gc_allocation_new(void* ptr,size_t size,void (*dtor)(void*)){
    Allocation* a=(Allocation*)malloc(sizeof(Allocation));
    a->ptr=ptr;
    a->size=size;
    a->tag=0;
    a->dtor=dtor;
    a->next=NULL;
    return a;
}

static void gc_allocation_delete(Allocation* a){
    free(a);
}

typedef struct AllocationMap{
    size_t capacity; 
    size_t min_capacity; 
    double downsize_factor; 
    double upsize_factor; 
    double sweep_factor; 
    size_t sweep_limit; 
    size_t size; 
    Allocation** allocs; 
}AllocationMap;

void gc_start(GarbageCollector* gc, void* bos){
    gc_start_ext(gc, bos, 1024, 1024, 0.2, 0.8, 0.5);
}

void gc_start_ext(GarbageCollector* gc,void* bos,size_t initial_capacity,size_t min_capacity,double downsize_load_factor,double upsize_load_factor,double sweep_factor){
    double downsize_limit = downsize_load_factor > 0.0 ? downsize_load_factor : 0.2;
    double upsize_limit = upsize_load_factor > 0.0 ? upsize_load_factor : 0.8;
    sweep_factor = sweep_factor > 0.0 ? sweep_factor : 0.5;
    gc->paused = false;
    gc->bos = bos;
    initial_capacity = initial_capacity < min_capacity ? min_capacity : initial_capacity;
    gc->allocs = gc_allocation_map_new(min_capacity, initial_capacity,sweep_factor, downsize_limit, upsize_limit);
}

void gc_pause(GarbageCollector* gc){
    gc->paused = true;
}

void gc_resume(GarbageCollector* gc){
    gc->paused = false;
}

size_t gc_stop(GarbageCollector* gc){
    gc_unroot_roots(gc);
    size_t collected = gc_sweep(gc);
    gc_allocation_map_delete(gc->allocs);
    return collected;
}

size_t gc_run(GarbageCollector* gc){
    gc_mark(gc);
    return gc_sweep(gc);
}

char* gc_strdup (GarbageCollector* gc, const char* s){
    size_t len = strlen(s) + 1;
    void *new = gc_malloc(gc, len);

    if (new == NULL) {
        return NULL;
    }
    return (char*) memcpy(new, s, len);
}
