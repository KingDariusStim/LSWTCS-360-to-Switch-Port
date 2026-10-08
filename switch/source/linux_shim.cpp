// Switch implementations of the Linux syscalls Xenia's base library uses (mutex.cc):
// futex wait/wake -> svcWaitForAddress / svcSignalToAddress, gettid -> kernel thread id.
#include <switch.h>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include "../include/linuxshim/linux/futex.h"
#include "../include/linuxshim/sys/syscall.h"

extern "C" long syscall(long number, ...) {
    va_list ap;
    va_start(ap, number);
    long rc = -1;
    if (number == SYS_futex) {
        void* addr = va_arg(ap, void*);
        int op = va_arg(ap, int) & ~FUTEX_PRIVATE_FLAG;
        int val = va_arg(ap, int);
        if (op == FUTEX_WAIT) {
            Result r = svcWaitForAddress(addr, ArbitrationType_WaitIfEqual, val, -1);
            if (R_SUCCEEDED(r)) rc = 0;
            else { errno = EAGAIN; rc = -1; }   // value changed / interrupted: caller re-checks
        } else if (op == FUTEX_WAKE) {
            svcSignalToAddress(addr, SignalType_Signal, 0, val);
            rc = 0;
        } else errno = ENOSYS;
    } else if (number == SYS_gettid) {
        u64 id = 0;
        svcGetThreadId(&id, CUR_THREAD_HANDLE);
        rc = (long)id;
    } else errno = ENOSYS;
    va_end(ap);
    return rc;
}
