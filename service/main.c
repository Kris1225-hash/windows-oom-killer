#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <winevt.h>
#include <wtsapi32.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#include "../include/oom_policy.h"
#include "../include/oom_protocol.h"

#define SERVICE_NAME L"WinOomKiller"
#define SETTINGS_KEY L"SOFTWARE\\WinOomKiller"
#define MIB (1024ull * 1024)

static SERVICE_STATUS_HANDLE service_status_handle;
static SERVICE_STATUS service_status;
static HANDLE stop_event;
static BOOL console_mode;

typedef struct OOM_BUGCHECK_INFO {
    DWORD code;
    const wchar_t *name;
    const wchar_t *description;
} OOM_BUGCHECK_INFO;

static const OOM_BUGCHECK_INFO oom_bugchecks[] = {
    {OOM_FATAL_NO_BACKING_STORE, L"OOM_FATAL_NO_BACKING_STORE",
     L"Windows ran out of committed-memory backing store."},
    {OOM_FATAL_PAGEFILE_DISABLED, L"OOM_FATAL_PAGEFILE_DISABLED",
     L"No enabled pagefile was available to back more committed memory."},
    {OOM_FATAL_PAGEFILE_EXHAUSTED, L"OOM_FATAL_PAGEFILE_EXHAUSTED",
     L"The configured pagefile had no usable capacity left."},
    {OOM_FATAL_PAGEFILE_GROWTH_FAILED, L"OOM_FATAL_PAGEFILE_GROWTH_FAILED",
     L"Windows reached maximum commit and could not grow the pagefile."},
    {OOM_FATAL_KILL_FAILED, L"OOM_FATAL_KILL_FAILED",
     L"The kernel could not terminate the selected recovery process."},
    {OOM_FATAL_KILL_TIMEOUT, L"OOM_FATAL_KILL_TIMEOUT",
     L"The selected recovery process did not terminate before the watchdog expired."},
    {OOM_FATAL_NO_KILLABLE_PROCESS, L"OOM_FATAL_NO_KILLABLE_PROCESS",
     L"No process was both large enough and safe to terminate."},
    {OOM_FATAL_MONITOR_UNRESPONSIVE, L"OOM_FATAL_MONITOR_UNRESPONSIVE",
     L"The user-mode memory monitor stopped responding during critical pressure."},
};

static void LogMessage(WORD type, const wchar_t *format, ...)
{
    wchar_t message[512];
    va_list args;
    HANDLE source;
    const wchar_t *strings[] = {message};

    va_start(args, format);
    _vsnwprintf_s(message, ARRAYSIZE(message), _TRUNCATE, format, args);
    va_end(args);
    if (console_mode)
        fwprintf(stderr, L"%ls\n", message);
    source = RegisterEventSourceW(NULL, SERVICE_NAME);
    if (source) {
        ReportEventW(source, type, 0, 0, NULL, 1, 0, strings, NULL);
        DeregisterEventSource(source);
    }
}

static DWORD ReadSetting(const wchar_t *name, DWORD fallback)
{
    DWORD value = fallback, size = (DWORD)sizeof(value), type;
    RegGetValueW(HKEY_LOCAL_MACHINE, SETTINGS_KEY, name, RRF_RT_REG_DWORD,
                 &type, &value, &size);
    return value;
}

static uint64_t ReadSettingQword(const wchar_t *name)
{
    uint64_t value = 0;
    DWORD size = (DWORD)sizeof(value), type;
    RegGetValueW(HKEY_LOCAL_MACHINE, SETTINGS_KEY, name, RRF_RT_REG_QWORD,
                 &type, &value, &size);
    return value;
}

static const OOM_BUGCHECK_INFO *FindBugcheckInfo(uint64_t code)
{
    size_t i;

    for (i = 0; i < ARRAYSIZE(oom_bugchecks); ++i)
        if (oom_bugchecks[i].code == code)
            return &oom_bugchecks[i];
    return NULL;
}

