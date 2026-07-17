# NodeToCode automation audit — 2026-07-16 09:27:15

## Verdict

**Genuine PASS for Version 185 / `1.2.85-ue427-headless-regression-harness-fix-candidate`.**

The process exit codes, orchestrator summary, persistent UE automation reports, per-case markers, restore markers and package result all agree.

- Bundle SHA-256: `062085eba5a6f5a882b7903b30cba4bfea2a61fe45a1c4c232d370a8d4cda619`
- Overall: `PASS`
- Recorded failures: `0`
- Crash/assert/ensure/access-violation markers: `0`
- Result directory retained: `False`

## Structured test reports

| Stage | Tests | Success | Success with warnings | Failed | Not run | In process | Errors | Warnings | Duration, s |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| MainManualReplay | 21 | 20 | 1 | 0 | 0 | 0 | 0 | 3 | 260.12 |
| RestoreFirstPass | 1 | 1 | 0 | 0 | 0 | 0 | 0 | 0 | 0.08 |
| RestoreSecondPass | 1 | 1 | 0 | 0 | 0 | 0 | 0 | 0 | 0.04 |

## Important regression evidence

- Composite canonical repeat:
  - first apply/compile/save/unload: PASS;
  - generated `K2Node_Composite_<n>` segment detected;
  - canonical dry run: PASS;
  - second apply/compile/save/unload: PASS;
  - Composite count before/after: `1 → 1`;
  - pending restore manifests before/after: `0 → 0`.
- Sandbox pin preflight:
  - invalid pin rejected before target mutation;
  - valid `Self → Knot → DynamicCast` dry run: PASS;
  - apply/compile/direct save/unload/reload: PASS;
  - persisted links: PASS;
  - pending restore manifests before/after: `0 → 0`.
- Deferred restore:
  - first process mutated the generated asset and created a real manifest;
  - second process verified backup bytes, consumed the manifest, compiled the restored asset, confirmed mutation removal and cleaned the fixture.

## Expected warnings

The main report contains exactly three test warnings. They belong to `MissingEnumReject`, which intentionally resolves a nonexistent enum and expects `enum_member_type_unresolved`.

Expected negative preflight FAIL markers found: `2`. They are paired with a successful test result and are not masked suite failures.

## Non-NodeToCode project warnings

`/Game/Test/test3` still produces:

- `Make Vector` backward-compatibility pin warnings: **21**
- unknown `MoveForward` Axis warning: **3**

These occur once per UE process and are unrelated to the generated NodeToCode fixtures. They should be fixed or the asset removed from the automation project to make the logs fully clean.

Magic Leap ZI/OpenVR/PIX startup messages are environment noise, not plugin failures.

## Final status

Version 185 is **verified by a real UE4.27.2 Editor build and the current mandatory automation scope**:

- build: PASS;
- ManualReplay: 21/21 successful;
- restore first pass: PASS;
- restore second pass: PASS;
- source package: PASS;
- hidden NodeToCode failures: none found.

This verifies the tested Blueprint importer, sandbox preflight, rollback, persistence and deferred disk restore paths. It does not expand support claims to deferred full AnimGraph, Blackboard/EQS, Behavior Tree asset editing, full Niagara graph round-trip, or project-level Struct/Enum import before those items are implemented and tested.
