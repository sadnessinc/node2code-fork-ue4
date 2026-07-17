# N2C Blueprint JSON Workflow

Current mode for the Back2Dead / sadnessinc fork of Node2Code.

> **AI authoring gate:** before generating any import JSON, read the top-level `N2C_AI_JSON_IMPORT_AUTHORING_RULES.md`. That document is the concise mandatory contract for supported capabilities, canonical pin names, prerequisites, unsupported requests and alternatives.

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

- `Export Project` opens an export picker and can export any combination of:
  - `Blueprint`
  - `Niagara System`
  - `Enum`
  - `Struct`
- `Import N2C` imports a project patch that can modify multiple Blueprint assets.

The old dedicated folder-export toolbar button is intentionally not restored. The adjacent `Export Options` dropdown is active again, but it only contains `Export project file list`. Use `Export Project` for full-project export, or use Content Browser `Export N2C...` on selected assets/folders for selected-scope export.

### Asset Editor toolbars

When a supported asset is open:

- Blueprint Editor -> `Export N2C` exports the current Blueprint.
- Niagara System Editor -> `Export N2C Niagara` exports the current Niagara System.
- Enum Editor -> `Export N2C Enum` exports the current Enum.
- Struct Editor -> `Export N2C Struct` exports the current Struct.

### Content Browser context menu

When one or more supported assets are selected:

- Right click -> `Export N2C...` opens the same type picker for the selected assets. Supported selected types are `Blueprint`, `Niagara System`, `Enum`, and `Struct`.
- For Niagara Systems, `Import N2C Niagara` remains available separately.

When one or more Content Browser folders are selected:

- Right click -> `Export N2C...` opens the same type picker. The exporter scans the selected folders recursively and writes a mixed N2C ZIP containing only the checked asset kinds.
- This replaces the old Niagara-only folder export action.

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

### 3.2 Mixed project / selected-assets export

Created by:

- main toolbar `Export Project`
- Content Browser selected assets -> `Export N2C...`

Both flows open a checkbox picker for `Blueprint`, `Niagara System`, `Enum`, and `Struct`.

Filename examples:

```text
N2C_Project_20260710_120000.zip
N2C_SelectedAssets_20260710_120100.zip
```

Archive structure:

```text
N2C_PROJECT_MANIFEST.json
blueprints/
  Game_Test_TestActor/
    N2C_TestActor_20260710_120100.json
    functions/
      001_UserConstructionScript.json
      002_TestFunction.json
niagara/
  Game_FX_NS_Test/
    N2C_NS_Test_20260710_120100.json
enums/
  Game_Data_E_SpellTag/
    N2C_E_SpellTag_20260710_120100.json
structs/
  Game_Data_ST_Spell/
    N2C_ST_Spell_20260710_120100.json
```

Manifest schema:

```json
{
  "schema": "N2C_PROJECT_EXPORT_V2",
  "export_kind": "MixedProjectArchive",
  "export_label": "Project",
  "engine_target": "UE4.27",
  "timestamp": "20260710_120000",
  "source_asset_count": 10,
  "exported_total_count": 10,
  "exported_blueprint_count": 4,
  "exported_niagara_system_count": 2,
  "exported_enum_count": 2,
  "exported_struct_count": 2,
  "skipped_asset_count": 0,
  "assets": []
}
```

### 3.3 Project file list export

Created by the adjacent toolbar dropdown:

```text
Export Options -> Export project file list
```

This dropdown is intentionally limited to the file-list export. It does not contain the old folder-export command.

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

### 3.4 Selected Niagara export

Created by:

```text
Content Browser -> select asset(s) -> right click -> Export N2C...
```

Filename example:

```text
N2C_SelectedNiagara_<YYYYMMDD_HHMMSS>.zip
```

Archive structure:

