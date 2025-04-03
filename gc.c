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

static double gc_allocation_map_load_factor(AllocationMap* am){
    return (double)am->size/(double)am->capacity;
}

static bool is_prime(size_t n){
    if(n<=1){
        return false;
    }
    if(n<=3){
        return true;    
    }
    if(n%2==0 || n%3==0){
        return false;
    }
    for(size_t i=5;i*i<=n;i+=6){ 
        if(n%i==0 || n%(i+2)==0){ 
            return false;
        }
    }
    return true;
}

static size_t next_prime(size_t n){
    while(!is_prime(n)){
        ++n;
    }
    return n;
}

static AllocationMap* gc_allocation_map_new(size_t min_capacity,size_t capacity,double sweep_factor,double downsize_factor,double upsize_factor){
    AllocationMap* am=(AllocationMap*)malloc(sizeof(AllocationMap));
    am->min_capacity=next_prime(min_capacity);
    am->capacity=next_prime(capacity);
    if(am->capacity < am->min_capacity){
        am->capacity=am->min_capacity;
    }
    am->sweep_factor=sweep_factor;
    am->sweep_limit=(int)(sweep_factor * am->capacity);
    am->downsize_factor=downsize_factor;
    am->upsize_factor=upsize_factor;
    am->allocs=(Allocation**)calloc(am->capacity,sizeof(Allocation*));
    am->size=0;
    return am;
}

static void gc_allocation_map_delete(AllocationMap* am){
    Allocation *alloc,*tmp;
    for(size_t i=0;i<am->capacity;++i){
        if((alloc=am->allocs[i])){
            while(alloc){
                tmp=alloc;
                alloc=alloc->next;
                gc_allocation_delete(tmp);
            }
        }
    }
    free(am->allocs);
    free(am);
}

static size_t gc_hash(void* ptr){
    return ((uintptr_t)ptr)>>3;
}

static void gc_allocation_map_resize(AllocationMap* am, size_t new_capacity){
    if (new_capacity <= am->min_capacity){
        return;
    }
    Allocation** resized_allocs=calloc(new_capacity, sizeof(Allocation*));
    for (size_t i=0;i<am->capacity;++i){
        Allocation* alloc=am->allocs[i];
        while(alloc){
            Allocation* next_alloc=alloc->next;
            size_t new_index=gc_hash(alloc->ptr)%new_capacity;
            alloc->next=resized_allocs[new_index];
            resized_allocs[new_index]=alloc;
            alloc=next_alloc;
        }
    }
    free(am->allocs);
    am->capacity=new_capacity;
    am->allocs=resized_allocs;
    am->sweep_limit=am->size+am->sweep_factor*(am->capacity - am->size);
}

static bool gc_allocation_map_resize_to_fit(AllocationMap* am){
    double load_factor = gc_allocation_map_load_factor(am);
    if (load_factor > am->upsize_factor) {
        gc_allocation_map_resize(am, next_prime(am->capacity * 2));
        return true;
    }
    if (load_factor < am->downsize_factor) {
        gc_allocation_map_resize(am, next_prime(am->capacity / 2));
        return true;
    }
    return false;
}

static Allocation* gc_allocation_map_get(AllocationMap* am, void* ptr){
    size_t index = gc_hash(ptr) % am->capacity;
    Allocation* cur = am->allocs[index];
    while(cur) {
        if (cur->ptr == ptr) {
            return cur;
        }
        cur = cur->next;
    }
    return NULL;
}

static Allocation* gc_allocation_map_put(AllocationMap* am,void* ptr,size_t size,void (*dtor)(void*)){
    size_t index = gc_hash(ptr) % am->capacity;
    Allocation* alloc = gc_allocation_new(ptr, size, dtor);
    Allocation* cur = am->allocs[index];
    Allocation* prev = NULL;
    while(cur != NULL) {
        if (cur->ptr == ptr) {
            alloc->next = cur->next;
            if (!prev) {
                am->allocs[index] = alloc;
            } else {
                prev->next = alloc;
            }
            gc_allocation_delete(cur);
            return alloc;
        }
        prev = cur;
        cur = cur->next;
    }
    cur = am->allocs[index];
    alloc->next = cur;
    am->allocs[index] = alloc;
    am->size++;
    void* p = alloc->ptr;
    if (gc_allocation_map_resize_to_fit(am)) {
        alloc = gc_allocation_map_get(am, p);
    }
    return alloc;
}

static void gc_allocation_map_remove(AllocationMap* am,void* ptr,bool allow_resize){
    size_t index = gc_hash(ptr) % am->capacity;
    Allocation* cur = am->allocs[index];
    Allocation* prev = NULL;
    Allocation* next;
    while(cur != NULL) {
        next = cur->next;
        if (cur->ptr == ptr) {
            if (!prev) {
            am->allocs[index] = cur->next;
            } else {
            prev->next = cur->next;
            }
            gc_allocation_delete(cur);
            am->size--;
        } 
        else {
            prev = cur;
        }
        cur = next;
    }
    if (allow_resize) {
    gc_allocation_map_resize_to_fit(am);
    }
}

static void* gc_mcalloc(size_t count, size_t size){
    if (!count) return malloc(size);
    return calloc(count, size);
}

static bool gc_needs_sweep(GarbageCollector* gc){
    return gc->allocs->size > gc->allocs->sweep_limit;
}

