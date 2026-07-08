# Node2Code - Back2Dead / sadnessinc Fork

AI-friendly Blueprint export and safe JSON patch import for Unreal Engine 4.27.

This is a fork of **NodeToCode** by **Protospatial / Nick McClure**. The Back2Dead fork adds strict JSON workflows for inspecting and modifying Blueprints with AI tools such as ChatGPT.

Original project/author attribution is preserved in the plugin descriptor and license files.

## What this fork adds

### AI-friendly Blueprint export

Exports Blueprint assets into readable JSON instead of requiring an AI model to reason over binary `.uasset` files.

Exported data includes:

- Blueprint metadata
- member variables
- variable categories
- variable flags/options
- functions
- function categories
- function metadata
- function flags/options
- local variables
- nodes
- pins
- exec links
- data links
- linear flow summary

Main schema:

```text
N2C_AI_EXPORT_V2
```

### Safe JSON patch import

Imports strict JSON patches into Blueprint assets.

Supported stable patch schemas:

```text
N2C_PATCH_V1
N2C_PROJECT_PATCH_V1
```

The importer supports:

- add/replace functions
- replace function body
- add/update member variables
- add local variables
- function categories
- variable categories
- function flags/options
- variable flags/options
- rename functions
- connect exec/data pins
- single Blueprint patches
- multi-Blueprint project patches

Stable imported node types:

```text
FunctionEntry / Entry
FunctionResult / Return
Branch / IfThenElse
CallFunction
VariableGet
VariableSet
```

### Batch export tools

The main editor toolbar has project-level export tools:

- export all Blueprints in `/Game`
- export Blueprints from selected folders
- export selected Blueprints from Content Browser right click
- export project file list JSON

Project file list schema:

```text
N2C_PROJECT_FILE_LIST_V3
```

It contains:

- `assets[]` — UE AssetRegistry metadata
- `content_disk_files[]` — real `.uasset` / `.umap` files on disk
- `source_files[]` — original import source files from AssetImportData
- `flat_files[]` — unique deduplicated file list
- `summary` — class/extension counters

### Optional Blueprint Assist auto-format

If Blueprint Assist is installed and enabled, Node2Code can auto-format functions changed by import.

Setting:

```text
Editor Preferences -> Plugins -> Node2Code -> Blueprint Assistant Extension -> Auto Format Imported Functions
```

The integration is optional. If Blueprint Assist is missing or disabled, imports still work.

Current behavior note: Blueprint Assist may finish formatting only when the changed function graph is opened, because its formatter depends on editor tab state and node size caching.

### Better UX and safety

Import flow:

1. Parse JSON.
2. Dry-run validate.
3. Show compact OK / NOT OK dialog.
4. Ask user confirmation.
5. Backup `.uasset` before mutation.
6. Apply patch.
7. Request Blueprint compile.
8. Optionally auto-format changed functions.

Backups:

```text
Saved/NodeToCode/Backups/
```

Exports:

```text
Saved/NodeToCode/AIExports/
Saved/NodeToCode/ProjectExports/
```

## Installation

1. Close Unreal Engine.
2. Create project plugin folder if it does not exist:

```text
<Project>/Plugins/
```

3. Copy/extract this repository as:

```text
<Project>/Plugins/node2code/NodeToCode.uplugin
```

4. Open the project.
5. Let Unreal rebuild the plugin.
6. Enable the plugin if Unreal asks.

Target engine used during fork development:

```text
Unreal Engine 4.27.2
```

## Toolbar usage

### In Blueprint Editor

| Button | Action |
|---|---|
| `Export N2C` | Export currently open Blueprint |
| `Import N2C` | Import `N2C_PATCH_V1` into the current Blueprint |

### In main Unreal Editor toolbar

| Button | Action |
|---|---|
| `Export N2C` | Export all Blueprint assets from `/Game` |
| `Import N2C` | Import `N2C_PROJECT_PATCH_V1` multi-Blueprint patch |
| `Export Options -> Export Blueprints from folders...` | Pick folders and export only Blueprints under them |
| `Export Options -> Export project file list` | Export JSON list of project assets/files/source files |

### In Content Browser

Select Blueprint assets, right click, then choose:

```text
Export N2C
```

This creates one archive containing only the selected Blueprints.

## Single Blueprint patch example

```json
{
  "schema": "N2C_PATCH_V1",
  "blueprint_name": "TestActor",
  "blueprint_path": "/Game/Test/TestActor.TestActor",
  "variables": [
    {
      "name": "B2D_Test_Message",
      "type": "String",
      "category": "N2C/Test Variables",
      "default_value": "Hello",
      "flags": {
        "instance_editable": true,
        "private": false
      }
    }
  ],
  "actions": [
    {
      "type": "add_or_replace_function",
      "function_name": "B2D_Test_Run",
      "category": "N2C/Test Functions",
      "function_flags": {
        "access": "public",
        "const": true
      },
      "inputs": [
        {"name": "InputMessage", "type": "String"}
      ],
      "outputs": [
        {"name": "bResult", "type": "Boolean"}
      ],
      "nodes": [
        {"id": "Entry", "type": "FunctionEntry", "pos_x": 0, "pos_y": 0},
        {
          "id": "Print",
          "type": "CallFunction",
          "function_name": "PrintString",
          "member_name": "PrintString",
          "function_path": "/Script/Engine.KismetSystemLibrary:PrintString",
          "pos_x": 300,
          "pos_y": 0,
          "pin_defaults": {
            "InString": "Hello from N2C",
            "bPrintToScreen": true,
            "bPrintToLog": true,
            "Duration": 2.0
          }
        },
        {
          "id": "Return",
          "type": "Return",
          "pos_x": 600,
          "pos_y": 0,
          "pin_defaults": {"bResult": true}
        }
      ],
      "exec_edges": [
        {"from_node_id": "Entry", "from_pin": "then", "to_node_id": "Print", "to_pin": "execute"},
        {"from_node_id": "Print", "from_pin": "then", "to_node_id": "Return", "to_pin": "execute"}
      ],
      "data_edges": []
    }
  ]
}
```

## Multi-Blueprint patch example

```json
{
  "schema": "N2C_PROJECT_PATCH_V1",
  "assets": [
    {
      "blueprint_path": "/Game/Test/TestActor.TestActor",
      "variables": [],
      "actions": []
    },
    {
      "blueprint_path": "/Game/Test/Test2.Test2",
      "variables": [],
      "actions": []
    }
  ]
}
```

## Documentation

Detailed internal docs are included in the repository:

```text
Docs/N2C_BLUEPRINT_JSON_WORKFLOW.md
Docs/N2C_FORK_ARCHITECTURE.md
Source/Documentation/N2C_BLUEPRINT_JSON_WORKFLOW.md
Source/Documentation/N2C_FORK_ARCHITECTURE.md
```

Use `N2C_BLUEPRINT_JSON_WORKFLOW.md` as the schema/workflow reference for generating or reading JSON.

Use `N2C_FORK_ARCHITECTURE.md` as the code map for future development.

## Current limitations

- The importer supports a stable subset of Blueprint node types, not every possible K2 node.
- Full arbitrary event graph reconstruction is not the stable target yet.
- Macro import/export is not stable yet.
- Complex object/class/struct pins may require schema extension.
- Niagara import/export is not implemented as a stable workflow yet.
- Blueprint Assist formatting is best-effort and editor-state dependent.

## Credits

Original NodeToCode plugin by:

```text
Protospatial / Nick McClure
```

Back2Dead fork and AI JSON/import workflow by:

```text
sadnessinc
```

## License

See the included `LICENSE` file from the original project.