static BOOL ParseBugcheckText(const wchar_t *text, uint64_t values[5])
{
    int i;

    for (i = 0; i < 5; ++i) {
        wchar_t *end;
        text = wcsstr(text, L"0x");
        if (!text)
            return FALSE;
        values[i] = wcstoull(text, &end, 16);
        if (end == text)
            return FALSE;
        text = end;
    }
    return TRUE;
}

/* Returns FALSE only when a custom crash still needs to reach a user session. */
static BOOL NotifyPreviousBugcheck(void)
{
    static const wchar_t *paths[] = {
        L"Event/System/EventRecordID",
        L"Event/EventData/Data[@Name='param1']",
    };
    static wchar_t title[] = L"Windows OOM Killer";
    EVT_HANDLE query = NULL, event = NULL, context = NULL;
    PEVT_VARIANT values = NULL;
    const OOM_BUGCHECK_INFO *info;
    uint64_t parsed[5], record_id, backing;
    DWORD returned, size = 0, count = 0, session, response;
    wchar_t message[1024];
    BOOL complete = TRUE;

    query = EvtQuery(NULL, L"System",
                     L"*[System[Provider[@Name='Microsoft-Windows-WER-SystemErrorReporting'] "
                     L"and (EventID=1001)]]",
                     EvtQueryChannelPath | EvtQueryReverseDirection);
    if (!query || !EvtNext(query, 1, &event, 0, 0, &returned))
        goto cleanup;
    context = EvtCreateRenderContext((DWORD)ARRAYSIZE(paths), paths,
                                     EvtRenderContextValues);
    if (!context)
        goto cleanup;
    EvtRender(context, event, EvtRenderEventValues, 0, NULL, &size, &count);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        goto cleanup;
    values = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size);
    if (!values || !EvtRender(context, event, EvtRenderEventValues, size, values,
                              &size, &count) || count != ARRAYSIZE(paths) ||
        (values[0].Type & EVT_VARIANT_TYPE_MASK) != EvtVarTypeUInt64 ||
        (values[1].Type & EVT_VARIANT_TYPE_MASK) != EvtVarTypeString ||
        !values[1].StringVal || !ParseBugcheckText(values[1].StringVal, parsed))
        goto cleanup;

    record_id = values[0].UInt64Val;
    info = FindBugcheckInfo(parsed[0]);
    if (!info || ReadSettingQword(L"LastNotifiedBugcheckRecord") == record_id)
        goto cleanup;

    session = WTSGetActiveConsoleSessionId();
    if (session == 0xFFFFFFFF) {
        complete = FALSE;
        goto cleanup;
    }
    backing = parsed[4];
    _snwprintf_s(message, ARRAYSIZE(message), _TRUNCATE,
                 L"Windows stopped itself because it could not safely recover from "
                 L"an out-of-memory condition.\n\nReason: %ls\n%ls\n\n"
                 L"Stop code: 0x%08llX\nCommit charge: %llu pages\n"
                 L"Commit limit: %llu pages\nAvailable physical memory: %llu pages\n"
                 L"Pagefile free: %lu pages\nBacking-store reason: %lu",
                 info->name, info->description, parsed[0], parsed[1], parsed[2],
                 parsed[3], (DWORD)backing, (DWORD)(backing >> 32));
    if (!WTSSendMessageW(WTS_CURRENT_SERVER_HANDLE, session, title,
                         (DWORD)(wcslen(title) * sizeof(*title)), message,
                         (DWORD)(wcslen(message) * sizeof(*message)),
                         MB_OK | MB_ICONWARNING | MB_SETFOREGROUND, 0, &response, FALSE)) {
        complete = FALSE;
        goto cleanup;
    }
    if (RegSetKeyValueW(HKEY_LOCAL_MACHINE, SETTINGS_KEY,
                        L"LastNotifiedBugcheckRecord", REG_QWORD, &record_id,
                        (DWORD)sizeof(record_id)) != ERROR_SUCCESS)
        LogMessage(EVENTLOG_WARNING_TYPE,
                   L"could not save bugcheck notification marker");
    LogMessage(EVENTLOG_INFORMATION_TYPE, L"notified session %lu about %ls",
               session, info->name);

