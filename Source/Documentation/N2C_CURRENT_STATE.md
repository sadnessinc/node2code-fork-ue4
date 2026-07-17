
# node2code current state — Version 195

## Product identity

The public product name and Unreal Editor `FriendlyName` are **node2code**. The plugin is developed and maintained by **sadnessinc**. It originated from the concept and early code of Node To Code, but the current implementation has been extensively rewritten and is no longer presented as a product fork.

Technical identifiers such as the `NodeToCode` module name, `NodeToCode.uplugin` filename, `N2C_` schemas, automation test names and saved-result folders remain unchanged for compatibility. Original source copyright notices are retained under Apache 2.0.

## Verified historical baseline

Version 189 was the last completed full automation PASS before the Version 190–195 correction sequence. Versions 190–194 exposed manual-import, contract-harness and Windows PowerShell 5.1 gaps. Version 195 is now the current production-verified release for the bounded importer surface described below.

## Version 195 changes

Version 195 began as the null-collection validator hotfix, but target-machine verification also corrected defects discovered after the previous candidate had not reached or completed the new runtime matrix:

- Windows PowerShell 5.1 collection cardinality, missing optional JSON fields, SHA-256 helper loading and failure diagnostics;
- UE4.27 `FString::Printf` compatibility and unattended package-save fallback;
- root-only `variables[]` patches, strict unknown variable-type rejection and missing pin-default target rejection;
- deterministic/reused graph nodes and edges, multiple FunctionResult reuse, FText reapply stability and `GetArrayItem` `Index` alias handling;
- collapsed graph fixtures, selected negative fixtures and canonical custom-thunk pin identities;
- distinct `VerifyFreshFirst`/`VerifyFreshSecond` filters, persisted state handling and semantic snapshot diagnostics/canonicalization.

The matrix remains **117 cases** with the same bounded totals and four-phase acceptance intent. Individual fixtures and verification rules were not byte-identical to the pre-verification candidate.

### Static validator hardening

- optional JSON properties are resolved through `Get-N2CJsonPropertyValue`;
- collection cardinality is normalized through `Get-N2CCollectionCount`;
- release metadata comes from `NodeToCode.uplugin` as the single source of truth;
- supported actions and emitted guards must remain mapped to contract cases;
- source packages reject generated caches and build artifacts.

### Exhaustive bounded import-contract matrix

`Source/Tests/Fixtures/N2C_IMPORT_CONTRACT_MATRIX_V1.json` contains 117 cases and is the authoritative runtime contract for the currently advertised importer surface:

- 38/38 supported action tokens;
- 63 node-constructor/alias families;
- 29/29 mapped importer guard codes;
- 36 negative no-mutation cases;
- variables, functions, graph operations, defaults, edges, delegates, macro/collapsed boundaries, SCS, interfaces, Struct/Enum/DataTable and subclass-specific Blueprints.

“Exhaustive” means exhaustive against the current importer support table and emitted guard set, not every arbitrary malformed JSON string or every possible UE pin pairing.

### Four-phase persistence proof

Each case runs through:

1. Apply + compile + save;
2. fresh-process load + compile + exact structural comparison;
3. identical reapply + semantic no-duplicate comparison;
4. second fresh-process load + compile + exact comparison.

Cleanup then removes generated contract assets and shared dependencies. Legacy ManualReplay and two-process disk restore run independently afterward.

### Snapshot comparison boundary

Exact fresh-process comparison remains strict for persisted structure. Semantic reapply comparison removes unstable editor GUIDs and also ignores inactive `default_value`, `default_object` and `default_text` fields on pins that already have links. It still checks node/pin inventory, pin types, link topology, unlinked defaults, variables, functions, graphs, components and interfaces.

Version 195 also normalizes comma-separated numeric tuple text in structural snapshots. The current implementation is broader than proven struct-pin usage and is recorded as a non-runtime verification limitation/TODO; it does not change imported Blueprint execution semantics.

## Current proof status

Version 195 is production-verified on the target UE4.27.2 project for its declared bounded importer scope:

- complete 11-stage gate: PASS;
- ContractMatrix: 117/117 in Apply, FreshFirst, Reapply and FreshSecond;
- cleanup: PASS;
- Legacy ManualReplay: 21/21;
- both deferred-restore processes: PASS;
- real Level Editor six-Blueprint import, fresh compile, duplicate-free second import and Project Export: PASS.

The attached automation bundle and project export were independently inspected after the run. Details and evidence boundaries are in `N2C_RELEASE_VERIFICATION_20260717.md` and `N2C_INDEPENDENT_RELEASE_AUDIT_20260717.md`.

## Project Export boundary

The verified Project Export contains 83 assets: 40 Blueprints, 39 Niagara systems, one enum and three structs. All six manual UI targets are present and structurally match the authored patch identities/edges. The archive is valid export and coverage evidence, but 12 unrelated project Blueprints are marked blocked by their coverage sidecars and therefore are not claimed as directly reimportable full clones. Two older `/Game/N2C_Test/Generated` assets were also captured and should be cleaned from the test project before distributing that project snapshot.

## Mandatory next-release P0

Version 195 UI acceptance exposed a tooling weakness: opening Import/Export through small toolbar buttons was slow and unreliable for external UI automation. The next feature release must add remappable Editor commands/hotkeys for both windows and make the release UI driver use those commands instead of coordinate clicks. This is tracked in `N2C_P0_EDITOR_COMMAND_SHORTCUTS.md` and is not claimed as implemented in Version 195.

## Deferred scope

Full AnimGraph fidelity, Behavior Tree asset/Blackboard/EQS authoring and full Niagara graph round-trip remain deferred and are not Version 195 release claims.
