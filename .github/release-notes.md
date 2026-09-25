windows oom killer `{{VERSION}}`, built from {{SHA}}.

> **safety notice:** this software terminates processes without the consent of their owners when commit memory runs out, and deliberately crashes windows with a bugcheck when recovery fails. only install it on a machine you are prepared to lose, and never where a crash could cost lives. everything installs **disarmed**. see LICENSE for the full terms.

| download | what it is |
|---|---|
| `WinOomKiller-{{VERSION}}-Release-x64.zip` | the normal build |
| `WinOomKiller-{{VERSION}}-Debug-x64.zip` | the normal build with debug runtime libraries, for troubleshooting |
| `WinOomKiller-{{VERSION}}-BugcheckTest-CRASH-TEST-ONLY-x64.zip` | **crash-test build. test only, not a release build.** it contains the manual bugcheck ioctl and the `--bugcheck-test` cli (LICENSE, section 3); an administrator can crash the machine with it on demand |
| `SHA256SUMS.txt` | checksums of the zips |

every zip holds `WinOomKillerSetup.exe` next to the driver and service it installs, plus `INSTALL.txt`.

- **in windows** (administrator prompt): `WinOomKillerSetup.exe install`
- **from WinRE** (troubleshoot → advanced options → command prompt): `WinOomKillerSetup.exe install --target D:\`, where `D:` is whatever letter your `C:` drive got in WinRE. the installer lists the windows installs it can see if you get it wrong
- **uninstall:** `WinOomKillerSetup.exe uninstall`, or `uninstall --target D:\` from WinRE. that is the way out if a machine is stuck in a bugcheck loop

the driver is **test-signed** by a throwaway ci certificate, so windows only loads it with test signing on (`bcdedit /set testsigning on`, secure boot off). `--arm` enables enforcement; without it the monitor only observes.
