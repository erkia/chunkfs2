#ifndef __CHUNKFS2_FUSE
#define __CHUNKFS2_FUSE

#define FUSE_USE_VERSION 30
#include <fuse.h>

#if (FUSE_VERSION >= 300)
    #define chunkfs_filler(filler, buf, name, stbuf, offset) (filler(buf, name, stbuf, offset, 0))
#else
    #define chunkfs_filler(filler, buf, name, stbuf, offset) (filler(buf, name, stbuf, offset))
#endif

#endif // __CHUNKFS2_FUSE
