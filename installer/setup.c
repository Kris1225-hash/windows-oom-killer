/*
 * WinOomKillerSetup: installs or removes the windows oom killer.
 *
 * online mode (a running windows) registers the services through the SCM,
 * exactly like scripts/install.ps1. offline mode (WinRE, WinPE, or another
 * windows install mounted on this machine) loads the target's SYSTEM and
 * SOFTWARE hives and writes the same service keys directly, so the install
 * takes effect on that windows' next boot.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <wctype.h>

#define SERVICE_NAME L"WinOomKiller"
#define DRIVER_NAME L"WinOomKillerDriver"
#define DRIVER_FILE L"WinOomKillerDriver.sys"
#define SERVICE_FILE L"WinOomKillerService.exe"
#define PRODUCT_DIR L"WinOomKiller"
#define SETTINGS_KEY L"SOFTWARE\\WinOomKiller"
#define TEST_TASK_NAME L"WinOomKillerBugcheckTests"
#define OFFLINE_SYSTEM L"WinOomKillerOfflineSystem"
#define OFFLINE_SOFTWARE L"WinOomKillerOfflineSoftware"
#define MAX_CONTROL_SETS 16
#define STOP_TIMEOUT_MS 30000ul

typedef struct SETUP_OPTIONS {
    BOOL install;
    BOOL arm;
    BOOL stage_only;
    const wchar_t *target;
    const wchar_t *payload;
} SETUP_OPTIONS;

typedef struct OFFLINE_TARGET {
    wchar_t windir[MAX_PATH];        /* offline windows dir as seen from here, e.g. D:\Windows */
    wchar_t root[MAX_PATH];          /* offline system drive as seen from here, e.g. D: */
    wchar_t online_windir[MAX_PATH]; /* the same dir as its own windows sees it, e.g. C:\Windows */
    wchar_t online_pf[MAX_PATH];     /* its program files dir, e.g. C:\Program Files */
    wchar_t offline_pf[MAX_PATH];    /* that program files dir as seen from here */
    HKEY system;
    HKEY software;
    BOOL system_loaded;
    BOOL software_loaded;
} OFFLINE_TARGET;

static int failures;

static void Fail(const wchar_t *format, ...)
{
    va_list args;

    va_start(args, format);
    fwprintf(stderr, L"error: ");
    vfwprintf(stderr, format, args);
    fwprintf(stderr, L"\n");
    va_end(args);
    ++failures;
}

static void Warn(const wchar_t *format, ...)
{
    va_list args;

    va_start(args, format);
    fwprintf(stderr, L"warning: ");
    vfwprintf(stderr, format, args);
    fwprintf(stderr, L"\n");
    va_end(args);
}

static void PrintSafetyNotice(void)
{
    wprintf(L"\n"
            L"  SAFETY NOTICE\n"
            L"  this software terminates processes without the consent of their owners\n"
            L"  when commit memory runs out, and deliberately crashes windows with a\n"
            L"  bugcheck when recovery fails. only install it on a machine you are\n"
            L"  prepared to lose. never deploy it where a crash could cost lives.\n"
            L"  it installs disarmed; see LICENSE for the full terms.\n");
#if defined(OOM_ENABLE_TEST_IOCTL)
    wprintf(L"\n"
            L"  CRASH-TEST BUILD\n"
            L"  this is the BugcheckTest build. its service can bugcheck the machine on\n"
            L"  demand (--bugcheck-test). it is test-only and not a release build.\n");
#endif
    wprintf(L"\n");
}

static void PrintUsage(void)
{
    wprintf(L"usage:\n"
            L"  WinOomKillerSetup install   [--target PATH] [--arm] [--stage-only] [--payload DIR]\n"
            L"  WinOomKillerSetup uninstall [--target PATH]\n"
            L"\n"
            L"  --target PATH  install into an offline windows instead of the running one.\n"
            L"                 PATH is that windows' drive (D:\\) or its windows dir\n"
            L"                 (D:\\Windows). required in WinRE, where your C: drive\n"
            L"                 usually shows up under another letter.\n"
            L"  --arm          enable enforcement (default: installed disarmed)\n"
            L"  --stage-only   register everything but load nothing until the next boot\n"
            L"                 (online only; offline installs always wait for a boot)\n"
            L"  --payload DIR  where " DRIVER_FILE L" and " SERVICE_FILE L" are\n"
            L"                 (default: next to this exe)\n");
}

static BOOL JoinPath(wchar_t *out, size_t out_count, const wchar_t *base, const wchar_t *tail)
{
    size_t length = wcslen(base);
    const wchar_t *separator = length && base[length - 1] == L'\\' ? L"" : L"\\";

    if (*tail == L'\\')
        ++tail;
    return _snwprintf_s(out, out_count, _TRUNCATE, L"%ls%ls%ls", base, separator, tail) >= 0;
}

static BOOL FileExists(const wchar_t *path)
{
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

static BOOL DirectoryExists(const wchar_t *path)
{
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

static BOOL IsRecoveryEnvironment(void)
{
    HKEY key;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\MiniNT", 0,
                      KEY_READ, &key) != ERROR_SUCCESS)
        return FALSE;
    RegCloseKey(key);
    return TRUE;
}

static BOOL EnablePrivilege(const wchar_t *name)
{
    TOKEN_PRIVILEGES privileges = {0};
    HANDLE token;
    BOOL ok;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return FALSE;
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    ok = LookupPrivilegeValueW(NULL, name, &privileges.Privileges[0].Luid) &&
         AdjustTokenPrivileges(token, FALSE, &privileges, 0, NULL, NULL) &&
         GetLastError() == ERROR_SUCCESS;
    CloseHandle(token);
    return ok;
}

