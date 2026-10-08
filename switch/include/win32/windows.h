// Win32 subset for the Switch build (libnx + newlib). Force-included into runtime sources that
// were written against <windows.h>, so they compile unchanged. Implemented in
// switch/source/win32_switch.cpp.
//
// Semantics that matter to the runtime are emulated faithfully:
//  - HANDLE points at a tagged object (file, directory, event, semaphore, mutex, waitable timer).
//  - Waits follow Win32 rules (auto-reset events and semaphores are consumed by the waiter that
//    is satisfied, mutexes are recursive and owned per thread, WaitForMultipleObjects any/all,
//    PulseEvent wakes current waiters only, waitable timers become signalled at their due time).
//  - Files: 64-bit offsets, Win32 create dispositions, attributes, FindFirstFile wildcards.
// Diagnostics-only APIs (VEH, thread suspend/context, module queries, VirtualProtect) report failure.
#pragma once
#ifndef LSW_WIN32_SWITCH_H
#define LSW_WIN32_SWITCH_H

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <strings.h>

// ---- basic types -----------------------------------------------------------------------------
typedef int BOOL;
typedef uint8_t BYTE;
typedef uint16_t WORD;
typedef uint16_t USHORT;
typedef int16_t SHORT;
typedef uint32_t DWORD;
typedef int32_t LONG;
typedef uint32_t ULONG;
typedef int64_t LONGLONG;
typedef uint64_t ULONGLONG;
typedef uint64_t DWORD64;
typedef size_t SIZE_T;
typedef void* HANDLE;
typedef void* HMODULE;
typedef void* HWND;
typedef const char* LPCSTR;
typedef char* LPSTR;
typedef const wchar_t* LPCWSTR;
typedef void* LPVOID;
typedef DWORD* LPDWORD;
typedef long LRESULT;
typedef wchar_t WCHAR;

#define WINAPI
#define CALLBACK
#define TRUE 1
#define FALSE 0
#define MAX_PATH 260
#define INFINITE 0xFFFFFFFFu

#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#define INVALID_SET_FILE_POINTER ((DWORD)-1)

typedef union _LARGE_INTEGER {
    struct { DWORD LowPart; LONG HighPart; };
    LONGLONG QuadPart;
} LARGE_INTEGER;

typedef struct _FILETIME { DWORD dwLowDateTime; DWORD dwHighDateTime; } FILETIME;
typedef struct _SYSTEMTIME {
    WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds;
} SYSTEMTIME;
typedef struct _TIME_ZONE_INFORMATION {
    LONG Bias; WCHAR StandardName[32]; SYSTEMTIME StandardDate; LONG StandardBias;
    WCHAR DaylightName[32]; SYSTEMTIME DaylightDate; LONG DaylightBias;
} TIME_ZONE_INFORMATION;
#define TIME_ZONE_ID_UNKNOWN 0

// ---- errors / waits --------------------------------------------------------------------------
#define ERROR_SUCCESS 0
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_PATH_NOT_FOUND 3
#define ERROR_ACCESS_DENIED 5
#define ERROR_INVALID_HANDLE 6
#define ERROR_FILE_EXISTS 80
#define ERROR_ALREADY_EXISTS 183
#define ERROR_NO_MORE_FILES 18
#define ERROR_NOT_SUPPORTED 50
#define WAIT_OBJECT_0 0u
#define WAIT_ABANDONED 0x80u
#define WAIT_TIMEOUT 258u
#define WAIT_FAILED 0xFFFFFFFFu
#define MAXIMUM_WAIT_OBJECTS 64

DWORD GetLastError(void);
void SetLastError(DWORD e);

// ---- files -----------------------------------------------------------------------------------
#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define FILE_SHARE_READ 1u
#define FILE_SHARE_WRITE 2u
#define FILE_SHARE_DELETE 4u
#define CREATE_NEW 1u
#define CREATE_ALWAYS 2u
#define OPEN_EXISTING 3u
#define OPEN_ALWAYS 4u
#define TRUNCATE_EXISTING 5u
#define FILE_ATTRIBUTE_READONLY 0x1u
#define FILE_ATTRIBUTE_DIRECTORY 0x10u
#define FILE_ATTRIBUTE_NORMAL 0x80u
#define FILE_FLAG_BACKUP_SEMANTICS 0x02000000u
#define FILE_BEGIN 0u
#define FILE_CURRENT 1u
#define FILE_END 2u

