// Win32 subset on libnx/newlib: see switch/include/win32/windows.h for the contract.
#include <switch.h>
#include "windows.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <string>
#include <cstring>
#include <dirent.h>
#include <strings.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
extern "C" void lsw_ft_ring_dump();   // kernel_stubs.cpp: game frame times of the last 120 swaps ([FTRING])

namespace {

thread_local DWORD t_last_error = 0;
std::atomic<DWORD> g_next_tid{1};
thread_local DWORD t_tid = 0;

enum class Kind { File, Dir, Find, Event, Semaphore, Mutex, Timer };

struct Obj { Kind kind; explicit Obj(Kind k) : kind(k) {} virtual ~Obj() = default; };

struct FileObj : Obj { int fd; std::string path; FileObj(int f, std::string p) : Obj(Kind::File), fd(f), path(std::move(p)) {} ~FileObj() override { ::close(fd); } };
struct DirObj : Obj { std::string path; explicit DirObj(std::string p) : Obj(Kind::Dir), path(std::move(p)) {} };
struct FindObj : Obj {
    DIR* d; std::string dir, pat;
    FindObj(DIR* dd, std::string di, std::string pa) : Obj(Kind::Find), d(dd), dir(std::move(di)), pat(std::move(pa)) {}
    ~FindObj() override { if (d) closedir(d); }
};

// All waitable state is guarded by one dispatcher lock; any state change notifies every waiter,
// which re-evaluates its own condition (Win32 dispatcher-database model).
std::mutex g_disp;
std::condition_variable g_disp_cv;
using Clock = std::chrono::steady_clock;

struct WaitObj : Obj {
    bool signaled = false, manual = false;
    uint64_t pulse_gen = 0;   // manual-reset PulseEvent: releases waiters that saw the old value
    int waiters = 0;
    LONG count = 0, max = 0;  // semaphore
    DWORD owner = 0; DWORD rec = 0;   // mutex (owner thread id, recursion)
    Clock::time_point due{}; bool armed = false; int64_t period_ms = 0;   // waitable timer
    explicit WaitObj(Kind k) : Obj(k) {}
};

DWORD tid() { if (!t_tid) t_tid = g_next_tid.fetch_add(1); return t_tid; }

Obj* obj(HANDLE h) {
    if (!h || h == INVALID_HANDLE_VALUE || (intptr_t)h == -2) return nullptr;
    return static_cast<Obj*>(h);
}
WaitObj* waitable(HANDLE h) {
    Obj* o = obj(h);
    if (!o || o->kind == Kind::File || o->kind == Kind::Dir || o->kind == Kind::Find) return nullptr;
    return static_cast<WaitObj*>(o);
}
FileObj* file(HANDLE h) { Obj* o = obj(h); return (o && o->kind == Kind::File) ? static_cast<FileObj*>(o) : nullptr; }

// Timer due -> signalled (caller holds g_disp).
void timer_tick(WaitObj* w, Clock::time_point now) {
    if (w->kind != Kind::Timer || !w->armed || now < w->due) return;
    w->signaled = true;
    if (w->period_ms > 0) { do w->due += std::chrono::milliseconds(w->period_ms); while (w->due <= now); }
    else w->armed = false;
}
bool ready(WaitObj* w, DWORD me, Clock::time_point now) {
    switch (w->kind) {
    case Kind::Event: return w->signaled;
    case Kind::Semaphore: return w->count > 0;
    case Kind::Mutex: return w->owner == 0 || w->owner == me;
    case Kind::Timer: timer_tick(w, now); return w->signaled;
    default: return false;
    }
}
void consume(WaitObj* w, DWORD me) {
    switch (w->kind) {
    case Kind::Event: if (!w->manual) w->signaled = false; break;
    case Kind::Semaphore: --w->count; break;
    case Kind::Mutex: w->owner = me; ++w->rec; break;
    case Kind::Timer: if (!w->manual) w->signaled = false; break;
    default: break;
    }
}

std::string norm(const char* p) {
    std::string s = p ? p : "";
    for (char& c : s) if (c == '\\') c = '/';
    return s;
}
void set_err_from_errno(const std::string& path) {
    switch (errno) {
    case ENOENT: {
        size_t sl = path.find_last_of('/');
        struct stat st;
        t_last_error = (sl != std::string::npos && sl > 0 && ::stat(path.substr(0, sl).c_str(), &st) != 0)
                           ? ERROR_PATH_NOT_FOUND : ERROR_FILE_NOT_FOUND;
        break;
    }
    case ENOTDIR: t_last_error = ERROR_PATH_NOT_FOUND; break;
    case EEXIST: t_last_error = ERROR_FILE_EXISTS; break;
    default: t_last_error = ERROR_ACCESS_DENIED; break;
    }
}

constexpr uint64_t kFileTimeUnixEpoch = 116444736000000000ull;   // 1601 -> 1970 in 100 ns units
FILETIME to_ft(uint64_t v) { FILETIME f; f.dwLowDateTime = DWORD(v); f.dwHighDateTime = DWORD(v >> 32); return f; }
uint64_t from_ft(const FILETIME* f) { return (uint64_t(f->dwHighDateTime) << 32) | f->dwLowDateTime; }
FILETIME unix_to_ft(time_t t) { return to_ft(uint64_t(t) * 10000000ull + kFileTimeUnixEpoch); }

// Gregorian calendar <-> day count since 1970-01-01 (H. Hinnant's algorithms).
int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = unsigned(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + int64_t(doe) - 719468;
}
void civil_from_days(int64_t z, int64_t* y, unsigned* m, unsigned* d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = unsigned(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = int64_t(yoe) + era * 400 + (*m <= 2);
}

bool wild_match(const char* p, const char* s) {   // '*' and '?', case-insensitive
    if (!*p) return !*s;
    if (*p == '*') { for (; ; ++s) { if (wild_match(p + 1, s)) return true; if (!*s) return false; } }
    if (!*s) return false;
    if (*p != '?' && tolower((unsigned char)*p) != tolower((unsigned char)*s)) return false;
    return wild_match(p + 1, s + 1);
}
bool find_fill(FindObj* f, WIN32_FIND_DATAA* fd) {
    while (dirent* e = readdir(f->d)) {
        if (!wild_match(f->pat.c_str(), e->d_name)) continue;
        memset(fd, 0, sizeof *fd);
        snprintf(fd->cFileName, sizeof fd->cFileName, "%s", e->d_name);
        struct stat st;
        std::string full = f->dir + e->d_name;
        if (::stat(full.c_str(), &st) == 0) {
            fd->dwFileAttributes = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
            fd->nFileSizeHigh = DWORD(uint64_t(st.st_size) >> 32); fd->nFileSizeLow = DWORD(st.st_size);
            fd->ftLastWriteTime = fd->ftCreationTime = fd->ftLastAccessTime = unix_to_ft(st.st_mtime);
        } else fd->dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
        return true;
    }
    t_last_error = ERROR_NO_MORE_FILES;
    return false;
}

}  // namespace

// ---- errors ------------------------------------------------------------------------------------
DWORD GetLastError(void) { return t_last_error; }
void SetLastError(DWORD e) { t_last_error = e; }

// ---- files -------------------------------------------------------------------------------------
HANDLE CreateFileA(LPCSTR p, DWORD access, DWORD, void*, DWORD disp, DWORD flags, HANDLE) {
    std::string path = norm(p);
    struct stat st;
    bool exists = ::stat(path.c_str(), &st) == 0;
    if (exists && S_ISDIR(st.st_mode)) {
        if (!(flags & FILE_FLAG_BACKUP_SEMANTICS)) { t_last_error = ERROR_ACCESS_DENIED; return INVALID_HANDLE_VALUE; }
        t_last_error = 0;
        return new DirObj(path);
    }
    int of = (access & GENERIC_WRITE) ? ((access & GENERIC_READ) ? O_RDWR : O_WRONLY) : O_RDONLY;
    switch (disp) {
    case CREATE_NEW: of |= O_CREAT | O_EXCL; break;
    case CREATE_ALWAYS: of |= O_CREAT | O_TRUNC; break;
    case OPEN_ALWAYS: of |= O_CREAT; break;
    case TRUNCATE_EXISTING: of |= O_TRUNC; break;
    default: break;
    }
    int fd = ::open(path.c_str(), of, 0666);
    if (fd < 0) { set_err_from_errno(path); return INVALID_HANDLE_VALUE; }
    t_last_error = (exists && (disp == CREATE_ALWAYS || disp == OPEN_ALWAYS)) ? ERROR_ALREADY_EXISTS : 0;
    return new FileObj(fd, path);
}
BOOL ReadFile(HANDLE h, void* buf, DWORD n, DWORD* got, void*) {
    FileObj* f = file(h);
    if (got) *got = 0;
    if (!f) { t_last_error = ERROR_INVALID_HANDLE; return FALSE; }
    DWORD total = 0;
    while (total < n) {
        ssize_t r = ::read(f->fd, (char*)buf + total, n - total);
        if (r < 0) { if (errno == EINTR) continue; t_last_error = ERROR_ACCESS_DENIED; if (got) *got = total; return FALSE; }
        if (r == 0) break;
        total += DWORD(r);
    }
    if (got) *got = total;
    return TRUE;
}
BOOL WriteFile(HANDLE h, const void* buf, DWORD n, DWORD* wrote, void*) {
    FileObj* f = file(h);
    if (wrote) *wrote = 0;
    if (!f) { t_last_error = ERROR_INVALID_HANDLE; return FALSE; }
    DWORD total = 0;
    while (total < n) {
        ssize_t r = ::write(f->fd, (const char*)buf + total, n - total);
        if (r < 0) { if (errno == EINTR) continue; t_last_error = ERROR_ACCESS_DENIED; if (wrote) *wrote = total; return FALSE; }
        total += DWORD(r);
    }
    if (wrote) *wrote = total;
    return TRUE;
}
BOOL SetFilePointerEx(HANDLE h, LARGE_INTEGER dist, LARGE_INTEGER* newpos, DWORD method) {
    FileObj* f = file(h);
    if (!f) { t_last_error = ERROR_INVALID_HANDLE; return FALSE; }
    static_assert(sizeof(off_t) == 8, "64-bit file offsets required (GAME.DAT > 2 GB)");
    off_t r = ::lseek(f->fd, off_t(dist.QuadPart), method == FILE_END ? SEEK_END : method == FILE_CURRENT ? SEEK_CUR : SEEK_SET);
    if (r < 0) { t_last_error = ERROR_ACCESS_DENIED; return FALSE; }
    if (newpos) newpos->QuadPart = r;
    return TRUE;
}
DWORD SetFilePointer(HANDLE h, LONG lo, LONG* hi, DWORD method) {
    LARGE_INTEGER d, n;
    d.QuadPart = hi ? ((LONGLONG)*hi << 32) | DWORD(lo) : (LONGLONG)lo;
    if (!SetFilePointerEx(h, d, &n, method)) return INVALID_SET_FILE_POINTER;
    if (hi) *hi = n.HighPart;
    return n.LowPart;
}
BOOL GetFileSizeEx(HANDLE h, LARGE_INTEGER* sz) {
    FileObj* f = file(h); struct stat st;
    if (!f || ::fstat(f->fd, &st) != 0) { t_last_error = ERROR_INVALID_HANDLE; return FALSE; }
    sz->QuadPart = st.st_size;
    return TRUE;
}
BOOL SetEndOfFile(HANDLE h) {
    FileObj* f = file(h);
    if (!f) { t_last_error = ERROR_INVALID_HANDLE; return FALSE; }
    off_t cur = ::lseek(f->fd, 0, SEEK_CUR);
    return cur >= 0 && ::ftruncate(f->fd, cur) == 0;
}
BOOL FlushFileBuffers(HANDLE h) { FileObj* f = file(h); return f && ::fsync(f->fd) == 0; }
BOOL GetFileTime(HANDLE h, FILETIME* c, FILETIME* a, FILETIME* w) {
    Obj* o = obj(h); struct stat st;
    int rc = -1;
    if (o && o->kind == Kind::File) rc = ::fstat(static_cast<FileObj*>(o)->fd, &st);
    else if (o && o->kind == Kind::Dir) rc = ::stat(static_cast<DirObj*>(o)->path.c_str(), &st);
    if (rc != 0) { t_last_error = ERROR_INVALID_HANDLE; return FALSE; }
    FILETIME t = unix_to_ft(st.st_mtime);
    if (c) *c = t; if (a) *a = t; if (w) *w = t;
    return TRUE;
}
DWORD GetFileAttributesA(LPCSTR p) {
    std::string path = norm(p); struct stat st;
    if (::stat(path.c_str(), &st) != 0) { set_err_from_errno(path); return INVALID_FILE_ATTRIBUTES; }
    return S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
}
BOOL GetFileAttributesExA(LPCSTR p, GET_FILEEX_INFO_LEVELS, void* out) {
    std::string path = norm(p); struct stat st;
    if (::stat(path.c_str(), &st) != 0) { set_err_from_errno(path); return FALSE; }
    auto* fa = static_cast<WIN32_FILE_ATTRIBUTE_DATA*>(out);
    fa->dwFileAttributes = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    fa->ftCreationTime = fa->ftLastAccessTime = fa->ftLastWriteTime = unix_to_ft(st.st_mtime);
    fa->nFileSizeHigh = DWORD(uint64_t(st.st_size) >> 32); fa->nFileSizeLow = DWORD(st.st_size);
    return TRUE;
}
BOOL DeleteFileA(LPCSTR p) { std::string path = norm(p); if (::unlink(path.c_str()) == 0) return TRUE; set_err_from_errno(path); return FALSE; }
BOOL CreateDirectoryA(LPCSTR p, void*) {
    std::string path = norm(p);
    if (::mkdir(path.c_str(), 0777) == 0) return TRUE;
    if (errno == EEXIST) t_last_error = ERROR_ALREADY_EXISTS; else set_err_from_errno(path);
    return FALSE;
}
BOOL RemoveDirectoryA(LPCSTR p) { std::string path = norm(p); if (::rmdir(path.c_str()) == 0) return TRUE; set_err_from_errno(path); return FALSE; }
HANDLE FindFirstFileA(LPCSTR pattern, WIN32_FIND_DATAA* fd) {
    std::string s = norm(pattern);
    size_t sl = s.find_last_of('/');
    std::string dir = sl == std::string::npos ? std::string("./") : s.substr(0, sl + 1);
    std::string pat = sl == std::string::npos ? s : s.substr(sl + 1);
    DIR* d = ::opendir(dir.c_str());
    if (!d) { set_err_from_errno(dir); return INVALID_HANDLE_VALUE; }
    auto* f = new FindObj(d, dir, pat);
    if (!find_fill(f, fd)) { delete f; t_last_error = ERROR_FILE_NOT_FOUND; return INVALID_HANDLE_VALUE; }
    return f;
}
BOOL FindNextFileA(HANDLE h, WIN32_FIND_DATAA* fd) {
    Obj* o = obj(h);
    if (!o || o->kind != Kind::Find) { t_last_error = ERROR_INVALID_HANDLE; return FALSE; }
    return find_fill(static_cast<FindObj*>(o), fd);
}
BOOL FindClose(HANDLE h) { Obj* o = obj(h); if (!o || o->kind != Kind::Find) return FALSE; delete o; return TRUE; }
BOOL CloseHandle(HANDLE h) {
    Obj* o = obj(h);
    if (!o) return (intptr_t)h == -2;   // pseudo handle
    if (o->kind == Kind::Event || o->kind == Kind::Semaphore || o->kind == Kind::Mutex || o->kind == Kind::Timer) {
        std::lock_guard<std::mutex> lk(g_disp);   // never free while a waiter is inside the dispatcher
        delete o;
    } else delete o;
    return TRUE;
}

// ---- synchronisation ---------------------------------------------------------------------------
HANDLE CreateEventA(void*, BOOL manual, BOOL initial, LPCSTR) {
    auto* w = new WaitObj(Kind::Event); w->manual = manual; w->signaled = initial; return w;
}
HANDLE CreateEventW(void* sa, BOOL manual, BOOL initial, LPCWSTR) { return CreateEventA(sa, manual, initial, nullptr); }
BOOL SetEvent(HANDLE h) {
    WaitObj* w = waitable(h); if (!w) return FALSE;
    { std::lock_guard<std::mutex> lk(g_disp); w->signaled = true; }
    g_disp_cv.notify_all();
    return TRUE;
}
BOOL ResetEvent(HANDLE h) {
    WaitObj* w = waitable(h); if (!w) return FALSE;
    std::lock_guard<std::mutex> lk(g_disp); w->signaled = false; return TRUE;
}
BOOL PulseEvent(HANDLE h) {
    WaitObj* w = waitable(h); if (!w) return FALSE;
    {
        std::lock_guard<std::mutex> lk(g_disp);
        if (w->manual) { ++w->pulse_gen; w->signaled = false; }   // releases everyone waiting now
        else if (w->waiters > 0) w->signaled = true;             // one waiter consumes it
    }
    g_disp_cv.notify_all();
    return TRUE;
}
HANDLE CreateSemaphoreA(void*, LONG initial, LONG maximum, LPCSTR) {
    auto* w = new WaitObj(Kind::Semaphore); w->count = initial; w->max = maximum; return w;
}
BOOL ReleaseSemaphore(HANDLE h, LONG n, LONG* prev) {
    WaitObj* w = waitable(h); if (!w || n <= 0) return FALSE;
    {
        std::lock_guard<std::mutex> lk(g_disp);
        if (prev) *prev = w->count;
        if ((int64_t)w->count + n > w->max) return FALSE;   // ERROR_TOO_MANY_POSTS
        w->count += n;
    }
    g_disp_cv.notify_all();
    return TRUE;
}
HANDLE CreateMutexA(void*, BOOL owner, LPCSTR) {
    auto* w = new WaitObj(Kind::Mutex); if (owner) { w->owner = tid(); w->rec = 1; } return w;
}
BOOL ReleaseMutex(HANDLE h) {
    WaitObj* w = waitable(h); if (!w) return FALSE;
    {
        std::lock_guard<std::mutex> lk(g_disp);
        if (w->owner != tid()) return FALSE;
        if (--w->rec == 0) w->owner = 0;
    }
    g_disp_cv.notify_all();
    return TRUE;
}
HANDLE CreateWaitableTimerA(void*, BOOL manual, LPCSTR) { auto* w = new WaitObj(Kind::Timer); w->manual = manual; return w; }
BOOL SetWaitableTimer(HANDLE h, const LARGE_INTEGER* due, LONG period_ms, void*, void*, BOOL) {
    WaitObj* w = waitable(h); if (!w || !due) return FALSE;
    {
        std::lock_guard<std::mutex> lk(g_disp);
        int64_t rel100ns;
        if (due->QuadPart < 0) rel100ns = -due->QuadPart;
        else { FILETIME now; GetSystemTimeAsFileTime(&now); rel100ns = due->QuadPart - (int64_t)from_ft(&now); if (rel100ns < 0) rel100ns = 0; }
        w->due = Clock::now() + std::chrono::nanoseconds(rel100ns * 100);
        w->armed = true; w->period_ms = period_ms; w->signaled = false;
    }
    g_disp_cv.notify_all();
    return TRUE;
}
BOOL CancelWaitableTimer(HANDLE h) {
    WaitObj* w = waitable(h); if (!w) return FALSE;
    std::lock_guard<std::mutex> lk(g_disp); w->armed = false; return TRUE;
}
DWORD WaitForMultipleObjects(DWORD n, const HANDLE* hs, BOOL all, DWORD ms) {
    if (n == 0 || n > MAXIMUM_WAIT_OBJECTS) { t_last_error = ERROR_INVALID_HANDLE; return WAIT_FAILED; }
    WaitObj* w[MAXIMUM_WAIT_OBJECTS]; uint64_t gen[MAXIMUM_WAIT_OBJECTS];
    for (DWORD i = 0; i < n; ++i) if (!(w[i] = waitable(hs[i]))) { t_last_error = ERROR_INVALID_HANDLE; return WAIT_FAILED; }
    const DWORD me = tid();
    const auto start = Clock::now();
    const auto deadline = ms == INFINITE ? Clock::time_point::max() : start + std::chrono::milliseconds(ms);
    std::unique_lock<std::mutex> lk(g_disp);
    for (DWORD i = 0; i < n; ++i) { gen[i] = w[i]->pulse_gen; ++w[i]->waiters; }
    DWORD result = WAIT_TIMEOUT;
    for (;;) {
        const auto now = Clock::now();
        auto ok = [&](DWORD i) { return ready(w[i], me, now) || w[i]->pulse_gen != gen[i]; };
        if (all) {
            bool every = true;
            for (DWORD i = 0; i < n && every; ++i) every = ok(i);
            if (every) { for (DWORD i = 0; i < n; ++i) if (w[i]->pulse_gen == gen[i]) consume(w[i], me); result = WAIT_OBJECT_0; break; }
        } else {
            DWORD hit = n;
            for (DWORD i = 0; i < n; ++i) if (ok(i)) { hit = i; break; }
            if (hit < n) { if (w[hit]->pulse_gen == gen[hit]) consume(w[hit], me); result = WAIT_OBJECT_0 + hit; break; }
        }
        if (now >= deadline) break;
        auto wake = deadline;
        for (DWORD i = 0; i < n; ++i) if (w[i]->kind == Kind::Timer && w[i]->armed && w[i]->due < wake) wake = w[i]->due;
        if (wake == Clock::time_point::max()) g_disp_cv.wait(lk);
        else g_disp_cv.wait_until(lk, wake);
    }
    for (DWORD i = 0; i < n; ++i) --w[i]->waiters;
    return result;
}
DWORD WaitForSingleObject(HANDLE h, DWORD ms) {
    if (!waitable(h)) { t_last_error = ERROR_INVALID_HANDLE; return WAIT_FAILED; }   // as Win32 for a bad handle
    return WaitForMultipleObjects(1, &h, FALSE, ms);
}

// ---- time --------------------------------------------------------------------------------------
BOOL QueryPerformanceCounter(LARGE_INTEGER* c) { c->QuadPart = (LONGLONG)armGetSystemTick(); return TRUE; }
BOOL QueryPerformanceFrequency(LARGE_INTEGER* f) { f->QuadPart = (LONGLONG)armGetSystemTickFreq(); return TRUE; }
ULONGLONG GetTickCount64(void) { return armTicksToNs(armGetSystemTick()) / 1000000ull; }
DWORD GetTickCount(void) { return (DWORD)GetTickCount64(); }
void Sleep(DWORD ms) { svcSleepThread(ms ? (int64_t)ms * 1000000ll : (int64_t)YieldType_WithoutCoreMigration); }
BOOL SwitchToThread(void) { svcSleepThread(YieldType_WithoutCoreMigration); return TRUE; }
void GetSystemTimeAsFileTime(FILETIME* ft) {
    struct timeval tv; gettimeofday(&tv, nullptr);
    *ft = to_ft(uint64_t(tv.tv_sec) * 10000000ull + uint64_t(tv.tv_usec) * 10ull + kFileTimeUnixEpoch);
}
BOOL FileTimeToSystemTime(const FILETIME* ft, SYSTEMTIME* st) {
    uint64_t v = from_ft(ft);
    if (v >> 63) return FALSE;
    const int64_t secs1601 = int64_t(v / 10000000ull);
    const int64_t days = secs1601 / 86400 - 134774;   // days 1601-01-01 -> 1970-01-01
    int64_t sod = secs1601 % 86400;
    int64_t y; unsigned m, d; civil_from_days(days, &y, &m, &d);
    st->wYear = WORD(y); st->wMonth = WORD(m); st->wDay = WORD(d);
    st->wDayOfWeek = WORD(((days % 7) + 7 + 4) % 7);   // 1970-01-01 was a Thursday
    st->wHour = WORD(sod / 3600); st->wMinute = WORD(sod / 60 % 60); st->wSecond = WORD(sod % 60);
    st->wMilliseconds = WORD(v / 10000ull % 1000);
    return TRUE;
}
BOOL SystemTimeToFileTime(const SYSTEMTIME* st, FILETIME* ft) {
    if (st->wMonth < 1 || st->wMonth > 12 || st->wDay < 1 || st->wDay > 31 || st->wHour > 23 || st->wMinute > 59 ||
        st->wSecond > 59 || st->wMilliseconds > 999 || st->wYear < 1601) return FALSE;
    const int64_t days = days_from_civil(st->wYear, st->wMonth, st->wDay) + 134774;
    const uint64_t secs = uint64_t(days) * 86400ull + st->wHour * 3600ull + st->wMinute * 60ull + st->wSecond;
    *ft = to_ft(secs * 10000000ull + st->wMilliseconds * 10000ull);
    return TRUE;
}
// The console's time zone (System Settings) via the time service's own rule. Bias = the offset in
// effect now, DST included, and no transition dates (the guest then never switches to daylight
// time itself). Was all zeros: the Load Game screen showed save times in UTC.
DWORD GetTimeZoneInformation(TIME_ZONE_INFORMATION* tz) {
    memset(tz, 0, sizeof *tz);
    u64 now = 0;
    TimeCalendarTime ct{}; TimeCalendarAdditionalInfo ci{};
    if (R_FAILED(timeGetCurrentTime(TimeType_Default, &now)) || R_FAILED(timeToCalendarTimeWithMyRule(now, &ct, &ci)))
        return TIME_ZONE_ID_UNKNOWN;
    tz->Bias = -ci.offset / 60;   // UTC = local + Bias (minutes)
    for (int i = 0; i < 7 && ci.timezoneName[i]; ++i) tz->StandardName[i] = tz->DaylightName[i] = WCHAR(ci.timezoneName[i]);
    return TIME_ZONE_ID_UNKNOWN;
}
DWORD timeBeginPeriod(DWORD) { return 0; }
DWORD timeEndPeriod(DWORD) { return 0; }

// ---- threads / process -------------------------------------------------------------------------
DWORD GetCurrentThreadId(void) { return tid(); }
DWORD GetCurrentProcessId(void) { return 1; }
HANDLE GetCurrentThread(void) { return (HANDLE)(intptr_t)-2; }
HANDLE OpenThread(DWORD, BOOL, DWORD) { t_last_error = ERROR_NOT_SUPPORTED; return nullptr; }
DWORD SuspendThread(HANDLE) { return (DWORD)-1; }
DWORD ResumeThread(HANDLE) { return (DWORD)-1; }
BOOL SetThreadPriority(HANDLE, int) { return TRUE; }
uintptr_t SetThreadAffinityMask(HANDLE, uintptr_t) { return 1; }
BOOL GetThreadContext(HANDLE, CONTEXT*) { return FALSE; }
BOOL SetThreadContext(HANDLE, const CONTEXT*) { return FALSE; }
USHORT RtlCaptureStackBackTrace(ULONG, ULONG, void**, ULONG* hash) { if (hash) *hash = 0; return 0; }

// ---- modules -----------------------------------------------------------------------------------
// The runtime loads XInput dynamically (kernel_stubs lsw_host_pad). On Switch "xinput1_4.dll" resolves
// to a libnx HID pad (player 1 + handheld) with the same XINPUT_STATE layout and button bits.
namespace {
PadState g_pad;
std::once_flag g_pad_once;
DWORD WINAPI nx_xinput_get_state(DWORD slot, void* out) {
    if (slot != 0) return 1167;   // ERROR_DEVICE_NOT_CONNECTED
    std::call_once(g_pad_once, [] {
        padConfigureInput(1, HidNpadStyleSet_NpadStandard);
        padInitializeDefault(&g_pad);
    });
    padUpdate(&g_pad);
    struct { DWORD packet; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } st{};
    const u64 b = padGetButtons(&g_pad);
    static const struct { u64 nx; WORD xi; } kMap[] = {
        {HidNpadButton_Up, 0x0001}, {HidNpadButton_Down, 0x0002}, {HidNpadButton_Left, 0x0004},
        {HidNpadButton_Right, 0x0008}, {HidNpadButton_Plus, 0x0010}, {HidNpadButton_Minus, 0x0020},
        {HidNpadButton_StickL, 0x0040}, {HidNpadButton_StickR, 0x0080}, {HidNpadButton_L, 0x0100},
        {HidNpadButton_R, 0x0200},
        // Face buttons by label (Nintendo convention: A confirms/jumps), not by position.
        {HidNpadButton_A, 0x1000}, {HidNpadButton_B, 0x2000}, {HidNpadButton_X, 0x4000},
        {HidNpadButton_Y, 0x8000}};
    for (auto& m : kMap) if (b & m.nx) st.buttons |= m.xi;
    // Capture combo (LSWTCS_CAPCOMBO, default on): hold ZL + ZR and click the right stick -> screenshot +
    // per-draw [GLDRAWCAP] log in stdout.log (shot_now) + every resolve/render target as PNG (resdump_now), for
    // glitches that are on screen only briefly (cutscenes). Files land in sdmc:/lswtcs. At most once per 3 s.
    {
        static const bool combo_on = [] { const char* e = getenv("LSWTCS_CAPCOMBO"); return !(e && e[0] == '0'); }();
        static u64 last_ns = 0;
        const u64 want = HidNpadButton_ZL | HidNpadButton_ZR | HidNpadButton_StickR;
        const u64 now_ns = armTicksToNs(armGetSystemTick());
        if (combo_on && (b & want) == want && now_ns - last_ns > 3000000000ull) {
            last_ns = now_ns;
            for (const char* n : {"shot_now", "resdump_now"}) if (FILE* t = fopen(n, "wb")) fclose(t);
            printf("[CAPCOMBO] capture requested (ZL+ZR+R-stick)\n");
            fflush(stdout);
            lsw_ft_ring_dump();
        }
    }
    st.lt = (b & HidNpadButton_ZL) ? 255 : 0;
    st.rt = (b & HidNpadButton_ZR) ? 255 : 0;
    const HidAnalogStickState l = padGetStickPos(&g_pad, 0), r = padGetStickPos(&g_pad, 1);
    st.lx = SHORT(l.x); st.ly = SHORT(l.y); st.rx = SHORT(r.x); st.ry = SHORT(r.y);
    memcpy(out, &st, sizeof st);
    return ERROR_SUCCESS;
}
}
HMODULE LoadLibraryA(LPCSTR name) {
    return (name && !strcasecmp(name, "xinput1_4.dll")) ? (HMODULE)&g_pad : nullptr;
}
void* GetProcAddress(HMODULE m, LPCSTR name) {
    if (m == (HMODULE)&g_pad && name && !strcmp(name, "XInputGetState")) return (void*)&nx_xinput_get_state;
    return nullptr;
}
HMODULE GetModuleHandleA(LPCSTR) { return nullptr; }
BOOL GetModuleHandleExA(DWORD, LPCSTR, HMODULE* out) { if (out) *out = nullptr; return FALSE; }
DWORD GetModuleFileNameA(HMODULE, LPSTR buf, DWORD n) { if (buf && n) buf[0] = 0; return 0; }

// ---- memory ------------------------------------------------------------------------------------
BOOL VirtualProtect(void*, SIZE_T, DWORD, DWORD* old) { if (old) *old = PAGE_READWRITE; return FALSE; }
void* VirtualAlloc(void* p, SIZE_T n, DWORD type, DWORD) {
    if (p || !(type & MEM_COMMIT)) return nullptr;   // address-specific reservations: Switch guest window only
    void* m = aligned_alloc(0x1000, (n + 0xFFF) & ~SIZE_T(0xFFF));
    if (m) memset(m, 0, n);
    return m;
}
BOOL VirtualFree(void* p, SIZE_T, DWORD) { free(p); return TRUE; }

// ---- exceptions / input / UI -------------------------------------------------------------------
void* AddVectoredExceptionHandler(ULONG, PVECTORED_EXCEPTION_HANDLER) { return (void*)1; }
SHORT GetAsyncKeyState(int) { return 0; }
DWORD GetWindowThreadProcessId(HWND, DWORD* pid) { if (pid) *pid = 0; return 0; }
HWND GetForegroundWindow(void) { return nullptr; }