static BOOL CreateDirectoryTree(const wchar_t *path)
{
    wchar_t partial[MAX_PATH];
    size_t i;

    if (wcslen(path) >= ARRAYSIZE(partial))
        return FALSE;
    for (i = 0; path[i]; ++i) {
        partial[i] = path[i];
        if ((path[i] == L'\\' || path[i] == L'/') && i > 2) {
            partial[i] = L'\0';
            if (!CreateDirectoryW(partial, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
                return FALSE;
            partial[i] = path[i];
        }
    }
    partial[i] = L'\0';
    return CreateDirectoryW(partial, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

static BOOL CopyPayloadFile(const wchar_t *source, const wchar_t *target_dir, const wchar_t *name)
{
    wchar_t target[MAX_PATH];

    if (!CreateDirectoryTree(target_dir)) {
        Fail(L"cannot create %ls: win32 error %lu", target_dir, GetLastError());
        return FALSE;
    }
    if (!JoinPath(target, ARRAYSIZE(target), target_dir, name))
        return FALSE;
    SetFileAttributesW(target, FILE_ATTRIBUTE_NORMAL);
    if (!CopyFileW(source, target, FALSE)) {
        DWORD error = GetLastError();
        Fail(L"cannot copy %ls to %ls: win32 error %lu%ls", source, target, error,
             error == ERROR_SHARING_VIOLATION ? L" (still loaded? reboot and retry)" : L"");
        return FALSE;
    }
    wprintf(L"  copied %ls\n", target);
    return TRUE;
}

/* Deletes a file, or schedules it for the next boot if something still holds it. */
static void RemoveFileOrDefer(const wchar_t *path, BOOL can_defer)
{
    DWORD error;

    SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL);
    if (DeleteFileW(path)) {
        wprintf(L"  removed %ls\n", path);
        return;
    }
    error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
        return;
    if (can_defer && MoveFileExW(path, NULL, MOVEFILE_DELAY_UNTIL_REBOOT)) {
        wprintf(L"  %ls is in use; it will be removed at the next boot\n", path);
        return;
    }
    Fail(L"cannot remove %ls: win32 error %lu", path, error);
}

static void RemoveDirectoryTree(const wchar_t *path, BOOL can_defer)
{
    wchar_t pattern[MAX_PATH], child[MAX_PATH];
    WIN32_FIND_DATAW entry;
    HANDLE find;

    if (!DirectoryExists(path) || !JoinPath(pattern, ARRAYSIZE(pattern), path, L"*"))
        return;
    find = FindFirstFileW(pattern, &entry);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(entry.cFileName, L".") || !wcscmp(entry.cFileName, L".."))
                continue;
            if (!JoinPath(child, ARRAYSIZE(child), path, entry.cFileName))
                continue;
            if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                !(entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                RemoveDirectoryTree(child, can_defer);
            else if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                RemoveDirectoryW(child);
            else
                RemoveFileOrDefer(child, can_defer);
        } while (FindNextFileW(find, &entry));
        FindClose(find);
    }
    if (RemoveDirectoryW(path))
        wprintf(L"  removed %ls\n", path);
    else if (can_defer && MoveFileExW(path, NULL, MOVEFILE_DELAY_UNTIL_REBOOT))
        wprintf(L"  %ls will be removed at the next boot\n", path);
    else
        Fail(L"cannot remove %ls: win32 error %lu", path, GetLastError());
}

static BOOL SetDword(HKEY key, const wchar_t *name, DWORD value)
{
    LSTATUS status = RegSetValueExW(key, name, 0, REG_DWORD, (const BYTE *)&value,
                                    (DWORD)sizeof(value));
    if (status != ERROR_SUCCESS)
        Fail(L"cannot write registry value %ls: win32 error %ld", name, status);
    return status == ERROR_SUCCESS;
}

static BOOL SetString(HKEY key, const wchar_t *name, DWORD type, const wchar_t *value,
                      size_t chars_with_nulls)
{
    LSTATUS status = RegSetValueExW(key, name, 0, type, (const BYTE *)value,
                                    (DWORD)(chars_with_nulls * sizeof(wchar_t)));
    if (status != ERROR_SUCCESS)
        Fail(L"cannot write registry value %ls: win32 error %ld", name, status);
    return status == ERROR_SUCCESS;
}

static BOOL SetSz(HKEY key, const wchar_t *name, DWORD type, const wchar_t *value)
{
    return SetString(key, name, type, value, wcslen(value) + 1);
}

static BOOL WriteSettings(HKEY hklm_software, BOOL arm)
{
    HKEY key;
    LSTATUS status;
    BOOL ok;

    status = RegCreateKeyExW(hklm_software, L"WinOomKiller", 0, NULL, 0, KEY_WRITE, NULL, &key,
                             NULL);
    if (status != ERROR_SUCCESS) {
        Fail(L"cannot create the settings key: win32 error %ld", status);
        return FALSE;
    }
    ok = SetDword(key, L"Armed", arm ? 1 : 0) &&
         SetDword(key, L"CommitHeadroomMiB", 512) &&
         SetDword(key, L"ConfirmationSamples", 1) &&
         SetDword(key, L"KillRetrySamples", 2) &&
         SetDword(key, L"MaxKills", 3);
    RegCloseKey(key);
    return ok;
}

static void DeleteKeyTree(HKEY parent, const wchar_t *subkey, const wchar_t *label)
{
    LSTATUS status = RegDeleteTreeW(parent, subkey);

    if (status == ERROR_SUCCESS) {
        LSTATUS key_status = RegDeleteKeyW(parent, subkey);
        if (key_status != ERROR_FILE_NOT_FOUND)
            status = key_status;
    }
    if (status == ERROR_SUCCESS)
        wprintf(L"  removed %ls\n", label);
    else if (status != ERROR_FILE_NOT_FOUND)
        Fail(L"cannot remove %ls: win32 error %ld", label, status);
}

static BOOL ResolvePayload(const SETUP_OPTIONS *options, wchar_t *driver, wchar_t *service)
{
    wchar_t dir[MAX_PATH];

    if (options->payload) {
        DWORD length = GetFullPathNameW(options->payload, ARRAYSIZE(dir), dir, NULL);
        if (!length || length >= ARRAYSIZE(dir)) {
            Fail(L"bad --payload path %ls", options->payload);
            return FALSE;
        }
    } else {
        wchar_t *slash = NULL;
        DWORD length = GetModuleFileNameW(NULL, dir, ARRAYSIZE(dir));
        if (length && length < ARRAYSIZE(dir))
            slash = wcsrchr(dir, L'\\');
        if (!slash)
            return FALSE;
        *slash = L'\0';
    }
    if (!JoinPath(driver, MAX_PATH, dir, DRIVER_FILE) ||
        !JoinPath(service, MAX_PATH, dir, SERVICE_FILE))
        return FALSE;
    if (!FileExists(driver) || !FileExists(service)) {
        Fail(L"%ls and %ls must be in %ls (or pass --payload DIR)", DRIVER_FILE, SERVICE_FILE,
             dir);
        return FALSE;
    }
    return TRUE;
}

/* ---- online: the running windows, through the service control manager ---- */

static BOOL StopServiceAndWait(SC_HANDLE scm, const wchar_t *name)
{
    SERVICE_STATUS status;
    SC_HANDLE service = OpenServiceW(scm, name, SERVICE_STOP | SERVICE_QUERY_STATUS);
    DWORD waited = 0;

    if (!service)
        return GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST;
    if (!ControlService(service, SERVICE_CONTROL_STOP, &status)) {
        DWORD error = GetLastError();
        CloseServiceHandle(service);
        if (error == ERROR_SERVICE_NOT_ACTIVE)
            return TRUE;
        Warn(L"cannot stop %ls: win32 error %lu", name, error);
        return FALSE;
    }
    while (QueryServiceStatus(service, &status) && status.dwCurrentState != SERVICE_STOPPED &&
           waited < STOP_TIMEOUT_MS) {
        Sleep(250);
        waited += 250;
    }
    CloseServiceHandle(service);
    if (status.dwCurrentState != SERVICE_STOPPED) {
        Warn(L"%ls did not stop within %lu seconds", name, STOP_TIMEOUT_MS / 1000);
        return FALSE;
    }
    wprintf(L"  stopped %ls\n", name);
    return TRUE;
}

static SC_HANDLE CreateOrUpdateService(SC_HANDLE scm, const wchar_t *name, DWORD type,
                                       DWORD start, const wchar_t *path,
                                       const wchar_t *dependencies)
{
    SC_HANDLE service = CreateServiceW(scm, name, name, SERVICE_ALL_ACCESS, type, start,
                                       SERVICE_ERROR_NORMAL, path, NULL, NULL, dependencies,
                                       NULL, NULL);
    DWORD error;

    if (service) {
        wprintf(L"  created service %ls\n", name);
        return service;
    }
    error = GetLastError();
    if (error == ERROR_SERVICE_EXISTS) {
        service = OpenServiceW(scm, name, SERVICE_ALL_ACCESS);
        if (service && ChangeServiceConfigW(service, type, start, SERVICE_ERROR_NORMAL, path,
                                            NULL, NULL, dependencies, NULL, NULL, NULL)) {
            wprintf(L"  updated service %ls\n", name);
            return service;
        }
        error = GetLastError();
        if (service)
            CloseServiceHandle(service);
    }
    Fail(L"cannot register service %ls: win32 error %lu%ls", name, error,
         error == ERROR_SERVICE_MARKED_FOR_DELETE ? L" (marked for deletion; reboot first)" : L"");
    return NULL;
}

static BOOL StartServiceChecked(SC_HANDLE service, const wchar_t *name)
{
    DWORD error;

    if (StartServiceW(service, 0, NULL)) {
        wprintf(L"  started %ls\n", name);
        return TRUE;
    }
    error = GetLastError();
    if (error == ERROR_SERVICE_ALREADY_RUNNING)
        return TRUE;
    if (error == ERROR_INVALID_IMAGE_HASH)
        Fail(L"windows refused to load %ls (error 577): the driver is test-signed. run "
             L"'bcdedit /set testsigning on' (secure boot must be off) and reboot, or rerun "
             L"with --stage-only", name);
    else
        Fail(L"cannot start %ls: win32 error %lu", name, error);
    return FALSE;
}

static BOOL SystemDir(wchar_t *out, UINT out_count)
{
    UINT length = GetSystemDirectoryW(out, out_count);
    return length && length < out_count;
}

static BOOL ProgramFilesDir(wchar_t *out, DWORD out_count)
{
    DWORD length = GetEnvironmentVariableW(L"ProgramW6432", out, out_count);

    if (!length || length >= out_count)
        length = GetEnvironmentVariableW(L"ProgramFiles", out, out_count);
    return length && length < out_count;
}

static BOOL DataDir(wchar_t *out, DWORD out_count)
{
    wchar_t base[MAX_PATH];
    DWORD length = GetEnvironmentVariableW(L"ProgramData", base, ARRAYSIZE(base));

    return length && length < ARRAYSIZE(base) && JoinPath(out, out_count, base, PRODUCT_DIR);
}

static int InstallOnline(const SETUP_OPTIONS *options)
{
    wchar_t driver_source[MAX_PATH], service_source[MAX_PATH], system_dir[MAX_PATH];
    wchar_t drivers_dir[MAX_PATH], driver_target[MAX_PATH];
    wchar_t pf[MAX_PATH], install_dir[MAX_PATH], service_target[MAX_PATH];
    wchar_t service_command[MAX_PATH + 2];
    SC_ACTION actions[] = {{SC_ACTION_RESTART, 2000}, {SC_ACTION_RESTART, 5000},
                           {SC_ACTION_RESTART, 10000}};
    SERVICE_FAILURE_ACTIONSW failure = {60, NULL, NULL, ARRAYSIZE(actions), actions};
    SERVICE_FAILURE_ACTIONS_FLAG failure_flag = {TRUE};
    SC_HANDLE scm, driver = NULL, service = NULL;
    HKEY software;

    if (!ResolvePayload(options, driver_source, service_source))
        return 1;
    scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!scm) {
        Fail(L"cannot open the service control manager (win32 error %lu); run as administrator",
             GetLastError());
        return 1;
    }

    if (!SystemDir(system_dir, ARRAYSIZE(system_dir)) ||
        !JoinPath(drivers_dir, ARRAYSIZE(drivers_dir), system_dir, L"drivers") ||
        !JoinPath(driver_target, ARRAYSIZE(driver_target), drivers_dir, DRIVER_FILE) ||
        !ProgramFilesDir(pf, ARRAYSIZE(pf)) ||
        !JoinPath(install_dir, ARRAYSIZE(install_dir), pf, PRODUCT_DIR) ||
        !JoinPath(service_target, ARRAYSIZE(service_target), install_dir, SERVICE_FILE)) {
        Fail(L"cannot resolve the install paths");
        CloseServiceHandle(scm);
        return 1;
    }
    _snwprintf_s(service_command, ARRAYSIZE(service_command), _TRUNCATE, L"\"%ls\"",
                 service_target);

    wprintf(L"installing into the running windows (%ls)\n", drivers_dir);
    StopServiceAndWait(scm, SERVICE_NAME);
    StopServiceAndWait(scm, DRIVER_NAME);

    if (CopyPayloadFile(driver_source, drivers_dir, DRIVER_FILE) &&
        CopyPayloadFile(service_source, install_dir, SERVICE_FILE))
        driver = CreateOrUpdateService(scm, DRIVER_NAME, SERVICE_KERNEL_DRIVER,
                                       SERVICE_DEMAND_START, driver_target, NULL);
    if (driver)
        service = CreateOrUpdateService(scm, SERVICE_NAME, SERVICE_WIN32_OWN_PROCESS,
                                        SERVICE_AUTO_START, service_command,
                                        DRIVER_NAME L"\0");
    if (service) {
        if (!ChangeServiceConfig2W(service, SERVICE_CONFIG_FAILURE_ACTIONS, &failure) ||
            !ChangeServiceConfig2W(service, SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, &failure_flag))
            Fail(L"cannot configure service recovery: win32 error %lu", GetLastError());
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE", 0, KEY_WRITE, &software) ==
            ERROR_SUCCESS) {
            if (WriteSettings(software, options->arm))
                wprintf(L"  wrote HKLM\\%ls\n", SETTINGS_KEY);
            RegCloseKey(software);
        } else {
            Fail(L"cannot open HKLM\\SOFTWARE for writing");
        }
        if (!failures && !options->stage_only && StartServiceChecked(driver, DRIVER_NAME))
            StartServiceChecked(service, SERVICE_NAME);
    }

    if (service)
        CloseServiceHandle(service);
    if (driver)
        CloseServiceHandle(driver);
    CloseServiceHandle(scm);
    if (failures)
        return 1;
    wprintf(L"%ls; enforcement is %ls\n", options->stage_only ? L"staged" : L"installed",
            options->arm ? L"ARMED" : L"disarmed");
    return 0;
}

