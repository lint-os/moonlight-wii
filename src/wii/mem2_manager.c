// MEM2 (video memory) allocator, ported from WiiMC-SSLC source/utils/mem2_manager.c.
// Core carve-from-arena + lwp-heap logic is unchanged from the original; the
// debug (file/line) plumbing and the build-specific memcpy wrap are dropped.

#include <ogc/lwp_heap.h>
#include <ogc/system.h>
#include <ogc/machine/processor.h>
#include <string.h>
#include "mem2_manager.h"

// libogc lwp_heap inline helpers (mirror of lwp_heap.inl)
static __inline__ heap_block* __lwp_heap_blockat(heap_block *block, u32 offset) {
  return (heap_block*)((char*)block + offset);
}
static __inline__ heap_block* __lwp_heap_usrblockat(void *ptr) {
  u32 offset = *(((u32*)ptr)-1);
  return __lwp_heap_blockat(ptr, -offset+-HEAP_BLOCK_USED_OVERHEAD);
}
static __inline__ bool __lwp_heap_blockin(heap_cntrl *heap, heap_block *block) {
  return ((u32)block>=(u32)heap->start && (u32)block<=(u32)heap->final);
}
static __inline__ bool __lwp_heap_blockfree(heap_block *block) {
  return !(block->front_flag&HEAP_BLOCK_USED);
}
static __inline__ u32 __lwp_heap_blocksize(heap_block *block) {
  return (block->front_flag&~HEAP_BLOCK_USED);
}

static u32 __lwp_heap_block_size(heap_cntrl *theheap, void *ptr) {
  heap_block *block;
  u32 dsize, level;

  _CPU_ISR_Disable(level);
  block = __lwp_heap_usrblockat(ptr);
  if(!__lwp_heap_blockin(theheap,block) || __lwp_heap_blockfree(block)) {
    _CPU_ISR_Restore(level);
    return 0;
  }
  dsize = __lwp_heap_blocksize(block);
  _CPU_ISR_Restore(level);
  return dsize;
}

typedef struct {
  heap_cntrl heap;
  void *heap_ptr;
  u32 size;
  void *old_arena2hi;
} st_mem2_area;

static bool _inited = false;
static st_mem2_area mem2_areas[MEM2_MAX];

#define ROUNDDOWN32(v) (((u32)(v)-0x1f)&~0x1f)

static void initMem2Areas(void) {
  int i;
  for(i = 0; i < MEM2_MAX; i++) {
    mem2_areas[i].heap_ptr = NULL;
    mem2_areas[i].size = 0;
    mem2_areas[i].old_arena2hi = NULL;
  }
  _inited = true;
}

void ClearMem2Area(const int area) {
  if(area >= MEM2_MAX)
    return;
  if(mem2_areas[area].size == 0) return;
  memset(mem2_areas[area].heap_ptr, 0, mem2_areas[area].size);
  __lwp_heap_init(&mem2_areas[area].heap, mem2_areas[area].heap_ptr, mem2_areas[area].size, 32);
}

bool AddMem2Area(u32 size, const int index) {
  u32 level;

  _CPU_ISR_Disable(level);

  if(!_inited)
    initMem2Areas();

  if(index >= MEM2_MAX || size == 0) {
    _CPU_ISR_Restore(level);
    return false;
  }

  if(mem2_areas[index].size == size) {
    _CPU_ISR_Restore(level);
    return true;
  }

  if(mem2_areas[index].size > 0 && !RemoveMem2Area(index)) {
    _CPU_ISR_Restore(level);
    return false;
  }

  mem2_areas[index].old_arena2hi = SYS_GetArena2Hi();
  mem2_areas[index].heap_ptr = (void *)ROUNDDOWN32(((u32)SYS_GetArena2Hi() - size));

  if((u32)mem2_areas[index].heap_ptr < (u32)SYS_GetArena2Lo()) {
    mem2_areas[index].old_arena2hi = NULL;
    mem2_areas[index].heap_ptr = NULL;
    _CPU_ISR_Restore(level);
    return false; // not enough mem2
  }

  SYS_SetArena2Hi(mem2_areas[index].heap_ptr);
  __lwp_heap_init(&mem2_areas[index].heap, mem2_areas[index].heap_ptr, size, 32);
  mem2_areas[index].size = size;
  _CPU_ISR_Restore(level);
  return true;
}

bool RemoveMem2Area(const int area) {
  u32 level;
  int i;

  _CPU_ISR_Disable(level);

  if(area >= MEM2_MAX || mem2_areas[area].size == 0) {
    _CPU_ISR_Restore(level);
    return false;
  }

  // a lower area is already inited - we cannot deinit this one yet
  for(i = 0; i < MEM2_MAX; i++) {
    if(i == area || mem2_areas[i].old_arena2hi == NULL)
      continue;
    if(mem2_areas[i].old_arena2hi < mem2_areas[area].old_arena2hi) {
      _CPU_ISR_Restore(level);
      return false;
    }
  }

  SYS_SetArena2Hi(mem2_areas[area].old_arena2hi);
  memset(&(mem2_areas[area].heap), 0, sizeof(heap_cntrl));
  mem2_areas[area].heap_ptr = NULL;
  mem2_areas[area].size = 0;
  mem2_areas[area].old_arena2hi = NULL;
  _CPU_ISR_Restore(level);
  return true;
}

// The lwp heap is initialized with a 32-byte page size, so every allocation is
// 32-byte aligned; the align argument is accepted for API parity and ignored.
void* mem2_memalign(u8 align, u32 size, const int area) {
  (void)align;
  if(size == 0)
    return NULL;
  if(area >= MEM2_MAX || mem2_areas[area].size == 0)
    return NULL;
  return __lwp_heap_allocate(&mem2_areas[area].heap, size);
}

void* mem2_malloc(u32 size, const int area) {
  return mem2_memalign(32, size, area);
}

void* mem2_calloc(u32 num, u32 size, const int area) {
  void *ptr = mem2_malloc(num*size, area);
  if(ptr == NULL) return NULL;
  memset(ptr, 0, num*size);
  return ptr;
}

void* mem2_realloc(void *ptr, u32 newsize, const int area) {
  void *newptr = NULL;

  if(ptr == NULL) return mem2_malloc(newsize, area);
  if(newsize == 0) {
    mem2_free(ptr, area);
    return NULL;
  }
  if(area >= MEM2_MAX || mem2_areas[area].size == 0)
    return NULL;

  u32 size = __lwp_heap_block_size(&mem2_areas[area].heap, ptr);
  if(size > newsize) size = newsize;

  newptr = mem2_malloc(newsize, area);
  if(newptr == NULL) return NULL;
  memcpy(newptr, ptr, size);
  mem2_free(ptr, area);
  return newptr;
}

void mem2_free(void *ptr, const int area) {
  if(!ptr)
    return;
  if(area >= MEM2_MAX || mem2_areas[area].size == 0)
    return;
  __lwp_heap_free(&mem2_areas[area].heap, ptr);
}

u32 mem2_size(const int area) {
  heap_iblock info;
  if(area >= MEM2_MAX || mem2_areas[area].size == 0)
    return 0;
  __lwp_heap_getinfo(&mem2_areas[area].heap, &info);
  return info.free_size;
}
