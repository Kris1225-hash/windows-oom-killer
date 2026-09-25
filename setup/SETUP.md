# setup

guides for building, installing, and testing windows-oom-killer without
losing a machine you care about. the README explains what the software
is; this folder explains how to get from a fresh clone to verified kills
and verified bugchecks.

## pick a path

| where you are                                        | read                                       |
|------------------------------------------------------|--------------------------------------------|
| nothing; building from a fresh clone                 | [from-scratch.md](from-scratch.md)         |
| built binaries; installing and testing on windows    | [windows.md](windows.md)                   |
| linux/macos host; running the portable checks only   | [linux.md](linux.md)                       |

## quick reference

facts below are drawn from the README, GATES.md evidence, and the source.

- **the golden rule: a disposable vm.** the software terminates processes
  by design and bugchecks the system when recovery fails. its own test
  tooling crashes the machine eight times in a row on purpose. the
  README says it plainly: do not pressure-test a machine containing
  anything you care about.

- **two build worlds.** the driver + service build only with visual
  studio 2022 + windows 11 sdk/wdk via msbuild. the policy engine
  (`shared/oom_policy.c`) is portable c and builds with cmake on any
  host; `tests/verify_contract.py` needs only python3.

- **three build configurations.** Debug and Release never contain the
  manual-crash test ioctl. `BugcheckTest` does (`OOM_ENABLE_TEST_IOCTL`)
  and adds the `--bugcheck-test` cli. never confuse the builds; the
  license (LICENSE, section 3) requires crash-test builds to stay
  distinguishable.

- **one installer, online or offline.** `WinOomKillerSetup.exe`
  (`installer/`) installs and uninstalls on the running windows via the
  scm, or with `--target D:\` into an offline windows from WinRE by
  writing its registry hives directly. github actions builds all three
  configurations on every push and pr and publishes zips from `master`
  and `v*` tags (`.github/workflows/build.yml`).

- **everything installs disarmed.** enforcement only happens when
  `HKLM\SOFTWARE\WinOomKiller\Armed` is 1 and the service restarted (or
  `install.ps1 -Arm`). even armed, the driver independently rejects
  kills unless telemetry says pressure is critical.

- **settings** (`HKLM\SOFTWARE\WinOomKiller`, all DWORD unless noted):
  `Armed` (0), `CommitHeadroomMiB` (512), `ConfirmationSamples` (1),
  `KillRetrySamples` (2), `MaxKills` (3). lowering thresholds is
  supported; raising them needs a matching driver ceiling
  (`OOM_COMMIT_HEADROOM_BYTES` in driver.c).

- **driver hard floors:** victims below 64 MiB private are rejected,
  system process (pid ≤ 4), protected and protected-light processes,
  the service itself, and pid-reuse (create-time mismatch) are all
  rejected. the kill cap is enforced in the kernel, not just the
  service.

- **the device has one owner.** one service instance holds
  `\\.\WinOomKiller` at a time. during manual bugcheck testing, keep
  the normal monitor stopped and start only the driver service, or
  ioctls answer `STATUS_ACCESS_DENIED`.

- **bugchecks:** `0xE0F00001`–`0xE0F00008` (see README table). all four
  parameters: commit charge (pages), commit limit (pages), available
  physical (pages), and `(backing_reason << 32) | pagefile_free_pages`.

- **where evidence lands:**
  - event viewer → system, id 1001, provider
    `Microsoft-Windows-WER-SystemErrorReporting`: recorded bugchecks
  - event source `WinOomKiller`: service log lines (started
    armed/disarmed, victim terminated, notifications, failures)
  - `C:\Windows\Minidump\*.dmp`: crash dumps
  - `C:\ProgramData\WinOomKiller\`: bugcheck test runner state,
    results, and log

- **notifications:** after a recovery kill, a popup appears in the
  victim's session (`WTSSendMessageW`) naming process, pid, reason, and
  private usage. after a reboot following a custom bugcheck, the
  service translates the latest WER event into a friendly one-time
  popup; `LastNotifiedBugcheckRecord` (QWORD) makes each crash report
  exactly once.

- **gates:** every stage of this project was verified against the
  checks in `GATES.md`, including a real kill under 32 GiB pressure
  (G4), a real `0xE0F00004` bugcheck with all four parameters (G5),
  and the eight-code reboot-resilient sweep (G8). re-run those checks
  after changing anything.