static void DeleteServiceByName(SC_HANDLE scm, const wchar_t *name)
{
    SC_HANDLE service = OpenServiceW(scm, name, DELETE);

    if (!service) {
        if (GetLastError() != ERROR_SERVICE_DOES_NOT_EXIST)
            Fail(L"cannot open service %ls: win32 error %lu", name, GetLastError());
        return;
    }
    if (DeleteService(service) || GetLastError() == ERROR_SERVICE_MARKED_FOR_DELETE)
        wprintf(L"  deleted service %ls\n", name);
    else
        Fail(L"cannot delete service %ls: win32 error %lu", name, GetLastError());
    CloseServiceHandle(service);
}

static void DeleteTestTaskOnline(void)
{
    wchar_t command[MAX_PATH + 64], system_dir[MAX_PATH];
    STARTUPINFOW startup = {0};
    PROCESS_INFORMATION process;
    DWORD exit_code = 1;

    if (!SystemDir(system_dir, ARRAYSIZE(system_dir)))
        return;
    _snwprintf_s(command, ARRAYSIZE(command), _TRUNCATE,
                 L"\"%ls\\schtasks.exe\" /Delete /TN " TEST_TASK_NAME L" /F", system_dir);
    startup.cb = (DWORD)sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    if (!CreateProcessW(NULL, command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL,
                        &startup, &process))
        return;
    WaitForSingleObject(process.hProcess, 30000);
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (exit_code == 0)
        wprintf(L"  removed scheduled task %ls\n", TEST_TASK_NAME);
}

