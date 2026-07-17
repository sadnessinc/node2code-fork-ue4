# Version 184 automation failure audit — 2026-07-16 09:05:58

## Result

- PowerShell parser/static validation: PASS
- UE4 Editor build: PASS
- Main ManualReplay: 19/21 PASS
- Failed: `GraphBoundaries`, `SandboxPinPreflight`
- Restore first/second: correctly NOT RUN because main failed
- Source package: PASS

## Root cause

Both failures came from new Version 184 test code using the interactive `bCompileAndSave=true` importer path inside `UE4Editor-Cmd`. Node creation and connections succeeded, but `PromptForCheckoutAndSave` returned failure in headless automation.

`SandboxPinPreflight` also read the sandbox success marker from the Apply report, although the marker belongs to the separate `DryRunPatch` report.

## Production implications

The log does not prove a production Composite identity or Knot connection failure. It does prove that the new regressions were not written using the established automation-safe persistence path. Version 185 corrects the harness and adds pending-restore queue assertions.
