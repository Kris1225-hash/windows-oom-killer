#ifndef OOM_TYPES_H
#define OOM_TYPES_H

#if defined(_KERNEL_MODE)
#include <ntdef.h>
typedef ULONG OOM_U32;
typedef LONG OOM_I32;
typedef ULONGLONG OOM_U64;
#else
#include <stdint.h>
typedef uint32_t OOM_U32;
typedef int32_t OOM_I32;
typedef uint64_t OOM_U64;
#endif

#endif