static int UninstallOnline(void)
{
    wchar_t path[MAX_PATH], pf[MAX_PATH], system_dir[MAX_PATH];
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);

    if (!scm) {
        Fail(L"cannot open the service control manager (win32 error %lu); run as administrator",
             GetLastError());
        return 1;
    }
    wprintf(L"uninstalling from the running windows\n");
    StopServiceAndWait(scm, SERVICE_NAME);
    StopServiceAndWait(scm, DRIVER_NAME);
    DeleteServiceByName(scm, SERVICE_NAME);
    DeleteServiceByName(scm, DRIVER_NAME);
    CloseServiceHandle(scm);

    DeleteTestTaskOnline();
    if (SystemDir(system_dir, ARRAYSIZE(system_dir)) &&
        JoinPath(path, ARRAYSIZE(path), system_dir, L"drivers\\" DRIVER_FILE))
        RemoveFileOrDefer(path, TRUE);
    if (ProgramFilesDir(pf, ARRAYSIZE(pf)) && JoinPath(path, ARRAYSIZE(path), pf, PRODUCT_DIR))
        RemoveDirectoryTree(path, TRUE);
    if (DataDir(path, ARRAYSIZE(path)))
        RemoveDirectoryTree(path, TRUE);
    DeleteKeyTree(HKEY_LOCAL_MACHINE, SETTINGS_KEY, L"HKLM\\" SETTINGS_KEY);

    if (failures)
        return 1;
    wprintf(L"uninstalled\n");
    return 0;
}