typedef struct _WIN32_FILE_ATTRIBUTE_DATA {
    DWORD dwFileAttributes; FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow;
} WIN32_FILE_ATTRIBUTE_DATA;
typedef enum { GetFileExInfoStandard } GET_FILEEX_INFO_LEVELS;
typedef struct _WIN32_FIND_DATAA {
    DWORD dwFileAttributes; FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1;
    char cFileName[MAX_PATH]; char cAlternateFileName[14];
} WIN32_FIND_DATAA;

HANDLE CreateFileA(LPCSTR path, DWORD access, DWORD share, void* sa, DWORD disposition, DWORD flags, HANDLE tmpl);
BOOL ReadFile(HANDLE h, void* buf, DWORD n, DWORD* got, void* ov);
BOOL WriteFile(HANDLE h, const void* buf, DWORD n, DWORD* wrote, void* ov);
BOOL SetFilePointerEx(HANDLE h, LARGE_INTEGER dist, LARGE_INTEGER* newpos, DWORD method);
DWORD SetFilePointer(HANDLE h, LONG lo, LONG* hi, DWORD method);
BOOL GetFileSizeEx(HANDLE h, LARGE_INTEGER* sz);
BOOL SetEndOfFile(HANDLE h);
BOOL FlushFileBuffers(HANDLE h);
BOOL GetFileTime(HANDLE h, FILETIME* c, FILETIME* a, FILETIME* w);
DWORD GetFileAttributesA(LPCSTR path);
BOOL GetFileAttributesExA(LPCSTR path, GET_FILEEX_INFO_LEVELS lvl, void* out);
BOOL DeleteFileA(LPCSTR path);
BOOL CreateDirectoryA(LPCSTR path, void* sa);
BOOL RemoveDirectoryA(LPCSTR path);
HANDLE FindFirstFileA(LPCSTR pattern, WIN32_FIND_DATAA* fd);
BOOL FindNextFileA(HANDLE h, WIN32_FIND_DATAA* fd);
BOOL FindClose(HANDLE h);
BOOL CloseHandle(HANDLE h);

// ---- synchronisation -------------------------------------------------------------------------
HANDLE CreateEventA(void* sa, BOOL manual, BOOL initial, LPCSTR name);
HANDLE CreateEventW(void* sa, BOOL manual, BOOL initial, LPCWSTR name);
#define CreateEvent(sa, m, i, n) CreateEventA((sa), (m), (i), nullptr)
BOOL SetEvent(HANDLE h);
BOOL ResetEvent(HANDLE h);
BOOL PulseEvent(HANDLE h);
HANDLE CreateSemaphoreA(void* sa, LONG initial, LONG maximum, LPCSTR name);
#define CreateSemaphore(sa, i, m, n) CreateSemaphoreA((sa), (i), (m), nullptr)
BOOL ReleaseSemaphore(HANDLE h, LONG n, LONG* prev);
HANDLE CreateMutexA(void* sa, BOOL owner, LPCSTR name);
#define CreateMutex(sa, o, n) CreateMutexA((sa), (o), nullptr)
BOOL ReleaseMutex(HANDLE h);
HANDLE CreateWaitableTimerA(void* sa, BOOL manual, LPCSTR name);
#define CreateWaitableTimer(sa, m, n) CreateWaitableTimerA((sa), (m), nullptr)
BOOL SetWaitableTimer(HANDLE h, const LARGE_INTEGER* due, LONG period_ms, void* cb, void* arg, BOOL resume);
BOOL CancelWaitableTimer(HANDLE h);
DWORD WaitForSingleObject(HANDLE h, DWORD ms);
DWORD WaitForMultipleObjects(DWORD n, const HANDLE* hs, BOOL all, DWORD ms);

// ---- time ------------------------------------------------------------------------------------
BOOL QueryPerformanceCounter(LARGE_INTEGER* c);
BOOL QueryPerformanceFrequency(LARGE_INTEGER* f);
ULONGLONG GetTickCount64(void);
DWORD GetTickCount(void);
void Sleep(DWORD ms);
BOOL SwitchToThread(void);
void GetSystemTimeAsFileTime(FILETIME* ft);
BOOL FileTimeToSystemTime(const FILETIME* ft, SYSTEMTIME* st);
BOOL SystemTimeToFileTime(const SYSTEMTIME* st, FILETIME* ft);
DWORD GetTimeZoneInformation(TIME_ZONE_INFORMATION* tz);
DWORD timeBeginPeriod(DWORD ms);
DWORD timeEndPeriod(DWORD ms);

