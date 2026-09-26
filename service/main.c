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
#define MAX_SAMPLE_SETTING 10u
#define MAX_RECOVERY_FAILURES 3u
/* WER writes the crash event a while after boot, often after this service starts. */
#define CRASH_EVENT_WINDOW_MS (10ull * 60 * 1000)
#define CRASH_RETRY_MS 10000u
#define CRASH_QUERY_TIMEOUT_MS 5000u
#define NOTIFIER_JOIN_MS 10000u
#define ERROR_LOG_INTERVAL_MS 60000u

static SERVICE_STATUS_HANDLE service_status_handle;
static SERVICE_STATUS service_status;
static SRWLOCK service_status_lock = SRWLOCK_INIT;
static HANDLE stop_event;
static HANDLE console_done_event;
static HANDLE event_source;
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
    /* RunMonitor keeps one source open, so a line costs one rpc instead of three. */
    if (event_source) {
        ReportEventW(event_source, type, 0, 0, NULL, 1, 0, strings, NULL);
        return;
    }
    source = RegisterEventSourceW(NULL, SERVICE_NAME);
    if (source) {
        ReportEventW(source, type, 0, 0, NULL, 1, 0, strings, NULL);
        DeregisterEventSource(source);
    }
}

/* A failed RegGetValueW can leave raw data of the wrong type in the buffer,
 * so a value is only trusted when the call succeeds. */
static BOOL ReadSettingValue(const wchar_t *name, DWORD flags, void *value, DWORD value_size)
{
    DWORD size = value_size, type = REG_NONE;
    LSTATUS status = RegGetValueW(HKEY_LOCAL_MACHINE, SETTINGS_KEY, name, flags, &type, value,
                                  &size);

    if (status == ERROR_SUCCESS)
        return TRUE;
    if (status != ERROR_FILE_NOT_FOUND)
        LogMessage(EVENTLOG_WARNING_TYPE,
                   L"ignoring setting %ls (registry type %lu): win32 error %ld; using the default",
                   name, type, status);
    return FALSE;
}

static DWORD ReadSetting(const wchar_t *name, DWORD fallback)
{
    DWORD value = 0;
    return ReadSettingValue(name, RRF_RT_REG_DWORD, &value, (DWORD)sizeof(value)) ? value
                                                                                  : fallback;
}

static uint64_t ReadSettingQword(const wchar_t *name)
{
    uint64_t value = 0;
    return ReadSettingValue(name, RRF_RT_REG_QWORD, &value, (DWORD)sizeof(value)) ? value : 0;
}

