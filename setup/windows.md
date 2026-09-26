# windows host: install, arm, test, remove

everything here assumes a **disposable vm** with nothing on it you care
about, snapshot/checkpointed before you start. the software kills
processes by design and bugchecks the machine when recovery fails; its
test tooling crashes the vm eight consecutive times on purpose.

## 0. prerequisites

- built binaries (`from-scratch.md`, step 3)
- the driver test-signed, or test-signing mode on the target vm:

  ```powershell
  bcdedit /set testsigning on
  shutdown /r /t 0
  ```

- administrator shell for everything below

## 1. install (disarmed first — always)

```powershell
.\scripts\install.ps1 `
  -DriverPath .\driver\x64\Debug\WinOomKillerDriver.sys `
  -ServicePath .\service\x64\Debug\WinOomKillerService.exe
```

what the installer does: copies the driver to
`%SystemRoot%\System32\drivers\WinOomKillerDriver.sys` and the service
to `%ProgramFiles%\WinOomKiller\`; registers the driver as demand-start
kernel service and the monitor as auto-start service depending on it;
writes the default settings under `HKLM\SOFTWARE\WinOomKiller`
(`Armed=0`, `CommitHeadroomMiB=512`, `ConfirmationSamples=1`,
`KillRetrySamples=2`, `MaxKills=3`); configures service recovery
(restart at 2s/5s/10s); then starts both unless `-StageOnly` was
passed (`-StageOnly` registers everything but loads nothing until the
next reboot).

`WinOomKillerSetup.exe install` (from a release zip, or
`installer\x64\<Config>\` next to the built driver and service) does
the same thing without powershell; `--stage-only` and `--arm` work the
same way.

### from WinRE, or into any windows that isn't running

`WinOomKillerSetup.exe` also installs into an offline windows: the
recovery environment, install media's shift+f10 prompt, or a windows
disk mounted on another machine. the scm isn't available there, so it
loads the target's `SYSTEM` and `SOFTWARE` hives and writes the same
service keys, recovery actions, and settings the scm would:

```bat
D:\WinOomKillerSetup.exe install --target D:\
```

`--target` is the drive your windows is on *as the recovery
environment sees it*. it is often not `C:`. without `--target` (or
pointed at the wrong place) the installer lists every windows install
it can see. unlock a bitlocker drive with `manage-bde -unlock` first.
the install takes effect on that windows' next boot.

verify the disarmed baseline before arming anything: event viewer →
windows logs → application/system, source `WinOomKiller`, should show
`monitor started disarmed: commit headroom ... pages`.

## 2. arm

```powershell
Set-ItemProperty HKLM:\SOFTWARE\WinOomKiller Armed -Value 1 -Type DWord
Restart-Service WinOomKiller
```

or re-run the installer with `-Arm`. the service logs
`monitor started armed`. `Armed` must be a `REG_DWORD`: `reg add` without
`/t REG_DWORD` writes a string, which the service ignores (it logs
`ignoring setting Armed` and stays disarmed). the driver still independently refuses kill
requests unless the telemetry says pressure is genuinely critical —
arming is necessary, not sufficient.

## 3. test a recovery kill

in a normal shell inside the vm:

```powershell
.\scripts\memory-hog.ps1 -MemoryMiB 1024
```

the hog allocates in 64 MiB chunks, touches every page (so it really
commits), prints its pid, and holds until ctrl-c. with a small enough
pagefile the commit headroom drops below `CommitHeadroomMiB` and, after
`ConfirmationSamples`, the service asks the driver to terminate the
largest eligible private-commit process — the hog.

expected outcome (this is gate G4's shape): the hog dies with the
service's `STATUS_NO_MEMORY` termination code, a popup appears in the
hog's session naming process, pid, reason, and private usage, the
`WinOomKiller` event source logs the kill, and commit headroom recovers
without a reboot. if nothing dies: check `Armed`, pagefile size vs.
`CommitHeadroomMiB`, and that the hog is neither session 0, a
protected/critical process, nor under the driver's 64 MiB victim floor.

tuning: registry thresholds may be lowered freely. the driver's
compiled ceilings (`OOM_COMMIT_HEADROOM_BYTES`, `OOM_MIN_VICTIM_BYTES`
in `include/oom_protocol.h`) cap what it will accept, so the service
clamps `CommitHeadroomMiB` to 512 and `ConfirmationSamples` /
`KillRetrySamples` to 1–10, logs every clamp, and lists the effective
values in its `monitor started` line.

## 4. bugcheck testing (the part that destroys the vm's uptime)

manual, one code at a time — uses the `BugcheckTest` build only:

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

the cli accepts any of the eight codes; omit the four parameters to
send zeros. the machine bugchecks immediately — that is the test.
after each crash and reboot: `Start-Service WinOomKillerDriver` again
before the next trigger, and leave the normal monitor stopped, because
the device permits one owner at a time (a second opener gets
`STATUS_ACCESS_DENIED`).

automated sweep of all eight codes across eight reboots:

```powershell
.\scripts\test-bugchecks.ps1 -Start
```

the runner registers a startup task, verifies each preceding system
event (WER id 1001) matches the triggered code and its four marker
parameters before causing the next crash, stops on any mismatch, and
removes its task after eight successes. cancel an active run with
`test-bugchecks.ps1 -Cancel`; resume an interrupted one with
`-Resume`. state, results, and the log live under
`C:\ProgramData\WinOomKiller\` and are written through (flushed) so
they survive the crashes they are recording.

after any custom bugcheck and reboot, the normal service translates the
latest WER event into a one-time friendly popup once an interactive
session exists. WER writes that event some time after boot, so the
service keeps checking every 10 seconds for the first ten minutes of
uptime; `LastNotifiedBugcheckRecord` guarantees each crash is reported
exactly once (gate G9).

## 5. uninstall

```powershell
WinOomKillerSetup.exe uninstall                 # the running windows
WinOomKillerSetup.exe uninstall --target D:\    # an offline windows, e.g. from WinRE
```

both remove the services, binaries, settings, the `WinOomKiller` event
source, the `ProgramData` folder, and the bugcheck runner's startup task. offline, every control set is
cleaned, so last-known-good can't bring the driver back. that makes
`uninstall --target` from WinRE the way out of a bugcheck loop
(`0xE0F00008` from a monitor that dies at boot, or an interrupted
`test-bugchecks.ps1` run).

the same thing by hand:

```powershell
Stop-Service WinOomKiller -Force -ErrorAction SilentlyContinue
Stop-Service WinOomKillerDriver -Force -ErrorAction SilentlyContinue
sc.exe delete WinOomKiller
sc.exe delete WinOomKillerDriver
Remove-Item "$env:ProgramFiles\WinOomKiller" -Recurse -Force
Remove-Item "$env:SystemRoot\System32\drivers\WinOomKillerDriver.sys" -Force
Remove-Item HKLM:\SOFTWARE\WinOomKiller -Recurse -Force
Remove-Item HKLM:\SYSTEM\CurrentControlSet\Services\EventLog\Application\WinOomKiller -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item "$env:ProgramData\WinOomKiller" -Recurse -Force -ErrorAction SilentlyContinue
Unregister-ScheduledTask WinOomKillerBugcheckTests -Confirm:$false -ErrorAction SilentlyContinue
```

## 6. troubleshooting

| symptom | cause |
|---|---|
| `STATUS_ACCESS_DENIED` from kill ioctl | driver rejected the victim: protected/critical, system process, pid ≤ 4, the service itself, under 64 MiB private, pid create-time mismatch (reuse), or telemetry not critical/armed |
| `STATUS_RETRY` from heartbeat | stale or duplicate telemetry sequence number; the service increments per sample |
| `ignoring setting ...` in the event log | that value is not a `REG_DWORD`; the service used the default (for `Armed`, disarmed) |
| `the driver declined to escalate` | pressure was below the driver's ceiling when the service asked for a bugcheck |
| `STATUS_ACCESS_DENIED` from any ioctl with the monitor running | device single-owner: another instance already holds `\\.\WinOomKiller` |
| driver won't start | not test-signed / test-signing mode off; check `sc.exe qc WinOomKillerDriver` binPath and the system event log |
| no popup after kill | victim ran in a session with no interactive user, or `WTSSendMessageW` failed — the service logs the win32 error |
| repeated bugcheck `0xE0F00008` | the monitor service died or hung during critical pressure; that bugcheck is the watchdog doing its job — look at why the service stopped |
