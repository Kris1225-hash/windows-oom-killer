param(
    [switch] $Start,
    [switch] $Resume,
    [switch] $Cancel
)

$ErrorActionPreference = 'Stop'
$taskName = 'WinOomKillerBugcheckTests'
$dataDir = Join-Path $env:ProgramData 'WinOomKiller'
$statePath = Join-Path $dataDir 'bugcheck-test-state.json'
$resultPath = Join-Path $dataDir 'bugcheck-test-results.json'
$logPath = Join-Path $dataDir 'bugcheck-test.log'
$runnerPath = Join-Path $dataDir 'test-bugchecks.ps1'
$testExe = Join-Path $env:ProgramFiles 'WinOomKiller\WinOomKillerService.exe'
$codes = 1..8 | ForEach-Object { '0xE0F000{0:X2}' -f $_ }
$utf8 = [Text.UTF8Encoding]::new($false)

function Write-DurableText([string] $Path, [string] $Text, [bool] $Append) {
    $mode = if ($Append) { [IO.FileMode]::Append } else { [IO.FileMode]::Create }
    $bytes = $utf8.GetBytes($Text)
    $stream = [IO.FileStream]::new($Path, $mode, [IO.FileAccess]::Write,
        [IO.FileShare]::Read, 4096, [IO.FileOptions]::WriteThrough)
    try {
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush($true)
    } finally {
        $stream.Dispose()
    }
}

function Write-TestLog([string] $Message) {
    Write-DurableText $logPath "$(Get-Date -Format o) $Message`r`n" $true
}

function Save-State($State) {
    Write-DurableText $statePath ($State | ConvertTo-Json -Depth 6) $false
}

function Remove-TestTask {
    Unregister-ScheduledTask $taskName -Confirm:$false -ErrorAction SilentlyContinue
}

function Fail-Test($State, [string] $Message) {
    $State.Failure = $Message
    Save-State $State
    Write-TestLog "failed: $Message"
    Remove-TestTask
    throw $Message
}

function Read-BugcheckEvent($Pending) {
    $started = ([datetime]$Pending.TriggeredAt).AddSeconds(-5)
    for ($attempt = 0; $attempt -lt 30; ++$attempt) {
        $event = Get-WinEvent -FilterHashtable @{LogName='System'; Id=1001; StartTime=$started} `
            -ErrorAction SilentlyContinue |
            Where-Object ProviderName -eq 'Microsoft-Windows-WER-SystemErrorReporting' |
            Select-Object -First 1
        if ($event) {
            $hex = [regex]::Matches($event.Message, '0x[0-9a-fA-F]+')
            if ($hex.Count -ge 5) {
                return @($hex[0..4] | ForEach-Object {
                    [Convert]::ToUInt64($_.Value.Substring(2), 16)
                })
            }
        }
        Start-Sleep 2
    }
    return $null
}

function Invoke-NextTest {
    $state = Get-Content $statePath -Raw | ConvertFrom-Json

    if ($state.Pending) {
        $actual = Read-BugcheckEvent $state.Pending
        if (!$actual) {
            Fail-Test $state 'the previous bugcheck event did not appear within 60 seconds'
        }
        $expected = @([uint64]$state.Pending.Code) + @($state.Pending.Parameters | ForEach-Object { [uint64]$_ })
        if (Compare-Object $expected $actual -SyncWindow 0) {
            Fail-Test $state "bugcheck mismatch: expected $($expected -join ', '), got $($actual -join ', ')"
        }
        $state.Results += [pscustomobject]@{
            Code = $state.Pending.Code
            Parameters = $state.Pending.Parameters
            PassedAt = (Get-Date -Format o)
        }
        $state.Pending = $null
        Save-State $state
    }

    if ([int]$state.NextIndex -ge $codes.Count) {
        $state.CompletedAt = Get-Date -Format o
        Write-DurableText $resultPath ($state | ConvertTo-Json -Depth 6) $false
        Remove-TestTask
        Remove-Item $statePath -Force
        Write-TestLog "complete: all $($codes.Count) custom bugchecks passed"
        return
    }

    $index = [int]$state.NextIndex
    $code = [Convert]::ToUInt64($codes[$index].Substring(2), 16)
    $parameters = @(
        [uint64](0x11110000 + $index),
        [uint64](0x22220000 + $index),
        [uint64](0x33330000 + $index),
        [uint64](0x44440000 + $index)
    )
    $state.Pending = [pscustomobject]@{
        Code = $code
        Parameters = $parameters
        TriggeredAt = (Get-Date -Format o)
    }
    $state.NextIndex = $index + 1
    Save-State $state
    Write-TestLog ('triggering 0x{0:X8} ({1})' -f $code, ($parameters -join ', '))

    Start-Service WinOomKillerDriver
    & $testExe --bugcheck-test $codes[$index] @parameters
    Fail-Test $state "bugcheck command returned with exit code $LASTEXITCODE"
}

$modeCount = 0
if ($Start) { ++$modeCount }
if ($Resume) { ++$modeCount }
if ($Cancel) { ++$modeCount }
if ($modeCount -ne 1) {
    throw 'use exactly one of -Start, -Resume, or -Cancel'
}

New-Item -ItemType Directory -Path $dataDir -Force | Out-Null

if ($Cancel) {
    Remove-TestTask
    Remove-Item $statePath -Force -ErrorAction SilentlyContinue
    Write-TestLog 'cancelled'
    return
}

if ($Start) {
    if (Test-Path $statePath) {
        throw "a bugcheck run is already active; use -Cancel first"
    }
    $repo = Split-Path $PSScriptRoot -Parent
    & (Join-Path $PSScriptRoot 'install.ps1') `
        -DriverPath (Join-Path $repo 'driver\x64\BugcheckTest\WinOomKillerDriver.sys') `
        -ServicePath (Join-Path $repo 'service\x64\BugcheckTest\WinOomKillerService.exe') `
        -StageOnly
    Set-Service WinOomKiller -StartupType Manual
    Copy-Item $PSCommandPath $runnerPath -Force
    $action = New-ScheduledTaskAction -Execute 'powershell.exe' `
        -Argument "-NoProfile -ExecutionPolicy Bypass -File `"$runnerPath`" -Resume"
    $trigger = New-ScheduledTaskTrigger -AtStartup
    $principal = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount `
        -RunLevel Highest
    Register-ScheduledTask $taskName -Action $action -Trigger $trigger `
        -Principal $principal -Force | Out-Null
    Save-State ([pscustomobject]@{
        StartedAt = (Get-Date -Format o)
        NextIndex = 0
        Pending = $null
        Results = @()
        Failure = $null
        CompletedAt = $null
    })
    Write-TestLog 'started; taking one clean reboot before the crash sequence'
    Restart-Computer -Force
    return
} else {
    Start-Sleep 20
}

Invoke-NextTest