static DWORD ClampSetting(const wchar_t *name, DWORD value, DWORD low, DWORD high)
{
    DWORD clamped = value < low ? low : value > high ? high : value;

    if (clamped != value)
        LogMessage(EVENTLOG_WARNING_TYPE, L"%ls=%lu is outside %lu..%lu; using %lu", name,
                   value, low, high, clamped);
    return clamped;
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

/* Returns FALSE while a custom crash may still need to reach a user session:
 * during the first minutes after boot (WER may not have logged it yet), when
 * the event log cannot be read, or when no session could be told. */
static BOOL NotifyPreviousBugcheck(void)
{
    static const wchar_t *paths[] = {
        L"Event/System/EventRecordID",
        L"Event/EventData/Data[@Name='param1']",
    };
    static wchar_t title[] = L"Windows OOM Killer";
    static uint64_t notified_record; /* in case the registry marker cannot be saved */
    EVT_HANDLE query = NULL, event = NULL, context = NULL;
    PEVT_VARIANT values = NULL;
    const OOM_BUGCHECK_INFO *info;
    uint64_t parsed[5], record_id, backing;
    DWORD returned, size = 0, count = 0, session, response;
    wchar_t message[1024];
    BOOL complete = GetTickCount64() >= CRASH_EVENT_WINDOW_MS;

    query = EvtQuery(NULL, L"System",
                     L"*[System[Provider[@Name='Microsoft-Windows-WER-SystemErrorReporting'] "
                     L"and (EventID=1001)]]",
                     EvtQueryChannelPath | EvtQueryReverseDirection);
    if (!query) {
        complete = FALSE;
        goto cleanup;
    }
    if (!EvtNext(query, 1, &event, CRASH_QUERY_TIMEOUT_MS, 0, &returned)) {
        if (GetLastError() != ERROR_NO_MORE_ITEMS)
            complete = FALSE;
        event = NULL;
        goto cleanup;
    }
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
    if (!info || record_id == notified_record ||
        ReadSettingQword(L"LastNotifiedBugcheckRecord") == record_id)
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
    notified_record = record_id;
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

/* Runs beside the monitor loop, so a slow event-log or wts rpc can never
 * delay a heartbeat past the driver's watchdog. */
static DWORD WINAPI CrashNotifier(void *context)
{
    UNREFERENCED_PARAMETER(context);
    for (;;) {
        if (NotifyPreviousBugcheck() ||
            WaitForSingleObject(stop_event, CRASH_RETRY_MS) != WAIT_TIMEOUT)
            break;
    }
    return 0;
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

/* A successful escalation never returns, so returning at all is worth a log line. */
static void Escalate(HANDLE device, const OOM_TELEMETRY *telemetry)
{
    if (SendIoctl(device, IOCTL_OOM_ESCALATE, telemetry, sizeof(*telemetry)))
        LogMessage(EVENTLOG_WARNING_TYPE,
                   L"the driver declined to escalate: pressure is below its ceiling");
    else
        LogMessage(EVENTLOG_ERROR_TYPE, L"escalation failed: win32 error %lu", GetLastError());
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

typedef enum VICTIM_RESULT {
    VICTIM_FOUND,
    VICTIM_NONE_ELIGIBLE,
    VICTIM_ENUM_FAILED /* last error is set */
} VICTIM_RESULT;

/* Victims the driver refused during the current pressure episode. */
typedef struct REFUSED_VICTIMS {
    DWORD count;
    uint32_t pid[MAX_RECOVERY_FAILURES];
    uint64_t create_time[MAX_RECOVERY_FAILURES];
} REFUSED_VICTIMS;

static BOOL WasRefused(const REFUSED_VICTIMS *refused, uint32_t pid, uint64_t create_time)
{
    DWORD i;

    for (i = 0; i < refused->count; ++i)
        if (refused->pid[i] == pid && refused->create_time[i] == create_time)
            return TRUE;
    return FALSE;
}

static VICTIM_RESULT FindVictim(uint32_t page_size, const REFUSED_VICTIMS *refused,
                                uint32_t *victim_pid, uint64_t *private_pages,
                                uint64_t *create_time, DWORD *victim_session,
                                wchar_t *victim_name, size_t victim_name_count)
{
    PROCESSENTRY32W entry = {(DWORD)sizeof(entry)};
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    DWORD self = GetCurrentProcessId();
    BOOL found = FALSE;

    if (snapshot == INVALID_HANDLE_VALUE)
        return VICTIM_ENUM_FAILED;
    if (!Process32FirstW(snapshot, &entry)) {
        DWORD error = GetLastError();
        CloseHandle(snapshot);
        SetLastError(error);
        return VICTIM_ENUM_FAILED;
    }

    do {
        HANDLE process;
        PROCESS_MEMORY_COUNTERS_EX counters = {0};
        PROCESS_PROTECTION_LEVEL_INFORMATION protection = {0};
        FILETIME created, exited, kernel, user;
        uint64_t pages, created_at;
        DWORD session;
        BOOL critical;

        if (entry.th32ProcessID <= 4 || entry.th32ProcessID == self ||
            !ProcessIdToSessionId(entry.th32ProcessID, &session) || session == 0)
            continue;
        process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
        if (!process)
            continue;
        /* the driver refuses protected processes; offering one would only burn a retry */
        if (!IsProcessCritical(process, &critical) || critical ||
            !GetProcessInformation(process, ProcessProtectionLevelInfo, &protection,
                                   (DWORD)sizeof(protection)) ||
            protection.ProtectionLevel != PROTECTION_LEVEL_NONE ||
            !GetProcessMemoryInfo(process, (PROCESS_MEMORY_COUNTERS *)&counters,
                                  (DWORD)sizeof(counters)) ||
            !GetProcessTimes(process, &created, &exited, &kernel, &user)) {
            CloseHandle(process);
            continue;
        }
        CloseHandle(process);
        pages = (uint64_t)counters.PrivateUsage / page_size;
        created_at = ((uint64_t)created.dwHighDateTime << 32) | created.dwLowDateTime;
        if (pages > *private_pages && !WasRefused(refused, entry.th32ProcessID, created_at)) {
            *victim_pid = entry.th32ProcessID;
            *private_pages = pages;
            *create_time = created_at;
            *victim_session = session;
            wcsncpy_s(victim_name, victim_name_count, entry.szExeFile, _TRUNCATE);
            found = TRUE;
        }
    } while (Process32NextW(snapshot, &entry));

    CloseHandle(snapshot);
    return found && *private_pages >= OOM_MIN_VICTIM_BYTES / page_size ? VICTIM_FOUND
                                                                      : VICTIM_NONE_ELIGIBLE;
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
    HANDLE device, notifier;
    OOM_POLICY_STATE policy = {0};
    OOM_THRESHOLDS thresholds;
    SYSTEM_INFO system_info;
    REFUSED_VICTIMS refused = {0};
    uint64_t sequence = 0;
    DWORD result = ERROR_SUCCESS;
    DWORD kill_failures = 0, enum_failures = 0, armed_setting, headroom_mib;
    ULONGLONG next_telemetry_error = 0;
    BOOL armed;

    event_source = RegisterEventSourceW(NULL, SERVICE_NAME);

    /* only an explicit REG_DWORD 1 arms; anything unreadable stays disarmed */
    armed_setting = ReadSetting(L"Armed", 0);
    armed = armed_setting == 1;
    if (armed_setting > 1)
        LogMessage(EVENTLOG_WARNING_TYPE, L"Armed=%lu is neither 0 nor 1; staying disarmed",
                   armed_setting);

    /* the driver refuses anything its own ceiling does not justify, so a larger
     * headroom would only produce refused kills */
    GetSystemInfo(&system_info);
    headroom_mib = ClampSetting(L"CommitHeadroomMiB", ReadSetting(L"CommitHeadroomMiB", 512), 0,
                                (DWORD)(OOM_COMMIT_HEADROOM_BYTES / MIB));
    thresholds.min_commit_headroom = (uint64_t)headroom_mib * MIB / system_info.dwPageSize;
    thresholds.confirmation_samples = ClampSetting(
        L"ConfirmationSamples", ReadSetting(L"ConfirmationSamples", 1), 1, MAX_SAMPLE_SETTING);
    thresholds.kill_retry_samples = ClampSetting(
        L"KillRetrySamples", ReadSetting(L"KillRetrySamples", 2), 1, MAX_SAMPLE_SETTING);
    thresholds.max_kills = ReadSetting(L"MaxKills", 3);
    if (!thresholds.max_kills || thresholds.max_kills > 10) {
        LogMessage(EVENTLOG_WARNING_TYPE, L"MaxKills=%lu is outside 1..10; using 3",
                   thresholds.max_kills);
        thresholds.max_kills = 3;
    }

    LogMessage(EVENTLOG_INFORMATION_TYPE,
               L"monitor started %ls: commit headroom %llu pages (%lu MiB), "
               L"%lu confirmation sample(s), %lu retry sample(s), at most %lu kill(s)",
               armed ? L"armed" : L"disarmed", thresholds.min_commit_headroom, headroom_mib,
               thresholds.confirmation_samples, thresholds.kill_retry_samples,
               thresholds.max_kills);

    notifier = CreateThread(NULL, 0, CrashNotifier, NULL, 0, NULL);
    if (!notifier)
        LogMessage(EVENTLOG_WARNING_TYPE, L"cannot start the crash notifier: win32 error %lu",
                   GetLastError());

    device = CreateFileW(OOM_DEVICE_PATH, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (device == INVALID_HANDLE_VALUE) {
        result = GetLastError();
        LogMessage(EVENTLOG_ERROR_TYPE, L"cannot open driver device: win32 error %lu",
                   result);
    }

    while (device != INVALID_HANDLE_VALUE) {
        OOM_TELEMETRY telemetry;
        OOM_SAMPLE sample;
        OOM_ACTION action;

        if (!CollectTelemetry(&telemetry, ++sequence, &thresholds, armed)) {
            DWORD error = GetLastError();
            if (GetTickCount64() >= next_telemetry_error) {
                LogMessage(EVENTLOG_ERROR_TYPE, L"memory telemetry failed: win32 error %lu",
                           error);
                next_telemetry_error = GetTickCount64() + ERROR_LOG_INTERVAL_MS;
            }
            if (WaitForSingleObject(stop_event, 1000) != WAIT_TIMEOUT)
                break;
            continue;
        }
        sample.commit_charge = telemetry.commit_charge_pages;
        sample.commit_limit = telemetry.commit_limit_pages;
        action = armed ? OomPolicyObserve(&policy, &thresholds, &sample) : OOM_ACTION_NONE;
        if (!(telemetry.flags & OOM_FLAG_CRITICAL)) {
            kill_failures = 0;
            enum_failures = 0;
            refused.count = 0;
        }

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
            Escalate(device, &telemetry);
        } else if (action == OOM_ACTION_KILL) {
            OOM_KILL_REQUEST kill = {0};
            DWORD victim_session = 0;
            wchar_t victim_name[MAX_PATH];
            VICTIM_RESULT victim;

            kill.telemetry = telemetry;
            victim = FindVictim(telemetry.page_size, &refused, &kill.victim_pid,
                                &kill.victim_private_pages, &kill.victim_create_time,
                                &victim_session, victim_name, ARRAYSIZE(victim_name));
            if (victim == VICTIM_ENUM_FAILED) {
                /* a snapshot needs commit too; one failure at the limit is not proof
                 * that nothing can be killed, so retry on the next samples first */
                LogMessage(EVENTLOG_ERROR_TYPE, L"cannot enumerate processes: win32 error %lu",
                           GetLastError());
                if (++enum_failures >= MAX_RECOVERY_FAILURES)
                    Escalate(device, &telemetry);
            } else if (victim == VICTIM_NONE_ELIGIBLE) {
                enum_failures = 0;
                if (telemetry.backing_reason == OOM_BACKING_UNKNOWN)
                    telemetry.backing_reason = OOM_BACKING_NO_ELIGIBLE_PROCESS;
                LogMessage(EVENTLOG_ERROR_TYPE, L"memory is critical and no safe victim exists");
                Escalate(device, &telemetry);
            } else if (SendIoctl(device, IOCTL_OOM_KILL, &kill, sizeof(kill))) {
                OOM_TELEMETRY after;
                enum_failures = 0;
                OomPolicyKillIssued(&policy);
                kill_failures = 0;
                /* the kill may have used most of the watchdog's budget: heartbeat
                 * before the slower logging and notification */
                if (CollectTelemetry(&after, ++sequence, &thresholds, armed) &&
                    !SendIoctl(device, IOCTL_OOM_HEARTBEAT, &after, sizeof(after))) {
                    result = GetLastError();
                    LogMessage(EVENTLOG_ERROR_TYPE, L"driver heartbeat failed: win32 error %lu",
                               result);
                    break;
                }
                LogMessage(EVENTLOG_WARNING_TYPE, L"terminated pid %lu using %llu private pages",
                           kill.victim_pid, kill.victim_private_pages);
                NotifyKilledProcess(victim_session, victim_name, kill.victim_pid,
                                    kill.victim_private_pages, telemetry.page_size);
            } else {
                DWORD error = GetLastError();
                enum_failures = 0;
                LogMessage(EVENTLOG_ERROR_TYPE, L"driver rejected kill of pid %lu: win32 error %lu",
                           kill.victim_pid, error);
                /* offer a different victim next time instead of the same one again */
                if (refused.count < ARRAYSIZE(refused.pid)) {
                    refused.pid[refused.count] = kill.victim_pid;
                    refused.create_time[refused.count] = kill.victim_create_time;
                    ++refused.count;
                }
                if (++kill_failures >= MAX_RECOVERY_FAILURES) {
                    telemetry.backing_reason = OOM_BACKING_KILL_REJECTED;
                    Escalate(device, &telemetry);
                }
            }
        }

        if (WaitForSingleObject(stop_event, 1000) != WAIT_TIMEOUT)
            break;
    }

    if (device != INVALID_HANDLE_VALUE) {
        if (result == ERROR_SUCCESS) {
            OOM_TELEMETRY disarm;
            if (CollectTelemetry(&disarm, ++sequence, &thresholds, FALSE))
                SendIoctl(device, IOCTL_OOM_HEARTBEAT, &disarm, sizeof(disarm));
        }
        CloseHandle(device);
    }

    /* also stops the notifier when the loop ended on a driver error */
    SetEvent(stop_event);
    if (notifier) {
        BOOL joined = WaitForSingleObject(notifier, NOTIFIER_JOIN_MS) == WAIT_OBJECT_0;
        CloseHandle(notifier);
        if (!joined)
            return result; /* it may still be logging; process exit closes the source */
    }
    if (event_source) {
        DeregisterEventSource(event_source);
        event_source = NULL;
    }
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
        DWORD error = GetLastError();
        fwprintf(stderr, L"cannot open driver device: win32 error %lu\n", error);
        return (int)error;
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

/* Serialised because the control handler and ServiceMain run on different
 * threads. STOPPED is final: the process can be torn down right after it. */
static BOOL SetServiceState(DWORD state, DWORD error, BOOL only_if_running)
{
    BOOL reported = FALSE;

    AcquireSRWLockExclusive(&service_status_lock);
    if (service_status.dwCurrentState != SERVICE_STOPPED &&
        (!only_if_running || service_status.dwCurrentState == SERVICE_RUNNING)) {
        service_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
        service_status.dwCurrentState = state;
        service_status.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP : 0;
        service_status.dwWin32ExitCode = error;
        service_status.dwCheckPoint = 0;
        /* a stop may first wait out a kill in progress (up to about 5 s) */
        service_status.dwWaitHint = state == SERVICE_STOP_PENDING ? 15000 : 0;
        SetServiceStatus(service_status_handle, &service_status);
        reported = TRUE;
    }
    ReleaseSRWLockExclusive(&service_status_lock);
    return reported;
}

static DWORD WINAPI ServiceControl(DWORD control, DWORD event_type, void *event_data,
                                   void *context)
{
    UNREFERENCED_PARAMETER(event_type);
    UNREFERENCED_PARAMETER(event_data);
    UNREFERENCED_PARAMETER(context);
    switch (control) {
    case SERVICE_CONTROL_STOP:
        if (SetServiceState(SERVICE_STOP_PENDING, ERROR_SUCCESS, TRUE))
            SetEvent(stop_event);
        return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
        return NO_ERROR;
    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
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
        SetServiceState(SERVICE_STOPPED, GetLastError(), FALSE);
        return;
    }
    SetServiceState(SERVICE_RUNNING, ERROR_SUCCESS, FALSE);
    error = RunMonitor();
    /* stop_event stays open: the control handler may still be signalling it */
    SetServiceState(SERVICE_STOPPED, error, FALSE);
}

/* Ctrl+C in --console mode stops like SERVICE_CONTROL_STOP, so the driver is
 * disarmed instead of being left armed as if the monitor had crashed. */
static BOOL WINAPI ConsoleControl(DWORD type)
{
    SetEvent(stop_event);
    /* returning from these ends the process, so let the disarm heartbeat go out */
    if (type == CTRL_CLOSE_EVENT || type == CTRL_LOGOFF_EVENT || type == CTRL_SHUTDOWN_EVENT)
        WaitForSingleObject(console_done_event, 4000);
    return TRUE;
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
        console_done_event = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (!stop_event || !console_done_event || !SetConsoleCtrlHandler(ConsoleControl, TRUE))
            return (int)GetLastError();
        error = RunMonitor();
        SetEvent(console_done_event);
        /* both events stay open: a late ctrl handler may still be using them */
        return (int)error;
    }
    if (!StartServiceCtrlDispatcherW(services))
        return (int)GetLastError();
    return 0;
}