static void* gc_allocate(GarbageCollector* gc, size_t count, size_t size, void(*dtor)(void*)){
    if (gc_needs_sweep(gc) && !gc->paused) {
        size_t freed_mem = gc_run(gc);
    }
    void* ptr = gc_mcalloc(count, size);
    size_t alloc_size = count ? count * size : size;
    if (!ptr && !gc->paused && (errno == EAGAIN || errno == ENOMEM)) {
        gc_run(gc);
        ptr = gc_mcalloc(count, size);
    }
    if (ptr) {
        Allocation* alloc = gc_allocation_map_put(gc->allocs, ptr, alloc_size, dtor);
        if (alloc) {
            ptr = alloc->ptr;
        } else {
            free(ptr);
            ptr = NULL;
        }
    }
    return ptr;
}

static void gc_make_root(GarbageCollector* gc, void* ptr){
    Allocation* alloc = gc_allocation_map_get(gc->allocs, ptr);
    if (alloc) {
        alloc->tag |= GC_TAG_ROOT;
    }
}

void* gc_malloc(GarbageCollector* gc, size_t size){
    return gc_malloc_ext(gc, size, NULL);
}

void* gc_malloc_static(GarbageCollector* gc, size_t size, void(*dtor)(void*)){
    void* ptr = gc_malloc_ext(gc, size, dtor);
    gc_make_root(gc, ptr);
    return ptr;
}

void* gc_make_static(GarbageCollector* gc, void* ptr){
    gc_make_root(gc, ptr);
    return ptr;
}

void* gc_malloc_ext(GarbageCollector* gc, size_t size, void(*dtor)(void*)){
    return gc_allocate(gc, 0, size, dtor);
}


void* gc_calloc(GarbageCollector* gc, size_t count, size_t size){
    return gc_calloc_ext(gc, count, size, NULL);
}


void* gc_calloc_ext(GarbageCollector* gc, size_t count, size_t size,void(*dtor)(void*)){
    return gc_allocate(gc, count, size, dtor);
}

void* gc_realloc(GarbageCollector* gc, void* p, size_t size){
    Allocation* alloc = gc_allocation_map_get(gc->allocs, p);
    if (p && !alloc) {
        errno = EINVAL;
        return NULL;
    }
    void* q = realloc(p, size);
    if (!q) {
        return NULL;
    }
    if (!p) {
        Allocation* alloc = gc_allocation_map_put(gc->allocs, q, size, NULL);
        return alloc->ptr;
    }
    if (p == q) {
        alloc->size = size;
    } else {
        void (*dtor)(void*) = alloc->dtor;
        gc_allocation_map_remove(gc->allocs, p, true);
        gc_allocation_map_put(gc->allocs, q, size, dtor);
    }
    return q;
}

void gc_free(GarbageCollector* gc, void* ptr){
    Allocation* alloc = gc_allocation_map_get(gc->allocs, ptr);
    if (alloc) {
        if (alloc->dtor) {
            alloc->dtor(ptr);
        }
        free(ptr);
        gc_allocation_map_remove(gc->allocs, ptr, true);
    } else {
    }
}

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

void gc_mark_alloc(GarbageCollector* gc, void* ptr){
    Allocation* alloc = gc_allocation_map_get(gc->allocs, ptr);
    if (alloc && !(alloc->tag & GC_TAG_MARK)) {
        alloc->tag |= GC_TAG_MARK;
        for (char* p = (char*) alloc->ptr;p <= (char*) alloc->ptr + alloc->size - PTRSIZE;++p) {
            gc_mark_alloc(gc, *(void**)p);
        }
    }
}

void gc_mark_stack(GarbageCollector* gc){
    void *tos = __builtin_frame_address(0);
    void *bos = gc->bos;
    for (char* p = (char*) tos; p <= (char*) bos - PTRSIZE; ++p) {
        gc_mark_alloc(gc, *(void**)p);
    }
}

void gc_mark_roots(GarbageCollector* gc){
    for (size_t i = 0; i < gc->allocs->capacity; ++i) {
        Allocation* chunk = gc->allocs->allocs[i];
        while (chunk) {
            if (chunk->tag & GC_TAG_ROOT) {
                gc_mark_alloc(gc, chunk->ptr);
            }
            chunk = chunk->next;
        }
    }
}

void gc_mark(GarbageCollector* gc){
    gc_mark_roots(gc);
    void (*volatile _mark_stack)(GarbageCollector*) = gc_mark_stack;
    jmp_buf ctx;
    memset(&ctx, 0, sizeof(jmp_buf));
    setjmp(ctx);
    _mark_stack(gc);
}

size_t gc_sweep(GarbageCollector* gc){
    size_t total = 0;
    for (size_t i = 0; i < gc->allocs->capacity; ++i) {
        Allocation* chunk = gc->allocs->allocs[i];
        Allocation* next = NULL;
        while (chunk) {
            if (chunk->tag & GC_TAG_MARK) {
                chunk->tag &= ~GC_TAG_MARK;
                chunk = chunk->next;
            } else {
                total += chunk->size;
                if (chunk->dtor) {
                    chunk->dtor(chunk->ptr);
                }
                free(chunk->ptr);
                next = chunk->next;
                gc_allocation_map_remove(gc->allocs, chunk->ptr, false);
                chunk = next;
            }
        }
    }
    gc_allocation_map_resize_to_fit(gc->allocs);
    return total;
}


void gc_unroot_roots(GarbageCollector* gc){
    for (size_t i = 0; i < gc->allocs->capacity; ++i) {
        Allocation* chunk = gc->allocs->allocs[i];
        while (chunk) {
            if (chunk->tag & GC_TAG_ROOT) {
                chunk->tag &= ~GC_TAG_ROOT;
            }
            chunk = chunk->next;
        }
    }
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
    return (char*) memcpy(new, s, len);
}
