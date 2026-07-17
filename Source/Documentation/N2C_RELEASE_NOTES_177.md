# NodeToCode Version 177 release notes

VersionName: `1.2.77-ue427-powershell-runtime-hotfix-candidate`

This source-only candidate fixes the PowerShell runtime and static-validator failures found in the real Version 176 Windows diagnostic bundle.

## Fixed

- replaced illegal `$pid = $process.Id` with `$processId = $process.Id`;
- replaced `$args` and `$matches` locals with non-automatic names;
- replaced literal `-NoOpenResult`/`-NoPause` source searches with AST parameter validation;
- added rejection of assignments to known read-only automatic variables;
- added final child stderr/stdout detail to friendly stage errors;
- retained the parser, exit-code, ZIP, pause and Explorer fixes from Versions 175–176.

No importer/exporter C++ behavior changed.

## Status

Cross-platform static source/package validation passes. Full UE4.27 build, main ManualReplay and both restore passes remain to be run with this corrected release; it is not marked verified.
