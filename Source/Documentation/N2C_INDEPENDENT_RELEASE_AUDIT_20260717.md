
# Independent node2code Version 195 release audit — 2026-07-17

## Scope

Post-run inspection covered the attached source-only plugin, production automation bundle and UI Project Export. No C++, PowerShell, fixture or runtime behavior was changed by this documentation correction.

## Archive hashes and integrity

| Artifact | SHA-256 | Result |
|---|---|---|
| attached production source ZIP | `1ef4a89408b9083a24be041bf88bef928ec7a5985f042c1bc48b4ea4cbd9287d` | CRC PASS; 199 unique entries; one `node2code/` root; no binaries/build output |
| automation bundle | `f39e6cb52d1578268ddec3750eae598ba531274763fb0867ac0646476b2064e9` | CRC PASS; 57 unique entries |
| Project Export | `a6d1ce1c48f0306c119e3e634b9f6ec1436acfbd0d1b673a7dc67e4b130d2d9f` | CRC PASS; 279 unique entries |
| manual project JSON | `cee06266b30789996df4410038a4c395bc1e9d9f9f7e79a89ca255450e42cee7` | three retained copies were byte-identical |

The attached source manifest contained 198 rows and matched every packaged file by size/SHA-256. All source JSON files parsed successfully; both AI-guide copies and both packaged manual-JSON copies matched.

## Automation bundle findings

`N2C_Automation_Summary.json` reports Version 195 and overall PASS. Independent report parsing confirmed:

- Apply 117/117;
- FreshFirst 117/117;
- Reapply 117/117;
- FreshSecond 117/117;
- cleanup 1/1;
- Legacy ManualReplay 21/21;
- restore first/second 1/1 each;
- report errors 0, report warnings 0, crash reports 0.

No access violation, assertion, fatal error or Blueprint compile-error marker was found. Expected negative-test diagnostics and normal UE environment noise were present. The build contains one non-blocking UE4.27 deprecation warning in the test-only UI smoke path.

The package generated inside Stage 11 was SHA-256 `8918467a3b889d0d9de7ab1b65eb362d052bd3720a3c0bd0db97768ec04cf481`, whereas the later attached production handoff was `1ef4...`. Byte identity between those two ZIPs is therefore not established. This is an evidence-chain qualification, not evidence of runtime failure.

## Project Export findings

The export manifest is internally consistent:

- source assets 83, exported 83, skipped 0;
- 40 Blueprints, 39 Niagara systems, one enum and three structs;
- every asset path and JSON target is unique and present;
- all Blueprint function/graph split files match their root exports;
- every Blueprint has a coverage sidecar;
- aggregate coverage recomputes to 28 safe, 12 blocked, 0 warning Blueprints.

For the six manual UI targets:

- all six are present;
- all six report `export_capture_safe=true`, `round_trip_verified=true`, zero runtime blockers and zero verification gaps;
- all 122 deterministic authored node identities and 99 authored edges from the manual patch were found;
- no missing authored node, node-class mismatch, missing edge, duplicate variable, duplicate function or duplicate node GUID was found;
- all 22 authored member variables were present; the additional exported dispatcher variable is expected.

The six sidecars also report `direct_import_supported=false`/`normalizer_not_implemented`. This means the Project Export is valid capture/verification evidence, not a directly reimportable full project clone. The manual `N2C_PROJECT_PATCH_V1` remains the verified import artifact.

The whole-project export includes 12 unrelated blocked Blueprints with known/deferred source content and two older generated test assets under `/Game/N2C_Test/Generated`. Neither invalidates the six-target manual acceptance, but stale test assets should be removed before distributing the test project.

## Source/document findings corrected here

- removed inaccurate claims that the V195 matrix/verification implementation was unchanged;
- documented that semantic reapply ignores inactive linked-pin defaults in addition to GUIDs;
- recorded broad numeric tuple normalization as a verification limitation/TODO;
- added missing current verification/legacy/audit documents to the documentation index;
- recorded exact automation/export/manual hashes and the gate-package/final-handoff distinction;
- clarified that project export coverage does not equal direct full-clone import support;
- clarified project-level import wording and idempotent second-import acceptance.

## Verdict

**PROD READY for the declared bounded UE4.27.2 Blueprint import scope.**

This verdict is supported by the complete runtime gate, two fresh-process persistence phases, idempotent reapply, legacy and restore suites, real six-Blueprint UI import/reimport, and matching export evidence. It is not a claim of full arbitrary-project round trip: 12 unrelated exported Blueprints remain coverage-blocked, full AnimGraph/Behavior Tree asset/Blackboard/EQS authoring and full Niagara graph round trip remain deferred, and the Project Export itself is not a direct import artifact.

The documentation-corrected source package changes documentation/manifest data only. Runtime/code files are byte-identical to the attached production source archive and therefore do not require another UE runtime run solely for these documentation corrections.