// ---- threads / process -----------------------------------------------------------------------
#define THREAD_SUSPEND_RESUME 0x2u
#define THREAD_GET_CONTEXT 0x8u
#define THREAD_QUERY_INFORMATION 0x40u
#define THREAD_PRIORITY_NORMAL 0
#define THREAD_PRIORITY_ABOVE_NORMAL 1
#define THREAD_PRIORITY_HIGHEST 2
#define THREAD_PRIORITY_TIME_CRITICAL 15
DWORD GetCurrentThreadId(void);
DWORD GetCurrentProcessId(void);
HANDLE GetCurrentThread(void);
HANDLE OpenThread(DWORD access, BOOL inherit, DWORD tid);
DWORD SuspendThread(HANDLE h);
DWORD ResumeThread(HANDLE h);
BOOL SetThreadPriority(HANDLE h, int prio);
uintptr_t SetThreadAffinityMask(HANDLE h, uintptr_t mask);

// Thread context (sampling profiler only; GetThreadContext always fails on Switch).
#define CONTEXT_CONTROL 0x1u
#define CONTEXT_DEBUG_REGISTERS 0x10u
typedef struct _CONTEXT {
    DWORD ContextFlags; DWORD EFlags; DWORD64 Rip, Rsp, Rbp;
    DWORD64 Dr0, Dr1, Dr2, Dr3, Dr6, Dr7;
} CONTEXT;
BOOL GetThreadContext(HANDLE h, CONTEXT* c);
BOOL SetThreadContext(HANDLE h, const CONTEXT* c);
USHORT RtlCaptureStackBackTrace(ULONG skip, ULONG n, void** frames, ULONG* hash);

// ---- modules ---------------------------------------------------------------------------------
#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT 0x2u
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 0x4u
HMODULE LoadLibraryA(LPCSTR name);
void* GetProcAddress(HMODULE m, LPCSTR name);
HMODULE GetModuleHandleA(LPCSTR name);
#define GetModuleHandle GetModuleHandleA
BOOL GetModuleHandleExA(DWORD flags, LPCSTR name, HMODULE* out);
DWORD GetModuleFileNameA(HMODULE m, LPSTR buf, DWORD n);

// ---- memory (diagnostics only on Switch: always fails) ---------------------------------------
#define PAGE_NOACCESS 0x01u
#define PAGE_READONLY 0x02u
#define PAGE_READWRITE 0x04u
#define MEM_COMMIT 0x1000u
#define MEM_RESERVE 0x2000u
#define MEM_RELEASE 0x8000u
BOOL VirtualProtect(void* p, SIZE_T n, DWORD prot, DWORD* old);
void* VirtualAlloc(void* p, SIZE_T n, DWORD type, DWORD prot);   // plain aligned heap allocation
BOOL VirtualFree(void* p, SIZE_T n, DWORD type);

// ---- structured exceptions (VEH never fires on Switch) ----------------------------------------
#define EXCEPTION_ACCESS_VIOLATION 0xC0000005u
#define EXCEPTION_SINGLE_STEP 0x80000004u
#define EXCEPTION_ILLEGAL_INSTRUCTION 0xC000001Du
#define EXCEPTION_CONTINUE_SEARCH 0
#define EXCEPTION_CONTINUE_EXECUTION (-1)
typedef struct _EXCEPTION_RECORD {
    DWORD ExceptionCode, ExceptionFlags; void* ExceptionRecord; void* ExceptionAddress;
    DWORD NumberParameters; uintptr_t ExceptionInformation[15];
} EXCEPTION_RECORD;
typedef struct _EXCEPTION_POINTERS { EXCEPTION_RECORD* ExceptionRecord; CONTEXT* ContextRecord; } EXCEPTION_POINTERS;
typedef LONG (*PVECTORED_EXCEPTION_HANDLER)(EXCEPTION_POINTERS*);
void* AddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER h);

// ---- input / UI ------------------------------------------------------------------------------
#define VK_RETURN 0x0D
#define VK_ESCAPE 0x1B
#define VK_SPACE 0x20
#define VK_LEFT 0x25
#define VK_UP 0x26
#define VK_RIGHT 0x27
#define VK_DOWN 0x28
SHORT GetAsyncKeyState(int vk);
DWORD GetWindowThreadProcessId(HWND w, DWORD* pid);
HWND GetForegroundWindow(void);

// ---- CRT name differences --------------------------------------------------------------------
#define _stricmp strcasecmp
#define _strnicmp strncasecmp

#endif
