# node2code TODO — after Version 195

## Version 195 release gate — completed

- Complete 11-stage UE4.27.2 automation: PASS.
- ContractMatrix Apply/fresh/reapply/second-fresh: 117/117 in every phase; cleanup PASS.
- Legacy ManualReplay 21/21 and both restore processes: PASS.
- Real UI smoke and six-Blueprint Level Editor project import/reimport/export: PASS.
- Production label and final source package are permitted; retained evidence is listed in `N2C_RELEASE_VERIFICATION_20260717.md`.

## P0 — Import/Export Editor shortcuts

- Implement remappable Level Editor commands for opening/focusing the node2code Import and Export windows.
- Map toolbar buttons and keyboard shortcuts to the same command handlers; do not duplicate Import/Export logic.
- Preferred candidate chords are `Ctrl+Alt+I` for Import and `Ctrl+Alt+E` for Export, subject to a UE4.27.2 conflict audit.
- Expose both commands under Editor Preferences → Keyboard Shortcuts → node2code.
- Make the real UI release test use the keyboard commands as its primary entry point; unchecked coordinate clicks cannot produce PASS.
- Test remapping persistence, duplicate-window prevention, unsafe-state handling and open/focus behavior after Editor restart.
- Full specification and acceptance criteria: `N2C_P0_EDITOR_COMMAND_SHORTCUTS.md`.

## Next-release legacy reduction

- Keep all Version 195 legacy cases in this release evidence.
- For the next release gate, retain mandatory legacy tests only for unique historical/end-to-end coverage.
- Reduce Update rows to their unique assertions and retire full duplicates according to `N2C_LEGACY_CONTRACT_COVERAGE_V195.md`.

## Matrix maintenance

`N2C_NEW_NODE_TEST_GATE`:

- keep action coverage equal to the supported action set extracted from importer source;
- keep every runtime/preflight guard mapped;
- add positive semantic assertions, negative no-mutation coverage and both fresh-process phases for every new path;
- keep authored node IDs stable and unique;
- use `import_scope` for long-lived multi-action graph patches;
- add cleanup coverage for any new generated dependency type.

## Deferred product scope

- full Animation Blueprint/AnimGraph reconstruction;
- Behavior Tree asset graph, Blackboard and EQS authoring;
- project Input Action/Axis Mapping creation rather than node-only import;
- full Niagara production graph/module round-trip;
- remaining unsupported-node families found by future project scans.

These items are not production claims of Version 195.


## Verification-infrastructure follow-up

- restrict numeric comma-tuple snapshot normalization to proven struct-pin representations;
- add a regression proving String/Name/Text literal `1.0,2.00` remains distinct from `1,2`;
- retain explicit documentation that linked-pin defaults are inactive and omitted only from semantic reapply comparison;
- replace the test-only deprecated `FAssetEditorManager::OpenEditorForAsset` API when moving beyond UE4.27;
- store a reproducible UI driver or a complete external UI-evidence packet with future release bundles;
- clean stale `/Game/N2C_Test/Generated` assets before distributing the test project;
- keep separate checksums for gate package, final handoff, documentation-only repacks, automation bundle, export and manual JSON.
