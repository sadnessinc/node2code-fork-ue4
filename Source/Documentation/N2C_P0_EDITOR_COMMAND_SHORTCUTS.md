# P0 — Editor command shortcuts for Import and Export

Status: **mandatory P0 for the next node2code release after Version 195**.

Version 195 remains production-verified without this feature. This item is a next-release UX and automation requirement, not a retroactive Version 195 capability claim.

## Problem

The Version 195 real-UI acceptance took materially longer because external UI automation had to locate and click small Level Editor toolbar buttons. Coordinate and image-based clicks are sensitive to window position, DPI, toolbar layout, focus and delayed Slate initialization. A missed click can leave the driver waiting on the wrong window even when the importer itself is correct.

## Required commands

Register two real UE4 Editor commands in the existing technical `NodeToCode` command context:

- `NodeToCode.OpenImportWindow` — opens or focuses the same Import flow used by the Level Editor toolbar command;
- `NodeToCode.OpenExportWindow` — opens or focuses the same Export window used by the Level Editor toolbar command.

The shortcuts must execute the existing command handlers. They must not duplicate Import/Export business logic or create a second code path.

## Shortcut behavior

- Both commands must be visible and remappable in **Editor Preferences → Keyboard Shortcuts → node2code**.
- Both commands must have documented default chords after a UE4.27.2 conflict audit.
- Preferred candidate defaults are:
  - Import: `Ctrl+Alt+I`;
  - Export: `Ctrl+Alt+E`.
- If either chord conflicts with an existing UE4.27.2 command, select stable non-conflicting defaults and update this document, README and UI tests in the same change.
- Repeated invocation must focus the existing NodeToCode window/dialog rather than opening duplicate windows.
- Commands must be disabled or return a clear controlled result while another Import/Export operation, rollback or queued restore action makes opening the window unsafe.
- The toolbar buttons and keyboard shortcuts must remain mapped to the same `FUICommandInfo`/`FExecuteAction` behavior.

## Mandatory UI automation use

The release UI driver must use the registered keyboard commands as its primary entry point.

A coordinate click on the toolbar may be retained only as a diagnostic/manual fallback. A release UI test must not report PASS when it reached Import or Export solely through an unchecked coordinate click.

For every shortcut invocation, the driver must verify:

1. the target `UE4Editor.exe` process and Level Editor window are active;
2. the expected node2code Import or Export window/dialog appears before timeout;
3. the window title or stable automation identity matches the requested command;
4. no duplicate node2code window was created;
5. the Editor log contains the expected command/open marker;
6. focus returns to the correct Editor window after the flow completes.

## Required tests

P0 is complete only when all of the following pass on UE4.27.2:

1. build and static command-registration validation;
2. Import shortcut opens the Import flow from the Level Editor;
3. Export shortcut opens the Export window from the Level Editor;
4. both commands appear in Editor Preferences and can be remapped;
5. a remapped chord persists after Editor restart;
6. repeated shortcut presses do not create duplicate windows;
7. commands fail safely during an active operation/restore state;
8. the real UI smoke imports the manual JSON without any toolbar coordinate click;
9. the real UI smoke opens Project Export without any toolbar coordinate click;
10. screenshots, window metadata, command markers and timeouts are retained in the automation evidence bundle.

## Release gate

This is a **P0 release blocker for the next feature release**. Do not mark the next release production-ready until both shortcuts and their real-UI automation coverage pass.
