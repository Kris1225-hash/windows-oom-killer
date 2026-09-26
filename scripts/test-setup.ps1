# Exercises WinOomKillerSetup.exe on a disposable machine (the CI runner):
# an online install/upgrade/uninstall through the SCM, then an offline
# install/uninstall into a fake windows tree built from saved hives, checking
# that the offline registry matches what the SCM itself wrote online.
# nothing is ever started: installs use --stage-only or go offline.
param([Parameter(Mandatory)] [string] $PackageDir)

$ErrorActionPreference = 'Stop'
$setup = Join-Path (Resolve-Path $PackageDir) 'WinOomKillerSetup.exe'
$services = 'HKLM\SYSTEM\CurrentControlSet\Services'
$eventSourceKey = 'EventLog\Application\WinOomKiller'
$temp = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { [IO.Path]::GetTempPath() }
$failed = 0

# native stderr must not become a terminating error under windows powershell 5.1
function Invoke-Setup([int] $Expect, [string[]] $Arguments) {
    $ErrorActionPreference = 'Continue'
    Write-Host "`n> WinOomKillerSetup $($Arguments -join ' ')"
    & $setup @Arguments
    if ($LASTEXITCODE -ne $Expect) {
        throw "WinOomKillerSetup $($Arguments -join ' ') exited $LASTEXITCODE, expected $Expect"
    }
}

function Check([bool] $Condition, [string] $Message) {
    if ($Condition) {
        Write-Host "  ok    $Message"
    } else {
        Write-Host "  FAIL  $Message"
        $script:failed++
    }
}

# "REG_TYPE data" for one value, via reg.exe so no handles stay open on loaded hives
function Get-RegValue([string] $Key, [string] $Name) {
    $ErrorActionPreference = 'Continue'
    $lines = & reg.exe query $Key /v $Name 2>$null
    if ($LASTEXITCODE -ne 0) { return $null }
    foreach ($line in $lines) {
        if ($line -match "^\s+$([regex]::Escape($Name))\s+(REG_\w+)\s*(.*)$") {
            return "$($Matches[1]) $($Matches[2])".Trim()
        }
    }
    return $null
}

function Test-RegKey([string] $Key) {
    $ErrorActionPreference = 'Continue'
    & reg.exe query $Key *> $null
    return $LASTEXITCODE -eq 0
}

function Test-Service([string] $Name) {
    $ErrorActionPreference = 'Continue'
    & sc.exe query $Name *> $null
    return $LASTEXITCODE -ne 1060
}

function Invoke-Reg([string[]] $Arguments) {
    $ErrorActionPreference = 'Continue'
    & reg.exe @Arguments | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "reg $($Arguments -join ' ') failed with $LASTEXITCODE" }
}

$monitorValues = 'Type', 'Start', 'ErrorControl', 'ImagePath', 'ObjectName', 'DependOnService',
    'FailureActions', 'FailureActionsOnNonCrashFailures'
$driverValues = 'Type', 'Start', 'ErrorControl'
$settings = [ordered]@{
    CommitHeadroomMiB = 'REG_DWORD 0x200'; ConfirmationSamples = 'REG_DWORD 0x1'
    KillRetrySamples = 'REG_DWORD 0x2'; MaxKills = 'REG_DWORD 0x3'
}

Write-Host '== cli'
Invoke-Setup 0 '--help'
Invoke-Setup 2 @()
Invoke-Setup 2 'install', '--bogus'
Invoke-Setup 2 'uninstall', '--arm'
Invoke-Setup 2 'install', '--target'
Invoke-Setup 2 'install', '--target', 'C:\', '--target', 'D:\'
Invoke-Setup 1 'install', '--target', (Join-Path $temp 'no-windows-here')

