# from scratch

fresh clone to built and unit-verified binaries. the driver and service
need windows; the policy tests and contract check run anywhere.

## 0. requirements

windows (driver + service):

- visual studio 2022 with the "desktop development with c++" workload
- windows 11 sdk and wdk (the gates evidence used wdk `10.0.26100.0`,
  msbuild 17.14.51)
- a test certificate for the driver, or a target vm with test-signing
  mode enabled — see [windows.md](windows.md)

any host (portable checks):

- cmake ≥ 3.16 and a c compiler
- python3

## 1. get the source

```bash
git clone git@github.com:Kris1225-hash/windows-oom-killer.git
cd windows-oom-killer
```

## 2. portable checks first (any host)

they need no windows toolchain and catch policy or protocol regressions
before you spend time on a full build:

```bash
cmake -S . -B build
cmake --build build
./build/oom_policy_test          # expect: oom policy tests passed

python3 tests/verify_contract.py # expect: driver contract verification passed
```

what they cover:

- `oom_policy_test` exercises the shared pressure policy: critical
  detection at the headroom threshold, confirmation samples, kill retry
  pacing, the hard `max_kills` bound with escalation past it, state
  reset when pressure clears, and both watchdog timeouts (kill deadline,
  monitor heartbeat)
- `verify_contract.py` re-derives the wire structs with ctypes
  (`OOM_TELEMETRY` = 64 bytes, `OOM_KILL_REQUEST` = 88 bytes), checks
  the eight bugcheck codes are unique, and asserts the safety
  invariants are literally present in the sources: protected-process
  and protected-light rejection, pid create-time verification, the
  `ZwTerminateProcess` + bounded wait, the
  `\KernelObjects\MaximumCommitCondition` watch, the test-ioctl
  confirmation gate, and the one-time bugcheck notification key

these are gates G1 and G2 in `GATES.md`; keep them green.

## 3. windows build (driver + service)

from an ordinary (non-elevated) developer shell with msbuild on path:

```powershell
msbuild .\WindowsOomKiller.sln /m /p:Configuration=Debug /p:Platform=x64
```

configurations:

| configuration  | contents                                                        |
|----------------|-----------------------------------------------------------------|
| `Debug`        | normal build, no test ioctl                                     |
| `Release`      | normal build, no test ioctl                                     |
| `BugcheckTest` | adds the admin-only manual-crash ioctl and the `--bugcheck-test` cli |

outputs land under `driver\x64\<Config>\WinOomKillerDriver.sys`,
`service\x64\<Config>\WinOomKillerService.exe`, and
`installer\x64\<Config>\WinOomKillerSetup.exe`. a clean rebuild must
produce zero errors and zero warnings and a signed `.sys` + `.cat`
(gate G3 evidence describes exactly this). the service and installer
link the c runtime statically, so they run on machines (and in WinRE)
with no vc++ runtime installed.

to get the same zip a release ships:

```powershell
.\scripts\package-release.ps1 -Configuration Debug -Version dev
.\scripts\test-setup.ps1 -PackageDir .\dist\WinOomKiller-dev-Debug-x64   # disposable machine only
```

`test-setup.ps1` is what ci runs (gate G10): it installs, upgrades, and
uninstalls on the machine it runs on, then does the same against a
fake offline windows built from saved hives.

do not ship or install a `BugcheckTest` build anywhere you are not
actively crash-testing — the license requires crash-test builds to stay
visibly distinct.

## 4. next

- installing, arming, kill tests, bugcheck tests, uninstalling:
  [windows.md](windows.md)
- what each bugcheck code means and its parameters: README, "bugchecks"
- the full verified history of this project's checks: `GATES.md`
