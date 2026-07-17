
# node2code Version 195 release verification — 2026-07-17

## Automated release gate

The complete UE4.27.2 11-stage gate passed in one target-machine run:

- static validation and `testEditor` build: PASS;
- ContractMatrix Apply: 117/117;
- ContractMatrix FreshFirst: 117/117;
- ContractMatrix Reapply: 117/117;
- ContractMatrix FreshSecond: 117/117;
- cleanup: PASS;
- Legacy ManualReplay: 21/21;
- deferred restore first/second processes: PASS;
- source packaging: PASS;
- automation report validation: PASS;
- crash reports collected: 0.

Automation bundle:

- file: `N2C_Automation_20260717_044339.zip`;
- SHA-256: `f39e6cb52d1578268ddec3750eae598ba531274763fb0867ac0646476b2064e9`.

The package created inside that 11-stage run was reported by `Package.log` as SHA-256 `8918467a3b889d0d9de7ab1b65eb362d052bd3720a3c0bd0db97768ec04cf481`, size 763220 bytes. The later attached production handoff source ZIP had SHA-256 `1ef4a89408b9083a24be041bf88bef928ec7a5985f042c1bc48b4ea4cbd9287d`, size 763238 bytes. Therefore byte-for-byte identity between the Stage 11 package and the later handoff is not claimed. The post-run independent audit verified the later handoff's internal manifest and confirmed the runtime/source files remained valid; the documentation-corrected package has its own external checksum.

## Interactive project import/export

A minimal `N2C_PATCH_V1` UI smoke passed through the asset editor. The full six-Blueprint `N2C_PROJECT_PATCH_V1` was imported as one Level Editor project operation. Dry-run passed; all six assets applied, compiled and saved. A fresh process compiled the whitelist with zero errors, warnings or load failures.

A second fresh Editor process imported the identical project JSON again: 6/6 applies passed, existing nodes/links were reused, and no `NEW` marker appeared. Project Export then produced `N2C_Project_20260717_004046.zip`.

Manual JSON:

- SHA-256: `cee06266b30789996df4410038a4c395bc1e9d9f9f7e79a89ca255450e42cee7`;
- all three retained copies were byte-identical at handoff.

Project Export:

- SHA-256: `a6d1ce1c48f0306c119e3e634b9f6ec1436acfbd0d1b673a7dc67e4b130d2d9f`;
- 83/83 selected assets exported, 0 skipped;
- 40 Blueprints, 39 Niagara systems, one enum and three structs;
- all six manual targets present;
- all six manual-target coverage sidecars report `export_capture_safe=true`, `round_trip_verified=true`, zero runtime blockers and zero verification gaps;
- 122 deterministic authored node identities and 99 authored edges from the manual patch were found without missing identities, class mismatches or missing links;
- no duplicate variables, functions or node GUIDs were found in the six manual targets.

The whole-project archive is export/capture evidence, not a claim that all 83 assets are directly reimportable. Coverage reports classify 28/40 Blueprints safe and 12/40 blocked by known/deferred original-project content. Two older generated test Blueprints under `/Game/N2C_Test/Generated` were also present; they do not invalidate the six-Blueprint UI result but should be removed from a distributable test-project snapshot.

## Evidence limitations

- The source-only plugin archive does not contain the entire external UI-driving environment/screenshots; UI evidence is therefore supported by retained logs, export output and final asset state rather than being fully reproducible from the source ZIP alone.
- Build logs contain one UE4.27 non-blocking deprecation warning in the test-only UI smoke path (`FAssetEditorManager::OpenEditorForAsset`).
- Semantic reapply comparison intentionally ignores inactive defaults on linked pins as well as unstable editor GUIDs.
- Broad numeric tuple normalization in snapshots is tracked as a future verification-infrastructure refinement.

## Legacy disposition

`N2C_LEGACY_CONTRACT_COVERAGE_V195.md` maps each legacy test to ContractMatrix coverage and Keep/Update/Retire status. Version 195 retained and passed all 21 legacy cases for compatibility.