cleanup:
    if (values)
        HeapFree(GetProcessHeap(), 0, values);
    if (context)
        EvtClose(context);
    if (event)
        EvtClose(event);
    if (query)
        EvtClose(query);
    return complete;
}

static BOOL SendIoctl(HANDLE device, DWORD code, const void *input, size_t input_size)
{
    DWORD returned;
    if (input_size > MAXDWORD) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    return DeviceIoControl(device, code, (void *)input, (DWORD)input_size, NULL, 0,
                           &returned, NULL);
}

typedef struct PAGEFILE_TOTALS {
    uint64_t total_pages;
    uint64_t used_pages;
} PAGEFILE_TOTALS;

static BOOL CALLBACK CountPagefile(void *context, PENUM_PAGE_FILE_INFORMATION info,
                                   const wchar_t *filename)
{
    PAGEFILE_TOTALS *totals = context;
    UNREFERENCED_PARAMETER(filename);
    totals->total_pages += info->TotalSize;
    totals->used_pages += info->TotalInUse;
    return TRUE;
}

static BOOL CollectTelemetry(OOM_TELEMETRY *telemetry, uint64_t sequence,
                             const OOM_THRESHOLDS *thresholds, BOOL armed)
{
    PERFORMANCE_INFORMATION performance = {(DWORD)sizeof(PERFORMANCE_INFORMATION)};
    PAGEFILE_TOTALS pagefiles = {0};
    BOOL pagefiles_known;
    OOM_SAMPLE sample;

    if (!GetPerformanceInfo(&performance, (DWORD)sizeof(performance)))
        return FALSE;

    ZeroMemory(telemetry, sizeof(*telemetry));
    telemetry->version = OOM_PROTOCOL_VERSION;
    telemetry->sequence = sequence;
    telemetry->service_pid = GetCurrentProcessId();
    telemetry->page_size = (uint32_t)performance.PageSize;
    telemetry->commit_charge_pages = performance.CommitTotal;
    telemetry->commit_limit_pages = performance.CommitLimit;
    telemetry->available_physical_pages = performance.PhysicalAvailable;
    pagefiles_known = EnumPageFilesW(CountPagefile, &pagefiles);
    if (pagefiles_known && pagefiles.total_pages >= pagefiles.used_pages)
        telemetry->pagefile_free_pages = pagefiles.total_pages - pagefiles.used_pages;
    else
        telemetry->pagefile_free_pages = 0;
    telemetry->flags = armed ? OOM_FLAG_ARMED : 0;

    sample.commit_charge = telemetry->commit_charge_pages;
    sample.commit_limit = telemetry->commit_limit_pages;
    if (OomSampleIsCritical(thresholds, &sample))
        telemetry->flags |= OOM_FLAG_CRITICAL;

    if (pagefiles_known && !pagefiles.total_pages)
        telemetry->backing_reason = OOM_BACKING_PAGEFILE_DISABLED;
    else if (pagefiles_known &&
             telemetry->pagefile_free_pages <= thresholds->min_commit_headroom)
        telemetry->backing_reason = OOM_BACKING_PAGEFILE_EXHAUSTED;
    else
        telemetry->backing_reason = OOM_BACKING_UNKNOWN;
    return TRUE;
}

