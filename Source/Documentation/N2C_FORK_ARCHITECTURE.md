# Node2Code Back2Dead Fork Architecture

This document maps the Back2Dead / sadnessinc fork so future changes can be made without re-reading the whole codebase.

## 1. Plugin identity

Root folder:

```text
Plugins/node2code/
```

Descriptor:

```text
NodeToCode.uplugin
```

Current fork purpose:

- Keep the original NodeToCode translation UI available.
- Add AI-friendly Blueprint JSON export.
- Add strict JSON patch import.
- Add multi-Blueprint/project patch import.
- Add batch export tools.
- Add optional Blueprint Assist auto-format after import.

Attribution:

- Original plugin: NodeToCode by Protospatial / Nick McClure.
- Back2Dead fork: sadnessinc.

## 2. Main source files

### `Source/Private/Core/N2CEditorIntegration.cpp`

Largest integration file. Contains most fork behavior.

Responsibilities:

- Blueprint Editor toolbar buttons:
  - `Export N2C`
  - `Import N2C`
- Main Editor toolbar buttons:
  - `Export N2C` / export all Blueprints
  - `Import N2C` / project patch import
  - `Export Options` dropdown
- Content Browser context menu export for selected Blueprints.
- Export all Blueprints.
- Export selected Blueprints.
- Export Blueprints from chosen folders.
- Export project file list JSON.
- Import dialogs and summary formatting.
- Project patch dispatch.
- Blueprint Assist auto-format bridge and retry logic.

Important internal areas:

```text
N2CEditorIntegration_Private namespace
  - dialog/report helpers
  - batch export helpers
  - project file list helpers
  - folder picker widget/helpers
  - Blueprint Assist helpers
FN2CEditorIntegration public methods
  - Startup / Shutdown
  - toolbar registration
  - command handlers
```

When debugging UI behavior, start here.

### `Source/Private/Core/N2CAIExport.cpp`

AI-friendly Blueprint exporter.

Responsibilities:

- Builds `N2C_AI_EXPORT_V2` JSON.
- Exports Blueprint metadata.
- Exports member variables and variable flags.
- Exports function JSON.
- Exports function category, metadata and flags.
- Exports local variables.
- Exports nodes, pins, exec/data edges and linear flow.
- Uses safe UE4.27 metadata reads to avoid asserts on missing metadata.

Important notes:

- Export uses readable timestamp filenames.
- Function inputs/outputs are represented by pins on FunctionEntry/FunctionResult nodes.
- Function local variables are read from `UK2Node_FunctionEntry::LocalVariables`.
- Function flags may be read from `FunctionFlags` or UE4.27 `ExtraFlags` depending on availability.

### `Source/Private/Core/N2CPatchImporter.cpp`

Strict JSON patch importer.

Responsibilities:

- Parses `N2C_PATCH_V1`.
- Validates patches in dry-run mode.
- Adds/updates member variables.
- Adds/replaces function graphs.
- Replaces function body while preserving/adjusting signature.
- Adds local variables.
- Applies function categories.
- Applies variable options/flags.
- Applies function options/flags.
- Renames functions.
- Creates supported node types.
- Connects exec/data pins.
- Performs compile request after patch apply.

Supported stable nodes:

```text
FunctionEntry / Entry
FunctionResult / Return
Branch / IfThenElse
CallFunction
VariableGet
VariableSet
```

Important helper behavior:

- `ResolveFunction` checks function path, generated class, skeleton generated class and parent class.
- Skeleton generated class support is required when a patch creates a helper function and a later action calls it in the same import.
- Pin connection is intentionally loose/tolerant, but still reports warnings.

### `Source/Public/Core/N2CSettings.h`

Settings classes.

Current split:

- `UN2CEditorPreferences` — clean visible Editor Preferences page for this fork.
- `UN2CSettings` — original legacy NodeToCode settings used by existing translation/LLM internals.

Visible setting location:

```text
Editor Preferences -> Plugins -> Node2Code
```

Current visible setting:

```text
Blueprint Assistant Extension
  Auto Format Imported Functions
```

Do not register the legacy `UN2CSettings` object as visible editor preferences, otherwise the UI becomes polluted with LLM provider/model/API key/theme options.

### `Source/Private/NodeToCode.cpp`

Module startup/shutdown.

Responsibilities:

- Initializes logging.
- Initializes styles.
- Starts editor integration.
- Registers Editor Preferences page.
- Unregisters Editor Preferences page on shutdown.

Important rule:

```cpp
RegisterSettings(..., GetMutableDefault<UN2CEditorPreferences>())
```

Do not register `UN2CSettings` here for the Back2Dead fork UI.

### `Source/Private/Core/N2CToolbarCommand.cpp`

Toolbar command text.

Current important labels:

```text
Export N2C
Import N2C
```

### `Source/Public/Models/N2CStyle.cpp`

Slate style registration.

Current fork icons:

```text
Resources/export_arrow_green.png
Resources/import_arrow_red.png
```

Export icon: green outgoing arrow.

Import icon: red incoming arrow.

### `Source/NodeToCode.Build.cs`

Build rules.

Important fork behavior:

- Searches for BlueprintAssist source in:
  - project plugins
  - engine plugins editor folder
  - engine plugins folder
