# linux / macos host: the portable parts

the driver and service are windows-only (wdk + win32). everything else
that can be verified without windows lives here, and it is worth
running on every change:

## policy engine tests

`shared/oom_policy.c` is freestanding c with no windows headers. build
and run its test anywhere:

```bash
cmake -S . -B build
cmake --build build
./build/oom_policy_test
# expect: oom policy tests passed
```

on windows, cmake's default visual studio generator is multi-config:
`cmake --build build --config Debug`, then `.\build\Debug\oom_policy_test.exe`
(or `ctest --test-dir build -C Debug`).

covers critical-pressure detection, confirmation sampling, kill retry
pacing, the `max_kills` bound with escalation (which stays escalation,
never another kill, until pressure clears), state reset when pressure
clears, and both watchdog timeouts. the checks are not `assert()`, so a
Release (`NDEBUG`) build still runs them, and `ctest` in the build dir
runs the same binary. the build also pins the real wire-struct layout
from `include/oom_protocol.h` with `_Static_assert`. this is gate G1.

## protocol contract check

```bash
python3 tests/verify_contract.py
# expect: driver contract verification passed
```

python3 only, no third-party packages. it checks the eight bugcheck
codes are unique, that the driver's `C_ASSERT`s and the policy test's
`_Static_assert`s on the wire-struct sizes are still there (its own
ctypes copy of the structs is only a sanity check), and asserts the
safety invariants are present in the driver, service, installer,
service project, and test-runner sources — protected and
critical-process rejection, pid create-time verification, the bounded
kill wait and explicit timeout handling, the maximum-commit watch, the
unbiased watchdog clock, the test-ioctl confirmation gate, the
one-time notification key, the settings and victim-selection rules,
the event-log registration, and that the installer writes `Armed=1`
only for `--arm` and prints the safety notice. it is a
literal-string contract, so
refactoring any of those sites requires updating the test in the same
change. this is gate G2.

## everything else

building `.sys`/`.exe`, installing, arming, kill tests, and bugcheck
tests happen on a windows vm — see [from-scratch.md](from-scratch.md)
and [windows.md](windows.md). there is no macos-specific guidance
because there is nothing macos-specific to do: cmake and python3 work
identically, and the windows parts are equally unavailable.
