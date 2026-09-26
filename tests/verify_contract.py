from pathlib import Path
import ctypes
import re

root = Path(__file__).resolve().parents[1]
header = (root / "include/oom_protocol.h").read_text()
driver = (root / "driver/driver.c").read_text()
service = (root / "service/main.c").read_text()
bugcheck_runner = (root / "scripts/test-bugchecks.ps1").read_text()
installer = (root / "installer/setup.c").read_text()

codes = re.findall(r"#define (OOM_FATAL_[A-Z_]+)\s+(0x[0-9A-Fa-f]+)u", header)
assert len(codes) == 8
assert len({int(value, 16) for _, value in codes}) == len(codes)


class Telemetry(ctypes.Structure):
    _fields_ = [
        ("version", ctypes.c_uint32), ("flags", ctypes.c_uint32),
        ("sequence", ctypes.c_uint64), ("service_pid", ctypes.c_uint32),
        ("page_size", ctypes.c_uint32), ("backing_reason", ctypes.c_uint32),
        ("reserved", ctypes.c_uint32), ("commit_charge_pages", ctypes.c_uint64),
        ("commit_limit_pages", ctypes.c_uint64),
        ("available_physical_pages", ctypes.c_uint64),
        ("pagefile_free_pages", ctypes.c_uint64),
    ]


class KillRequest(ctypes.Structure):
    _fields_ = [("telemetry", Telemetry), ("victim_pid", ctypes.c_uint32),
                ("reserved", ctypes.c_uint32), ("victim_private_pages", ctypes.c_uint64),
                ("victim_create_time", ctypes.c_uint64)]


assert ctypes.sizeof(Telemetry) == 64
assert ctypes.sizeof(KillRequest) == 88
assert "KeBugCheckEx(code, (ULONG_PTR)t->commit_charge_pages" in driver
assert "(ULONG_PTR)t->commit_limit_pages" in driver
assert "(ULONG_PTR)t->available_physical_pages" in driver
assert "OomBackingParameter(t)" in driver
assert "PsIsProtectedProcess(process)" in driver
assert "PsIsProtectedProcessLight(process)" in driver
assert "PsGetProcessCreateTimeQuadPart(process) != kill->victim_create_time" in driver
assert "ZwWaitForSingleObject(process_handle" in driver
assert "ctx->kill_pending = FALSE" in driver
assert "driver_config.EvtDriverUnload = OomEvtDriverUnload" in driver
assert 'L"\\\\KernelObjects\\\\MaximumCommitCondition"' in driver
assert "attributes.EvtCleanupCallback = OomEvtDeviceCleanup" in driver
assert "OomEvtTimerCleanup" not in driver
assert "IsProcessCritical(process, &critical)" in service
assert "WTSSendMessageW(WTS_CURRENT_SERVER_HANDLE, session" in service
assert "Microsoft-Windows-WER-SystemErrorReporting" in service
assert 'L"LastNotifiedBugcheckRecord"' in service
assert "FindBugcheckInfo(parsed[0])" in service
assert "#if defined(OOM_ENABLE_TEST_IOCTL)" in header
assert "test->confirm != OOM_TEST_CONFIRM" in driver
assert 'L"--bugcheck-test"' in service
assert "if (Compare-Object $expected $actual -SyncWindow 0)" in bugcheck_runner
assert "Remove-TestTask" in bugcheck_runner
assert 'SetDword(key, L"Armed", arm ? 1 : 0)' in installer
assert 'options->arm = TRUE' in installer and installer.count("options->arm = TRUE") == 1
assert "PrintSafetyNotice();" in installer

# STATUS_TIMEOUT is a success-class NTSTATUS: a kill wait that timed out must
# be caught explicitly, bugcheck when armed, and fail the ioctl otherwise.
assert "if (status == STATUS_TIMEOUT) {" in driver
assert "OomBugcheck(OOM_FATAL_KILL_TIMEOUT, &kill->telemetry)" in driver
assert "return STATUS_IO_TIMEOUT;" in driver
# the driver refuses critical processes itself, failing closed
assert "ProcessBreakOnTermination, &critical" in driver
assert "ULONG critical = 1;" in driver
# watchdog time must not count sleep/hibernate against the monitor
assert "KeQueryUnbiasedInterruptTime()" in driver and "KeQueryInterruptTime()" not in driver
# the ceilings live in one place, shared by the driver and the service's clamps
assert "#define OOM_COMMIT_HEADROOM_BYTES" in header and "#define OOM_MIN_VICTIM_BYTES" in header
assert "#define OOM_COMMIT_HEADROOM_BYTES" not in driver
assert "#define OOM_MIN_VICTIM_BYTES" not in driver
assert "(DWORD)(OOM_COMMIT_HEADROOM_BYTES / MIB)" in service
assert "OOM_MIN_VICTIM_BYTES / page_size" in service
# only an explicit REG_DWORD 1 arms the monitor
assert 'armed_setting = ReadSetting(L"Armed", 0);' in service and "armed = armed_setting == 1;" in service
assert "if (status == ERROR_SUCCESS)\n        return TRUE;" in service
# never offer the driver a victim it will refuse
assert "ProcessProtectionLevelInfo" in service and "IsProcessCritical(process, &critical)" in service
# offline uninstall must not stop early on a long SYSTEM-hive key name
assert "wchar_t name[256]" in installer and "status == ERROR_MORE_DATA" in installer
print("driver contract verification passed")
