param(
    [Parameter(Mandatory)] [string] $DriverPath,
    [Parameter(Mandatory)] [string] $ServicePath,
    [switch] $Arm,
    [switch] $StageOnly
)

$ErrorActionPreference = 'Stop'
$installDir = Join-Path $env:ProgramFiles 'WinOomKiller'
$driverTarget = Join-Path $env:SystemRoot 'System32\drivers\WinOomKillerDriver.sys'
$serviceTarget = Join-Path $installDir 'WinOomKillerService.exe'
$serviceExists = Get-Service WinOomKiller -ErrorAction SilentlyContinue
$driverExists = Get-Service WinOomKillerDriver -ErrorAction SilentlyContinue

# $ErrorActionPreference does not cover native commands, so check sc.exe by hand
function Assert-Sc([string] $What) {
    if ($LASTEXITCODE -ne 0) { throw "sc.exe $What failed with exit code $LASTEXITCODE" }
}

if ($serviceExists) { Stop-Service WinOomKiller -Force }
if ($driverExists) { Stop-Service WinOomKillerDriver -Force }

New-Item -ItemType Directory -Force $installDir | Out-Null
Copy-Item $DriverPath $driverTarget -Force
Copy-Item $ServicePath $serviceTarget -Force

if ($driverExists) {
    & sc.exe config WinOomKillerDriver type= kernel start= demand binPath= $driverTarget | Out-Null
    Assert-Sc 'config WinOomKillerDriver'
} else {
    & sc.exe create WinOomKillerDriver type= kernel start= demand binPath= $driverTarget | Out-Null
    Assert-Sc 'create WinOomKillerDriver'
}

if ($serviceExists) {
    & sc.exe config WinOomKiller binPath= "`"$serviceTarget`"" start= auto depend= WinOomKillerDriver | Out-Null
    Assert-Sc 'config WinOomKiller'
} else {
    & sc.exe create WinOomKiller binPath= "`"$serviceTarget`"" start= auto depend= WinOomKillerDriver | Out-Null
    Assert-Sc 'create WinOomKiller'
}

# New-Item -Force would wipe an existing key, so create it only when missing.
# Armed always follows -Arm; tuned thresholds survive a reinstall.
$settings = 'HKLM:\SOFTWARE\WinOomKiller'
if (-not (Test-Path $settings)) { New-Item -Path $settings | Out-Null }
New-ItemProperty $settings Armed -PropertyType DWord -Value ([int]$Arm.IsPresent) -Force | Out-Null
$settingsKey = Get-Item $settings
foreach ($default in @{ CommitHeadroomMiB = 512; ConfirmationSamples = 1; KillRetrySamples = 2; MaxKills = 3 }.GetEnumerator()) {
    if ($settingsKey.GetValueNames() -notcontains $default.Key -or
        $settingsKey.GetValueKind($default.Key) -ne [Microsoft.Win32.RegistryValueKind]::DWord) {
        New-ItemProperty $settings $default.Key -PropertyType DWord -Value $default.Value -Force | Out-Null
    }
}
# the service exe carries the event log message table
$eventSource = 'HKLM:\SYSTEM\CurrentControlSet\Services\EventLog\Application\WinOomKiller'
New-Item -Path $eventSource -Force | Out-Null
New-ItemProperty $eventSource EventMessageFile -PropertyType ExpandString -Value $serviceTarget -Force | Out-Null
New-ItemProperty $eventSource TypesSupported -PropertyType DWord -Value 7 -Force | Out-Null
& sc.exe failure WinOomKiller reset= 60 actions= restart/2000/restart/5000/restart/10000 | Out-Null
Assert-Sc 'failure WinOomKiller'
& sc.exe failureflag WinOomKiller 1 | Out-Null
Assert-Sc 'failureflag WinOomKiller'

if (-not $StageOnly) {
    Start-Service WinOomKillerDriver
    Start-Service WinOomKiller
}
Write-Host "$(if ($StageOnly) {'staged'} else {'installed'}); enforcement is $(if ($Arm) {'armed'} else {'disarmed'})"
