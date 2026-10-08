// Switch overrides for Xenia's FFmpeg config (xenia-canary/third_party/ffmpeg-xenia/config.h):
// its non-Windows branch describes Linux desktop. Turn off what newlib/libnx lacks.
#include_next "config.h"
#undef HAVE_POSIX_MEMALIGN
#define HAVE_POSIX_MEMALIGN 0   // newlib: memalign only
#undef HAVE_MEMALIGN
#define HAVE_MEMALIGN 1
#undef CONFIG_ICONV
#define CONFIG_ICONV 0
#undef HAVE_MMAP
#define HAVE_MMAP 0
#undef HAVE_MPROTECT
#define HAVE_MPROTECT 0
#undef HAVE_SCHED_GETAFFINITY
#define HAVE_SCHED_GETAFFINITY 0
#undef HAVE_SYSCONF
#define HAVE_SYSCONF 0
#undef HAVE_GETRUSAGE
#define HAVE_GETRUSAGE 0
#undef HAVE_SETRLIMIT
#define HAVE_SETRLIMIT 0
#undef HAVE_LINUX_PERF_EVENT_H
#define HAVE_LINUX_PERF_EVENT_H 0
#undef HAVE_SYS_RESOURCE_H
#define HAVE_SYS_RESOURCE_H 0
#undef HAVE_TERMIOS_H
#define HAVE_TERMIOS_H 0
#undef HAVE_ASM_TYPES_H
#define HAVE_ASM_TYPES_H 0
#undef HAVE_PTHREAD_CANCEL
#define HAVE_PTHREAD_CANCEL 0
#undef HAVE_SYMVER
#define HAVE_SYMVER 0
#undef HAVE_SYS_SOUNDCARD_H
#define HAVE_SYS_SOUNDCARD_H 0
#undef HAVE_GLOB
#define HAVE_GLOB 0
#undef HAVE_SECTION_DATA_REL_RO
#define HAVE_SECTION_DATA_REL_RO 0
#undef HAVE_STRUCT_RUSAGE_RU_MAXRSS
#define HAVE_STRUCT_RUSAGE_RU_MAXRSS 0
