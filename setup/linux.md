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

covers critical-pressure detection, confirmation sampling, kill retry
pacing, the `max_kills` bound with escalation (which stays escalation,
never another kill, until pressure clears), state reset when pressure
clears, and both watchdog timeouts. the checks are not `assert()`, so a
Release (`NDEBUG`) build still runs them, and `ctest` in the build dir
runs the same binary. this is gate G1.

## protocol contract check

```bash
python3 tests/verify_contract.py
# expect: driver contract verification passed
```

python3 only, no third-party packages. it re-derives the wire structs
with ctypes and pins their sizes (telemetry 64 bytes, kill request 88
bytes), checks the eight bugcheck codes are unique, and asserts the
safety invariants are present in the driver, service, and test-runner
sources — protected-process rejection, pid create-time verification,
the maximum-commit watch, the test-ioctl confirmation gate, the
one-time notification key, and that the installer writes `Armed=1`
only for `--arm` and always prints the safety notice. it is a
literal-string contract, so
refactoring any of those sites requires updating the test in the same
change. this is gate G2.

## everything else

building `.sys`/`.exe`, installing, arming, kill tests, and bugcheck
tests happen on a windows vm — see [from-scratch.md](from-scratch.md)
and [windows.md](windows.md). there is no macos-specific guidance
because there is nothing macos-specific to do: cmake and python3 work
identically, and the windows parts are equally unavailable.
