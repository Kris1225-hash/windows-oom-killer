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

if ($serviceExists) { Stop-Service WinOomKiller -Force }
if ($driverExists) { Stop-Service WinOomKillerDriver -Force }

New-Item -ItemType Directory -Force $installDir | Out-Null
Copy-Item $DriverPath $driverTarget -Force
Copy-Item $ServicePath $serviceTarget -Force

if ($driverExists) {
    & sc.exe config WinOomKillerDriver type= kernel start= demand binPath= $driverTarget | Out-Null
} else {
    & sc.exe create WinOomKillerDriver type= kernel start= demand binPath= $driverTarget | Out-Null
}

if ($serviceExists) {
    & sc.exe config WinOomKiller binPath= "`"$serviceTarget`"" start= auto depend= WinOomKillerDriver | Out-Null
} else {
    & sc.exe create WinOomKiller binPath= "`"$serviceTarget`"" start= auto depend= WinOomKillerDriver | Out-Null
}

$settings = 'HKLM:\SOFTWARE\WinOomKiller'
New-Item -Path $settings -Force | Out-Null
New-ItemProperty $settings Armed -PropertyType DWord -Value ([int]$Arm.IsPresent) -Force | Out-Null
New-ItemProperty $settings CommitHeadroomMiB -PropertyType DWord -Value 512 -Force | Out-Null
New-ItemProperty $settings ConfirmationSamples -PropertyType DWord -Value 1 -Force | Out-Null
New-ItemProperty $settings KillRetrySamples -PropertyType DWord -Value 2 -Force | Out-Null
New-ItemProperty $settings MaxKills -PropertyType DWord -Value 3 -Force | Out-Null
& sc.exe failure WinOomKiller reset= 60 actions= restart/2000/restart/5000/restart/10000 | Out-Null
& sc.exe failureflag WinOomKiller 1 | Out-Null

if (-not $StageOnly) {
    Start-Service WinOomKillerDriver
    Start-Service WinOomKiller
}
Write-Host "$(if ($StageOnly) {'staged'} else {'installed'}); enforcement is $(if ($Arm) {'armed'} else {'disarmed'})"
