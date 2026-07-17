> **Historical record.** This file describes an earlier release or evidence run. It is not authoritative for Version 173. See `N2C_CURRENT_STATE.md` and `N2C_DOCUMENT_AUTHORITY.md`.

# N2C P0 automated verification report — production closeout 2026-07-14

Engine: UE4.27.2. Target: `back2dead_ue4Editor Win64 Development`. Descriptor: Version 171, `1.2.71-ue427-p0-production-verified`.

## Verdicts

- P0 automated verification: **PASS**.
- Real Back2Dead fresh export audit: **PASS**.
- Production deployment: **COMPLETE**.
- Overall P0 production acceptance: **PASS**.

No manual Editor or PIE verification is claimed.

## Final P0 closure

- FunctionEntry/FunctionResult: impure, pure internal-exec and multiple-result generated fixtures plus record-level production fingerprints.
- UE4.27 actual `K2Node_InputAction` and `K2Node_InputKey` graph classes with mapping/key/modifier/input flags.
- linked DataTable identity from row struct plus preserved DataTable/RowName/OutRow contracts.
- linked CreateWidget Class input plus concrete ReturnValue type.
- typed MakeArray element type, input count, defaults and links.
- exact macro/collapsed `K2Node_Tunnel` and collapsed-graph `K2Node_Composite` ownership, ordered pin, link and BoundGraph persistence.
- corrected claimed-P0 audit policy for obtainable-but-missing metadata.

## Automation evidence

- fixture manifest: 83/83 entries, all required families present;
- final full filter: `NodeToCode.Verification.P0`, 85 passed, 0 failed;
- root, coverage parity, coverage preflight, scanner contract and included smoke all passed;
- standalone `NodeToCode.Verification.P0Smoke`: PASS;
- positive cases completed new-process persistence and cleanup; rejects failed at expected Preflight codes without mutation.

## Production evidence

- 303/303 supported assets exported: 156 Blueprint, 82 Niagara System, 37 Enum, 28 Struct;
- skipped supported assets: 0; archive entries: 1,362; sidecars: 156/156;
- 21,358 total coverage records;
- 21,207 claimed-P0 records verified, 0 nonverified, 0 missing metadata;
- FunctionEntry 709/709; FunctionResult 1,341/1,341;
- 1,781 unique boundary fingerprints, 0 unmatched;
- `A_Room` boundaries 381/381 and `AC_Attack` 361/361;
- Tunnel 38/38, Composite 1/1, 39 graph-boundary fingerprints and 0 unmatched;
- `BACK2DEAD_CURRENT_WORK_SCOPE_V1`: 29 assets, 11,386 records, runtime nonverified 0;
- unknown node classes promoted: 0.

Detailed matrices are recorded in `N2C_BACK2DEAD_PRODUCTION_AUDIT_20260714.md` and the external timestamped production-closeout directory.

## Remaining boundary

Forty-nine strict project-wide records remain outside the named current-work gate: Material Parameter Collection call 16, Animation Blueprint nodes 21, AddComponent 5, EaseFunction 3, Timeline 3 and InputTouch 1. They remain guarded/unsupported and visible. InputTouch is P5 and is not a Back2Dead production blocker.

The 46 pre-existing unknown-structure serialization records belong to Back2Dead gameplay-asset maintenance and were not changed.
