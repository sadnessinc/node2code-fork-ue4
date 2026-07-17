# Full technical review — Version 173

Release: **`1.2.73-ue427-full-review-refactor-candidate`**  
Target: **UE4.27.2 / Win64 / Visual Studio 2019 v142**  
Review scope: the complete supplied plugin archive, source, tests, fixtures, scripts and documentation.

## Evidence boundary

The review environment did not contain an installed UE4.27.2 Editor or the Windows VS2019/v142 toolchain. Therefore:

- C++ Editor build: **NOT RUN**;
- main ManualReplay automation: **NOT RUN**;
- deferred restore first pass: **NOT RUN**;
- deferred restore second pass: **NOT RUN**;
- static source/package validation: **PASS**.

The release remains a candidate. Static checks do not prove UE compilation, Blueprint compile behavior, save/reopen persistence or editor lifecycle safety.

## Input state

The supplied descriptor was Version 172 / `1.2.72-ue427-editor-import-hotfix`. The archive contained 159 files and about 139.5 MB unpacked, including generated `Binaries/` and `Intermediate/` output. The authoritative source state already contained the Version 172 safety line:

- pre-apply save and backup;
- a real Editor Undo path instead of relying on `Transaction.Cancel()`;
- structural rollback verification;
- queued disk restore and startup processing;
- strict missing node/pin/rejected-connection errors;
- User Defined Struct identity matching;
- literal and linked `GetDataTableRow`, `Out Row` alias and row-struct handling;
- Array/Delay alias parity;
- structured user diagnostics.

These behaviors were checked in source and retained. Their runtime success is not re-claimed for Version 173 without Editor evidence.

## Source audit

### UE4.27 compatibility review

- Reviewed module dependencies and added explicit `AIModule` for AIController/BT Blueprint test parents.
- Removed an empty include-path declaration and an empty preprocessor block.
- Checked all `K2Node_*.h` includes used by the plugin against the supplied UE4.27 BlueprintGraph reference tree: **106 checked, 0 missing**.
- No new UE5-only API was intentionally introduced.
- Test-only APIs and fault injection are guarded by `WITH_DEV_AUTOMATION_TESTS`.

This is a source-level compatibility review only; unresolved externals, private/NO_API linkage and exact compiler signatures still require the real UE4.27 build.

### Import/preflight

- Raw `Byte/uint8` and enum-backed Byte are now distinguished.
- Raw Byte may have an empty `PinSubCategoryObject`.
- Explicit enum-backed Byte still requires a resolved `UEnum`.
- Missing enum identity produces `enum_member_type_unresolved` before mutation.
- Root/member-variable actions participate in the same semantic dry-run path used by apply.
- Existing strict runtime node, pin and schema-connection failures remain hard errors.

### Rollback/restore

- The transaction is allowed to close before `GEditor->UndoTransaction()`; no active `Transaction.Cancel()` rollback call remains.
- The structural snapshot remains deterministic through sorted graph/node/pin/link records and semantic identities.
- A test-only post-mutation fault proves the actual failure-to-Undo branch rather than a preflight-only rejection.
- Deferred restore queue creation was factored into one helper shared by UI and automation.
- Restore automation now requires a dedicated first process and a dedicated new Editor process.

### Lifecycle limits found

The importer and editor integration remain very large translation units (about 8.5k and 11.5k lines). A broad architectural split was intentionally deferred because changing object/editor lifetime boundaries before a real verified baseline would add risk. Version 173 performs only localized, reviewable extraction.

## Test audit

The Version 172 archive had extensive `NodeToCode.Verification.*` tests, but it did not expose the required ManualReplay release gate or a dedicated restore second pass. Version 173 registers 22 release cases:

- 20 cases in the main `NodeToCode.ManualReplay` filter;
- one restore first-pass case;
- one restore second-pass case.

The main cases emit mandatory `N2C_MANUAL_REPLAY_CASE|case=...|result=PASS/FAIL` markers. The Windows orchestrator rejects a stage when any required marker is absent, even when the process exits zero or the queue merely becomes empty.

Positive round-trip cases reuse the existing real verifier: apply, Blueprint compile, save, unload, child Editor process, reopen, recompile, persisted contract/snapshot comparison and cleanup. Flow/arrays and contextual EventGraph fixtures were changed from presence-only/dangling-node checks to connected executable paths.

BTTask, BTService and BTDecorator tests prove only class-specific Blueprint-subclass creation and Blueprint graph persistence. They do not claim Behavior Tree asset editing, Blackboard or EQS support.

## Fixture audit

- All package JSON files parse.
- P0 manifest: 83 unique declared cases.
- ManualReplay manifest: 22 unique cases and exact registered-test membership.
- Added raw Byte positive fixture and missing-enum negative fixture.
- Patch fixtures are checked for schema/action shape, duplicate node IDs, unknown edge endpoints and required pin names.

Most legacy P0 cases are implemented inline in C++ and described by the historical manifest rather than stored as one JSON file per case. The manifest is retained as historical regression metadata, not current PASS evidence.

## Automation/scripts audit

`RUN_N2C_AUTOMATION_AND_PACK.cmd` calls one PowerShell orchestrator which:

1. validates the source package;
2. resolves exactly one `.uproject`;
3. resolves UE4.27 from `-EngineRoot` or `UE427_ROOT`;
4. builds the Editor target;
5. runs main ManualReplay;
6. runs restore first pass;
7. starts a new process for restore second pass;
8. captures logs/reports/recent crashes;
9. packages one diagnostic ZIP and SHA-256;
10. creates the source-only plugin ZIP.

Timeouts and process-tree termination are present. PASS requires exit zero, queue completion, no fatal/failure marker and every expected case marker. The scripts were statically reviewed in Linux but could not be executed because Windows PowerShell, UE4.27 and VS2019 were unavailable.

## Documentation audit

The current authority chain is `N2C_CURRENT_STATE.md` → this review → release notes → architecture/schema/automation/rollback/TODO documents. Dated reports, Version 172 notes, unsupported-node scans and P0 evidence are marked historical and cannot override current candidate status.

## Package cleanup

Removed generated or stale release content:

- `Binaries/`;
- `Intermediate/`;
- DLL/PDB/OBJ/LIB/EXP and generated response/resource output;
- stale static/package manifests from Version 172.

The final package contains only descriptor, source, resources, scripts, tests/fixtures, current documentation, historical reference documentation and license. A new `PACKAGE_MANIFEST.json` records per-file SHA-256 and excludes itself.

## Remaining known limits

- Real UE4.27.2 C++ compilation and Editor automation are still required.
- Save-failure and schema-rejected-connection fault injection do not yet have dedicated production-apply hooks.
- Duplicate-import prevention is not exhaustively tested for every graph family.
- Legacy `FN2CPinTypeCompatibility` compares object subtype strings and does not resolve class inheritance; production graph connections continue to use UE schema validation.
- Giant translation units remain a maintainability issue.
- Full Behavior Tree asset editing, Blackboard, EQS, broad AnimGraph fidelity, InputTouch P5 and full Niagara graph round-trip remain deferred/partial.
