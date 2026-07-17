[CmdletBinding()]
param(
    [string]$ScriptsRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

try {
    $resolvedRoot = (Resolve-Path -LiteralPath $ScriptsRoot).Path
    $scriptFiles = @(Get-ChildItem -LiteralPath $resolvedRoot -Filter *.ps1 -File -Recurse | Sort-Object FullName)
    if ($scriptFiles.Count -eq 0) {
        throw 'No PowerShell scripts were found.'
    }

    foreach ($scriptFile in $scriptFiles) {
        $tokens = $null
        $parseErrors = $null
        [void][System.Management.Automation.Language.Parser]::ParseFile(
            $scriptFile.FullName,
            [ref]$tokens,
            [ref]$parseErrors
        )
        if (@($parseErrors).Count -gt 0) {
            $firstError = @($parseErrors)[0]
            Write-Host ('PowerShell parser error: {0}:{1}:{2}: {3}' -f `
                $scriptFile.FullName,
                $firstError.Extent.StartLineNumber,
                $firstError.Extent.StartColumnNumber,
                $firstError.Message) -ForegroundColor Red
            exit 2
        }
    }

    Write-Host ('N2C_POWERSHELL_PARSE|result=PASS|scripts={0}' -f $scriptFiles.Count)
    exit 0
}
catch {
    Write-Host ('PowerShell syntax preflight failed: {0}' -f $_.Exception.Message) -ForegroundColor Red
    exit 3
}
