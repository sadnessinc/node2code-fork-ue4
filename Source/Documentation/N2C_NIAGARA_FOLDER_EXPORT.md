# N2C Niagara / Mixed Folder Export

## Current status

The old dedicated toolbar folder-export button is not part of the active toolbar UI. The supported folder export flow is now the Content Browser mixed export command:

```text
Content Browser folder(s) -> right click -> Export N2C...
```

The command opens the same export picker used by `Export Project` and selected-asset export. Available checkboxes:

- Blueprint
- Niagara System
- Enum
- Struct

The exporter scans selected folders recursively, filters assets by the checked types, and writes one mixed N2C ZIP archive with `N2C_PROJECT_MANIFEST.json` plus typed folders such as `blueprints/`, `niagara/`, `enums/`, and `structs/`.

## Related toolbar actions

```text
Main toolbar -> Export Project
Export Options -> Export project file list
```

`Export Project` is for whole-project mixed export. `Export Options` is intentionally limited to the project file-list helper; it does not contain the old folder-export action.

## Reason

Folder export is now routed through Content Browser `Export N2C...` so the same picker, manifest format, asset filters and ZIP layout are used for Blueprint, Niagara System, Enum and Struct exports. This avoids maintaining a separate Niagara-only folder exporter.
