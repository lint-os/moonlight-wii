/*
 * Hand-written config.h for the prebuilt Wii ffmpeg 0.10 static libraries
 * (libavcodec.a / libavutil.a / libswscale.a from the WiiMC-SSLC mplayer
 * build, devkitPPC release 24). It provides the macros the public ffmpeg
 * headers expect. Values match the Wii (PowerPC 750, big endian) target.
 */
#ifndef FFMPEG_CONFIG_H
#define FFMPEG_CONFIG_H

#define HAVE_AV_CONFIG_H 1

#define ARCH_PPC 1
#define ARCH_32BIT 1
#define ARCH_64BIT 0

#define CONFIG_PPC 1
#define CONFIG_PPC750 1
#define CONFIG_PPC4XX 0
#define CONFIG_ALTIVEC 1
#define CONFIG_PPC_PERM 1

#define CONFIG_AVUTIL 1
#define CONFIG_AVCODEC 1
#define CONFIG_SWSCALE 1
#define CONFIG_STATIC 1
#define CONFIG_SHARED 0
#define CONFIG_PIC 0
#define CONFIG_SMALL 0
#define CONFIG_FASTDIV 1

#define HAVE_BIGENDIAN 1
#define HAVE_INLINE_ASM 1
#define HAVE_FAST_CLZ 1
#define HAVE_FAST_UNALIGNED 0
#define HAVE_LOCAL_ALIGNED_8 1
#define HAVE_LOCAL_ALIGNED_16 1

#define HAVE_CBRTF 1
#define HAVE_EXP2 1
#define HAVE_EXP2F 1
#define HAVE_LLRINT 1
#define HAVE_LLRINTF 1
#define HAVE_LRINT 1
#define HAVE_LRINTF 1
#define HAVE_LOG2 1
#define HAVE_LOG2F 1
#define HAVE_ROUND 1
#define HAVE_ROUNDF 1
#define HAVE_TRUNC 1
#define HAVE_TRUNCF 1

#define HAVE_GETHRTIME 0
#define HAVE_GETTIMEOFDAY 1
#define HAVE_LOCALTIME 1
#define HAVE_GMTIME 1
#define HAVE_MKTIME 1
#define HAVE_MKSTEMP 1
#define HAVE_TIME 1
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_UNISTD_H 1
#define HAVE_MEMALIGN 1
#define HAVE_MALLINFO 1
#define HAVE_POSIX_MEMALIGN 1
#define HAVE_SYS_MMAN_H 0

#endif /* FFMPEG_CONFIG_H */
