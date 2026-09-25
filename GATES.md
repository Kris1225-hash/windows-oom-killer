# Gates: windows oom killer

Scope: build a kernel-backed Windows OOM monitor that kills a selected memory hog and bugchecks only after recovery fails

- [x] G0: this ledger states checks that can fail
  CHECK: inspect this file — every gate after G0 names a fail-able CHECK command or VM EVIDENCE (no claim-only boxes)
  EXPECT: G1–G11 each document how they can fail; no silent aspirational gates
  EVIDENCE: 2026-08-31 — LINT OK (3 expected manual-gate warnings); 2026-09-19 — replaced machine-local `gate-lint.mjs` CHECK path (not shipped in-repo) with this portable ledger inspection

- [x] G1: the portable pressure policy bounds recovery kills and resets when pressure clears
  CHECK: cmake -S . -B build && cmake --build build && ./build/oom_policy_test
  EXPECT: oom policy tests passed
  EVIDENCE: 2026-08-31 — oom policy tests passed

- [x] G2: the driver protocol keeps telemetry and bugcheck parameters stable
  CHECK: python3 tests/verify_contract.py
  EXPECT: driver contract verification passed
  EVIDENCE: 2026-08-31 — driver contract verification passed

- [x] G3: the driver and service build with the Windows WDK toolchain
  CHECK: `MSBuild.exe WindowsOomKiller.sln /m /t:Rebuild /p:Configuration=Debug /p:Platform=x64`
  EXPECT: signed driver/catalog and service build with no errors or warnings
  EVIDENCE: 2026-09-01 — WDK 10.0.26100.0 / MSBuild 17.14.51: signability passed, signed `.sys` and `.cat`, 0 errors, 0 warnings; 2026-09-25 — master had stopped compiling since the 09-19 DriverEntry hardening (`goto fail` to an undefined label, C2094); fixed to `goto fail_init`, then Debug, Release, and BugcheckTest (driver, service, installer) rebuilt with 0 errors, 0 warnings on Build Tools 17.14.37614 + WDK 10.0.26100.0

- [x] G4: controlled memory pressure in the VM kills an eligible process before Windows becomes unresponsive
  EVIDENCE: 2026-09-01 — with 32 GiB RAM and a fixed 5 GiB pagefile, pid 3292 consumed 9,128,618 private pages; the driver terminated it and restored commit headroom from 46 MiB to about 35 GiB without a reboot

- [x] G5: forced recovery failure in the VM produces the expected custom bugcheck and four parameters
  EVIDENCE: 2026-09-01 — Windows recorded bugcheck `0xE0F00004` (`OOM_FATAL_PAGEFILE_GROWTH_FAILED`) with charge `0x93fc3b`, limit `0x93fc67`, available pages `0xceb61`, and packed backing data `0x000000030013f148`; minidump `090126-3156-01.dmp` was written and the VM rebooted automatically

- [x] G6: a successful recovery kill notifies the affected interactive session
  EVIDENCE: 2026-09-01 — pid 1620 was terminated at 9,097,866 private pages, `WTSSendMessageW` returned without a logged error, commit recovered to about 35 GiB, and the VM remained online

- [x] G7: the separate bugcheck-test build can trigger a selected custom code and parameters while regular builds reject the test ioctl
  EVIDENCE: 2026-09-01 — the Debug service contained no trigger flag and its driver rejected the test ioctl; the BugcheckTest build produced `0xE0F00001 (1, 2, 3, 4)` and minidump `090126-3375-01.dmp`; the regular Debug build was restored disarmed afterward

- [x] G8: the reboot-resilient test runner verifies every custom bugcheck code and all four parameters
  EVIDENCE: 2026-09-01 — `scripts/test-bugchecks.ps1 -Start` triggered and verified `0xE0F00001` through `0xE0F00008` across eight automatic reboots; every System event matched its four unique marker parameters, `Failure` was null, and the runner removed its startup task after completion

- [x] G9: after reboot, the service translates a custom bugcheck into a one-time interactive warning
  EVIDENCE: 2026-09-01 — the service translated the latest `0xE0F00008` System event, queued its friendly explanation to session 1, persisted event record `3079`, and did not notify again after a service restart; Debug, Release, and BugcheckTest service builds completed with 0 errors and 0 warnings

- [x] G10: `WinOomKillerSetup.exe` installs and removes the driver and service online and offline, including from a recovery environment
  CHECK: `scripts\test-setup.ps1 -PackageDir <unzipped release package>` (disposable machine; ci runs it for every configuration)
  EXPECT: all installer checks passed
  EVIDENCE: 2026-09-25 — VM (overlay of the kernel-dev VM, Windows 11 26200, test signing on): `test-setup.ps1` passed on the Release package, including an upgrade over the running `install.ps1` install and offline service keys identical to what the SCM wrote online (`FailureActions` byte-for-byte). From Windows 11 install media WinPE (MiniNT, `X:` system drive, no vc++ runtime), `install` without `--target` refused and listed `--target C:\`, `--target X:\` was refused, and `install --target C:\` exited 0; after boot `WinOomKillerDriver` and `WinOomKiller` were RUNNING and the monitor logged `monitor started disarmed: commit headroom 131072 pages`. `uninstall --target C:\` from WinPE exited 0; after boot both services were absent (1060), and the binaries and settings were gone

- [x] G11: github actions builds all three configurations, smoke-tests each installer, and publishes one release with all three zips
  CHECK: push to `master` and open the `build` workflow run
  EXPECT: checks, Debug, Release, and BugcheckTest jobs green; a `build-<n>` prerelease holding three zips and `SHA256SUMS.txt`
  EVIDENCE: 2026-09-25 — run 36189003513 on `6fecadf`: all five jobs green on the first run, `test-setup.ps1` printed `all installer checks passed` for every configuration on windows-2022, and prerelease `build-1` published the Debug, Release, and `BugcheckTest-CRASH-TEST-ONLY` zips with a `SHA256SUMS.txt` that verifies; the shipped `WinOomKillerSetup.exe` imports only `KERNEL32` and `ADVAPI32`
