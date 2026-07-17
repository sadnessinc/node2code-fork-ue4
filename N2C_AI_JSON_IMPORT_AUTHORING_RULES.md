# node2code AI JSON import authoring rules — UE4.27.2 / Version 195

**This is the mandatory first document for any AI that creates or edits an N2C import JSON.**

The AI must not invent Unreal node identities, pin names, classes, asset paths or capabilities. It must derive them from a current N2C export, UE4.27 reference source, an existing verified fixture, or this document. When a requested operation is unsupported, the AI must say so clearly and provide a supported alternative instead of emitting a plausible-looking JSON.

## 1. Choose the correct schema

- One existing Blueprint: `N2C_PATCH_V1`.
- Several existing Blueprints: `N2C_PROJECT_PATCH_V1` with `assets[]`.
- One existing User Defined Enum: `N2C_ENUM_PATCH_V1`, imported from the Enum editor or selected-asset Content Browser command.
- One existing User Defined Struct: `N2C_STRUCT_PATCH_V1`, imported from the Struct editor or selected-asset Content Browser command.

`N2C_PROJECT_PATCH_V1` currently imports **Blueprint entries only**. Do not put Struct or Enum entries into a project patch until the P1 project-asset importer is implemented.

Import the complete project patch as **one project-level operation** from **Import N2C** on the Level Editor main toolbar. Do not feed the same project JSON separately to each Blueprint-editor toolbar: those asset-level commands are for the single-asset `N2C_PATCH_V1` schema. For release acceptance, repeat that same project-level import in a fresh Editor process and require an idempotent result with no new nodes or duplicate links. Export the multi-Blueprint project from the matching **Export Project** command on the Level Editor toolbar.

Use exactly one `assets[]` entry per `blueprint_path`. Put all actions for the same Blueprint in that entry, in dependency order. Never split one Blueprint across duplicate project entries: a later failure can otherwise leave a partially applied earlier entry.

## 2. Required workflow before writing JSON

1. Export the current target asset or project with N2C.
2. Read the root JSON, relevant `functions/*.json`, coverage sidecars and current plugin version.
3. Resolve every asset/class/function/enum/struct path exactly.
4. Use `input_pins[].name` and `output_pins[].name` as canonical pin names. Never use `display_name`, node title, localized UI text or a guessed friendly label.
5. Create the smallest patch that solves the request.
6. Run the N2C dry run. Version 195 applies the patch to a transient duplicate Blueprint, so real UE4 node allocation, pin lookup and schema connections are tested before target mutation.
7. If dry run shows any Problem or `N2C_PREFLIGHT_GUARD`, do not continue. Correct the JSON or explain that the request is unsupported.

## 3. Universal JSON rules

- Every node `id` is unique within its action.
- Every edge references existing node IDs in the same action or an explicitly supported existing-boundary alias.
- `from_pin` is an output and `to_pin` is an input.
- Use canonical internal case-sensitive `PinName` values.
- Keep required exec flow complete. An impure function needs an internal route from `FunctionEntry.then` to every reachable `FunctionResult.execute`.
- Pure Blueprint functions still require the internal Entry-to-Result exec link; only their external call node lacks exec pins.
- Do not leave runtime nodes as disconnected presence probes. Connect the data and execution path needed for the claimed case.
- Supply all identity metadata required by the node family: owner class, function path, enum/struct path, delegate property, component property, macro graph identity, row struct, and linked/literal variant flags.
- Use one target Blueprint per project asset entry and order prerequisites before consumers.
- Do not rely on a save/compile from an earlier asset entry to make a later entry for the same Blueprint valid.
- Defaults on linked data input pins must be omitted or empty.
- Author object/class pin literals as canonical full paths, but treat the JSON string only as input to `UEdGraphSchema_K2`; the schema decides whether UE stores it in `DefaultObject`, `DefaultValue`, or `DefaultTextValue`.
- Do not put unsupported defaults on function locals or parameters. Assign them with graph nodes instead.


## 4. Stable graph import identity and repeat-import safety

Every long-lived `patch_graph` action must have a stable identity. Version 195 derives that identity from `graph_name`, action type, and the sorted authored node IDs, but AI-authored production JSON should also provide an explicit stable `import_scope` (or `action_id`) so later edits do not accidentally change the derived scope.

