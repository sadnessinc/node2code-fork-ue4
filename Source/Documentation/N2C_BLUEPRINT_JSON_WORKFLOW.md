# N2C Blueprint JSON Workflow

Current mode for the Back2Dead / sadnessinc fork of Node2Code.

This document is the source of truth for creating, reading and validating JSON exports/imports used by the fork.

## 1. Main goals

The fork has two separate workflows:

1. **Export Blueprint(s) to AI-readable JSON** so ChatGPT can inspect Blueprint functions, variables, metadata, flags and links without parsing binary `.uasset` files.
2. **Import strict JSON patches** so ChatGPT can safely add or replace Blueprint functions, variables and selected metadata.

The importer intentionally does **not** accept free-form text. It accepts only strict JSON patch schemas.

## 2. Editor UI

### Blueprint Editor toolbar

When a Blueprint is open:

- `Export N2C` exports the current Blueprint.
- `Import N2C` imports a single-Blueprint patch into the current Blueprint.

### Main Editor toolbar

In the main Unreal Editor toolbar:

- `Export N2C` exports all Blueprint assets from `/Game` into one archive.
- `Import N2C` imports a project patch that can modify multiple Blueprint assets.
- `Export Options` contains:
  - `Export Blueprints from folders...`
  - `Export project file list`

### Content Browser context menu

When one or more Blueprint assets are selected:

- Right click -> `Export N2C` exports only those selected Blueprints into one archive.

## 3. Export archive types

### 3.1 Single Blueprint export

Filename:

```text
N2C_<BlueprintName>_<YYYYMMDD_HHMMSS>.zip
```

Example:

```text
N2C_TestActor_20260708_202032.zip
```

Typical archive structure:

```text
N2C_TestActor_20260708_202032/
  N2C_TestActor_20260708_202032.json
  functions/
    001_UserConstructionScript.json
    002_TestFunction.json
    003_EventGraph.json
```

Root JSON schema:

```json
{
  "schema": "N2C_AI_EXPORT_V2",
  "export_kind": "Blueprint",
  "engine_target": "UE4.27",
  "metadata": {
    "blueprint_name": "TestActor",
    "blueprint_path": "/Game/Test/TestActor.TestActor",
    "blueprint_class": "/Game/Test/TestActor.TestActor_C",
    "parent_class": "/Script/Engine.Actor",
    "is_blueprint_dirty": false
  },
  "variables": [],
  "functions": [],
  "dependencies": []
}
```

### 3.2 Multi Blueprint project export

Created by:

- main toolbar `Export N2C` / all Blueprints
- content browser selected Blueprint export
- folder Blueprint export

Filename examples:

```text
N2C_AllBlueprints_20260708_213838.zip
N2C_SelectedBlueprints_20260708_213904.zip
N2C_FolderBlueprints_20260708_223358.zip
```

Archive structure:

```text
N2C_PROJECT_MANIFEST.json
blueprints/
  Game_Test_TestActor/
    N2C_TestActor_20260708_213904.json
    functions/
      001_UserConstructionScript.json
      002_TestFunction.json
  Game_Test_Test2/
    N2C_Test2_20260708_213904.json
    functions/
      001_UserConstructionScript.json
      002_EventGraph.json
```

Manifest schema:

```json
{
  "schema": "N2C_PROJECT_EXPORT_V1",
  "export_label": "AllBlueprints",
  "engine_target": "UE4.27",
  "timestamp": "20260708_213838",
  "source_asset_count": 10,
  "exported_blueprint_count": 10,
  "skipped_asset_count": 0,
  "blueprints": [],
  "skipped_assets": []
}
```

### 3.3 Project file list export

Created by:

```text
Export Options -> Export project file list
```

Filename:

```text
N2C_ProjectFileList_<YYYYMMDD_HHMMSS>.zip
```

The archive contains only JSON:

```text
N2C_PROJECT_FILE_LIST.json
```

Current schema:

```json
{
  "schema": "N2C_PROJECT_FILE_LIST_V3",
  "engine_target": "UE4.27",
  "assets": [],
  "content_disk_files": [],
  "source_files": [],
  "flat_files": [],
  "summary": {}
}
```

Meaning:

- `assets[]` — UE asset metadata known by AssetRegistry.
- `content_disk_files[]` — real `.uasset` / `.umap` files under project Content.
- `source_files[]` — original import source files from `AssetImportData`, when Unreal still remembers them.
- `flat_files[]` — unique deduplicated file list. One file may have multiple `kinds`.
- `summary` — class and extension counters useful for fast project overview.

Example asset entry:

```json
{
  "asset_name": "TestActor",
  "class_name": "Blueprint",
  "package_name": "/Game/Test/TestActor",
  "package_path": "/Game/Test",
  "object_path": "/Game/Test/TestActor.TestActor",
  "project_file": "D:/Project/Content/Test/TestActor.uasset",
  "project_file_extension": ".uasset",
  "source_file_count": 0,
  "source_files": []
}
```

Example source file entry:

```json
{
  "kind": "source_file",
  "disk_filename": "D:/SourceArt/SM_Chair.fbx",
  "disk_extension": ".fbx",
  "display_file": "SM_Chair.fbx",
  "exists": false,
  "owner_assets": ["/Game/StarterContent/Props/SM_Chair.SM_Chair"],
  "owner_asset_count": 1,
  "asset_classes": ["StaticMesh"]
}
```

`exists: false` is normal for assets imported from old source paths that no longer exist on disk.

## 4. Blueprint export content

### 4.1 Member variables

Exported under root `variables[]`.

Important fields:

```json
{
  "name": "B2D_Test_GlobalMessage",
  "type": "string",
  "category": "N2C/Test Variables",
  "default_value": "Hello",
  "tooltip": "",
  "flags": {
    "instance_editable": true,
    "blueprint_read_only": false,
    "save_game": false,
    "transient": false,
    "expose_on_spawn": false,
    "private": false,
    "property_flags_raw": 0
  }
}
```

Supported variable flag meanings:

- `instance_editable` — visible/editable per instance.
- `blueprint_read_only` — Blueprint read-only property.
- `save_game` — SaveGame flag.
- `transient` — transient property.
- `expose_on_spawn` — expose on spawn pin.
- `private` — Blueprint private variable.
- `property_flags_raw` — raw UE flags for debugging only.

### 4.2 Functions

Each function is included in root `functions[]` and also in `functions/*.json`.

Important fields:

```json
{
  "name": "B2D_Test_MainCallsHelper",
  "category": "N2C/Test Main",
  "tooltip": "",
  "keywords": "",
  "compact_node_title": "",
  "call_in_editor": false,
  "deprecated": false,
  "deprecation_message": "",
  "function_flags": {
    "access": "public",
    "private": false,
    "protected": false,
    "public": true,
    "pure": false,
    "const": true,
    "blueprint_callable": true,
    "function_flags_raw": 0
  },
  "local_variables": [],
  "nodes": [],
  "exec_edges": [],
  "data_edges": [],
  "linear_flow": []
}
```

`inputs` and `outputs` are not duplicated as top-level arrays in the current export. Read them from pins:

- Function inputs = output pins on `K2Node_FunctionEntry`.
- Function outputs = input pins on `K2Node_FunctionResult`.

### 4.3 Local variables

Exported under each function as `local_variables[]`.

Example:

```json
{
  "name": "LocalPrefix",
  "type": "string",
  "category": "",
  "default_value": "",
  "flags": {}
}
```

### 4.4 Nodes

Common node entry:

```json
{
  "id": "Print_Custom_A",
  "type": "CallFunction",
  "node_class": "K2Node_CallFunction",
  "title": "Print String",
  "pos_x": 300,
  "pos_y": 64,
  "function_name": "PrintString",
  "member_name": "PrintString",
  "function_path": "/Script/Engine.KismetSystemLibrary:PrintString",
  "pins": [],
  "pin_defaults": {}
}
```

