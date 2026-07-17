[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ProjectRoot,
    [string]$EngineRoot,
    [ValidateSet('Development','DebugGame')][string]$Configuration = 'Development'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
$root = (Resolve-Path -LiteralPath $ProjectRoot).Path
if (-not $EngineRoot) { $EngineRoot = $env:UE427_ROOT }
if (-not $EngineRoot) { throw 'UE4.27 root is required. Pass -EngineRoot or set UE427_ROOT.' }
$engine = (Resolve-Path -LiteralPath $EngineRoot).Path
$uprojects = @(Get-ChildItem -LiteralPath $root -Filter *.uproject -File)
if ($uprojects.Count -ne 1) { throw "Expected exactly one .uproject under '$root'; found $($uprojects.Count)." }
$targetFiles = @(Get-ChildItem -LiteralPath (Join-Path $root 'Source') -Filter '*Editor.Target.cs' -File -ErrorAction SilentlyContinue | Select-Object -First 1)
$target = if ($targetFiles.Count -gt 0) { $targetFiles[0].BaseName -replace '\.Target$','' } else { $uprojects[0].BaseName + 'Editor' }
$build = Join-Path $engine 'Engine\Build\BatchFiles\Build.bat'
if (-not (Test-Path -LiteralPath $build -PathType Leaf)) { throw "UE4 build command not found: $build" }
$logDir = Join-Path $root 'Saved\NodeToCode\CodexLogs'; New-Item -ItemType Directory -Path $logDir -Force | Out-Null
$log = Join-Path $logDir ("Build_{0}_{1}.log" -f $target,(Get-Date -Format 'yyyyMMdd_HHmmss'))
$projectArgument = '-Project={0}' -f $uprojects[0].FullName
& $build $target Win64 $Configuration $projectArgument -WaitMutex -NoHotReloadFromIDE 2>&1 | Tee-Object -FilePath $log
$code = if ($null -eq $LASTEXITCODE) { 1 } else { [int]$LASTEXITCODE }
Write-Host "N2C_BUILD|result=$(if($code -eq 0){'PASS'}else{'FAIL'})|exit_code=$code|target=$target|log=$log"
exit $code
