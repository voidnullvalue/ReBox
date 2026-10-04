#ifndef HR54_COMPOSITOR_PROTOCOL_H
#define HR54_COMPOSITOR_PROTOCOL_H
#include <stdint.h>
/* Distinct namespaces. Values never identify one another implicitly. */
typedef uint32_t Hr54VirtualBitmapId;
typedef uint32_t Hr54HardwareSurfaceId;
typedef uint32_t Hr54DrawlistSurfaceId;
typedef uint16_t Hr54DrawlistTextureHandle;
enum {
    HR54_LOCAL_DRAWLIST_HOST=0,
    HR54_DRAWLIST_SHM_KEY=0x8719,
    HR54_DRAWLIST_SHM_BYTES=0x754848,
    HR54_DRAWLIST_SURFACE_BASE=0x210,
    HR54_DRAWLIST_SURFACE_BYTES=60,
    HR54_DRAWLIST_MAX_SURFACES=0xa50,
    HR54_UMP_GET_VIRTUAL_SURFACE=7,
    HR54_UMP_DAMAGE_REGION=12,
    HR54_UMP_SET_VIRTUAL_SURFACE_Z=41,
    HR54_VIRTUAL_SURFACE_HIDDEN_Z=-1
};
/* Offset descriptors for OFFLINE analysis, not structs to write into shared
 * memory. The vendor library owns synchronization, references, and queues. */
enum {
    HR54_DL_NEXT=0x00, HR54_DL_TARGET_TEXTURE=0x04, HR54_DL_DEPTH=0x08,
    HR54_DL_RESOLUTION=0x14, HR54_DL_HOST=0x24, HR54_DL_CLIENT=0x25,
    HR54_DL_LOCK=0x28, HR54_DL_PENDING_COUNT=0x2c,
    HR54_DL_BUILD_FRAME=0x30, HR54_DL_PENDING_HEAD=0x32,
    HR54_DL_PENDING_TAIL=0x34, HR54_DL_RETAINED_FRAME=0x36,
    HR54_DL_FLAGS=0x3a, HR54_DL_DESTROY_FLAG=0x40
};
#endif
