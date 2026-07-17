[CmdletBinding()]
param(
    [string]$EngineRoot,
    [Parameter(Mandatory)][string]$Pattern,
    [string[]]$RelativeRoots = @('Engine\Source\Editor\BlueprintGraph','Engine\Source\Editor\Kismet','Engine\Source\Runtime')
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
if (-not $EngineRoot) { $EngineRoot = $env:UE427_ROOT }
if (-not $EngineRoot) { throw 'UE4.27 root is required. Pass -EngineRoot or set UE427_ROOT.' }
$engine = (Resolve-Path -LiteralPath $EngineRoot).Path
$rg = Get-Command rg -ErrorAction SilentlyContinue
$roots = @($RelativeRoots | ForEach-Object { Join-Path $engine $_ } | Where-Object { Test-Path -LiteralPath $_ -PathType Container })
if (-not $roots.Count) { throw "No UE4.27 source roots were found under '$engine'." }
if ($rg) {
    & $rg.Source -n --glob '*.h' --glob '*.cpp' -- $Pattern @roots
    $searchExitCode = if ($null -eq $LASTEXITCODE) { 1 } else { [int]$LASTEXITCODE }
    exit $searchExitCode
}
$sourceMatches = @(Get-ChildItem -LiteralPath $roots -Recurse -File -Include *.h,*.cpp | Select-String -Pattern $Pattern)
$sourceMatches | ForEach-Object { "{0}:{1}:{2}" -f $_.Path,$_.LineNumber,$_.Line.Trim() }
exit $(if($sourceMatches.Count -gt 0){0}else{1})
