param(
    [Parameter(Mandatory=$true)][string]$Game,
    [Parameter(Mandatory=$true)][string]$Scenario,
    [int]$DurationSeconds = 180,
    [string]$Executable = '',
    [string]$OutputDirectory = '',
    [switch]$DisableDiagnostics,
    # Calibration only: window screenshots named by elapsed seconds and the last presentation id, so
    # a scene (e.g. the helicopter cutscene) can be bounded by flip count instead of launch time.
    [int]$ScreenshotEverySeconds = 0,
    # Extra emulator variables for an A/B candidate, e.g. 'KYTY_LABEL_FLUSH_EVERY=0'.
    [string]$Environment = ''
)
$ErrorActionPreference = 'Stop'
if ($DurationSeconds -lt 1) { throw 'DurationSeconds must be positive' }
if ($ScreenshotEverySeconds -lt 0) { throw 'ScreenshotEverySeconds must not be negative' }
if ($ScreenshotEverySeconds -gt 0) {
    Add-Type -AssemblyName System.Drawing
    Add-Type -Namespace KytyCapture -Name Native -MemberDefinition @'
[System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]
public struct Rect { public int Left, Top, Right, Bottom; }
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern bool GetWindowRect(System.IntPtr window, out Rect rect);
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern bool SetProcessDPIAware();
'@
    # Without this, scaled displays return logical window coordinates and the copy is offset.
    $null = [KytyCapture.Native]::SetProcessDPIAware()
}
function Get-LastPresentation([string]$Csv) {
    if (!(Test-Path -LiteralPath $Csv)) { return 0 }
    $stream = [IO.File]::Open($Csv, 'Open', 'Read', 'ReadWrite')
    try {
        $length = [Math]::Min([long]65536, $stream.Length)
        $null = $stream.Seek(-$length, 'End')
        $buffer = New-Object byte[] $length
        $null = $stream.Read($buffer, 0, $length)
    } finally { $stream.Dispose() }
    $lines = [Text.Encoding]::ASCII.GetString($buffer) -split "`n" | Where-Object { $_ -match '^(present|sample|final),' }
    if (!$lines) { return 0 }
    return [long](($lines[-1] -split ',')[2])
}
function Save-WindowShot($Process, [string]$Path) {
    $rect = New-Object KytyCapture.Native+Rect
    if ($Process.MainWindowHandle -eq [IntPtr]::Zero -or !([KytyCapture.Native]::GetWindowRect($Process.MainWindowHandle, [ref]$rect))) { return }
    $width = $rect.Right - $rect.Left; $height = $rect.Bottom - $rect.Top
    if ($width -le 0 -or $height -le 0) { return }
    $bitmap = New-Object Drawing.Bitmap $width, $height
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    try {
        $graphics.CopyFromScreen($rect.Left, $rect.Top, 0, 0, $bitmap.Size)
        $bitmap.Save($Path, [Drawing.Imaging.ImageFormat]::Png)
    } finally { $graphics.Dispose(); $bitmap.Dispose() }
}
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
if (!$Executable) { $Executable = Join-Path $repo '_Build/windows/install/kyty_emulator.exe' }
$Executable = (Resolve-Path -LiteralPath $Executable).Path
$Game = (Resolve-Path -LiteralPath $Game).Path
if ($Game.Contains('"')) { throw 'Invalid quoted game path' }
if (!$OutputDirectory) {
    $OutputDirectory = Join-Path $repo ('_Build/windows/benchmarks/native-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Use a new output directory to preserve existing results' }
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path
$git = Get-Command git -ErrorAction SilentlyContinue
if (!$git) { $git = Get-Command 'C:/Program Files/Git/cmd/git.exe' }
$arguments = '--game "' + $Game + '" --gpu 0 --present-mode Immediate --bindless --redzone --tessellation --readback-linear-images true'
$variables = @('KYTY_PERF_CSV', 'KYTY_PERF_READBACK_CSV', 'KYTY_IGNORE_WRITTEN_OVERLAP', 'KYTY_LABELS_AFTER_GPU', 'KYTY_DEFER_RELEASE_MEM_INTERRUPTS')
$extra = [ordered]@{}
foreach ($pair in ($Environment -split ';' | Where-Object { $_ })) {
    $name, $value = $pair -split '=', 2
    if ($name -notmatch '^KYTY_[A-Z0-9_]+$' -or $null -eq $value) { throw "Invalid -Environment entry: $pair" }
    $extra[$name] = $value
}
$variables += @($extra.Keys | Where-Object { $variables -notcontains $_ })
$saved = @{}
foreach ($variable in $variables) { $saved[$variable] = [Environment]::GetEnvironmentVariable($variable, 'Process') }
try {
    [Environment]::SetEnvironmentVariable('KYTY_PERF_CSV', $(if ($DisableDiagnostics) { $null } else { Join-Path $OutputDirectory 'renderer.csv' }), 'Process')
    [Environment]::SetEnvironmentVariable('KYTY_PERF_READBACK_CSV', $(if ($DisableDiagnostics) { $null } else { Join-Path $OutputDirectory 'readbacks.csv' }), 'Process')
    [Environment]::SetEnvironmentVariable('KYTY_IGNORE_WRITTEN_OVERLAP', '1', 'Process')
    [Environment]::SetEnvironmentVariable('KYTY_LABELS_AFTER_GPU', '1', 'Process')
    # The previous experimental PR remains disabled in the diagnostic reference.
    [Environment]::SetEnvironmentVariable('KYTY_DEFER_RELEASE_MEM_INTERRUPTS', '0', 'Process')
    foreach ($name in $extra.Keys) { [Environment]::SetEnvironmentVariable($name, $extra[$name], 'Process') }
    $manifest = [ordered]@{
        schema = 1; scenario = $Scenario; startedUtc = (Get-Date).ToUniversalTime().ToString('o')
        executable = $Executable; executableSha256 = (Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash
        game = $Game; arguments = $arguments; durationSeconds = $DurationSeconds
        gitHead = (& $git.Source -C $repo rev-parse HEAD); gitStatus = @(& $git.Source -C $repo status --porcelain)
        submodules = @(& $git.Source -C $repo submodule status)
        diagnostics = !$DisableDiagnostics; screenshotEverySeconds = $ScreenshotEverySeconds; environment = @{}
    }
    foreach ($variable in $variables) { $manifest.environment[$variable] = [Environment]::GetEnvironmentVariable($variable, 'Process') }
    # Preserve the exact dirty-source patch and build configuration used for this capture.
    $patchPath = Join-Path $OutputDirectory 'source.patch'
    & $git.Source -C $repo diff HEAD "--output=$patchPath"
    if ($LASTEXITCODE -ne 0) { throw 'Could not preserve source patch' }
    foreach ($relative in @(& $git.Source -C $repo ls-files --others --exclude-standard)) {
        $destination = Join-Path $OutputDirectory (Join-Path 'untracked-sources' $relative)
        New-Item -ItemType Directory -Path (Split-Path $destination) -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $repo $relative) -Destination $destination
    }
    Copy-Item -LiteralPath (Join-Path $repo '_Build/windows/CMakeCache.txt') -Destination (Join-Path $OutputDirectory 'CMakeCache.txt')
    $nvidia = Get-Command nvidia-smi -ErrorAction SilentlyContinue
    if ($nvidia) { $manifest.gpu = @(& $nvidia.Source --query-gpu=name,driver_version,pstate,power.draw,clocks.current.graphics,temperature.gpu,utilization.gpu,memory.used --format=csv,noheader) }
    $process = Start-Process -FilePath $Executable -ArgumentList $arguments -WorkingDirectory (Split-Path $Executable) -RedirectStandardOutput (Join-Path $OutputDirectory 'stdout.txt') -RedirectStandardError (Join-Path $OutputDirectory 'stderr.txt') -PassThru
    $manifest.pid = $process.Id
    $manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'manifest.json') -Encoding UTF8
    Write-Output "Capture PID=$($process.Id), output=$OutputDirectory"
    $started = Get-Date
    $deadline = $started.AddSeconds($DurationSeconds)
    $nextShot = $started.AddSeconds([Math]::Max($ScreenshotEverySeconds, 1))
    if ($ScreenshotEverySeconds -gt 0) { New-Item -ItemType Directory -Path (Join-Path $OutputDirectory 'screenshots') | Out-Null }
    while ((Get-Date) -lt $deadline -and !$process.HasExited) {
        Start-Sleep -Seconds 1
        $process.Refresh()
        if (!$process.HasExited -and $ScreenshotEverySeconds -gt 0 -and (Get-Date) -ge $nextShot) {
            $nextShot = $nextShot.AddSeconds($ScreenshotEverySeconds)
            $elapsed = [int]((Get-Date) - $started).TotalSeconds
            $presentation = Get-LastPresentation (Join-Path $OutputDirectory 'renderer.csv')
            # The screen copy includes anything overlapping the window; keep the emulator in front.
            try { Save-WindowShot $process (Join-Path $OutputDirectory ('screenshots/t{0:D4}s-p{1:D6}.png' -f $elapsed, $presentation)) }
            catch { Write-Warning "Screenshot failed: $_" }
        }
        if (!$process.HasExited) {
            [pscustomobject]@{ utc=(Get-Date).ToUniversalTime().ToString('o'); cpuSeconds=$process.TotalProcessorTime.TotalSeconds; workingSet=$process.WorkingSet64; privateBytes=$process.PrivateMemorySize64; windowTitle=$process.MainWindowTitle } | Export-Csv -LiteralPath (Join-Path $OutputDirectory 'process.csv') -NoTypeInformation -Append
        }
    }
    $manifest.endedUtc = (Get-Date).ToUniversalTime().ToString('o')
    $manifest.exited = $process.HasExited
    if ($process.HasExited) { $manifest.exitCode = $process.ExitCode }
    else { Stop-Process -Id $process.Id -Force; $manifest.forcedStop = $true }
    $manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'manifest.json') -Encoding UTF8
} finally {
    foreach ($variable in $variables) { [Environment]::SetEnvironmentVariable($variable, $saved[$variable], 'Process') }
}