/* ---- offline: another windows, through its registry hive files ---- */

static BOOL IsWindowsDir(const wchar_t *dir)
{
    wchar_t hive[MAX_PATH];
    return JoinPath(hive, ARRAYSIZE(hive), dir, L"System32\\config\\SYSTEM") && FileExists(hive);
}

static void ListWindowsInstalls(void)
{
    wchar_t running[MAX_PATH], dir[16];
    DWORD drives = GetLogicalDrives();
    wchar_t letter;
    BOOL any = FALSE;

    GetWindowsDirectoryW(running, ARRAYSIZE(running));
    for (letter = L'A'; letter <= L'Z'; ++letter) {
        if (!(drives & (1ul << (letter - L'A'))) || (wchar_t)towupper(running[0]) == letter)
            continue;
        _snwprintf_s(dir, ARRAYSIZE(dir), _TRUNCATE, L"%lc:\\Windows", letter);
        if (!IsWindowsDir(dir))
            continue;
        if (!any)
            fwprintf(stderr, L"windows installs visible from here:\n");
        fwprintf(stderr, L"  --target %lc:\\\n", letter);
        any = TRUE;
    }
    if (!any)
        fwprintf(stderr, L"no windows install is visible from here. if your windows drive "
                         L"is bitlocker-encrypted, unlock it first:\n"
                         L"  manage-bde -unlock D: -RecoveryPassword <your recovery key>\n");
}

/* Maps --target to a windows dir. Returns FALSE with *is_running set if it is this windows. */
static BOOL ResolveTargetWindir(const wchar_t *target, wchar_t *windir, BOOL *is_running)
{
    wchar_t given[MAX_PATH], full[MAX_PATH], candidate[MAX_PATH], running[MAX_PATH];
    size_t length;

    *is_running = FALSE;
    if (wcslen(target) + 2 > ARRAYSIZE(given)) {
        Fail(L"--target path is too long");
        return FALSE;
    }
    wcscpy_s(given, ARRAYSIZE(given), target);
    if (wcslen(given) == 2 && given[1] == L':')
        wcscat_s(given, ARRAYSIZE(given), L"\\");
    length = GetFullPathNameW(given, ARRAYSIZE(full), full, NULL);
    if (!length || length >= ARRAYSIZE(full)) {
        Fail(L"bad --target path %ls", target);
        return FALSE;
    }
    if (IsWindowsDir(full))
        wcscpy_s(candidate, ARRAYSIZE(candidate), full);
    else if (!JoinPath(candidate, ARRAYSIZE(candidate), full, L"Windows") ||
             !IsWindowsDir(candidate)) {
        Fail(L"%ls has no Windows\\System32\\config\\SYSTEM; point --target at the drive your "
             L"windows is installed on", full);
        ListWindowsInstalls();
        return FALSE;
    }
    length = wcslen(candidate);
    while (length > 3 && candidate[length - 1] == L'\\')
        candidate[--length] = L'\0';

    GetWindowsDirectoryW(running, ARRAYSIZE(running));
    if (!_wcsicmp(candidate, running)) {
        *is_running = TRUE;
        return FALSE;
    }
    wcscpy_s(windir, MAX_PATH, candidate);
    return TRUE;
}

static BOOL LoadHive(OFFLINE_TARGET *target, const wchar_t *file, const wchar_t *mount,
                     HKEY *key, BOOL *loaded)
{
    wchar_t path[MAX_PATH];
    LSTATUS status;

    if (!JoinPath(path, ARRAYSIZE(path), target->windir, file))
        return FALSE;
    RegUnLoadKeyW(HKEY_LOCAL_MACHINE, mount); /* a previous run that died mid-way */
    status = RegLoadKeyW(HKEY_LOCAL_MACHINE, mount, path);
    if (status != ERROR_SUCCESS) {
        Fail(L"cannot load %ls: win32 error %ld%ls", path, status,
             status == ERROR_SHARING_VIOLATION ? L" (that windows is running or the hive is "
                                                 L"loaded elsewhere)"
             : status == ERROR_PRIVILEGE_NOT_HELD ? L" (run as administrator)" : L"");
        return FALSE;
    }
    *loaded = TRUE;
    status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, mount, 0, KEY_ALL_ACCESS, key);
    if (status != ERROR_SUCCESS) {
        Fail(L"cannot open the loaded hive %ls: win32 error %ld", mount, status);
        return FALSE;
    }
    return TRUE;
}

