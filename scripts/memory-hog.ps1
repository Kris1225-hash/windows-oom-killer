param(
    [Parameter(Mandatory)] [ValidateRange(64, 1048576)] [int] $MemoryMiB
)

$chunks = [System.Collections.Generic.List[byte[]]]::new()
$chunkMiB = 64
for ($allocated = 0; $allocated -lt $MemoryMiB; $allocated += $chunkMiB) {
    $bytes = [byte[]]::new($chunkMiB * 1MB)
    for ($i = 0; $i -lt $bytes.Length; $i += 4096) { $bytes[$i] = 1 }
    $chunks.Add($bytes)
    Write-Host "allocated $($allocated + $chunkMiB) MiB"
}
Write-Host "holding memory in pid $PID; press ctrl-c to release"
Wait-Event
