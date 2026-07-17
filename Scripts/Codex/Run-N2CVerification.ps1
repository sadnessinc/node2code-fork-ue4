[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ProjectRoot,
    [string]$EngineRoot,
    [string]$TestFilter = 'NodeToCode.ManualReplay'
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
$safeFilter = $TestFilter -replace '[^A-Za-z0-9_.-]','_'
$log = Join-Path $logDir ("Verify_{0}_{1}.log" -f $safeFilter,(Get-Date -Format 'yyyyMMdd_HHmmss'))

& $editor $uprojects[0].FullName -unattended -nop4 -NoSplash -NullRHI -NoSound -stdout -FullStdOutLogOutput "-ExecCmds=Automation RunTests $TestFilter; Quit" '-TestExit=Automation Test Queue Empty' "-AbsLog=$log"
$processCode = if ($null -eq $LASTEXITCODE) { 1 } else { [int]$LASTEXITCODE }

$reason = 'ok'
$requiredCases = @()
if ($TestFilter -eq 'NodeToCode.ManualReplay') {
    $manifestPath = Join-Path $root 'Plugins\node2code\Source\Tests\Fixtures\ManualReplay\N2C_MANUAL_REPLAY_MANIFEST_V1.json'
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
        throw "ManualReplay manifest not found: $manifestPath"
    }
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    $requiredCases = @($manifest.cases | Where-Object process -eq 'main' | ForEach-Object { ([string]$_.test -split '\.')[-1] })
    if ($requiredCases.Count -eq 0) { throw 'ManualReplay manifest contains no main-process cases.' }
}

if (-not (Test-Path -LiteralPath $log -PathType Leaf)) {
    $reason = 'log_missing'
}
else {
    $text = Get-Content -LiteralPath $log -Raw
    $fatal = $text -match '(?im)\bFatal error:|Assertion failed:|Unhandled Exception:|Automation Test Failed|Result=\{Fail|N2C_MANUAL_REPLAY_CASE\|[^\r\n]*\|result=FAIL'
    $completed = $text.Contains('Automation Test Queue Empty') -or $text.Contains('Automation Test Queue is empty')
    $missingCases = @($requiredCases | Where-Object { -not $text.Contains("N2C_MANUAL_REPLAY_CASE|case=$_|result=PASS") })
    if ($processCode -ne 0) { $reason = "exit_code_$processCode" }
    elseif ($fatal) { $reason = 'failure_marker' }
    elseif (-not $completed) { $reason = 'queue_completion_marker_missing' }
    elseif ($missingCases.Count -gt 0) { $reason = 'required_case_markers_missing:' + ($missingCases -join ',') }
}

$passed = $reason -eq 'ok'
$exitCode = if ($passed) { 0 } else { 1 }
Write-Host "N2C_VERIFY|result=$(if($passed){'PASS'}else{'FAIL'})|exit_code=$processCode|reason=$reason|filter=$TestFilter|log=$log"
exit $exitCode
