param(
    [int]$Frames = 180,
    [int]$Draws = 256,
    [int]$Runs = 3,
    [string]$Executable = '',
    [string]$OutputDirectory = '',
    [string]$Python = 'G:\kity_work\tools\python313\python.exe'
)
$ErrorActionPreference = 'Stop'
if ($Frames -lt 1 -or $Frames -gt 5000 -or $Draws -lt 64 -or $Draws -gt 5000 -or $Draws % 64 -ne 0 -or $Runs -lt 1 -or $Runs -gt 20) {
    throw 'Frames: 1..5000; draws: multiples of 64 up to 5000; runs: 1..20'
}
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
if (!$Executable) { $Executable = Join-Path $repo '_Build/windows/shader_recompiler_compute_tests.exe' }
$Executable = (Resolve-Path -LiteralPath $Executable).Path
if (!$OutputDirectory) {
    $OutputDirectory = Join-Path $repo ('_Build/windows/benchmarks/renderer-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Use a new output directory to preserve captures' }
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path
if ($OutputDirectory.Contains('"')) { throw 'Invalid quoted output path' }
$git = Get-Command git -ErrorAction SilentlyContinue
if (!$git) { $git = Get-Command 'C:/Program Files/Git/cmd/git.exe' }
$manifest = [ordered]@{
    schema = 1; startedUtc = (Get-Date).ToUniversalTime().ToString('o')
    executable = $Executable; executableSha256 = (Get-FileHash -LiteralPath $Executable).Hash
    frames = $Frames; draws = $Draws; runs = $Runs
    gitHead = (& $git.Source -C $repo rev-parse HEAD)
    gitStatus = @(& $git.Source -C $repo status --porcelain)
    results = @()
}
& $git.Source -C $repo diff HEAD "--output=$(Join-Path $OutputDirectory 'source.patch')"
if ($LASTEXITCODE -ne 0) { throw 'Could not preserve source patch' }
foreach ($relative in @(& $git.Source -C $repo ls-files --others --exclude-standard)) {
    $destination = Join-Path $OutputDirectory (Join-Path 'untracked-sources' $relative)
    New-Item -ItemType Directory -Path (Split-Path $destination) -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $repo $relative) -Destination $destination
}
Copy-Item -LiteralPath (Join-Path $repo '_Build/windows/CMakeCache.txt') -Destination (Join-Path $OutputDirectory 'CMakeCache.txt')
$manifest.archivedExecutable = Join-Path $OutputDirectory 'benchmark.exe'
Copy-Item -LiteralPath $Executable -Destination $manifest.archivedExecutable
foreach ($dll in @(Get-ChildItem -LiteralPath (Split-Path $Executable) -Filter '*.dll' -File)) {
    Copy-Item -LiteralPath $dll.FullName -Destination (Join-Path $OutputDirectory $dll.Name)
}
if ((Get-FileHash -LiteralPath $manifest.archivedExecutable).Hash -ne $manifest.executableSha256) {
    throw 'Archived executable differs from capture executable'
}
$variables = @('KYTY_PERF_CSV','KYTY_PERF_READBACK_CSV')
$saved = @{}
foreach ($variable in $variables) {
    $saved[$variable] = [Environment]::GetEnvironmentVariable($variable, 'Process')
    [Environment]::SetEnvironmentVariable($variable, $null, 'Process')
}
$process = $null
try {
    for ($run = 1; $run -le $Runs; $run++) {
        $directory = Join-Path $OutputDirectory ("run-{0:D2}" -f $run)
        New-Item -ItemType Directory -Path $directory | Out-Null
        $arguments = '--renderer-benchmark "' + $directory + '" ' + $Frames + ' ' + $Draws
        $process = Start-Process -FilePath $Executable -ArgumentList $arguments -WorkingDirectory (Split-Path $Executable) -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $directory 'stdout.txt') -RedirectStandardError (Join-Path $directory 'stderr.txt')
        # Keep the process handle open so Windows PowerShell can read ExitCode after exit.
        $null = $process.Handle
        Write-Output "Renderer benchmark run $run/$Runs PID=$($process.Id)"
        $deadline = (Get-Date).AddSeconds(180)
        while (!$process.WaitForExit(1000) -and (Get-Date) -lt $deadline) { }
        if (!$process.HasExited) {
            Stop-Process -Id $process.Id -Force
            throw 'Benchmark exceeded 180 seconds; capture incomplete'
        }
        if ($process.ExitCode -ne 0) { throw "Benchmark failed (exit=$($process.ExitCode)); see $directory" }
        & $Python (Join-Path $PSScriptRoot 'renderer.py') $directory --output (Join-Path $directory 'summary.json')
        if ($LASTEXITCODE -ne 0) { throw "Analyzer rejected $directory" }
        $workload = Get-Content -LiteralPath (Join-Path $directory 'workload.json') -Raw | ConvertFrom-Json
        $manifest.results += [ordered]@{ directory=$directory; exitCode=$process.ExitCode; workloadVersion=$workload.workload_version }
    }
    $manifest.complete = $true
} finally {
    if ($process -and !$process.HasExited) { Stop-Process -Id $process.Id -Force }
    foreach ($variable in $variables) { [Environment]::SetEnvironmentVariable($variable, $saved[$variable], 'Process') }
    $manifest.endedUtc = (Get-Date).ToUniversalTime().ToString('o')
    $manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'manifest.json') -Encoding UTF8
}
Write-Output "Renderer captures: $OutputDirectory"
& $Python (Join-Path $PSScriptRoot 'renderer.py') $OutputDirectory --batch --output (Join-Path $OutputDirectory 'batch-summary.json')
if ($LASTEXITCODE -ne 0) { throw 'Batch validation failed' }
