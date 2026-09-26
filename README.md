# windows oom killer

a small windows service and kmdf control driver that recover from commit exhaustion before the machine falls over.

the service uses documented win32 memory counters to detect low commit headroom. it picks the non-critical, non-protected, non-session-0 process with the largest private commit. the driver validates that request, rejects the system process, protected processes, and critical processes, checks the pid creation time against reuse, and calls `ZwTerminateProcess`.

if a victim does not exit within five seconds, three recovery kills do not clear pressure, the monitor stops responding during critical pressure, or no safe victim exists, the driver calls `KeBugCheckEx`. the driver also watches windows' `\KernelObjects\MaximumCommitCondition`, so a crashed service cannot silently disable protection before pressure begins.

after a successful recovery kill, the service displays a popup in the affected user's session with the process name, pid, reason, and private memory usage.

after a custom OOM bugcheck and reboot, the service translates the latest Windows bugcheck event and shows the friendly reason and all four parameters in the console session (not an rdp session). WER logs that event some time after boot, so the service keeps looking for ten minutes; a marker stops a crash from being reported twice.

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

## releases

every push and pull request builds Debug, Release, and BugcheckTest on github actions, packages each with `WinOomKillerSetup.exe`, and smoke-tests the installer online and offline. pushes to `master` publish a prerelease (`build-<n>`); pushing a `v*` tag publishes a release. the BugcheckTest zip is named `…-BugcheckTest-CRASH-TEST-ONLY-…` and is test-only.

each zip installs with one exe, from windows or from WinRE:

```powershell
WinOomKillerSetup.exe install                  # the running windows, disarmed
WinOomKillerSetup.exe install --target D:\     # from WinRE: D: is whatever letter your C: got there
WinOomKillerSetup.exe uninstall [--target D:\]
```

the offline mode writes the service keys straight into the target's registry hives, so `uninstall --target` from WinRE is also the way out of a bugcheck loop. the ci driver is test-signed, so the target needs test signing on. `INSTALL.txt` in each zip has the details.

## build

requirements are visual studio 2022 with desktop c++ and the windows 11 sdk/wdk.

```powershell
msbuild .\WindowsOomKiller.sln /m /p:Configuration=Debug /p:Platform=x64
.\scripts\package-release.ps1 -Configuration Debug -Version dev   # optional: the release zip, in .\dist
```

the portable policy check also runs on linux:

```sh
cmake -S . -B build
cmake --build build
./build/oom_policy_test
```

on windows, cmake's default visual studio generator is multi-config:
`cmake --build build --config Debug`, then `.\build\Debug\oom_policy_test.exe`
(or `ctest --test-dir build -C Debug`).

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

`-Start` first installs the BugcheckTest binaries from this checkout (build them first), sets the monitor to Manual, and reboots. the runner verifies each preceding System event before triggering the next code. it stops after eight successful crashes or immediately on a mismatch. results and logs are written under `C:\ProgramData\WinOomKiller`. cancel an active run with `test-bugchecks.ps1 -Cancel`. the crash-test build stays installed afterwards, so reinstall a Debug or Release build when you are done.

## install in a disposable vm

the build test-signs the driver, so the target needs test-signing mode on (`bcdedit /set testsigning on`, secure boot off) or windows refuses to load it. install disarmed first:

```powershell
.\scripts\install.ps1 `
  -DriverPath .\driver\x64\Debug\WinOomKillerDriver.sys `
  -ServicePath .\service\x64\Debug\WinOomKillerService.exe
```

verify the service and event log, then set `HKLM\SOFTWARE\WinOomKiller\Armed` to `1` (a `REG_DWORD`) and restart the service. passing `-Arm` to the installer does both during later test runs.

use `-StageOnly` to register a disarmed install without loading it until the next reboot. an existing install is stopped first, so the machine is unmonitored until then.

defaults are 512 mib commit headroom and one critical sample. if pressure remains critical, it can kill up to three victims two samples apart; after that the service asks the driver to bugcheck, which it does if its own ceiling agrees. the driver refuses any kill the telemetry does not justify against its hard 512 mib ceiling, so the service clamps `CommitHeadroomMiB` to 0–512, and clamps `ConfirmationSamples`, `KillRetrySamples` and `MaxKills` to 1–10 (service limits, not driver ones). it logs every value it had to change. settings must be `REG_DWORD`: a value of any other type is ignored in favour of the default, so a mistyped `Armed` leaves the monitor disarmed. reinstalling keeps tuned thresholds and only sets `Armed`.

`scripts/memory-hog.ps1 -MemoryMiB 1024` supplies a bounded test victim. do not pressure-test a machine containing anything you care about.

## api choices

the implementation follows microsoft's documented [`KeBugCheckEx`](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-kebugcheckex), [`ZwTerminateProcess`](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntddk/nf-ntddk-zwterminateprocess), [driver process-termination constraints](https://learn.microsoft.com/en-us/windows-hardware/drivers/driversecurity/driver-security-dev-best-practices), and [memory performance APIs](https://learn.microsoft.com/en-us/windows/win32/memory/memory-performance-information).

## setup guides

the [setup/](setup/) directory has the step-by-step guides: building from a fresh clone ([from-scratch](setup/from-scratch.md)), installing, arming, kill-testing, bugcheck-testing, and removing on windows ([windows](setup/windows.md)), and running the portable policy and contract checks on any host ([linux](setup/linux.md)). start at [SETUP.md](setup/SETUP.md).

## license

the last resort license, version 1 — see [LICENSE](LICENSE). permissive like mit for use, modification, and redistribution, including commercial, with conditions shaped by what this software does: modified builds that change victim selection, thresholds, watchdog timing, safety checks, or bugcheck behavior must carry their changes on their face and may not travel under the original name; safety notices stay verbatim and conspicuous; nothing ships armed by default; crash-test builds stay visibly separate from release builds; and nobody deploys this where a deliberate crash could cost lives. the no-warranty and no-liability sections are in full caps and state the quiet part out loud: a bugcheck raised under the documented conditions is designed behavior, not a defect.