static void CloseOfflineTarget(OFFLINE_TARGET *target)
{
    if (target->software)
        RegCloseKey(target->software);
    if (target->system)
        RegCloseKey(target->system);
    target->software = target->system = NULL;
    if (target->software_loaded && RegUnLoadKeyW(HKEY_LOCAL_MACHINE, OFFLINE_SOFTWARE))
        Fail(L"cannot unload the offline SOFTWARE hive; reboot before using it");
    if (target->system_loaded && RegUnLoadKeyW(HKEY_LOCAL_MACHINE, OFFLINE_SYSTEM))
        Fail(L"cannot unload the offline SYSTEM hive; reboot before using it");
    target->software_loaded = target->system_loaded = FALSE;
}

static BOOL ReadSz(HKEY root, const wchar_t *subkey, const wchar_t *name, wchar_t *out,
                   DWORD out_count)
{
    DWORD size = out_count * (DWORD)sizeof(wchar_t);
    return RegGetValueW(root, subkey, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND,
                        NULL, out, &size) == ERROR_SUCCESS;
}

static void ExpandSystemDrive(wchar_t *path, size_t count, wchar_t drive)
{
    static const wchar_t token[] = L"%SystemDrive%";
    wchar_t rest[MAX_PATH];

    if (_wcsnicmp(path, token, ARRAYSIZE(token) - 1))
        return;
    wcscpy_s(rest, ARRAYSIZE(rest), path + ARRAYSIZE(token) - 1);
    _snwprintf_s(path, count, _TRUNCATE, L"%lc:%ls", drive, rest);
}

static BOOL OpenOfflineTarget(const wchar_t *windir, OFFLINE_TARGET *target)
{
    size_t windir_length, tail_length;
    const wchar_t *tail;

    ZeroMemory(target, sizeof(*target));
    wcscpy_s(target->windir, ARRAYSIZE(target->windir), windir);
    if (!EnablePrivilege(L"SeBackupPrivilege") || !EnablePrivilege(L"SeRestorePrivilege")) {
        Fail(L"cannot enable the backup/restore privileges; run as administrator");
        return FALSE;
    }
    if (!LoadHive(target, L"System32\\config\\SYSTEM", OFFLINE_SYSTEM, &target->system,
                  &target->system_loaded) ||
        !LoadHive(target, L"System32\\config\\SOFTWARE", OFFLINE_SOFTWARE, &target->software,
                  &target->software_loaded))
        return FALSE;

    /* The offline windows describes its own paths with its own drive letter. */
    if (!ReadSz(target->software, L"Microsoft\\Windows NT\\CurrentVersion", L"SystemRoot",
                target->online_windir, ARRAYSIZE(target->online_windir)))
        wcscpy_s(target->online_windir, ARRAYSIZE(target->online_windir), L"C:\\Windows");
    if (!ReadSz(target->software, L"Microsoft\\Windows\\CurrentVersion", L"ProgramFilesDir",
                target->online_pf, ARRAYSIZE(target->online_pf)))
        wcscpy_s(target->online_pf, ARRAYSIZE(target->online_pf), L"C:\\Program Files");
    ExpandSystemDrive(target->online_pf, ARRAYSIZE(target->online_pf), target->online_windir[0]);

    tail = target->online_windir + 2; /* "\Windows" */
    tail_length = wcslen(tail);
    windir_length = wcslen(windir);
    if (target->online_windir[1] != L':' || windir_length <= tail_length ||
        _wcsicmp(windir + windir_length - tail_length, tail)) {
        Fail(L"%ls does not match that windows' own SystemRoot %ls", windir,
             target->online_windir);
        return FALSE;
    }
    wcsncpy_s(target->root, ARRAYSIZE(target->root), windir, windir_length - tail_length);
    if (towupper(target->online_pf[0]) != towupper(target->online_windir[0]) ||
        target->online_pf[1] != L':') {
        Fail(L"that windows keeps program files on another drive (%ls); install it online "
             L"instead", target->online_pf);
        return FALSE;
    }
    return JoinPath(target->offline_pf, ARRAYSIZE(target->offline_pf), target->root,
                    target->online_pf + 2);
}

static BOOL ReadControlSet(HKEY system, const wchar_t *name, DWORD *value)
{
    DWORD size = (DWORD)sizeof(*value);
    return RegGetValueW(system, L"Select", name, RRF_RT_REG_DWORD, NULL, value, &size) ==
               ERROR_SUCCESS &&
           *value >= 1 && *value <= 999;
}