Node IDs inside patch JSON must be unique within the function action.

### 4.5 Edges

Exec edge:

```json
{
  "from_node_id": "Entry",
  "from_pin": "then",
  "to_node_id": "Branch",
  "to_pin": "execute"
}
```

Data edge:

```json
{
  "from_node_id": "Entry",
  "from_pin": "InputMessage",
  "to_node_id": "Print_Custom_A",
  "to_pin": "InString"
}
```

Pin names are matched loosely by the importer, but patches should still use UE-readable names.

## 5. Import patch schemas

## 5.1 Single Blueprint patch

Schema:

```json
{
  "schema": "N2C_PATCH_V1",
  "blueprint_name": "TestActor",
  "blueprint_path": "/Game/Test/TestActor.TestActor",
  "variables": [],
  "actions": []
}
```

Use this from the Blueprint Editor `Import N2C` button.

Supported root field aliases:

- `variables[]` can be used to add/update member variables.
- variable action aliases also exist for backward compatibility: `add_variables`, `add_member_variables`.

### 5.2 Project patch

Schema:

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

Use this from the main editor `Import N2C` button.

Import order is per asset in array order. Put helper functions before functions that call them when both are created in the same patch.

## 6. Import actions

### 6.1 Add or replace function

```json
{
  "type": "add_or_replace_function",
  "function_name": "B2D_Test_Run",
  "category": "N2C/Test Main",
  "function_flags": {
    "access": "public",
    "const": true,
    "pure": false
  },
  "inputs": [
    {"name": "InputMessage", "type": "String"}
  ],
  "outputs": [
    {"name": "bResult", "type": "Boolean"},
    {"name": "ResultText", "type": "String"}
  ],
  "local_variables": [
    {"name": "LocalMessage", "type": "String"}
  ],
  "nodes": [],
  "exec_edges": [],
  "data_edges": []
}
```

### 6.2 Replace function body

```json
{
  "type": "replace_function_body",
  "function_name": "ExistingFunction",
  "replace_body": true,
  "nodes": [],
  "exec_edges": [],
  "data_edges": []
}
```

Signatures and locals can be added/updated in the same action.

### 6.3 Add/update member variables

As root `variables[]`:

```json
{
  "name": "B2D_Test_Message",
  "type": "String",
  "category": "N2C/Test Variables",
  "default_value": "Hello",
  "flags": {
    "instance_editable": true,
    "private": false,
    "save_game": false,
    "expose_on_spawn": true
  }
}
```

Supported type strings include common Blueprint names:

```text
Boolean / Bool
Integer / Int
Float
String
Name
Text
Vector
Rotator
Object
Actor
```

More complex object/class/struct types may require exact UE pin type data and are not the main stable target yet.

### 6.4 Rename function

```json
{
  "type": "rename_function",
  "old_name": "OldFunctionName",
  "new_name": "NewFunctionName"
}
```

### 6.5 Set function category

```json
{
  "type": "set_function_category",
  "function_name": "B2D_Test_Run",
  "category": "N2C/Test Main"
}
```

When creating a new function in the same patch, prefer putting `category` directly inside `add_or_replace_function`. Separate `set_function_category` validation expects the function graph to already exist before mutation.

### 6.6 Function flags/options

Use inside a function action:

```json
{
  "function_flags": {
    "access": "private",
    "const": true,
    "pure": false
  }
}
```

Supported:

- `access`: `public`, `protected`, `private`
- `const`: boolean
- `pure`: boolean

Do not set `pure: true` on functions that still need exec pins unless you intentionally want a pure function layout.

### 6.7 Variable flags/options

Use inside variable entry:

```json
{
  "flags": {
    "instance_editable": true,
    "blueprint_read_only": false,
    "save_game": false,
    "transient": false,
    "expose_on_spawn": true,
    "private": false
  }
}
```

## 7. Supported import node types

Stable importer node types:

```text
FunctionEntry / Entry
FunctionResult / Return
Branch / IfThenElse
CallFunction
VariableGet
VariableSet
```

### 7.1 FunctionEntry

```json
{"id": "Entry", "type": "FunctionEntry", "pos_x": 0, "pos_y": 0}
```

### 7.2 Return

```json
{
  "id": "Return",
  "type": "Return",
  "pos_x": 800,
  "pos_y": 0,
  "pin_defaults": {
    "bResult": true,
    "ResultText": "Done"
  }
}
```

### 7.3 Branch

```json
{"id": "Branch", "type": "Branch", "pos_x": 250, "pos_y": 0}
```

### 7.4 CallFunction: engine/library function

```json
{
  "id": "Print_Response",
  "type": "CallFunction",
  "function_name": "PrintString",
  "member_name": "PrintString",
  "function_path": "/Script/Engine.KismetSystemLibrary:PrintString",
  "pos_x": 500,
  "pos_y": 0,
  "pin_defaults": {
    "InString": "Hello from N2C patch",
    "bPrintToScreen": true,
    "bPrintToLog": true,
    "Duration": 3.0
  }
}
```

### 7.5 CallFunction: self Blueprint function

```json
{
  "id": "Call_Helper",
  "type": "CallFunction",
  "function_name": "B2D_Test_Helper",
  "member_name": "B2D_Test_Helper",
  "pos_x": 300,
  "pos_y": 0
}
```

No `function_path` is needed for a function on the same Blueprint. If the helper is created earlier in the same patch, the importer checks both `GeneratedClass` and `SkeletonGeneratedClass` so later actions can call it before a full compile.

## 8. Safety model

Every import does:

1. JSON parse.
2. Dry-run validation.
3. Confirmation dialog.
4. Backup before mutation.
5. Apply patch.
6. Compile request.
7. Optional Blueprint Assist auto-format.

Backups are written under:

```text
Saved/NodeToCode/Backups/
```

Import dialogs use this structure:

```text
OK / NOT OK
Blueprint : <Name>

Apply patch now?

What new:
- variable: ...
- function: ...

What changed:
- function category: Old -> New
- function options: ...
```

On failure:

```text
NOT OK
Blueprint : <Name>

Problems:
- ERROR: ...
- WARNING: ...
```

## 9. Blueprint Assist auto-format

Optional setting:

```text
Editor Preferences -> Plugins -> Node2Code -> Blueprint Assistant Extension -> Auto Format Imported Functions
```

When enabled, after import the plugin attempts to format every changed function graph through Blueprint Assist.

Checks before formatting:

- BlueprintAssist plugin exists.
- BlueprintAssist is enabled.
- BlueprintAssist module can be loaded.
- Node2Code was compiled with BlueprintAssist source available.
- The Node2Code setting is enabled.

Current practical behavior:

- Formatting may visibly complete only after opening the changed function graph, because Blueprint Assist depends on active graph/editor tab state and cached node sizes.
- This is cosmetic and does not affect imported Blueprint logic.

## 10. AI authoring rules

When ChatGPT creates patches:

1. Use strict JSON only.
2. Prefer `N2C_PATCH_V1` for one Blueprint and `N2C_PROJECT_PATCH_V1` for multiple Blueprints.
3. Always include `blueprint_path` when known.
4. Create helper functions before functions that call helpers.
5. Use unique node IDs inside every function action.
6. Use `function_path` for engine/library calls, but omit it for self Blueprint functions.
7. Keep functions small and testable.
8. Use categories to keep generated functions organized.
9. Use `private` helper functions unless they need to be called externally.
10. Avoid unsupported node types unless the importer has been extended.

## 11. Known limitations

- Full arbitrary Blueprint graph import is not implemented.
- Complex struct/class/object pin types may need importer extension.
- Macro, event graph and latent flow support is not the stable target yet.
- Niagara import/export is not part of the stable Blueprint JSON workflow yet.
- Auto-format is best-effort because Blueprint Assist formatting depends on editor state.