- Keep every node `id` stable across repeated imports. Version 195 derives a deterministic `NodeGuid` from the target graph, stable action scope, and node ID.
- Reusing the same scope and node ID reuses the existing node instead of creating a duplicate. Existing identical links are also reused.
- A node ID is a durable semantic identity, not a display label. Do not change the node type under the same ID. Delete/replace the old node through a supported lifecycle action or assign a new ID.
- Do not reuse one `import_scope` for unrelated graph patches.
- Changing the set of node IDs changes the fallback derived scope. For maintained JSON, use an explicit scope before the first production import.
- Repeat the exact same import after save/reopen. It must not add nodes, links, functions, macros, dispatchers, variables, components, interfaces, or graph boundaries.

The mandatory Version 195 contract matrix applies every supported case, verifies it in a fresh UE process, reapplies it, and verifies it again in a second fresh process. A feature is not release-ready unless it is mapped into that matrix.

Release tooling rule: no validator-owned JSON field—required or optional—may be read directly from a `ConvertFrom-Json` object under `Set-StrictMode`; use `Get-N2CJsonPropertyValue` so missing fields produce contextual schema diagnostics rather than `PropertyNotFoundStrict`. Collection cardinality must be normalized through `Get-N2CCollectionCount`, never through direct `.Count` on a value that may be a scalar. The validator audits its own AST for both forbidden patterns. This is a tooling rule rather than an import-schema field, but it is mandatory when extending the validator or adding a new manifest.

The `V193/` prefix present in the supplied manual JSON `import_scope` values is intentionally retained in Version 195. It is part of durable import identity, not release metadata. Renaming it during a version bump would cause repeat import to create new nodes instead of reusing the old ones.

## 5. Canonical UE4.27 pin examples

| Node | Canonical pins used by import JSON |
|---|---|
| `K2Node_Knot` | input `InputPin`, output `OutputPin` |
| `K2Node_FunctionEntry` | output exec `then`; signature input values use their declared names |
| `K2Node_FunctionResult` | input exec `execute`; signature output values use their declared names |
| `K2Node_IfThenElse` | `execute`, `Condition`, `then`, `else` |
| `K2Node_ExecutionSequence` | `execute`, `then_0`, `then_1`, ... |
| `K2Node_Select` | `Index`, `Option 0`, `Option 1`, ... and `ReturnValue` for non-enum Select; use exact enum entry PinNames for enum Select. `A`/`B` and False/True are compatibility aliases only. |
| `K2Node_GetArrayItem` | `Array`, `Dimension 1`, `Output` |
| array custom-thunk calls | normally `execute`, `then`, `TargetArray`, function-specific inputs, `ReturnValue` where present |
| `ArrayContains` / `ArrayFind` | `TargetArray`, `ItemToFind`; never guessed `Item` |
| `ArraySet` | `execute`, `then`, `TargetArray`, `Index`, `Item`, `bSizeToFit`; never display-style `SizeToFit` |
| `ArrayRemove` | `execute`, `then`, `TargetArray`, `IndexToRemove`; never guessed `Index` |
| `ArrayRemoveItem` | `execute`, `then`, `TargetArray`, `Item` |
| `K2Node_MakeStruct` | output pin is the exact `StructType->GetFName()` value, e.g. `ST_N2C_Final`; **not** `ReturnValue` |
| `K2Node_BreakStruct` | input pin is the exact `StructType->GetFName()` value, e.g. `ST_N2C_Final`; **not** generic `Struct` |
| `K2Node_SetFieldsInStruct` | `execute`, `then`, `StructRef`, selected member pins, `StructOut` |
| `K2Node_GetDataTableRow` | `execute`, `then` (Row Found), `RowNotFound`, `DataTable`, `RowName`, `ReturnValue` |
| `K2Node_SwitchEnum` | `execute`, `Selection`, and the exact enum value PinName such as `IE_Pressed`; do not write `Default` unless a verified export has that pin |
| `K2Node_ForEachElementInEnum` | `execute`, `SkipHidden`, `LoopBody`, `EnumValue`, `then` (Completed) |
| class/object/text data pins | author canonical JSON text, then let the importer use `GetPinDefaultValuesFromString`; class/object literals are normally stored in `DefaultObject` with empty `DefaultValue` |
| `K2Node_SpawnActorFromClass` | `execute`, `then`, `Class`, `SpawnTransform`, `ReturnValue`. `SpawnTransform` must have an incoming link; a literal default is not compile-safe in UE4.27. |
| `K2Node_CreateWidget` | `execute`, `then`, `Class`, `OwningPlayer`, `ReturnValue` |
| `K2Node_InputAction` | `Pressed`, `Released` |
| `K2Node_InputKey` | `Pressed`, `Released`, plus explicit modifier/consume/pause/override metadata in the node JSON |
| `K2Node_InputAxisEvent` | output exec `then`, value `AxisValue` |
| `K2Node_InputAxisKeyEvent` | output exec `then`, value `AxisValue` |

