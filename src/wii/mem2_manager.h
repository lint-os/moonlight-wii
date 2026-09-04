// MEM2 (video memory) allocator, ported from WiiMC-SSLC source/utils/mem2_manager.
// Carves regions out of the MEM2 arena (SYS_GetArena2Hi/Lo) and manages each with
// libogc's lwp heap. Used for the oversized Y/UV GPU texture buffers, exactly like
// WiiMC's GX_AllocTextureMemory (MEM2_VIDEO area).

#ifndef _MEM2MANAGER_H_
#define _MEM2MANAGER_H_

#include <ogc/system.h>
#include <gctypes.h>
#include <gcbool.h>

enum mem2_areas_enum {
  MEM2_VIDEO,
  MEM2_MAX
};

bool  AddMem2Area(u32 size, const int index);
bool  RemoveMem2Area(const int area);
void  ClearMem2Area(const int area);
void* mem2_memalign(u8 align, u32 size, const int area);
void* mem2_malloc(u32 size, const int area);
void* mem2_calloc(u32 num, u32 size, const int area);
void* mem2_realloc(void* ptr, u32 newsize, const int area);
void  mem2_free(void* ptr, const int area);
u32   mem2_size(const int area);

#endif
