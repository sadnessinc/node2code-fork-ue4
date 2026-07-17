[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ProjectRoot,
    [string]$EngineRoot
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
$root = (Resolve-Path -LiteralPath $ProjectRoot).Path
if (-not $EngineRoot) { $EngineRoot = $env:UE427_ROOT }
if (-not $EngineRoot) { throw 'UE4.27 root is required. Pass -EngineRoot or set UE427_ROOT.' }
$engine = (Resolve-Path -LiteralPath $EngineRoot).Path
$uprojects = @(Get-ChildItem -LiteralPath $root -Filter *.uproject -File)
if ($uprojects.Count -ne 1) { throw "Expected exactly one .uproject under '$root'." }
$editor = Join-Path $engine 'Engine\Binaries\Win64\UE4Editor-Cmd.exe'
if (-not (Test-Path -LiteralPath $editor -PathType Leaf)) { throw "UE4Editor-Cmd not found: $editor" }
$logDir = Join-Path $root 'Saved\NodeToCode\CodexLogs'
New-Item -ItemType Directory -Path $logDir -Force | Out-Null
$log = Join-Path $logDir ("ProjectExport_{0}.log" -f (Get-Date -Format 'yyyyMMdd_HHmmss'))
$startedUtc = (Get-Date).ToUniversalTime()

& $editor $uprojects[0].FullName -unattended -nop4 -NoSplash -NullRHI -NoSound -stdout -FullStdOutLogOutput '-ExecCmds=Automation RunTests NodeToCode.Verification.ProjectExport.Full; Quit' '-TestExit=Automation Test Queue Empty' "-AbsLog=$log"
$processCode = if ($null -eq $LASTEXITCODE) { 1 } else { [int]$LASTEXITCODE }

$reason = 'ok'
if (-not (Test-Path -LiteralPath $log -PathType Leaf)) {
    $reason = 'log_missing'
}
else {
    $text = Get-Content -LiteralPath $log -Raw
    $fatal = $text -match '(?im)\bFatal error:|Assertion failed:|Unhandled Exception:|Automation Test Failed|Result=\{Fail'
    $completed = $text.Contains('Automation Test Queue Empty') -or $text.Contains('Automation Test Queue is empty')
    if ($processCode -ne 0) { $reason = "exit_code_$processCode" }
    elseif ($fatal) { $reason = 'failure_marker' }
    elseif (-not $completed) { $reason = 'queue_completion_marker_missing' }
}

$exportRoot = Join-Path $root 'Saved\NodeToCode\ProjectExports'
$archive = $null
if (Test-Path -LiteralPath $exportRoot -PathType Container) {
    $archive = Get-ChildItem -LiteralPath $exportRoot -Filter 'N2C_Project_*.zip' -File -ErrorAction SilentlyContinue |
        Where-Object LastWriteTimeUtc -ge $startedUtc.AddSeconds(-2) |
        Sort-Object LastWriteTimeUtc -Descending |
        Select-Object -First 1
}
if ($reason -eq 'ok' -and $null -eq $archive) { $reason = 'fresh_export_archive_missing' }

$passed = $reason -eq 'ok'
$archivePath = if ($null -eq $archive) { '' } else { $archive.FullName }
Write-Host "N2C_PROJECT_EXPORT|result=$(if($passed){'PASS'}else{'FAIL'})|exit_code=$processCode|reason=$reason|archive=$archivePath|log=$log"
exit $(if($passed){0}else{1})
