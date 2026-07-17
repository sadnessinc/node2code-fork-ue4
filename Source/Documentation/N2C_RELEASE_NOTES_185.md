# NodeToCode Version 185 release notes

Version: **185**  
VersionName: **1.2.85-ue427-headless-regression-harness-fix-candidate**

## Runtime evidence that led to this release

Version 184 built successfully on UE4.27.2. Main ManualReplay completed 21 tests: 19 passed and two failed. Restore stages were correctly skipped after main failure.

The failures were in newly added regression harness code, not in a proven production importer path:

- `GraphBoundaries`: the canonical repeat helper called `ApplyPatchToBlueprint(..., bCompileAndSave=true)` from `UE4Editor-Cmd`. That path uses interactive `PromptForCheckoutAndSave`, which returned failure in the commandlet. The test then rolled back and queued two unnecessary generated-fixture restores.
- `SandboxPinPreflight`: the negative dry-run rejection worked. The positive patch created and connected `Self.self -> Knot.InputPin -> Knot.OutputPin -> Cast.Object`, but the test again used interactive save and also expected the successful sandbox marker in the Apply report instead of the separate DryRun report.

## Changes

- Both regressions now use `ApplyPatchToBlueprint(..., false, false)`, explicit compile and direct `UPackage::SavePackage` through the established test helper.
- Composite canonical repeat now performs two full direct-save/unload/reload cycles and asserts one Composite after the second persisted reload.
- Sandbox pin preflight now performs an explicit positive `DryRunPatch`, real Apply, compile, save, unload/reload and reciprocal-link assertions for both Knot connections.
- Both tests compare pending restore manifest counts before and after, preventing automation-only test failures from silently polluting the production restore queue.

## Status

Static validation may pass in this environment, but Version 185 remains `candidate` until the full target-machine runner completes 21/21 main cases plus both restore processes.


## Automation restore hygiene

The runner quarantines only stale automation-fixture restore manifests under `/Game/N2C_Test/Generated/` before launching the main UE process; user asset restore entries are left untouched.