```text
N2C_PROJECT_MANIFEST.json
niagara/
  Game_FX_NS_Effect_Aura_Generic_Loop/
    N2C_NS_Effect_Aura_Generic_Loop_<YYYYMMDD_HHMMSS>.json
```

Manifest schema:

```json
{
  "schema": "N2C_NIAGARA_PROJECT_EXPORT_V1",
  "export_kind": "NiagaraProjectArchive",
  "engine_target": "UE4.27",
  "timestamp": "20260709_203000",
  "source_asset_count": 1,
  "exported_niagara_count": 1,
  "skipped_asset_count": 0,
  "assets": [],
  "import_status": "parameter_import_v1_supported_no_graph_rebuild"
}
```

Per-asset JSON uses the shared AI export envelope:

```json
{
  "schema": "N2C_AI_EXPORT_V2",
  "export_kind": "Niagara",
  "engine_target": "UE4.27",
  "metadata": {
    "asset_name": "NS_Effect_Aura_Generic_Loop",
    "asset_path": "/Game/FX/NS_Effect_Aura_Generic_Loop.NS_Effect_Aura_Generic_Loop",
    "asset_class": "NiagaraSystem",
    "package_name": "/Game/FX/NS_Effect_Aura_Generic_Loop",
    "is_asset_dirty": false
  },
  "niagara_asset": {
    "asset_name": "NS_Effect_Aura_Generic_Loop",
    "asset_path": "/Game/FX/NS_Effect_Aura_Generic_Loop.NS_Effect_Aura_Generic_Loop",
    "class": "NiagaraSystem",
    "export_mode": "compact_typed_niagara_summary_v3_readable_stack",
    "properties_note": "Compatibility/root summary only. Use niagara_summary for AI-readable Niagara data and full_reflection/important_sections for raw reflected data.",
    "properties": [],
    "niagara_summary": {
      "available": true,
      "exposed_parameters": {},
      "system_scripts": [],
      "emitters": [],
      "local_subobjects": {}
    },
    "important_sections": {
      "emitter_handles": {},
      "exposed_parameters": {},
      "system_spawn_script": {},
      "system_update_script": {},
      "system_compiled_data": {},
      "editor_data": {},
      "fixed_bounds": {}
    },
    "full_reflection": {
      "name": "NS_Effect_Aura_Generic_Loop",
      "path": "/Game/FX/NS_Effect_Aura_Generic_Loop.NS_Effect_Aura_Generic_Loop",
      "class": "NiagaraSystem",
      "properties": []
    }
  },
  "warnings": []
}
```

Current Niagara export is a read-only compact typed Niagara summary export with an extra AI-friendly `readable_stack` section. It is meant for AI inspection of asset paths, classes, emitters, exposed/user parameters, system scripts, emitter scripts, renderer properties, decoded rapid iteration parameters, readable stage/module/input/value data, visual hints, compact graph/subobject indexes and reflected property values available through UE reflection, with recursive object duplication and huge VM/shader bytecode omitted. It is not a binary-safe Niagara graph clone and does not restore Niagara assets yet.

Import UI is implemented for parameter-only import:

```text
Content Browser -> select Niagara asset(s) -> right click -> Import N2C Niagara
```

The import button reads an N2C Niagara ZIP/JSON, runs a dry-run, asks for confirmation, then applies matching numeric/vector/color/bool parameters to the selected/current NiagaraSystem. It does not create/remove emitters, modules, renderers or graph links yet.


### 3.5 Enum / Struct import

Enum import UI:

```text
Enum Editor -> Import N2C Enum
Content Browser -> select UserDefinedEnum asset(s) -> right click -> Import N2C Enum
```

Struct import UI:

```text
Struct Editor -> Import N2C Struct
Content Browser -> select UserDefinedStruct asset(s) -> right click -> Import N2C Struct
```

Both imports accept either a direct `.json` file or an N2C `.zip` archive. When a mixed project archive is selected, the importer searches `N2C_PROJECT_MANIFEST.json` first and then falls back to the matching `enums/` or `structs/` JSON entries.