- Defines `N2C_WITH_BLUEPRINT_ASSIST=1` only when the source is found.
- Avoids requiring BlueprintAssist when not present.

### `NodeToCode.uplugin`

Descriptor.

Important fork behavior:

- Declares optional BlueprintAssist plugin dependency.
- This avoids UBT warning when NodeToCode is built with BlueprintAssist integration.

## 3. UI behavior map

### Blueprint Editor

| Button | Purpose | Output/Input |
|---|---|---|
| `Export N2C` | Export currently open Blueprint | `N2C_<BlueprintName>_<timestamp>.zip` |
| `Import N2C` | Import single Blueprint patch | `N2C_PATCH_V1` |

### Main Editor toolbar

| Button | Purpose | Output/Input |
|---|---|---|
| `Export N2C` | Export all Blueprints in `/Game` | `N2C_AllBlueprints_<timestamp>.zip` |
| `Import N2C` | Import multi-Blueprint project patch | `N2C_PROJECT_PATCH_V1` |
| `Export Options -> Export Blueprints from folders...` | Export recursive Blueprints from selected folders | `N2C_FolderBlueprints_<timestamp>.zip` |
| `Export Options -> Export project file list` | Export full project file/asset/source list | `N2C_ProjectFileList_<timestamp>.zip` |

### Content Browser

| Action | Purpose | Output |
|---|---|---|
| Right click selected Blueprint(s) -> `Export N2C` | Export selected Blueprint assets only | `N2C_SelectedBlueprints_<timestamp>.zip` |

## 4. Import flow

Single Blueprint patch:

```text
Blueprint Editor Import N2C
-> select N2C_PATCH_V1 JSON
-> dry-run validation
-> confirmation dialog
-> backup current .uasset
-> mutate Blueprint
-> compile request
-> optional Blueprint Assist auto-format
```

Project patch:

```text
Main Editor Import N2C
-> select N2C_PROJECT_PATCH_V1 JSON
-> validate all assets/actions
-> confirmation dialog
-> backup every touched Blueprint
-> apply each asset patch
-> compile touched Blueprints
-> optional Blueprint Assist auto-format for changed functions
```

## 5. Export flow

All/selected/folder Blueprint exports share this structure:

```text
collect Blueprint assets
-> load each Blueprint safely
-> call AI export builder
-> write per-Blueprint JSON + functions folder
-> write project manifest
-> write ZIP with native ZIP writer
```

The fork no longer relies on PowerShell `Compress-Archive` for export ZIP creation.

## 6. Project file list flow

Project file list export:

```text
AssetRegistry scan /Game
-> collect UE asset metadata
-> map package names to content disk files
-> inspect AssetImportData for source files
-> deduplicate flat file list
-> write N2C_PROJECT_FILE_LIST.json
-> zip it
```

Schema:

```text
N2C_PROJECT_FILE_LIST_V3
```

Arrays:

```text
assets[]
content_disk_files[]
source_files[]
flat_files[]
summary
```

## 7. Blueprint Assist auto-format integration

The integration is optional and best-effort.

Build-time condition:

```text
N2C_WITH_BLUEPRINT_ASSIST=1
```

Runtime conditions:

```text
BlueprintAssist plugin exists
BlueprintAssist plugin is enabled
BlueprintAssist module can load
Node2Code Editor Preference is enabled
```

Current formatting strategy:

1. Open/focus the changed function graph.
2. Let BlueprintAssist process the active tab.
3. Select all nodes through graph selection API.
4. Build `FEdGraphFormatterParameters` with all graph nodes.
5. Add pending format node to Blueprint Assist graph handler.
6. Call `UpdateNodesRequiringFormatting()`.
7. Retry several times because node sizes/editor tabs may not be ready on the first tick.

Known cosmetic behavior:

- Some graphs finish formatting only after the user opens the function graph.
- Imported logic is still valid even if formatting is delayed.

## 8. Safety guarantees

Implemented safety features:

- Dry-run validation before mutation.
- User confirmation before apply.
- `.uasset` backup before apply.
- Warnings/errors surfaced in compact dialog.
- Project patch validation before multi-asset mutation.
- Compile requested after patch apply.
- Auto-format failure does not fail import.

Backup folder:

```text
Saved/NodeToCode/Backups/
```

Export folders:

```text
Saved/NodeToCode/AIExports/
Saved/NodeToCode/ProjectExports/
```

## 9. Current known limitations

- Importer supports a stable subset of node types, not every Blueprint node.
- Full event graph reconstruction is not the main stable workflow yet.
- Macros are not a stable import target yet.
- Complex object/class/struct pin types may need manual schema extension.
- Niagara import/export is not implemented as stable schema yet.
- Blueprint Assist formatting is editor-state dependent.

## 10. Future extension points

Recommended next files to extend:

- Add more import node types: `N2CPatchImporter.cpp`.
- Add more export metadata: `N2CAIExport.cpp`.
- Add new toolbar/context actions: `N2CEditorIntegration.cpp`.
- Add new visible settings: `UN2CEditorPreferences` in `N2CSettings.h`.
- Add new docs: `Docs/` and `Source/Documentation/`.