Aliases are compatibility helpers, not an authoring source. Always prefer the exported canonical pin `name`.

### Struct pin identity rule

UE4.27 `UK2Node_MakeStruct::AllocateDefaultPins()` and `UK2Node_BreakStruct::AllocateDefaultPins()` call `CreatePin(..., StructType->GetFName())`. Therefore the pin name depends on the concrete struct object. For `/Game/N2C_FinalManual/ST_N2C_Final.ST_N2C_Final`, both the Make output and Break input are `ST_N2C_Final`.

Correct connected chain:

```json
{"from_node_id":"MakeStruct","from_pin":"ST_N2C_Final","to_node_id":"SetFields","to_pin":"StructRef"}
{"from_node_id":"SetFields","from_pin":"StructOut","to_node_id":"BreakStruct","to_pin":"ST_N2C_Final"}
```

`ReturnValue` for MakeStruct and `Struct` for BreakStruct remain importer compatibility aliases for older generated patches, but new AI-authored JSON must not emit them. The release validator rejects those aliases in the final manual fixture.


## 6. Collapsed graph and Composite identities

UE4.27 owns a collapsed graph under a generated `UK2Node_Composite` UObject. The runtime path therefore contains an unstable segment such as:

```text
...:EventGraph.K2Node_Composite_0.N2C_FinalCollapsed
```

That numeric object name is not durable and may change after recreation or after another Composite node is inserted. For `bound_graph_identity`, use one of these two forms:

1. the exact identity copied from a current N2C export; or
2. the canonical identity with only `.K2Node_Composite_<n>` removed:

```text
/Game/N2C_FinalManual/BP_N2C_FinalActor.BP_N2C_FinalActor|collapsed_graph|N2C_FinalCollapsed|/Game/N2C_FinalManual/BP_N2C_FinalActor.BP_N2C_FinalActor:EventGraph.N2C_FinalCollapsed
```

Version 195 canonicalizes only that generated Composite segment. It still requires the exact Blueprint owner, `collapsed_graph` kind, parent graph path and collapsed graph name. Do not remove or guess any other path segment.

For repeat imports, keep one `create_collapsed_graph`/`replace_collapsed_graph` action per collapsed graph name. The dry-run must pass against both a new graph and an existing saved/reloaded Composite, and the second apply must leave exactly one matching Composite node.

## 7. Preconditions that JSON cannot create yet

- `InputAction` and `InputAxisEvent` require existing Project Settings mappings. Version 195 cannot create Action Mapping or Axis Mapping. Ask the user to create them, or use `InputKey` / `InputAxisKeyEvent` when that is an acceptable equivalent. Project mapping creation is P4.
- `ComponentBoundEvent` requires the component property to already exist in the compiled Blueprint class. Add the SCS component, compile and save, then import the event patch in a second pass.
- Delegate operation nodes require a resolvable existing dispatcher property. For a new dispatcher with a complex signature, ask the user to create/compile it first or split the workflow into creation, compile, then binding.
- Linked `GetDataTableRow` requires `row_struct_path` and a typed connection from `ReturnValue`/Out Row to a compatible struct pin.
- User Defined Struct member pins must use the exported friendly/internal name or persistent GUID.

## 8. Verified scope for ordinary strict JSON

The mandatory release tests cover: variables including arrays/sets/maps/raw Byte; function boundaries; Branch/Sequence/Select/Knot; array aliases and custom thunks; struct Make/Break/SetFields; enum literal/cast/compare/switch/foreach/name; literal and linked DataTable rows; CallFunction/Parent/OnMember/Message; DynamicCast/Self; existing delegates and ComponentBoundEvent; Input Action/Key/Axis/AxisKey; SpawnActor; CreateWidget; standard macros; Tunnel/Composite; Widget Blueprint, AIController and BT Blueprint subclasses; rollback and deferred disk restore.

