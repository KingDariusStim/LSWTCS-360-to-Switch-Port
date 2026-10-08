// Switch: the two Linux syscalls Xenia's base library uses (see switch/source/linux_shim.cpp).
// Only on the include path of the vendored Xenia sources.
#pragma once
#define SYS_futex 98
#define SYS_gettid 178
#ifdef __cplusplus
extern "C"
#endif
long syscall(long number, ...);
