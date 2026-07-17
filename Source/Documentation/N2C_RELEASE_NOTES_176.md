# NodeToCode Version 176 release notes

VersionName: `1.2.76-ue427-powershell-parser-hotfix-candidate`

This source-only candidate fixes the Windows PowerShell parser failure discovered when launching Version 175. The runner could not start because a double-quoted string used `$DisplayName:` without delimiting the variable name.

## Fixed

- changed `Write-Host "$DisplayName: $resultText"` to `Write-Host "${DisplayName}: $resultText"`;
- added a native `System.Management.Automation.Language.Parser` pass for every `.ps1` file to the static validator;
- retained the user-friendly Success/Error output, stopped-stage/log/result paths, key pause and automatic Explorer opening from Version 175.

No importer/exporter C++ behavior changed.

## Status

Static source/package validation passes. Full UE4.27 build, main ManualReplay and both restore passes remain to be run with this corrected release; it is not marked verified.
