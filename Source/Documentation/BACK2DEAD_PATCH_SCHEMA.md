# `N2C_PATCH_V1` — authoritative Blueprint import schema

Release identity: **Version 173 / `1.2.73-ue427-full-review-refactor-candidate`**.

`N2C_PATCH_V1` is the strict JSON format accepted by Blueprint import. It is not free-form pseudocode. Unsupported or unresolved runtime semantics must be rejected before mutation.

## Root

```json
{
  "schema": "N2C_PATCH_V1",
  "target_blueprint": "BP_Example",
  "actions": []
}
```

- `schema` is required and must equal `N2C_PATCH_V1`.
- `actions` is required and must be non-empty.
- `target_blueprint` is an optional safety identity and may be a Blueprint name or path.
- `automation_force_failure_after_mutation` exists only in `WITH_DEV_AUTOMATION_TESTS` builds. Production imports ignore no hidden equivalent.

## Supported action families

- `add_or_replace_function`, `replace_function_body`
- `add_member_variables`, `add_variables`
- `add_event_graph_nodes`, `patch_graph`
- `add_macro`
- `add_event_dispatcher`, `add_event_dispatchers`
- `rename_function`, `rename_variable`, `rename_custom_event`
- delete functions, variables, dispatchers, macros and selected graph nodes through the singular/plural aliases implemented by the importer

Function actions may contain `inputs`, `outputs`, `local_variables`, `nodes`, `exec_edges` and `data_edges`. Graph actions identify the target graph and use the same node/edge representation.

## Function boundary rule

Generated pure and impure Blueprint functions retain internal exec boundary pins. A minimal body therefore includes the internal Entry-to-Result connection even when the external call node is pure:

```json
{
  "type": "add_or_replace_function",
  "function_name": "GetValue",
  "replace_body": true,
  "function_flags": { "pure": true, "access": "public" },
  "outputs": [{ "name": "Value", "type": "int" }],
  "nodes": [
    { "id": "Entry", "type": "Entry" },
    { "id": "Return", "type": "Return", "pin_defaults": { "Value": "7" } }
  ],
  "exec_edges": [
    { "from_node_id": "Entry", "from_pin": "then", "to_node_id": "Return", "to_pin": "execute" }
  ]
}
```

Multiple Result nodes are supported when each branch has a complete exec path and a unique node id.

## Member variables and pin types

```json
{
  "type": "add_member_variables",
  "variables": [
    { "name": "RawValue", "type": "byte", "default_value": "37" },
    { "name": "Mode", "type": "enum", "enum_path": "/Game/Data/E_Mode.E_Mode" },
    { "name": "Actors", "type": "object", "sub_category_object": "/Script/Engine.Actor", "container": "Array" }
  ]
}
```

Scalar categories include exec, bool, byte, enum, int, float/real, name, string, text, object, class, soft object, soft class, struct, delegate and multicast delegate. Containers use scalar/array/set/map aliases accepted by the importer.

### Raw Byte versus enum-backed Byte

- `type: "byte"` without an enum subtype means raw `uint8`. It must have no `PinSubCategoryObject`.
- `type: "enum"`, or a Byte declaration carrying an enum path/subtype, is enum-backed and requires a resolvable `UEnum`.
- A missing enum is a hard preflight error: `enum_member_type_unresolved`.
- Integer and raw Byte defaults are normalized to integer strings; enum defaults keep internal enum names such as `NewEnumerator0`.

Object, class, soft-reference, struct and enum identities must resolve to the correct UE type. Local/input/output defaults that UE4.27 cannot persist safely are rejected rather than silently discarded.

## Nodes

The importer and coverage classifier share aliases for the supported UE4.27 families, including:

- FunctionEntry/FunctionResult, Branch, Sequence, Select and Knot
- Self, DynamicCast, Variable Get/Set
- CallFunction, CallParentFunction, CallFunctionOnMember and Message
- raw Array custom thunks, `K2Node_CallArrayFunction`, MakeArray, ArrayLength and GetArrayItem copy/ref
- MakeStruct, BreakStruct and SetFields/SetMembersInStruct
- EnumLiteral, CastByteToEnum, enum equality/inequality and SwitchEnum
- literal and linked GetDataTableRow
- SpawnActor and literal/linked CreateWidget
- delegates and ComponentBoundEvent
- Input Action/Key/Axis/AxisKey
- exact Standard Macro references
- Tunnel and Composite/collapsed graph boundaries

Support is metadata-dependent. A class name alone does not prove importability. Exact function, class, member, macro, struct, enum, component and graph identities required by a constructor must be present and resolvable.

## Edges and pin names

```json
{
  "from_node_id": "Entry",
  "from_pin": "then",
  "to_node_id": "Branch",
  "to_pin": "execute"
}
```

Use `exec_edges` and `data_edges`; legacy edge field aliases remain schema-compatible where implemented. Every edge must reference existing node ids. Missing source/target nodes, missing pins and schema-rejected connections are hard errors.

Pin resolution accepts the documented UE4.27 internal aliases, including:

- FunctionEntry `then` and FunctionResult `execute`
- GetDataTableRow output `Out Row`
- Array and Delay aliases shared with the coverage classifier

Enum switch branches should use internal enum values, not localized display labels. User Defined Struct fields resolve by persistent GUID, internal name, friendly name and normalized base name; unknown fields are rejected.

## GetDataTableRow

Literal table:

```json
{
  "id": "GetRow",
  "type": "K2Node_GetDataTableRow",
  "data_table_path": "/Game/Data/DT_Items.DT_Items",
  "row_name": "Default"
}
```

Linked table input is accepted only when the incoming typed source allows the importer to determine and preserve the row struct. The output alias `Out Row` is supported. An untyped linked table is rejected before mutation.

## Construction order and strict lifecycle

For runtime graph nodes the importer follows the UE4.27-safe sequence:

```text
create → allocate pins → configure identity/state → reconstruct → resolve pins → connect
```

Toolbar apply follows:

```text
parse/shape preflight
→ live semantic preflight
→ save current asset
→ pre-apply .uasset backup
→ transaction and mutation
→ compile
→ save
→ commit, or real Editor Undo on failure
→ structural snapshot verification
→ queued startup disk restore when Undo cannot be proven
```

`FScopedTransaction::Cancel()` is not treated as rollback. A report may claim rollback success only after the structural verifier confirms equality.

## Intentional claims boundary

BTTask, BTService and BTDecorator coverage refers to Blueprint subclasses and persisted Blueprint graph state. It does not implement Behavior Tree asset, Blackboard or EQS editing. Full AnimGraph and full Niagara graph round-trip are not claimed by this schema.
