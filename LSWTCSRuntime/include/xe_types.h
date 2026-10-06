#pragma once
#include <cstdint>
#include <cstddef>

typedef uint8_t  uint8;
typedef uint16_t uint16;
typedef uint32_t uint32;
typedef uint64_t uint64;
typedef int8_t   int8;
typedef int16_t  int16;
typedef int32_t  int32;
typedef int64_t  int64;

// Xbox 360 NTSTATUS codes
#define STATUS_SUCCESS              0x00000000
#define STATUS_PENDING              0x00000103
#define STATUS_TIMEOUT              0x00000102
#define STATUS_UNSUCCESSFUL         0xC0000001
#define STATUS_NOT_IMPLEMENTED      0xC0000002
#define STATUS_ACCESS_DENIED        0xC0000022
#define STATUS_OBJECT_NAME_NOT_FOUND 0xC0000034
#define STATUS_END_OF_FILE          0xC0000011

typedef uint32_t NTSTATUS;
typedef uint32_t HANDLE;
typedef uint32_t BOOL;
typedef void*    PVOID;

#define INVALID_HANDLE_VALUE 0xFFFFFFFF
#define TRUE  1
#define FALSE 0
