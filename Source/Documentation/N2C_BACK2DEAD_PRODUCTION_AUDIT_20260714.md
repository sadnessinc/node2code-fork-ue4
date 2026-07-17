> **Historical record.** This file describes an earlier release or evidence run. It is not authoritative for Version 173. See `N2C_CURRENT_STATE.md` and `N2C_DOCUMENT_AUTHORITY.md`.

# node2code Back2Dead production audit — 2026-07-14

Engine: Unreal Engine 4.27.2  
Project: `<PROJECT_ROOT>/<PROJECT_NAME>.uproject`  Descriptor: Version 171, `1.2.71-ue427-p0-production-verified`

## Verdict

- P0 automated verification: **PASS**.
- Real Back2Dead fresh export audit: **PASS**.
- Production deployment: **COMPLETE**.
- Overall P0 production acceptance: **PASS**.

No manual Editor or PIE verification is claimed.

## Build and automation

The actual `back2dead_ue4Editor Win64 Development` target built with UE4.27.2 and Visual Studio 2019/v142, exit 0. The final consolidated `NodeToCode.Verification.P0` run completed 85 tests with 85 passed and 0 failed. Standalone `NodeToCode.Verification.P0Smoke` also passed.

## Export integrity

The generic headless hook reported PASS and the surrounding process exited 0:

```text
supported assets: 303
Blueprint: 156
Niagara System: 82
Enum: 37
Struct: 28
skipped: 0
archive entries: 1,362
Blueprint sidecars: 156/156
```

Final archive: `Saved/NodeToCode/ProjectExports/N2C_Project_20260714_131750.zip`. All 1,362 entries were fully read and parsed as JSON. SHA-256: `21CC5E5F68870BC8C87E53E4CACB9ED6ECE77DF3069D3B66A9FDC947917FB114`.

## Corrected production coverage

```text
coverage records: 21,358
verified: 21,207
cosmetic_only: 102
guarded: 8
unsupported: 41
claimed-P0 records: 21,207
claimed-P0 nonverified: 0
claimed-P0 missing metadata: 0
unknown promoted: 0
```

Key P0 totals are all verified: InputAction 13, InputKey 10, InputAxisEvent 10, InputAxisKeyEvent 2, CreateWidget 14, MakeArray 6 and GetDataTableRow 26. Every existing fixture-proven core, delegate, struct, enum, macro and interface Message family in the acceptance matrix is also zero-nonverified.

## Function boundaries

```text
FunctionEntry: 709/709 verified
FunctionResult: 1,341/1,341 verified
records: 2,050
unique fingerprints: 1,781
unmatched: 0
A_Room: 381/381 verified
AC_Attack: 361/361 verified
```

Fingerprints cover Blueprint/graph identity, Entry/Result role, function flags and purity, result count, ordered pin identity/type/container/subtype/reference/const/defaults and reciprocal linked endpoint topology. Representative impure, pure internal-exec and multiple-result fixtures pass fresh-process persistence; every production record maps to its exact fingerprint evidence.

## Graph boundaries

```text
K2Node_Tunnel: 38/38 verified
K2Node_Composite: 1/1 verified
durable record fingerprints: 39
unmatched: 0
A_Room Tunnel: 8/8
AC_Attack Tunnel: 2/2
AC_Buffs Tunnel: 2/2
Enemy Tunnel: 2/2
MainSpawner: Composite 1/1 plus its two inner Tunnel boundaries
```

The prior Tunnel total was 36. The final exporter intentionally adds the collapsed graph's distinct entry and exit tunnels, producing the correct final total of 38. The 13 `P0GraphBoundaries` fixtures cover macro/collapsed creation, multiple pins, Composite persistence, production fingerprints and fail-before-mutation guards.

## `BACK2DEAD_CURRENT_WORK_SCOPE_V1`

The named scope contains 29 present assets and 11,386 ordinary Blueprint records. Runtime nonverified, fixture-proven nonverified and unknown promoted are all zero. `A_Room` has 3,154/3,154 runtime records verified, including 381 FunctionEntry/Result records and 8 Tunnel records. Combat components, every current `BFL_*` asset, AIController/Enemy/MainSpawner code and BTTask/BTService/BTDecorator Blueprint code all pass per asset; exact rows are in `current_work_scope_acceptance_matrix.csv`.

Behavior Tree, Blackboard and EQS asset editing is not implemented. Those asset editors are distinct from the supported ordinary Blueprint code in AI task/service/decorator classes.

## Remaining deferred fail-closed records

| Class/family | Count |
|---|---:|
| `K2Node_CallMaterialParameterCollectionFunction` | 16 |
| Animation Blueprint nodes | 21 |
| `K2Node_AddComponent` | 5 |
| `K2Node_EaseFunction` | 3 |
| `K2Node_Timeline` | 3 |
| `K2Node_InputTouch` | 1 |

These 49 records remain guarded/unsupported and were not promoted. InputTouch is P5, intentionally deferred, and is not a Back2Dead production blocker.

## Gameplay-asset warning separation

The export reproduced 46 pre-existing unknown-structure serialization lines in four gameplay classes: `AC_RunRouteDirector`, `WD_CurrentRoom`, `WD_Map` and `BFL_RoomFunctions`. They are preserved verbatim in the external audit note, were not repaired or mutated, and do not invalidate node2code P0 acceptance.

## Generated audit files

The final external audit is `Saved/NodeToCode/ProductionCloseout/20260714_105439/audit_amended_final`. It contains the coverage matrices, all five function-boundary CSVs, all five graph-boundary CSVs and all five current-work-scope CSVs. The audit reads one coverage sidecar at a time from the ZIP.

`Plugins/node2code/Scripts/Codex` is optional generic tooling. Direct UE4 build, automation and export commands are authoritative.
