> **Historical record.** This file describes an earlier release or evidence run. It is not authoritative for Version 173. See `N2C_CURRENT_STATE.md` and `N2C_DOCUMENT_AUTHORITY.md`.

# NodeToCode Version 172 — UE4.27 Editor Import Hotfix

Version: `1.2.72-ue427-editor-import-hotfix`

Status: source hotfix candidate. It has passed static source/schema validation in this package, but still requires the supplied UE4.27 Editor apply/compile/save/reopen tests before a final production-ready claim.

## Why this release exists

Manual Editor acceptance of Version 171 exposed a safety failure: a compile-error patch could leave partial graph mutations in memory while the dialog claimed rollback. The same run also exposed weak field/pin validation for `SetFieldsInStruct`, linked `GetDataTableRow`, edge creation, and a coverage/importer alias mismatch.

## Hotfixes

### P0 safety
- Failed graph actions stop immediately; later actions are not applied.
- Missing node IDs, missing pins and rejected UE connections are hard errors in strict mode.
- Node-construction failures abort before edge application.
- `FScopedTransaction::Cancel()` is no longer used as rollback.
- A completed Editor transaction is undone on apply, compile or save failure.
- A structural snapshot hash verifies the post-Undo Blueprint against the pre-apply state.
- A pre-apply `.uasset` backup is created before mutation.
- If in-memory rollback cannot be verified, a disk restore is automatically queued in `Saved/NodeToCode/Backups/PendingRestore` for the next UE startup. The dialog tells the user not to save and to restart UE.
- The package is saved only after successful apply and compile.

### Struct import
- `SetFieldsInStruct` resolves User Defined Struct members by persistent GUID, exact internal name, friendly/display name, and generated UDS base name.
- Requested fields are validated before mutation.
- A node is never silently removed while edge application continues.
- Make/Break/SetFields member identities accept exported internal names and persistent GUIDs.

### DataTable import
- `Out Row`, `out_row` and `ReturnValue` resolve to the UE4.27 internal result pin.
- Linked `GetDataTableRow` requires an outgoing typed row-struct authority before mutation.
- Explicit `row_struct_path` is restored after UE4.27 connection callbacks.
- Runtime verification rejects linked DataTable nodes that remain wildcard or lose their row struct.

### Coverage and diagnostics
- Coverage recognizes the short importer aliases `ArrayGet`, `ArrayAdd`, `ArrayAddUnique`, `ArraySet`, `ArrayRemove`, `ArrayRemoveItem`, `ArrayContains`, `ArrayFind`, `ArrayLength`, `ArrayClear` and `Delay`.
- Coverage blockers no longer produce `Problems: none`.
- User dialogs show readable causes; raw `N2C_*` diagnostics remain in technical details/logs.
- The dialog distinguishes preflight rejection, verified rollback, queued disk fallback and unverified failure.

## Still P1
- Raw `Byte/uint8` member variable creation without an Enum subtype.
- Broader localization of every legacy diagnostic line.
- P5 InputTouch and excluded asset graph categories remain outside the verified scope.

## Release packaging
This release is source-only. `Binaries`, `Intermediate`, caches, logs and generated build output are intentionally excluded. Build the project Editor target against UE4.27.2 / VS2019 v142.