static BOOL FindVictim(uint32_t page_size, uint32_t *victim_pid, uint64_t *private_pages,
                       uint64_t *create_time, DWORD *victim_session,
                       wchar_t *victim_name, size_t victim_name_count)
{
    PROCESSENTRY32W entry = {(DWORD)sizeof(entry)};
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    DWORD self = GetCurrentProcessId();
    BOOL found = FALSE;

    if (snapshot == INVALID_HANDLE_VALUE)
        return FALSE;
    if (!Process32FirstW(snapshot, &entry)) {
        CloseHandle(snapshot);
        return FALSE;
    }

    do {
        HANDLE process;
        PROCESS_MEMORY_COUNTERS_EX counters = {0};
        FILETIME created, exited, kernel, user;
        DWORD session;
        BOOL critical;

        if (entry.th32ProcessID <= 4 || entry.th32ProcessID == self ||
            !ProcessIdToSessionId(entry.th32ProcessID, &session) || session == 0)
            continue;
        process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
        if (!process)
            continue;
        if (!IsProcessCritical(process, &critical) || critical ||
            !GetProcessMemoryInfo(process, (PROCESS_MEMORY_COUNTERS *)&counters,
                                  (DWORD)sizeof(counters)) ||
            !GetProcessTimes(process, &created, &exited, &kernel, &user)) {
            CloseHandle(process);
            continue;
        }
        CloseHandle(process);
        if ((uint64_t)counters.PrivateUsage / page_size > *private_pages) {
            *victim_pid = entry.th32ProcessID;
            *private_pages = (uint64_t)counters.PrivateUsage / page_size;
            *create_time = ((uint64_t)created.dwHighDateTime << 32) | created.dwLowDateTime;
            *victim_session = session;
            wcsncpy_s(victim_name, victim_name_count, entry.szExeFile, _TRUNCATE);
            found = TRUE;
        }
    } while (Process32NextW(snapshot, &entry));

    CloseHandle(snapshot);
    return found && *private_pages >= 64 * MIB / page_size;
}

static void NotifyKilledProcess(DWORD session, const wchar_t *name, DWORD pid,
                                uint64_t private_pages, uint32_t page_size)
{
    static wchar_t title[] = L"Windows OOM Killer";
    wchar_t message[512];
    DWORD response;

    _snwprintf_s(message, ARRAYSIZE(message), _TRUNCATE,
                 L"%ls (PID %lu) was terminated because Windows was almost out of "
                 L"committed memory. It was using %llu MiB of private memory.",
                 name, pid, private_pages * page_size / MIB);
    if (!WTSSendMessageW(WTS_CURRENT_SERVER_HANDLE, session, title,
                         (DWORD)(wcslen(title) * sizeof(*title)), message,
                         (DWORD)(wcslen(message) * sizeof(*message)),
                         MB_OK | MB_ICONWARNING | MB_SETFOREGROUND, 0, &response, FALSE))
        LogMessage(EVENTLOG_WARNING_TYPE,
                   L"could not notify session %lu: win32 error %lu", session,
                   GetLastError());
}

