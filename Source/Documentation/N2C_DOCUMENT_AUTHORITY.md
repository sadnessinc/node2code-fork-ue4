
# N2C document authority — Version 195

## Naming authority

- Public product name and Unreal Editor `FriendlyName`: **node2code**.
- Current author/maintainer: **sadnessinc**.
- Internal compatibility identifiers remain `NodeToCode` and `N2C_` where already established.
- Historical origin and original-author attribution belong in documentation/legal notices, not current Unreal Editor product metadata.

Authoritative current documents:

1. `N2C_AI_JSON_IMPORT_AUTHORING_RULES.md`
2. `N2C_CURRENT_STATE.md`
3. `N2C_RELEASE_NOTES_195.md`
4. `N2C_VERIFICATION_WORKFLOW.md`
5. `N2C_IMPORT_CONTRACT_MATRIX_V1.json`
6. `N2C_NODE_TEST_REQUIREMENTS_V1.json`
7. `N2C_LEGACY_CONTRACT_COVERAGE_V195.md`
8. `N2C_RELEASE_VERIFICATION_20260717.md`
9. `N2C_INDEPENDENT_RELEASE_AUDIT_20260717.md`
10. `N2C_TODO.md`
11. `N2C_P0_EDITOR_COMMAND_SHORTCUTS.md`

Runtime authority order:

1. target-machine UE4.27.2 build;
2. all 117 cases across Apply/FreshFirst/Reapply/FreshSecond;
3. cleanup;
4. Legacy ManualReplay;
5. two-process disk restore;
6. interactive manual project Apply/restart/reapply;
7. project export verification;
8. exact archive SHA-256 evidence.

Static audit and package hashes are necessary but are not substitutes for UE runtime proof. A project export is capture/coverage evidence and must not be represented as directly reimportable when its sidecar reports `direct_import_supported=false` or blockers. A later handoff/repack is not byte-identical to a gate package unless their hashes match.
