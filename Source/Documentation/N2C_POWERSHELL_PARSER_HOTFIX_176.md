# N2C PowerShell parser hotfix — Version 176

## Failure

Windows PowerShell rejected this Version 175 line before the script could execute:

```powershell
Write-Host "$DisplayName: $resultText"
```

Inside an expandable string, PowerShell interprets a colon immediately after a simple variable reference as scoped-variable syntax, similar to `$env:Path`. Because the reference was not valid, parsing stopped at line 150. No static validation, Build or automation stage ran.

## Correction

The variable is now explicitly delimited:

```powershell
Write-Host "${DisplayName}: $resultText"
```

## Regression guard

`Validate-N2CFiles.ps1` now invokes `System.Management.Automation.Language.Parser::ParseFile` for every `.ps1` under `Scripts/`. Any parser error reports the file, line, column and parser message and fails static validation before Build/Test orchestration.

## Evidence boundary

This is an automation-only hotfix. It does not change Blueprint importer/exporter C++ behavior and does not prove UE4.27 Editor tests pass.
