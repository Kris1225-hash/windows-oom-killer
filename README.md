# windows oom killer

a small windows service and kmdf control driver that recover from commit exhaustion before the machine falls over.

the service uses documented win32 memory counters to detect low commit headroom. it picks the non-critical, non-session-0 process with the largest private commit. the driver validates that request, rejects the system process and protected processes, checks the pid creation time against reuse, and calls `ZwTerminateProcess`.

if a victim does not exit within five seconds, three recovery kills do not clear pressure, the monitor stops responding during critical pressure, or no safe victim exists, the driver calls `KeBugCheckEx`. the driver also watches windows' `\KernelObjects\MaximumCommitCondition`, so a crashed service cannot silently disable protection before pressure begins.

after a successful recovery kill, the service displays a popup in the affected user's session with the process name, pid, reason, and private memory usage.

after a custom OOM bugcheck and reboot, the service translates the latest Windows bugcheck event and displays the friendly reason and all four parameters once an interactive session is available. each crash is reported only once.

the split is deliberate. windows exposes good supported memory and per-process commit APIs to user mode. putting undocumented memory-manager structures in the driver would make it version-fragile for no gain. the watchdog and final recovery path remain in the kernel.

## bugchecks

| code | name |
|---|---|
| `0xe0f00001` | `OOM_FATAL_NO_BACKING_STORE` |
| `0xe0f00002` | `OOM_FATAL_PAGEFILE_DISABLED` |
| `0xe0f00003` | `OOM_FATAL_PAGEFILE_EXHAUSTED` |
| `0xe0f00004` | `OOM_FATAL_PAGEFILE_GROWTH_FAILED` |
| `0xe0f00005` | `OOM_FATAL_KILL_FAILED` |
| `0xe0f00006` | `OOM_FATAL_KILL_TIMEOUT` |
| `0xe0f00007` | `OOM_FATAL_NO_KILLABLE_PROCESS` |
| `0xe0f00008` | `OOM_FATAL_MONITOR_UNRESPONSIVE` |

all codes use the same parameters:

1. commit charge, in pages
2. commit limit, in pages
3. available physical memory, in pages
4. backing-store reason in the high 32 bits and saturated pagefile-free pages in the low 32 bits

## build

requirements are visual studio 2022 with desktop c++ and the windows 11 sdk/wdk.

```powershell
msbuild .\WindowsOomKiller.sln /m /p:Configuration=Debug /p:Platform=x64
```

the portable policy check also runs on linux:

```sh
cmake -S . -B build
cmake --build build
./build/oom_policy_test
```

### manual bugcheck build

the `BugcheckTest` configuration adds an admin-only manual crash ioctl and cli. debug and release builds do not contain it.

```powershell
msbuild .\WindowsOomKiller.sln /m /p:Configuration=BugcheckTest /p:Platform=x64
.\scripts\install.ps1 `
  -DriverPath .\driver\x64\BugcheckTest\WinOomKillerDriver.sys `
  -ServicePath .\service\x64\BugcheckTest\WinOomKillerService.exe `
  -StageOnly
Set-Service WinOomKiller -StartupType Manual
Start-Service WinOomKillerDriver
.\service\x64\BugcheckTest\WinOomKillerService.exe `
  --bugcheck-test 0xE0F00001 1 2 3 4
```

the command accepts any of the eight custom codes. omit the four parameters to send zeros. after each crash and reboot, run `Start-Service WinOomKillerDriver` before the next test. leave the normal monitor stopped because the driver device permits one owner at a time.

to verify all eight codes across automatic reboots:

```powershell
.\scripts\test-bugchecks.ps1 -Start
```

the runner verifies each preceding System event before triggering the next code. it stops after eight successful crashes or immediately on a mismatch. results and logs are written under `C:\ProgramData\WinOomKiller`. cancel an active run with `test-bugchecks.ps1 -Cancel`.

## install in a disposable vm

the driver will need a test signature or windows test-signing mode during development. install disarmed first:

```powershell
.\scripts\install.ps1 `
  -DriverPath .\driver\x64\Debug\WinOomKillerDriver.sys `
  -ServicePath .\service\x64\Debug\WinOomKillerService.exe
```

verify the service and event log, then set `HKLM\SOFTWARE\WinOomKiller\Armed` to `1` and restart the service. passing `-Arm` to the installer does both during later test runs.

use `-StageOnly` to register a disarmed install without loading it until the next reboot.

defaults are 512 mib commit headroom and one critical sample. if pressure remains critical, it can kill up to three victims two samples apart before the driver watchdog gives up. the driver independently refuses recovery requests outside that hard ceiling. lower registry thresholds are supported; raising them needs a matching driver ceiling.

`scripts/memory-hog.ps1 -MemoryMiB 1024` supplies a bounded test victim. do not pressure-test a machine containing anything you care about.

## api choices

the implementation follows microsoft's documented [`KeBugCheckEx`](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-kebugcheckex), [`ZwTerminateProcess`](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntddk/nf-ntddk-zwterminateprocess), [driver process-termination constraints](https://learn.microsoft.com/en-us/windows-hardware/drivers/driversecurity/driver-security-dev-best-practices), and [memory performance APIs](https://learn.microsoft.com/en-us/windows/win32/memory/memory-performance-information).
