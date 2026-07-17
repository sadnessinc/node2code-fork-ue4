# NodeToCode Version 180 release notes

**Version:** 180  
**VersionName:** `1.2.80-ue427-powershell-progress-parser-hotfix-candidate`

## Fixed

- replaced unsafe `$total:` progress interpolation with format-operator construction;
- added a standalone pre-launch parser pass over every PowerShell script;
- added file/line/column parser diagnostics before the main runner starts;
- added static checks for unsafe `$variable:` interpolation;
- retained live `N/20` progress, elapsed time, failed-case summaries, result ZIP opening and pause behavior.

## Status

Source-only candidate. Static validation is expected to pass; Windows PowerShell 5.1 runtime and UE4.27 Editor automation must be rerun on the target machine.
