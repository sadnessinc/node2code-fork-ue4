
# node2code release notes — Version 195

Version: `1.2.95-ue427-powershell-null-collection-selftest-hotfix`

## Product metadata refresh

The Unreal Editor product metadata now identifies the plugin as **node2code**, created and maintained by **sadnessinc**. The descriptor uses a concise user-facing description and links `CreatedByURL` to `https://github.com/sadnessinc`. Original-project attribution remains in documentation and legally required source notices, not in the Unreal Editor author metadata.

This branding/documentation refresh does not rename the internal `NodeToCode` C++ module, schema prefixes, automation test IDs or historical file names. Those identifiers remain stable for compatibility.

## Initial hotfix

Version 194 stopped at Stage 1 because its validator helper counted an explicitly bound null shape incorrectly under Windows PowerShell 5.1. Version 195 replaced `@($Value).Length` behavior with explicit null/scalar/enumerable handling and added collection-shape self-tests plus AST/static guards.

## Additional corrections found by the production gate

The first V195 candidate had not yet completed the new UE runtime matrix. Target-machine verification therefore produced additional fixes before the production handoff:

- UE4.27-compatible `FString::Printf` usage;
- local SHA-256 helper rather than relying on `Get-FileHash` autoloading;
- unattended-only direct package-save fallback in the verification path;
- root-only variable patches and strict unknown-type/missing-pin rejection;
- multiple FunctionResult reuse and stable repeated graph imports;
- FText reapply stability and linked-edge default preservation;
- narrow `GetArrayItem` `Index` alias resolution;
- collapsed graph fixture boundary metadata and mappings;
- corrected negative fixtures and canonical custom-thunk pin names;
- distinct first/second fresh-process test filters;
- richer snapshot diagnostics and semantic normalization.

The matrix still contains 117 cases and retains the same four-phase acceptance intent, but several fixtures and verification rules changed. The corrected cases include at least `collapsed_create`, `collapsed_replace`, `fn_missing_replace_reject`, `graph_incompatible_edge_reject` and `graph_unknown_node_reject`. Durable graph import scopes were retained where their semantic identity remained unchanged.

## Verification result

Production verification completed on 2026-07-17:

- full target-machine 11-stage gate: PASS;
- 117/117 Apply, FreshFirst, Reapply and FreshSecond;
- cleanup PASS;
- Legacy ManualReplay 21/21;
- both restore processes PASS;
- real UI smoke and six-Blueprint project import/reimport/export PASS.

Evidence and archive hashes are recorded in `N2C_RELEASE_VERIFICATION_20260717.md`; the independent post-run archive/export review is in `N2C_INDEPENDENT_RELEASE_AUDIT_20260717.md`.

## Non-blocking verification limitations

- semantic reapply snapshots ignore inactive defaults on linked pins in addition to unstable GUIDs;
- numeric comma-tuple snapshot normalization is broader than proven struct-pin use and should be narrowed in a later release;
- the UE4.27 build contains a non-blocking deprecation warning for test-only `FAssetEditorManager::OpenEditorForAsset`, relevant to a future engine upgrade rather than UE4.27 runtime correctness.

## Post-release P0 follow-up

The Version 195 UI run showed that coordinate-based toolbar interaction is too slow and fragile for the mandatory real-Editor gate. The next feature release has a P0 requirement to add remappable Import/Export Editor shortcuts and use them as the primary UI automation entry point. This follow-up is specified in `N2C_P0_EDITOR_COMMAND_SHORTCUTS.md`; it is not an implemented Version 195 capability.