**Release gate: `N2C_NEW_NODE_TEST_GATE`.** Version 195 contains 117 reusable import contracts covering all 38 currently supported action tokens, 63 constructor/alias families, and 29 mapped runtime guards. The matrix is bounded by the importer capabilities declared in the current source; it is not a claim that arbitrary malformed JSON or deferred UE systems are supported.

**Release gate: `N2C_NEW_NODE_TEST_GATE`.** A new node, alias, constructor, graph family, asset subclass or pin rule is not verified until it is added to `Source/Tests/Fixtures/N2C_NODE_TEST_REQUIREMENTS_V1.json`, a mandatory regression is registered, and this document is updated in the same release.

## 9. Unsupported or deferred requests and required alternatives

The AI must explicitly tell the user when one of these is requested:

- **Project patch creation/import of Struct and Enum assets:** not supported in the project-patch orchestrator. Alternative: create/import each existing UserDefinedStruct or UserDefinedEnum through its asset editor or selected-asset Content Browser command. P1 will add project-level Struct/Enum entries and tests.
- **Native C++ enum or native C++ struct mutation:** unsupported. Alternative: modify C++ source and rebuild, or use a UserDefinedEnum/UserDefinedStruct.
- **Creation of Project Settings Action/Axis mappings:** unsupported. Alternative: user creates mappings manually, or JSON uses direct key/axis-key events. P4.
- **Full AnimGraph/state-machine/transition fidelity:** deferred. Alternative: edit the Anim Blueprint manually or limit the request to the separately verified minimal graph family.
- **Behavior Tree asset graph, Blackboard or EQS editing:** unsupported. Alternative: create the BT/Blackboard/EQS assets manually; N2C can currently patch the Blueprint subclasses only.
- **Full Niagara graph round-trip:** deferred. Alternative: use the separate limited Niagara import/export workflow and explain its exact supported module/input scope.
- **InputTouch:** P5/deferred. Alternative: InputAction or InputKey where suitable.
- **Unsafe generic native MakeStruct/BreakStruct:** guarded. Alternative: use the specialized Kismet node/function for that native type.
- **Timeline and AddComponent K2 graph authoring outside a verified fixture:** do not claim production support. Alternative: SCS component hierarchy for components; timers/state variables/functions for timeline-like logic.
- **Function local defaults:** unsupported. Alternative: assign values inside the function graph.

Never silently omit an unsupported operation. State what cannot be done, why, and provide the closest supported implementation.

## 10. Final AI checklist

Before returning JSON, confirm:

- current export and engine target are UE4.27.2;
- correct schema and one entry per Blueprint path;
- exact asset/function/type identities;
- exact canonical pin names from export/reference source;
- no duplicate node IDs;
- every edge endpoint exists and directions are correct;
- full required exec/data flow;
- object/class literals use canonical paths and are expected to be schema-converted into `DefaultObject`;
- compiled member defaults are expected to persist on the generated-class CDO and exporter, not necessarily in `FBPVariableDescription::DefaultValue`;
- every SpawnActorFromClass has a linked SpawnTransform;
- every CreateDelegate is linked to a typed delegate input and names a bindable handler;
- external prerequisites are listed to the user;
- unsupported portions are disclosed with alternatives;
- dry run is expected to pass the transient sandbox before the user clicks OK.

## UE4.27 schema-native pin-default storage

The JSON value is an authored representation, not the final in-memory `UEdGraphPin` layout. Version 195 always asks `UEdGraphSchema_K2::GetPinDefaultValuesFromString` to produce the correct tuple and validates that tuple with `IsPinDefaultValid`. This prevents a valid class literal such as `/Script/Engine.Actor` from being rejected merely because the same path was also left in the string field.

Authoring examples:

```json
{"id":"SpawnActor","type":"K2Node_SpawnActorFromClass","pin_defaults":{"Class":"/Script/Engine.Actor"}}
{"id":"MakeTransform","type":"K2Node_CallFunction","function_path":"/Script/Engine.KismetMathLibrary.MakeTransform","pin_defaults":{"Rotation":"(Pitch=0,Yaw=0,Roll=0)"}}
```

Expected internal result:

- class/object: resolved UObject/UClass in `DefaultObject`, usually empty `DefaultValue`;
- text: schema-generated `DefaultTextValue`;
- scalar/enum/struct/soft reference: schema-approved string/object/text combination for that exact pin type.

