# N2C_PATCH_V1 schema — Back2Dead fork

`N2C_PATCH_V1` is the only format accepted by the **Import N2C** button.

The importer does not accept free-form pseudocode. This is intentional: strict JSON makes import safer and avoids guessing Blueprint graph mutations.

---

## Minimal patch

```json
{
  "schema": "N2C_PATCH_V1",
  "actions": [
    {
      "type": "add_or_replace_function",
      "function_name": "DoesSpellRequireDangerToken",
      "replace_body": true,
      "inputs": [
        { "name": "SpellName", "type": "Name" }
      ],
      "outputs": [
        { "name": "RequiresDangerToken", "type": "Boolean" }
      ],
      "nodes": [
        { "id": "Entry", "type": "FunctionEntry" },
        { "id": "Return", "type": "Return" }
      ],
      "exec_edges": [
        {
          "from_node_id": "Entry",
          "from_pin": "Then",
          "to_node_id": "Return",
          "to_pin": "Execute"
        }
      ]
    }
  ]
}
```

---

## Root fields

| Field | Required | Meaning |
|---|---:|---|
| `schema` | yes | Must be `N2C_PATCH_V1` |
| `actions` | yes | List of patch actions |
| `target_blueprint` | no | Optional safety guard. If set, it must equal current Blueprint name or path. |

---

## Supported action types

### `add_or_replace_function`
Creates the function if it does not exist. If `replace_body=true`, safely clears only the function body. The existing function graph, entry/result signature nodes, and existing signature pins are preserved; duplicate signature pins from the patch are skipped.

### `replace_function_body`
Replaces only the body of an existing function graph. This action fails validation if the function does not already exist.

---

## Supported node types

| Patch type | Blueprint node |
|---|---|
| `FunctionEntry`, `Entry` | function entry |
| `FunctionResult`, `Return` | function result / return |
| `Branch` | branch / if-then-else |
| `CallFunction`, `call_function` | function call |
| `VariableGet`, `variable_get` | variable get |
| `VariableSet`, `variable_set` | variable set |

Unsupported node types now fail validation before any Blueprint mutation. This is intentional: the importer must not silently create partial unsafe graphs.

---

## Edge fields

Edges can be placed in:

```json
"exec_edges": []
"data_edges": []
"edges": []
```

Each edge supports:

```json
{
  "from_node_id": "Entry",
  "from_pin": "Then",
  "to_node_id": "Return",
  "to_pin": "Execute"
}
```

Pin names are matched loosely by internal name or display name. Edges referencing unknown node IDs fail validation before mutation.

---

## Supported pin types

MVP pin type names:

```text
Exec
Boolean / Bool
Name
String
Text
Float / Real / Double
Integer / Int
Byte
Object
Class
Struct
```

For struct/object/class pins, `sub_category_object` can be supplied when the asset/class path is known.

---

## Safety notes

The importer always runs dry-run before applying from the toolbar.

Apply is aborted if:
- JSON cannot be parsed;
- schema is not `N2C_PATCH_V1`;
- `actions[]` is missing or empty;
- `.uasset` backup cannot be created.

The importer requests Blueprint compilation after apply, but Unreal compiler output remains the source of truth for final graph validity.


---

## Local variables

A function action can declare Blueprint local variables with `local_variables`:

```json
"local_variables": [
  { "name": "LocalCounter", "type": "Integer" },
  { "name": "LocalPrefix", "type": "String" },
  { "name": "bLocalAllowed", "type": "Boolean" }
]
```

The importer adds missing locals to the target function graph and skips duplicates on repeated import. Local variable defaults are not applied in this MVP; set them in the graph body with `VariableSet` nodes if needed.

---

## Pin defaults

A patch can set input pin defaults in either format:

```json
{
  "id": "Return",
  "type": "Return",
  "pin_defaults": {
    "bResult": false
  }
}
```

or compact scalar fields:

```json
{
  "id": "Return",
  "type": "Return",
  "bResult": false
}
```

Only scalar `boolean`, `number`, and `string` values are applied as defaults.

---

## Safety improvements in this fork revision

- Dry-run now validates action type, function name, node list, duplicate node IDs, required Entry/Return nodes, supported node types, and edge node references.
- Apply repeats the same validation before backup and mutation.
- `replace_function_body` no longer deletes the whole function graph. It removes non-signature nodes and breaks old links while preserving Entry/Return signature nodes.
- Re-importing the same patch no longer duplicates existing input/output signature pins.
- Optional `target_blueprint` prevents applying a patch to the wrong Blueprint.

================================================================================
# Patch schema update — function categories, rename, member variables

Supported since Back2Dead/sadnessinc varfolders hotfix:

## Root-level member variables

```json
{
  "schema": "N2C_PATCH_V1",
  "target_blueprint": "TestActor",
  "variables": [
    {
      "name": "B2D_Test_GlobalMessage",
      "type": "String",
      "default": "Created by patch",
      "category": "N2C/Test Variables"
    }
  ],
  "actions": []
}
```

Supported aliases:
- `variables`
- `member_variables`

Supported fields:
- `name`
- `type`
- `default` or `default_value`
- `category` or `folder`
- `sub_category_object` for object/struct/class/enum pin types when needed

Implementation uses UE editor helpers:
- `FBlueprintEditorUtils::AddMemberVariable`
- `FBlueprintEditorUtils::FindNewVariableIndex`
- `FBlueprintEditorUtils::SetBlueprintVariableCategory`

## Action: add_member_variables

```json
{
  "type": "add_member_variables",
  "variables": [
    { "name": "MyCounter", "type": "Integer", "default": 0, "category": "Debug" }
  ]
}
```

Alias:
- `add_variables`

## Function category / folder

For new or replaced functions:

```json
{
  "type": "add_or_replace_function",
  "function_name": "MyFunction",
  "category": "N2C/Helpers",
  "nodes": [...]
}
```

Standalone action:

```json
{
  "type": "set_function_category",
  "function_name": "MyFunction",
  "category": "N2C/Helpers"
}
```

Alias:
- `move_function_to_category`

Implementation uses `FBlueprintEditorUtils::SetBlueprintFunctionOrMacroCategory`.

## Action: rename_function

```json
{
  "type": "rename_function",
  "old_function_name": "OldHelper",
  "new_function_name": "NewHelper",
  "category": "N2C/Helpers"
}
```

Aliases:
- `from`
- `to`

Implementation uses:
- `FBlueprintEditorUtils::RenameGraph`
- `FBlueprintEditorUtils::ReplaceFunctionReferences`

Safety rules:
- source function must already exist;
- destination function must not already exist;
- `.uasset` backup is created before mutation;
- dry-run validates names and action shape first;
- Blueprint is compiled after patch apply.

