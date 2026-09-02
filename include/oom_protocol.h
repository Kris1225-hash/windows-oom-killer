#ifndef OOM_PROTOCOL_H
#define OOM_PROTOCOL_H

#include "oom_types.h"

#define OOM_PROTOCOL_VERSION 1u
#define OOM_DEVICE_PATH L"\\\\.\\WinOomKiller"

#define OOM_FLAG_ARMED    0x00000001u
#define OOM_FLAG_CRITICAL 0x00000002u

typedef enum OOM_BACKING_REASON {
    OOM_BACKING_UNKNOWN = 0,
    OOM_BACKING_PAGEFILE_DISABLED = 1,
    OOM_BACKING_PAGEFILE_EXHAUSTED = 2,
    OOM_BACKING_PAGEFILE_GROWTH_FAILED = 3,
    OOM_BACKING_NO_ELIGIBLE_PROCESS = 4,
    OOM_BACKING_KILL_REJECTED = 5
} OOM_BACKING_REASON;

/* Private bugcheck range. Parameters are charge, limit, available pages,
 * then (backing reason << 32) | saturated pagefile-free pages. */
#define OOM_FATAL_NO_BACKING_STORE       0xE0F00001u
#define OOM_FATAL_PAGEFILE_DISABLED      0xE0F00002u
#define OOM_FATAL_PAGEFILE_EXHAUSTED     0xE0F00003u
#define OOM_FATAL_PAGEFILE_GROWTH_FAILED 0xE0F00004u
#define OOM_FATAL_KILL_FAILED            0xE0F00005u
#define OOM_FATAL_KILL_TIMEOUT           0xE0F00006u
#define OOM_FATAL_NO_KILLABLE_PROCESS    0xE0F00007u
#define OOM_FATAL_MONITOR_UNRESPONSIVE   0xE0F00008u

typedef struct OOM_TELEMETRY {
    OOM_U32 version;
    OOM_U32 flags;
    OOM_U64 sequence;
    OOM_U32 service_pid;
    OOM_U32 page_size;
    OOM_U32 backing_reason;
    OOM_U32 reserved;
    OOM_U64 commit_charge_pages;
    OOM_U64 commit_limit_pages;
    OOM_U64 available_physical_pages;
    OOM_U64 pagefile_free_pages;
} OOM_TELEMETRY;

typedef struct OOM_KILL_REQUEST {
    OOM_TELEMETRY telemetry;
    OOM_U32 victim_pid;
    OOM_U32 reserved;
    OOM_U64 victim_private_pages;
    OOM_U64 victim_create_time;
} OOM_KILL_REQUEST;

typedef struct OOM_DRIVER_STATUS {
    OOM_U32 version;
    OOM_U32 armed;
    OOM_U32 kill_pending;
    OOM_I32 last_kill_status;
    OOM_U64 last_sequence;
    OOM_U32 owner_pid;
    OOM_U32 reserved;
} OOM_DRIVER_STATUS;

#if defined(OOM_ENABLE_TEST_IOCTL)
#define OOM_TEST_CONFIRM 0x4F4F4D21u
typedef struct OOM_BUGCHECK_REQUEST {
    OOM_U32 confirm;
    OOM_U32 code;
    OOM_U64 parameters[4];
} OOM_BUGCHECK_REQUEST;
#endif

#if defined(_WIN32)
#define IOCTL_OOM_HEARTBEAT CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_WRITE_DATA)
#define IOCTL_OOM_KILL      CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_WRITE_DATA)
#define IOCTL_OOM_ESCALATE  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_WRITE_DATA)
#define IOCTL_OOM_STATUS    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_READ_DATA)
#if defined(OOM_ENABLE_TEST_IOCTL)
#define IOCTL_OOM_TEST_BUGCHECK CTL_CODE(FILE_DEVICE_UNKNOWN, 0x80F, METHOD_BUFFERED, FILE_WRITE_DATA)
#endif
#endif

#endif