static BOOL WriteOfflineServices(OFFLINE_TARGET *target, DWORD control_set)
{
    /* SERVICE_FAILURE_ACTIONS as the SCM stores it: reset 60s, restart after 2s/5s/10s. */
    static const DWORD failure_actions[] = {60, 0, 0, 3, 0x14, SC_ACTION_RESTART, 2000,
                                            SC_ACTION_RESTART, 5000, SC_ACTION_RESTART, 10000};
    static const wchar_t dependencies[] = DRIVER_NAME L"\0";
    wchar_t path[MAX_PATH], service_command[MAX_PATH + 2];
    HKEY driver = NULL, service = NULL;
    LSTATUS status;
    BOOL ok;

    _snwprintf_s(path, ARRAYSIZE(path), _TRUNCATE, L"ControlSet%03lu\\Services\\" DRIVER_NAME,
                 control_set);
    status = RegCreateKeyExW(target->system, path, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &driver,
                             NULL);
    if (status == ERROR_SUCCESS) {
        _snwprintf_s(path, ARRAYSIZE(path), _TRUNCATE,
                     L"ControlSet%03lu\\Services\\" SERVICE_NAME, control_set);
        status = RegCreateKeyExW(target->system, path, 0, NULL, 0, KEY_ALL_ACCESS, NULL,
                                 &service, NULL);
    }
    if (status != ERROR_SUCCESS) {
        Fail(L"cannot create the service keys in ControlSet%03lu: win32 error %ld", control_set,
             status);
        if (driver)
            RegCloseKey(driver);
        return FALSE;
    }

    _snwprintf_s(service_command, ARRAYSIZE(service_command), _TRUNCATE,
                 L"\"%ls\\" PRODUCT_DIR L"\\" SERVICE_FILE L"\"", target->online_pf);
    ok = SetDword(driver, L"Type", SERVICE_KERNEL_DRIVER) &&
         SetDword(driver, L"Start", SERVICE_DEMAND_START) &&
         SetDword(driver, L"ErrorControl", SERVICE_ERROR_NORMAL) &&
         SetSz(driver, L"ImagePath", REG_EXPAND_SZ,
               L"\\SystemRoot\\System32\\drivers\\" DRIVER_FILE) &&
         SetSz(driver, L"DisplayName", REG_SZ, DRIVER_NAME) &&
         SetDword(service, L"Type", SERVICE_WIN32_OWN_PROCESS) &&
         SetDword(service, L"Start", SERVICE_AUTO_START) &&
         SetDword(service, L"ErrorControl", SERVICE_ERROR_NORMAL) &&
         SetSz(service, L"ImagePath", REG_EXPAND_SZ, service_command) &&
         SetSz(service, L"DisplayName", REG_SZ, SERVICE_NAME) &&
         SetSz(service, L"ObjectName", REG_SZ, L"LocalSystem") &&
         SetString(service, L"DependOnService", REG_MULTI_SZ, dependencies,
                   ARRAYSIZE(dependencies)) &&
         RegSetValueExW(service, L"FailureActions", 0, REG_BINARY,
                        (const BYTE *)failure_actions,
                        (DWORD)sizeof(failure_actions)) == ERROR_SUCCESS &&
         SetDword(service, L"FailureActionsOnNonCrashFailures", 1);
    RegCloseKey(service);
    RegCloseKey(driver);
    if (ok)
        wprintf(L"  registered services in ControlSet%03lu\n", control_set);
    return ok;
}

static int InstallOffline(const SETUP_OPTIONS *options, const wchar_t *windir)
{
    wchar_t driver_source[MAX_PATH], service_source[MAX_PATH];
    wchar_t drivers_dir[MAX_PATH], install_dir[MAX_PATH];
    OFFLINE_TARGET target;
    DWORD current = 0, fallback = 0;

    if (!ResolvePayload(options, driver_source, service_source))
        return 1;
    if (options->stage_only)
        Warn(L"--stage-only is implied offline: nothing loads until that windows boots");
    wprintf(L"installing into the offline windows at %ls\n", windir);
    if (OpenOfflineTarget(windir, &target)) {
        wprintf(L"  that windows calls itself %ls\n", target.online_windir);
        if (!ReadControlSet(target.system, L"Current", &current))
            Fail(L"the offline SYSTEM hive has no Select\\Current control set");
        if (!failures && JoinPath(drivers_dir, ARRAYSIZE(drivers_dir), windir,
                                  L"System32\\drivers") &&
            JoinPath(install_dir, ARRAYSIZE(install_dir), target.offline_pf, PRODUCT_DIR) &&
            CopyPayloadFile(driver_source, drivers_dir, DRIVER_FILE) &&
            CopyPayloadFile(service_source, install_dir, SERVICE_FILE) &&
            WriteOfflineServices(&target, current)) {
            if (ReadControlSet(target.system, L"Default", &fallback) && fallback != current)
                WriteOfflineServices(&target, fallback);
            if (WriteSettings(target.software, options->arm))
                wprintf(L"  wrote HKLM\\%ls\n", SETTINGS_KEY);
        }
    }
    CloseOfflineTarget(&target);
    if (failures)
        return 1;
    wprintf(L"staged into %ls; it loads on that windows' next boot. enforcement is %ls\n"
            L"the driver is test-signed: that windows needs test signing on (secure boot off)\n"
            L"or it will refuse the driver. from WinRE: bcdedit /set {default} testsigning on\n",
            windir, options->arm ? L"ARMED" : L"disarmed");
    return 0;
}

/* The bugcheck test runner's startup task, removed from the task scheduler's cache. */
static void DeleteTestTaskOffline(OFFLINE_TARGET *target)
{
    static const wchar_t *lists[] = {L"Tasks", L"Boot", L"Logon", L"Plain", L"Maintenance"};
    static const wchar_t cache[] = L"Microsoft\\Windows NT\\CurrentVersion\\Schedule\\TaskCache";
    wchar_t tree[MAX_PATH], entry[MAX_PATH], id[64], file[MAX_PATH];
    size_t i;

    _snwprintf_s(tree, ARRAYSIZE(tree), _TRUNCATE, L"%ls\\Tree\\" TEST_TASK_NAME, cache);
    if (ReadSz(target->software, tree, L"Id", id, ARRAYSIZE(id)) && id[0] == L'{') {
        for (i = 0; i < ARRAYSIZE(lists); ++i) {
            _snwprintf_s(entry, ARRAYSIZE(entry), _TRUNCATE, L"%ls\\%ls\\%ls", cache, lists[i],
                         id);
            DeleteKeyTree(target->software, entry, entry);
        }
    }
    DeleteKeyTree(target->software, tree, L"scheduled task " TEST_TASK_NAME);
    if (JoinPath(file, ARRAYSIZE(file), target->windir, L"System32\\Tasks\\" TEST_TASK_NAME))
        RemoveFileOrDefer(file, FALSE);
}

