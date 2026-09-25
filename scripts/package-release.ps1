param(
    [Parameter(Mandatory)] [ValidateSet('Debug', 'Release', 'BugcheckTest')] [string] $Configuration,
    [Parameter(Mandatory)] [ValidatePattern('^[0-9A-Za-z][0-9A-Za-z.\-]*$')] [string] $Version,
    [string] $OutDir = 'dist'
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$flavor = if ($Configuration -eq 'BugcheckTest') { 'BugcheckTest-CRASH-TEST-ONLY' } else { $Configuration }
$name = "WinOomKiller-$Version-$flavor-x64"
$outRoot = (New-Item -ItemType Directory -Force $OutDir).FullName
$stage = Join-Path $outRoot $name
$zip = Join-Path $outRoot "$name.zip"
$driverOut = Join-Path $repo "driver\x64\$Configuration"
$serviceOut = Join-Path $repo "service\x64\$Configuration"
$setupOut = Join-Path $repo "installer\x64\$Configuration"

Remove-Item $stage, $zip -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory $stage, (Join-Path $stage 'symbols') | Out-Null

Copy-Item (Join-Path $setupOut 'WinOomKillerSetup.exe') $stage
Copy-Item (Join-Path $driverOut 'WinOomKillerDriver.sys') $stage
Copy-Item (Join-Path $serviceOut 'WinOomKillerService.exe') $stage
Copy-Item (Join-Path $repo 'LICENSE'), (Join-Path $repo 'README.md') $stage
Copy-Item (Join-Path $repo 'installer\INSTALL.txt') $stage
foreach ($pdb in 'WinOomKillerSetup', 'WinOomKillerDriver', 'WinOomKillerService') {
    Get-ChildItem $setupOut, $driverOut, $serviceOut -Filter "$pdb.pdb" -ErrorAction SilentlyContinue |
        Select-Object -First 1 | Copy-Item -Destination (Join-Path $stage 'symbols')
}

# the wdk's signed package folder (.sys + .inf + .cat) and its throwaway test certificate
$package = Join-Path $driverOut 'WinOomKillerDriver'
if (Test-Path (Join-Path $package 'winoom.inf')) {
    Copy-Item $package (Join-Path $stage 'driver-package') -Recurse
}
$certificate = Join-Path $driverOut 'WinOomKillerDriver.cer'
if (Test-Path $certificate) { Copy-Item $certificate $stage }

$commit = $env:GITHUB_SHA
if (-not $commit) {
    $commit = try { git -C $repo rev-parse HEAD 2>$null } catch { $null }
}
if (-not $commit) { $commit = 'unknown' }
$buildInfo = @(
    "windows oom killer $Version",
    "configuration: $Configuration (x64)",
    "commit: $commit",
    "built: $((Get-Date).ToUniversalTime().ToString('u'))"
)
if ($Configuration -eq 'BugcheckTest') {
    $buildInfo += @(
        '',
        'CRASH-TEST BUILD - TEST ONLY - NOT A RELEASE BUILD.',
        'this build contains the manual bugcheck ioctl and the --bugcheck-test cli',
        '(LICENSE, section 3). installing it lets an administrator crash the machine',
        'on demand. only use it on a disposable machine you are actively crash-testing.'
    )
}
Set-Content (Join-Path $stage 'BUILD.txt') $buildInfo -Encoding ascii

Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
Get-ChildItem $stage -Recurse -File | ForEach-Object {
    '{0,10:N0}  {1}' -f $_.Length, $_.FullName.Substring($stage.Length + 1)
}
Write-Host "packaged $zip"

if ($env:GITHUB_OUTPUT) {
    "dir=$stage" | Out-File $env:GITHUB_OUTPUT -Append -Encoding utf8
    "zip=$zip" | Out-File $env:GITHUB_OUTPUT -Append -Encoding utf8
}