Do not compare raw JSON text against only `UEdGraphPin::DefaultValue` after import. Compare semantic type and resolved object/value, compile, save, unload, reopen, and export.

## UE4.27 CreateDelegate lifecycle and pin identity

`K2Node_CreateDelegate` uses the canonical internal output pin name `OutputDelegate` (the editor displays it as **Event**). Author JSON edges as `CreateDelegate.OutputDelegate -> AddDelegate.Delegate` or `RemoveDelegate.Delegate`. The importer also accepts legacy `Delegate`/`Event` aliases for compatibility, but exports, fixtures and documentation use `OutputDelegate`.

The selected handler is only valid after `OutputDelegate` is connected to a typed delegate input. A handler function may be created earlier in the same patch; keep the action order `add_or_replace_function` before `patch_graph`. The importer defers signature-aware validation until the delegate edges exist. Do not emit an unconnected CreateDelegate presence probe.

For struct values, provide normal UE ImportText, but do not guess that a visually plausible string is valid. Version 195 parses named `FVector`/`FRotator` input and normalizes it to the UE4.27 K2 custom CSV representation (`X,Y,Z` / `Pitch,Yaw,Roll`) before passing it to the Blueprint schema. Every non-wildcard authored pin default is then converted by `UEdGraphSchema_K2::GetPinDefaultValuesFromString` into the correct three-part storage tuple: `DefaultValue`, `DefaultObject`, and `DefaultTextValue`. The converted tuple is validated with `IsPinDefaultValid` before assignment.

This distinction is mandatory for object and class pins. JSON should contain a canonical object/class path such as `/Script/Engine.Actor`, but the imported UE pin normally stores that literal in `DefaultObject` while leaving the string `DefaultValue` empty. Never require the authored path to remain in `DefaultValue`, and never write both fields manually. Text pins similarly use the schema-provided `DefaultTextValue`. Invalid/trailing member struct text is rejected with `member_default_import_text_invalid` / `member_default_import_text_trailing_data`; invalid node pin defaults fail before target mutation with `pin_default_invalid`.

After Blueprint compilation, `FBPVariableDescription::DefaultValue` is not an authoritative persistence check and may be empty. Verify a compiled member default from the generated-class CDO (`GeneratedClass->GetDefaultObject()` plus the reflected property) and from a fresh N2C export after save/unload/reopen.

## UE4.27 SpawnActor transform contract

`K2Node_SpawnActorFromClass.SpawnTransform` is mandatory and must be connected to a transform output. The canonical fixture uses:

```text
KismetMathLibrary.MakeTransform.ReturnValue -> SpawnActor.SpawnTransform
```

Do not put a serialized transform into `SpawnActor.pin_defaults.SpawnTransform`. UE4.27 expands SpawnActor into begin/finish spawn function calls whose transform parameters are passed by reference; an unconnected literal reaches compile as an invalid by-ref default. Strict preflight rejects this form with `spawn_transform_link_missing` before target mutation.

## Deferred restore lifecycle

When verified in-memory Undo fails, the backup manifest is applied only on a fresh UE startup before the asset is reopened. Do not expect or implement a same-process `OnPreExit` copy: UE4.27 may still hold the package file during shutdown.


## Verification snapshot boundary

The release harness keeps two comparisons:

- exact fresh-process snapshots, which verify persisted structure after save/reopen;
- semantic reapply snapshots, which ignore unstable editor GUIDs and inactive defaults on pins that already have incoming links, while still checking node/pin identity, link topology, unlinked defaults, variables, functions, graphs, components and interfaces.

Current Version 195 snapshot normalization also canonicalizes comma-separated numeric tuple text broadly. Treat this as verification infrastructure, not as permission to rewrite String/Name/Text literals. A later release should restrict numeric tuple normalization to proven struct-pin representations and add a regression ensuring a string such as `1.0,2.00` remains distinct from `1,2`.

## Release metadata consistency

`NodeToCode.uplugin` is the current release source of truth. The required manual file is `N2C_FINAL_MANUAL_ALL_STRICT_SUPPORTED_V<Version>.json`; its `target_plugin_version` and `target_plugin_version_name`, the ManualReplay manifest and the node requirement matrix must match the descriptor. Do not copy a previous release number into validator code or package include lists.
