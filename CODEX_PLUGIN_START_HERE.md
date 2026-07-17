# Start here — node2code Version 195

Public product name: **node2code**. Internal `NodeToCode`/`N2C_` identifiers remain stable for compatibility.

Current release: **Version 195 / `1.2.95-ue427-powershell-null-collection-selftest-hotfix`**.

Read in this order:

1. `N2C_AI_JSON_IMPORT_AUTHORING_RULES.md` before authoring import JSON.
2. `Source/Documentation/N2C_CURRENT_STATE.md`.
3. `Source/Documentation/N2C_RELEASE_NOTES_195.md`.
4. `Source/Documentation/N2C_VERIFICATION_WORKFLOW.md`.
5. `Source/Tests/Fixtures/N2C_IMPORT_CONTRACT_MATRIX_V1.json`.
6. `Source/Documentation/N2C_LEGACY_CONTRACT_COVERAGE_V195.md`;
7. `Source/Documentation/N2C_RELEASE_VERIFICATION_20260717.md`;
8. `Source/Documentation/N2C_INDEPENDENT_RELEASE_AUDIT_20260717.md`;
9. `Source/Documentation/N2C_TODO.md`;
10. `Source/Documentation/N2C_P0_EDITOR_COMMAND_SHORTCUTS.md`.

Run the complete 11-stage gate:

```bat
Scripts\RUN_N2C_AUTOMATION_AND_PACK.cmd -KeepResultDirectory
```

Version 195 fixes the remaining Windows PowerShell 5.1 `PropertyNotFoundStrict` collection-count failure and retains a 117-case bounded matrix. Several fixtures, idempotency paths, phase filters and snapshot rules were corrected during target-machine verification; the coverage totals and four-phase acceptance intent remain 117 Apply/compile/save, fresh reopen, idempotent reapply, second fresh reopen and cleanup. The legacy 21-case suite and two-process disk restore then run as independent regressions.

`N2C_NEW_NODE_TEST_GATE`: no new supported action, node path, pin/default rule or guard may be added without matrix coverage and fresh-process proof.

Next-release P0: add remappable Import/Export Editor shortcuts and make real UI acceptance open both flows through those commands rather than toolbar coordinate clicks.