Write-Host "`n== online: this runner's windows"
$serviceExe = Join-Path $env:ProgramFiles 'WinOomKiller\WinOomKillerService.exe'
$driverSys = Join-Path $env:SystemRoot 'System32\drivers\WinOomKillerDriver.sys'
Invoke-Setup 0 'install', '--stage-only'
Check (Test-Path $serviceExe) "service copied to $serviceExe"
Check (Test-Path $driverSys) "driver copied to $driverSys"
Check ((Get-RegValue "$services\WinOomKillerDriver" 'Type') -eq 'REG_DWORD 0x1') 'driver is a kernel driver'
Check ((Get-RegValue "$services\WinOomKillerDriver" 'Start') -eq 'REG_DWORD 0x3') 'driver is demand-start'
Check ((Get-RegValue "$services\WinOomKiller" 'Start') -eq 'REG_DWORD 0x2') 'monitor is auto-start'
Check ((Get-RegValue "$services\WinOomKiller" 'DependOnService') -eq 'REG_MULTI_SZ WinOomKillerDriver') 'monitor depends on the driver'
Check ((Get-RegValue "$services\WinOomKiller" 'ImagePath') -eq "REG_EXPAND_SZ `"$serviceExe`"") 'monitor image path is quoted'
Check ((Get-RegValue 'HKLM\SOFTWARE\WinOomKiller' 'Armed') -eq 'REG_DWORD 0x0') 'installed disarmed'
foreach ($name in $settings.Keys) {
    Check ((Get-RegValue 'HKLM\SOFTWARE\WinOomKiller' $name) -eq $settings[$name]) "$name default"
}
Check ((& sc.exe query WinOomKillerDriver | Out-String) -match 'STOPPED') '--stage-only loaded nothing'
$eventMessageFile = Get-RegValue "$services\$eventSourceKey" 'EventMessageFile'
Check ($eventMessageFile -eq "REG_EXPAND_SZ $serviceExe") 'event source uses the service exe as its message file'
Check ((Get-RegValue "$services\$eventSourceKey" 'TypesSupported') -eq 'REG_DWORD 0x7') 'event source supports error, warning, information'

# what the SCM wrote is the reference for the offline writer below
$reference = @{}
foreach ($name in $monitorValues) { $reference["WinOomKiller\$name"] = Get-RegValue "$services\WinOomKiller" $name }
foreach ($name in $driverValues) { $reference["WinOomKillerDriver\$name"] = Get-RegValue "$services\WinOomKillerDriver" $name }
Check ($reference['WinOomKiller\FailureActions'] -like 'REG_BINARY 3C00000000000000000000000300000014000000*') 'scm recorded the failure actions'

# a tuned threshold must survive a reinstall; a wrongly typed one is repaired
Invoke-Reg 'add', 'HKLM\SOFTWARE\WinOomKiller', '/v', 'CommitHeadroomMiB', '/t', 'REG_DWORD', '/d', '256', '/f'
Invoke-Reg 'add', 'HKLM\SOFTWARE\WinOomKiller', '/v', 'MaxKills', '/t', 'REG_SZ', '/d', '3', '/f'
Invoke-Setup 0 'install', '--stage-only', '--arm'
Check ((Get-RegValue 'HKLM\SOFTWARE\WinOomKiller' 'Armed') -eq 'REG_DWORD 0x1') 'reinstall over existing services with --arm'
Check ((Get-RegValue 'HKLM\SOFTWARE\WinOomKiller' 'CommitHeadroomMiB') -eq 'REG_DWORD 0x100') 'reinstall keeps a tuned threshold'
Check ((Get-RegValue 'HKLM\SOFTWARE\WinOomKiller' 'MaxKills') -eq 'REG_DWORD 0x3') 'reinstall repairs a wrongly typed threshold'
Invoke-Setup 0 'install', '--stage-only'
Check ((Get-RegValue 'HKLM\SOFTWARE\WinOomKiller' 'Armed') -eq 'REG_DWORD 0x0') 'reinstall without --arm disarms'
Check ((Get-RegValue 'HKLM\SOFTWARE\WinOomKiller' 'CommitHeadroomMiB') -eq 'REG_DWORD 0x100') 'disarming keeps the tuned threshold'

Invoke-Setup 0 'uninstall'
Check (-not (Test-Service 'WinOomKiller')) 'monitor service deleted'
Check (-not (Test-Service 'WinOomKillerDriver')) 'driver service deleted'
Check (-not (Test-Path $serviceExe)) 'service binary removed'
Check (-not (Test-Path $driverSys)) 'driver binary removed'
Check (-not (Test-RegKey 'HKLM\SOFTWARE\WinOomKiller')) 'settings removed'
Check (-not (Test-RegKey "$services\$eventSourceKey")) 'event source removed'
Invoke-Setup 0 'uninstall'

Write-Host "`n== offline: a fake windows tree, as WinRE would see a C: drive"
$root = Join-Path $temp 'oomkiller-offline-windows'
$windir = Join-Path $root 'Windows'
$config = Join-Path $windir 'System32\config'
$scratch = 'HKCU\Software\WinOomKillerCiHive'
$taskId = '{0F0F0F0F-1111-2222-3333-444455556666}'
$taskCache = 'Microsoft\Windows NT\CurrentVersion\Schedule\TaskCache'
Remove-Item $root -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory $config, (Join-Path $windir 'System32\Tasks') | Out-Null

if (Test-RegKey $scratch) { Invoke-Reg 'delete', $scratch, '/f' }
Invoke-Reg 'add', "$scratch\Select", '/v', 'Current', '/t', 'REG_DWORD', '/d', '1', '/f'
Invoke-Reg 'add', "$scratch\Select", '/v', 'Default', '/t', 'REG_DWORD', '/d', '1', '/f'
Invoke-Reg 'add', "$scratch\ControlSet001\Services\Unrelated", '/v', 'Start', '/t', 'REG_DWORD', '/d', '4', '/f'
Invoke-Reg 'add', "$scratch\ControlSet002\Services\WinOomKillerDriver", '/v', 'Start', '/t', 'REG_DWORD', '/d', '3', '/f'
Invoke-Reg 'save', $scratch, (Join-Path $config 'SYSTEM'), '/y'
Invoke-Reg 'delete', $scratch, '/f'
Invoke-Reg 'add', "$scratch\Microsoft\Windows NT\CurrentVersion", '/v', 'SystemRoot', '/t', 'REG_SZ', '/d', 'C:\WINDOWS', '/f'
Invoke-Reg 'add', "$scratch\Microsoft\Windows\CurrentVersion", '/v', 'ProgramFilesDir', '/t', 'REG_SZ', '/d', 'C:\Program Files', '/f'
Invoke-Reg 'add', "$scratch\$taskCache\Tree\WinOomKillerBugcheckTests", '/v', 'Id', '/t', 'REG_SZ', '/d', $taskId, '/f'
Invoke-Reg 'add', "$scratch\$taskCache\Tasks\$taskId", '/v', 'Path', '/t', 'REG_SZ', '/d', '\WinOomKillerBugcheckTests', '/f'
Invoke-Reg 'add', "$scratch\$taskCache\Boot\$taskId", '/f'
Invoke-Reg 'save', $scratch, (Join-Path $config 'SOFTWARE'), '/y'
Invoke-Reg 'delete', $scratch, '/f'
Set-Content (Join-Path $windir 'System32\Tasks\WinOomKillerBugcheckTests') '<Task />'

$offlineSystem = 'HKLM\WinOomKillerCiSystem'
$offlineSoftware = 'HKLM\WinOomKillerCiSoftware'
function Mount-Offline {
    Invoke-Reg 'load', $offlineSystem, (Join-Path $config 'SYSTEM')
    Invoke-Reg 'load', $offlineSoftware, (Join-Path $config 'SOFTWARE')
}
function Dismount-Offline {
    [GC]::Collect()
    Invoke-Reg 'unload', $offlineSystem
    Invoke-Reg 'unload', $offlineSoftware
}

Invoke-Setup 0 'install', '--target', $root, '--arm'
Check (Test-Path (Join-Path $windir 'System32\drivers\WinOomKillerDriver.sys')) 'driver copied into the offline windows'
Check (Test-Path (Join-Path $root 'Program Files\WinOomKiller\WinOomKillerService.exe')) 'service copied into the offline program files'
Mount-Offline
try {
    $offlineServices = "$offlineSystem\ControlSet001\Services"
    foreach ($key in $reference.Keys) {
        $service, $name = $key -split '\\'
        $actual = Get-RegValue "$offlineServices\$service" $name
        Check ($actual -eq $reference[$key]) "offline $key matches the scm ($actual)"
    }
    Check ((Get-RegValue "$offlineServices\WinOomKillerDriver" 'ImagePath') -eq 'REG_EXPAND_SZ \SystemRoot\System32\drivers\WinOomKillerDriver.sys') 'offline driver image path'
    Check ((Get-RegValue "$offlineServices\$eventSourceKey" 'EventMessageFile') -eq $eventMessageFile) 'offline event source matches the online one'
    Check ((Get-RegValue "$offlineServices\$eventSourceKey" 'TypesSupported') -eq 'REG_DWORD 0x7') 'offline event source types'
    Check ((Get-RegValue "$offlineSoftware\WinOomKiller" 'Armed') -eq 'REG_DWORD 0x1') 'offline install honours --arm'
    foreach ($name in $settings.Keys) {
        Check ((Get-RegValue "$offlineSoftware\WinOomKiller" $name) -eq $settings[$name]) "offline $name default"
    }
} finally {
    Dismount-Offline
}

Invoke-Setup 0 'uninstall', '--target', $windir
Check (-not (Test-Path (Join-Path $windir 'System32\drivers\WinOomKillerDriver.sys'))) 'offline driver removed'
Check (-not (Test-Path (Join-Path $root 'Program Files\WinOomKiller'))) 'offline program files dir removed'
Check (-not (Test-Path (Join-Path $windir 'System32\Tasks\WinOomKillerBugcheckTests'))) 'offline test task file removed'
Mount-Offline
try {
    Check (-not (Test-RegKey "$offlineSystem\ControlSet001\Services\WinOomKiller")) 'offline monitor service removed'
    Check (-not (Test-RegKey "$offlineSystem\ControlSet001\Services\WinOomKillerDriver")) 'offline driver service removed'
    Check (-not (Test-RegKey "$offlineSystem\ControlSet001\Services\$eventSourceKey")) 'offline event source removed'
    Check (-not (Test-RegKey "$offlineSystem\ControlSet002\Services\WinOomKillerDriver")) 'last-known-good control set cleaned too'
    Check (Test-RegKey "$offlineSystem\ControlSet001\Services\Unrelated") 'unrelated services left alone'
    Check (-not (Test-RegKey "$offlineSoftware\WinOomKiller")) 'offline settings removed'
    Check (-not (Test-RegKey "$offlineSoftware\$taskCache\Tree\WinOomKillerBugcheckTests")) 'offline task tree entry removed'
    Check (-not (Test-RegKey "$offlineSoftware\$taskCache\Tasks\$taskId")) 'offline task definition removed'
    Check (-not (Test-RegKey "$offlineSoftware\$taskCache\Boot\$taskId")) 'offline boot trigger removed'
} finally {
    Dismount-Offline
}

if ($failed) { throw "$failed installer check(s) failed" }
Write-Host "`nall installer checks passed"