Enum import supports `N2C_AI_EXPORT_V2` with `export_kind: "Enum"` and the patch schema `N2C_ENUM_PATCH_V1`. It imports `values[]` / `enum_values[]`, runs a dry-run report, asks for confirmation, then resizes and renames the UserDefinedEnum entries. Native C++ enums are not modified.

Struct import supports `N2C_AI_EXPORT_V2` with `export_kind: "Struct"` and the patch schema `N2C_STRUCT_PATCH_V1`. It imports `fields[]` / `struct_fields[]`, runs a dry-run report, asks for confirmation, then adds or updates UserDefinedStruct fields and their pin types. Native C++ structs are not modified. Extra existing fields are kept unless the JSON root has `replace_fields: true`.

UE4.27 compatibility note: `FStructVariableDescription::Category` is type data, not UI folder/category metadata. The importer must use it only for `PinType.PinCategory`; otherwise fields can silently become the default `int32`. The importer therefore does not write UI category metadata for struct fields in this fork. The validated field type path writes `Category`, `SubCategory`, `SubCategoryObject`, `ContainerType` and `PinValueType` from the parsed `FEdGraphPinType`.

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
- Niagara export exists as selected-asset read-only compact typed Niagara summary ZIP (`N2C_NIAGARA_PROJECT_EXPORT_V1`) and as Niagara Editor toolbar export for the currently edited asset. Niagara import supports parameter-only ZIP/JSON import in both Content Browser and Niagara Editor; graph/module recreation is still not implemented.
- Auto-format is best-effort because Blueprint Assist formatting depends on editor state.


## Niagara Import Actions V1

Niagara import now supports a safe action block in addition to parameter value updates. The block can be placed either at root `niagara_import_actions` or under `niagara_asset.import_actions`. Supported actions:

- `user_parameters`: add/update exposed `User.*` parameters. Supported types: `float`, `int32`, `bool`, `vector2`, `vector3`, `vector4`, `color`/`linear_color`, `quat`.
- `emitters`: `op=duplicate` duplicates an existing emitter handle by source index into a new emitter name. This is the first safe new-emitter path.
- `modules`: adds a Niagara module script by asset object path using Niagara's `AddModuleIfMissing`. Supported stages: System Spawn, System Update, Emitter Spawn, Emitter Update, Particle Spawn, Particle Update.

This is not yet a full graph-clone importer: it does not create arbitrary custom node graphs, remove modules, import renderers/materials, or create brand-new scratch modules.

## Niagara import: input overrides

`niagara_asset.import_actions.input_overrides` can link existing module inputs to User parameters or set a custom HLSL dynamic input.

Example link:

```json
{
  "op": "link_user_parameter",
  "target": "all_emitters",
  "stage": "Particle Spawn",
  "module": "SphereLocation",
  "input": "Sphere Radius",
  "type": "float",
  "user_parameter": "User.N2C_Radius"
}
```

Example formula:

```json
{
  "op": "custom_hlsl",
  "target": "all_emitters",
  "stage": "Emitter Update",
  "module": "SpawnRate",
  "input": "SpawnRate",
  "type": "float",
  "expression": "max(1000.0, User.N2C_SpawnRateBase + abs(sin(Engine.Time * User.N2C_WaveFrequency)) * User.N2C_SpawnRateAmplitude)"
}
```


### Niagara low-level input override diagnostic safety

Import actions can now set `niagara_asset.import_actions.input_overrides_apply_limit` to apply only the first N resolved low-level input override attempts. This is intended for UE4.27 Niagara graph debugging: dry-run still lists all actions, but apply can be limited to isolate the exact graph node/pin operation that triggers an editor assertion. The importer also writes `N2C Niagara graph import trace:` breadcrumbs before creating ParameterMapSet/Get/CustomHlsl nodes and before rewiring pins.
