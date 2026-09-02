# Gates: windows oom killer

Scope: build a kernel-backed Windows OOM monitor that kills a selected memory hog and bugchecks only after recovery fails

- [x] G0: this ledger states checks that can fail
  CHECK: node /home/kris/.agents/skills/unlazy/scripts/gate-lint.mjs GATES.md
  EXPECT: LINT OK
  EVIDENCE: 2026-08-31 — LINT OK (3 expected manual-gate warnings)

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
  EVIDENCE: 2026-09-01 — WDK 10.0.26100.0 / MSBuild 17.14.51: signability passed, signed `.sys` and `.cat`, 0 errors, 0 warnings

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