static int UninstallOffline(const wchar_t *windir)
{
    wchar_t name[256], path[MAX_PATH]; /* registry key names are at most 255 chars */
    DWORD index, name_count, control_sets = 0;
    LSTATUS status;
    OFFLINE_TARGET target;

    wprintf(L"uninstalling from the offline windows at %ls\n", windir);
    if (OpenOfflineTarget(windir, &target)) {
        wprintf(L"  that windows calls itself %ls\n", target.online_windir);
        /* every control set, so last-known-good cannot bring the driver back either */
        for (index = 0;; ++index) {
            name_count = ARRAYSIZE(name);
            status = RegEnumKeyExW(target.system, index, name, &name_count, NULL, NULL, NULL,
                                   NULL);
            if (status == ERROR_NO_MORE_ITEMS)
                break;
            if (status == ERROR_MORE_DATA)
                continue; /* too long to be a ControlSetNNN key */
            if (status != ERROR_SUCCESS) {
                Fail(L"cannot enumerate the offline SYSTEM hive: win32 error %ld", status);
                break;
            }
            if (_wcsnicmp(name, L"ControlSet", 10))
                continue;
            ++control_sets;
            _snwprintf_s(path, ARRAYSIZE(path), _TRUNCATE, L"%ls\\Services\\" SERVICE_NAME,
                         name);
            DeleteKeyTree(target.system, path, path);
            _snwprintf_s(path, ARRAYSIZE(path), _TRUNCATE, L"%ls\\Services\\" DRIVER_NAME, name);
            DeleteKeyTree(target.system, path, path);
        }
        if (!control_sets)
            Fail(L"the offline SYSTEM hive has no ControlSet keys");
        DeleteKeyTree(target.software, L"WinOomKiller", L"HKLM\\" SETTINGS_KEY);
        DeleteTestTaskOffline(&target);

        if (JoinPath(path, ARRAYSIZE(path), windir, L"System32\\drivers\\" DRIVER_FILE))
            RemoveFileOrDefer(path, FALSE);
        if (JoinPath(path, ARRAYSIZE(path), target.offline_pf, PRODUCT_DIR))
            RemoveDirectoryTree(path, FALSE);
        if (JoinPath(path, ARRAYSIZE(path), target.root, L"ProgramData\\" PRODUCT_DIR))
            RemoveDirectoryTree(path, FALSE);
    }
    CloseOfflineTarget(&target);
    if (failures)
        return 1;
    wprintf(L"uninstalled from %ls; the driver will not load on its next boot\n", windir);
    return 0;
}

/* ---- entry ---- */

static BOOL ParseOptions(int argc, wchar_t **argv, SETUP_OPTIONS *options)
{
    int i;

    ZeroMemory(options, sizeof(*options));
    if (argc < 2)
        return FALSE;
    if (!_wcsicmp(argv[1], L"install"))
        options->install = TRUE;
    else if (_wcsicmp(argv[1], L"uninstall"))
        return FALSE;
    for (i = 2; i < argc; ++i) {
        if (!_wcsicmp(argv[i], L"--target") && i + 1 < argc)
            options->target = argv[++i];
        else if (!_wcsicmp(argv[i], L"--payload") && i + 1 < argc && options->install)
            options->payload = argv[++i];
        else if (!_wcsicmp(argv[i], L"--arm") && options->install)
            options->arm = TRUE;
        else if (!_wcsicmp(argv[i], L"--stage-only") && options->install)
            options->stage_only = TRUE;
        else {
            fwprintf(stderr, L"unknown or misplaced option: %ls\n\n", argv[i]);
            return FALSE;
        }
    }
    return TRUE;
}

/* Keeps the window open when someone double-clicks the exe in explorer. */
static void PauseIfOwnConsole(void)
{
    DWORD processes[2], mode;

    if (GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode) &&
        GetConsoleProcessList(processes, ARRAYSIZE(processes)) == 1) {
        wprintf(L"\npress enter to close\n");
        (void)getwchar();
    }
}

static int Run(int argc, wchar_t **argv)
{
    SETUP_OPTIONS options;
    wchar_t windir[MAX_PATH];
    BOOL recovery = IsRecoveryEnvironment(), is_running = FALSE;

    setvbuf(stdout, NULL, _IONBF, 0); /* keep stdout and stderr in order when piped */

    if (argc == 2 && (!wcscmp(argv[1], L"--help") || !wcscmp(argv[1], L"-h") ||
                      !wcscmp(argv[1], L"/?"))) {
        PrintUsage();
        return 0;
    }
    if (!ParseOptions(argc, argv, &options)) {
        PrintUsage();
        return 2;
    }
    PrintSafetyNotice();

    if (!options.target) {
        if (recovery) {
            Fail(L"this is a recovery environment, not your windows. pass --target with the "
                 L"letter your C: drive has here");
            ListWindowsInstalls();
            return 2;
        }
        return options.install ? InstallOnline(&options) : UninstallOnline();
    }
    if (!ResolveTargetWindir(options.target, windir, &is_running)) {
        if (!is_running)
            return 1;
        if (recovery) {
            Fail(L"%ls is the recovery environment itself; pass the letter your C: drive has "
                 L"here", options.target);
            ListWindowsInstalls();
            return 2;
        }
        return options.install ? InstallOnline(&options) : UninstallOnline();
    }
    return options.install ? InstallOffline(&options, windir) : UninstallOffline(windir);
}

int wmain(int argc, wchar_t **argv)
{
    int result;

    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    result = Run(argc, argv);

    PauseIfOwnConsole();
    return result;
}
