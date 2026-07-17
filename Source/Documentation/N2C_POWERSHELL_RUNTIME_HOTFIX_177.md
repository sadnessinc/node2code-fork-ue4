# N2C PowerShell runtime hotfix — Version 177

## Observed failure

A real Windows run of Version 176 reached stage 1 and stopped with:

```text
Cannot overwrite variable PID because it is read-only or constant.
```

The runner contained:

```powershell
$pid = $process.Id
```

PowerShell variable names are case-insensitive. `$pid` and `$PID` are therefore the same built-in read-only automatic variable.

The same diagnostic bundle contained a second latent static-validator failure:

```text
Automation UX/process marker missing: -NoOpenResult
```

The validator searched the raw source for command-line spelling `-NoOpenResult`, while the real script declaration is:

```powershell
[switch]$NoOpenResult
```

That text check was invalid and would fail even when the parameter existed.

## Corrections

- changed `$pid` to `$processId`;
- changed the local automation argument array from `$args` to `$editorArguments`;
- changed `Search-UE427Source.ps1` local `$matches` to `$sourceMatches`;
- parsed the orchestrator AST and verified `NoOpenResult` and `NoPause` in its parameter block;
- removed the false raw-text checks for `-NoOpenResult` and `-NoPause`;
- added AST scanning for assignments to known read-only automatic variables;
- added cross-platform static scanning for the same class of assignments;
- made child-process failures include the last non-empty stderr/stdout line in the user-facing `Details:` field.

## Evidence boundary

This hotfix changes PowerShell automation only. It does not change importer/exporter C++ behavior and does not prove UE4.27 Editor automation passes.