static DWORD RunMonitor(void)
{
    HANDLE device;
    OOM_POLICY_STATE policy = {0};
    OOM_THRESHOLDS thresholds;
    SYSTEM_INFO system_info;
    uint64_t sequence = 0;
    DWORD result = ERROR_SUCCESS;
    DWORD kill_failures = 0;
    BOOL armed = ReadSetting(L"Armed", 0) != 0;
    BOOL bugcheck_notification_complete = NotifyPreviousBugcheck();
    ULONGLONG next_bugcheck_notification = GetTickCount64() + 10000;

    GetSystemInfo(&system_info);
    thresholds.min_commit_headroom =
        (uint64_t)ReadSetting(L"CommitHeadroomMiB", 512) * MIB / system_info.dwPageSize;
    thresholds.confirmation_samples = ReadSetting(L"ConfirmationSamples", 1);
    thresholds.kill_retry_samples = ReadSetting(L"KillRetrySamples", 2);
    thresholds.max_kills = ReadSetting(L"MaxKills", 3);
    if (!thresholds.confirmation_samples)
        thresholds.confirmation_samples = 1;
    if (!thresholds.kill_retry_samples)
        thresholds.kill_retry_samples = 1;
    if (!thresholds.max_kills || thresholds.max_kills > 10)
        thresholds.max_kills = 3;

    LogMessage(EVENTLOG_INFORMATION_TYPE,
               L"monitor started %ls: commit headroom %llu pages",
               armed ? L"armed" : L"disarmed", thresholds.min_commit_headroom);

    device = CreateFileW(OOM_DEVICE_PATH, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (device == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        LogMessage(EVENTLOG_ERROR_TYPE, L"cannot open driver device: win32 error %lu",
                   error);
        return error;
    }

    for (;;) {
        OOM_TELEMETRY telemetry;
        OOM_SAMPLE sample;
        OOM_ACTION action;

        if (!bugcheck_notification_complete &&
            GetTickCount64() >= next_bugcheck_notification) {
            bugcheck_notification_complete = NotifyPreviousBugcheck();
            next_bugcheck_notification = GetTickCount64() + 10000;
        }

        if (!CollectTelemetry(&telemetry, ++sequence, &thresholds, armed)) {
            LogMessage(EVENTLOG_ERROR_TYPE, L"memory telemetry failed: win32 error %lu",
                       GetLastError());
            if (WaitForSingleObject(stop_event, 1000) != WAIT_TIMEOUT)
                break;
            continue;
        }
        sample.commit_charge = telemetry.commit_charge_pages;
        sample.commit_limit = telemetry.commit_limit_pages;
        action = armed ? OomPolicyObserve(&policy, &thresholds, &sample) : OOM_ACTION_NONE;
        if (!(telemetry.flags & OOM_FLAG_CRITICAL))
            kill_failures = 0;

        if (!SendIoctl(device, IOCTL_OOM_HEARTBEAT, &telemetry, sizeof(telemetry))) {
            result = GetLastError();
            LogMessage(EVENTLOG_ERROR_TYPE, L"driver heartbeat failed: win32 error %lu",
                       result);
            break;
        }

        if (action == OOM_ACTION_ESCALATE) {
            LogMessage(EVENTLOG_ERROR_TYPE,
                       L"memory is still critical after %lu recovery kills",
                       policy.kills_issued);
            SendIoctl(device, IOCTL_OOM_ESCALATE, &telemetry, sizeof(telemetry));
        } else if (action == OOM_ACTION_KILL) {
            OOM_KILL_REQUEST kill = {0};
            DWORD victim_session;
            wchar_t victim_name[MAX_PATH];
            kill.telemetry = telemetry;
            if (!FindVictim(telemetry.page_size, &kill.victim_pid,
                            &kill.victim_private_pages, &kill.victim_create_time,
                            &victim_session, victim_name, ARRAYSIZE(victim_name))) {
                if (telemetry.backing_reason == OOM_BACKING_UNKNOWN)
                    telemetry.backing_reason = OOM_BACKING_NO_ELIGIBLE_PROCESS;
                LogMessage(EVENTLOG_ERROR_TYPE, L"memory is critical and no safe victim exists");
                SendIoctl(device, IOCTL_OOM_ESCALATE, &telemetry, sizeof(telemetry));
            } else if (SendIoctl(device, IOCTL_OOM_KILL, &kill, sizeof(kill))) {
                OomPolicyKillIssued(&policy);
                kill_failures = 0;
                LogMessage(EVENTLOG_WARNING_TYPE, L"terminated pid %lu using %llu private pages",
                           kill.victim_pid, kill.victim_private_pages);
                NotifyKilledProcess(victim_session, victim_name, kill.victim_pid,
                                    kill.victim_private_pages, telemetry.page_size);
            } else {
                LogMessage(EVENTLOG_ERROR_TYPE, L"driver rejected kill of pid %lu: win32 error %lu",
                           kill.victim_pid, GetLastError());
                if (++kill_failures >= 3) {
                    telemetry.backing_reason = OOM_BACKING_KILL_REJECTED;
                    SendIoctl(device, IOCTL_OOM_ESCALATE, &telemetry, sizeof(telemetry));
                }
            }
        }

        if (WaitForSingleObject(stop_event, 1000) != WAIT_TIMEOUT)
            break;
    }

    if (result == ERROR_SUCCESS) {
        OOM_TELEMETRY disarm;
        if (CollectTelemetry(&disarm, ++sequence, &thresholds, FALSE))
            SendIoctl(device, IOCTL_OOM_HEARTBEAT, &disarm, sizeof(disarm));
    }
    CloseHandle(device);
    return result;
}

#if defined(OOM_ENABLE_TEST_IOCTL)
static BOOL ParseU64(const wchar_t *text, uint64_t *value)
{
    wchar_t *end;
    unsigned long long parsed = wcstoull(text, &end, 0);

    if (!*text || *end)
        return FALSE;
    *value = (uint64_t)parsed;
    return TRUE;
}

static int RunBugcheckTest(int argc, wchar_t **argv)
{
    OOM_BUGCHECK_REQUEST request = {OOM_TEST_CONFIRM};
    HANDLE device;
    DWORD returned;
    uint64_t code;
    int i;

    if ((argc != 3 && argc != 7) || !ParseU64(argv[2], &code) || code > UINT32_MAX) {
        fwprintf(stderr, L"usage: %ls --bugcheck-test CODE [P1 P2 P3 P4]\n", argv[0]);
        return ERROR_INVALID_PARAMETER;
    }
    request.code = (uint32_t)code;
    for (i = 0; i < argc - 3; ++i) {
        if (!ParseU64(argv[i + 3], &request.parameters[i])) {
            fwprintf(stderr, L"invalid parameter: %ls\n", argv[i + 3]);
            return ERROR_INVALID_PARAMETER;
        }
    }

    device = CreateFileW(OOM_DEVICE_PATH, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (device == INVALID_HANDLE_VALUE) {
        fwprintf(stderr, L"cannot open driver device: win32 error %lu\n", GetLastError());
        return (int)GetLastError();
    }
    if (!DeviceIoControl(device, IOCTL_OOM_TEST_BUGCHECK, &request, sizeof(request),
                         NULL, 0, &returned, NULL)) {
        DWORD error = GetLastError();
        fwprintf(stderr, L"bugcheck request rejected: win32 error %lu\n", error);
        CloseHandle(device);
        return (int)error;
    }
    CloseHandle(device);
    return ERROR_SUCCESS;
}
#endif

static void SetServiceState(DWORD state, DWORD error)
{
    service_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    service_status.dwCurrentState = state;
    service_status.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP : 0;
    service_status.dwWin32ExitCode = error;
    SetServiceStatus(service_status_handle, &service_status);
}

static DWORD WINAPI ServiceControl(DWORD control, DWORD event_type, void *event_data,
                                   void *context)
{
    UNREFERENCED_PARAMETER(event_type);
    UNREFERENCED_PARAMETER(event_data);
    UNREFERENCED_PARAMETER(context);
    if (control == SERVICE_CONTROL_STOP) {
        SetServiceState(SERVICE_STOP_PENDING, ERROR_SUCCESS);
        SetEvent(stop_event);
    }
    return ERROR_SUCCESS;
}

static void WINAPI ServiceMain(DWORD argc, wchar_t **argv)
{
    DWORD error;
    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);

    service_status_handle = RegisterServiceCtrlHandlerExW(SERVICE_NAME, ServiceControl, NULL);
    if (!service_status_handle)
        return;
    stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!stop_event) {
        SetServiceState(SERVICE_STOPPED, GetLastError());
        return;
    }
    SetServiceState(SERVICE_RUNNING, ERROR_SUCCESS);
    error = RunMonitor();
    CloseHandle(stop_event);
    SetServiceState(SERVICE_STOPPED, error);
}

int wmain(int argc, wchar_t **argv)
{
    SERVICE_TABLE_ENTRYW services[] = {{SERVICE_NAME, ServiceMain}, {NULL, NULL}};

#if defined(OOM_ENABLE_TEST_IOCTL)
    if (argc >= 2 && !wcscmp(argv[1], L"--bugcheck-test"))
        return RunBugcheckTest(argc, argv);
#endif
    if (argc == 2 && !wcscmp(argv[1], L"--console")) {
        DWORD error;
        console_mode = TRUE;
        stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (!stop_event)
            return (int)GetLastError();
        error = RunMonitor();
        CloseHandle(stop_event);
        return (int)error;
    }
    if (!StartServiceCtrlDispatcherW(services))
        return (int)GetLastError();
    return 0;
}
