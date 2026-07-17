# NodeToCode Version 190 — interactive Apply compile-parity hotfix

Version: **190**  
VersionName: **1.2.90-ue427-manual-apply-compile-parity-hotfix-candidate**

## Failure reproduced

Version 189 passed the complete command-line ladder on UE4.27.2: build, 21/21 main ManualReplay tests, both deferred-restore processes and packaging. The authoritative interactive project patch then failed while compiling `BP_N2C_FinalActor`.

The UE compiler reported three independent import defects:

1. `SpawnActorFromClass.SpawnTransform` was authored as an unconnected literal. UE4.27 expands the node into `BeginDeferredActorSpawnFromClass` and `FinishSpawningActor`; both receive `SpawnTransform` by reference and therefore require a real input link.
2. `UK2Node_CreateDelegate` was asked to validate immediately after allocation, before `OutputDelegate` had a typed connection. UE4.27 cleared `SelectedFunctionName`, which later produced `Create Event: missing a function/event name` and an unknown delegate output type.
3. The member default for `N2C_Rotator` was copied as raw JSON text. UE4.27 could not parse that string as the serialized Blueprint property default, so compile emitted `Can't parse default value`.

The same shutdown exposed a restore lifecycle defect: an `OnPreExit` retry attempted to replace the `.uasset` while the failing Editor process still held the package. The manifest remained queued, but the retry produced a misleading delete failure.

## Corrections

- `CreateDelegate` now keeps the requested function name after allocation and performs signature-aware stabilization only after delegate edges exist.
- Same-patch handler functions are covered: the test creates `N2C_P0_DelegateHandler`, binds it through `CreateDelegate`, compiles, saves, unloads and verifies it in a fresh UE process.
- `SpawnActorFromClass` now has a strict semantic guard: `SpawnTransform` must have an incoming data edge. Literal or unconnected transforms are rejected before target mutation with `spawn_transform_link_missing`.
- The final manual JSON uses `KismetMathLibrary.MakeTransform.ReturnValue -> SpawnActor.SpawnTransform` and no longer writes a literal transform on the spawn node.
- Struct member defaults are parsed into a temporary `FStructOnScope` with the exact UE4.27 `UScriptStruct::ImportText` path and re-exported canonically before mutation. Invalid text is rejected with `member_default_import_text_invalid`.
- Deferred rollback restore is startup-only. No restore copy is attempted from `OnPreExit` of the failing process.
- Static-audit inventory excludes `PACKAGE_MANIFEST.json` and validates it in a dedicated pass, avoiding an audit/manifest hash cycle while retaining complete membership and SHA-256 verification.
- Python and PowerShell static validation enforce all three contracts.

## Test coverage

The existing 21-case ManualReplay suite is retained, but two cases are strengthened:

- `ContextualEventGraph`: compile/save/reopen of linked `MakeTransform -> SpawnActorFromClass`, plus a negative unlinked-transform preflight rejection.
- `Delegates`: dispatcher prerequisite plus a handler function created in the same patch and bound through both legacy `Delegate` and canonical `OutputDelegate` source-pin forms.
- `RawByteDefaultReopenExport`: retains raw Byte coverage, adds Rotator default canonicalization through compile/save/fresh reopen/export, and rejects invalid struct default text before mutation.

## Proof status

Version 189 remains the latest complete automation PASS. Version 190 is a source-only candidate until the full UE4.27.2 ladder and the interactive final project patch both pass.
