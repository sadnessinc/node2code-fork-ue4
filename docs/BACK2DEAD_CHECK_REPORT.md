# Back2Dead fork check report

Date: 2026-07-08

## Checked in this packaging environment

Passed:
- zip archive integrity check;
- JSON validity check for `NodeToCode.uplugin`;
- JSON validity check for `examples/N2C_PATCH_V1_minimal_function.json`;
- source diff check against original uploaded plugin;
- toolbar command reference check: old `OpenWindowCommand`, `CollectNodesCommand`, `CopyJsonCommand` references were removed from active toolbar code;
- basic bracket balance check for newly added/modified Back2Dead files;
- documentation files are present.

## Not checked here

Unreal Engine C++ compile was **not** run in this container because UE4.27 source/build tools are not available here.

Required final check inside Unreal:
1. Put plugin into project `Plugins/NodeToCode` or engine plugin folder.
2. Delete old plugin `Binaries` / `Intermediate` if present.
3. Regenerate project files if using C++ project.
4. Start UE4.27 and let it rebuild plugin.
5. Open any Blueprint.
6. Confirm toolbar shows exactly:
   - `Export N2C`
   - `Import N2C`
7. Click `Export N2C` and verify output appears in:
   - `Saved/NodeToCode/AIExports/...`
8. Copy `examples/N2C_PATCH_V1_minimal_function.json` into clipboard or load it through `Import N2C`.
9. Confirm dry-run dialog appears.
10. Confirm backup appears in:
    - `Saved/NodeToCode/Backups/...`
11. Confirm Blueprint compiler output after patch apply.

## Known conservative limitations

- Niagara is export-only by safe reflection.
- Niagara import/mutation is intentionally not implemented.
- Complex Blueprint nodes are intentionally not guessed.
- Unsupported patch node types produce warnings and are skipped.
- If Unreal compiler reports errors after patch apply, use the generated backup and/or undo transaction.


---

## Additional safety review / fixes

Performed after the second review request.

Fixed issues:

1. `replace_function_body` was too destructive.
   - Before: removed the whole function graph and recreated it.
   - Now: clears only non-signature body nodes, breaks old links, and preserves existing Entry/Return signature nodes.

2. Dry-run was too permissive.
   - Before: dry-run mostly checked schema/actions and reported that a function would be patched.
   - Now: validates action types, existing function requirement for `replace_function_body`, node IDs, duplicate IDs, supported node types, required Entry/Return, and edge node references.

3. Repeated patch import could duplicate signature pins.
   - Before: `CreateUserDefinedPin` was called every time.
   - Now: existing Entry output pins / Return input pins are detected and duplicate pins are skipped.

4. Patch could be applied to the wrong Blueprint by mistake.
   - Added optional `target_blueprint` guard. If present, it must match current Blueprint name or path.

5. Return/default values in patch nodes were ignored.
   - Added `pin_defaults` support and compact scalar defaults on node JSON fields.

Remaining limitation:

- UE4.27 C++ compilation and live Blueprint graph mutation tests must still be run inside Unreal Engine. This repository-level check validates source structure and static safety only.

================================================================================
# 2026-07-08 UE4.27 compile hotfix check

User-reported compile errors fixed:

1. `N2CAIExport.cpp` C2027 undefined `IContentBrowserSingleton`
   - Removed direct `ContentBrowserModule.Get().GetSelectedAssets(...)` usage.
   - Replaced with `GEditor->GetContentBrowserSelections(SelectedAssets)`.
   - Added `#include "Editor.h"`.

2. `N2CPatchImporter.cpp` C2672/C2784 `AddFunctionGraph` overload/template inference
   - Replaced ambiguous/null signature call with explicit template call:
     `FBlueprintEditorUtils::AddFunctionGraph<UFunction>(Blueprint, Graph, true, SignatureFunction);`

Packaging change:
   - Zip root folder is now `node2code/`.

Local checks performed without Unreal Engine:
   - Verified removed direct references to `IContentBrowserSingleton` / `ContentBrowserModule` from source.
   - Verified `AddFunctionGraph` call no longer passes raw `nullptr`.
   - Verified example JSON remains valid.
   - Verified new zip integrity after packaging.


## 2026-07-08 local variable importer hotfix

- Added `local_variables` / `locals` support to N2C_PATCH_V1 function actions.
- Re-importing the same patch skips duplicate local variables.
- Signature input/output additions now report added pins in the patch report.

## Local Variables Compile Hotfix 2

User-reported UE4.27 error:

```text
C2039: "LocalVariables" is not a member of "UEdGraph"
```

Fix:

- Removed direct access to `Graph->LocalVariables`.
- Replaced duplicate check with `FBlueprintEditorUtils::FindLocalVariable(...)`.
- Added local-variable support guard through `FBlueprintEditorUtils::DoesSupportLocalVariables(...)`.

Limitation:

- Unreal Engine compile still must be verified inside UE4.27.

## varfolders hotfix static check

Checked by static scan in container:
- no `Graph->LocalVariables` usage;
- no `IContentBrowserSingleton` direct usage;
- no `AddFunctionGraph(..., nullptr)` template inference issue;
- JSON test patch validates through `json.tool`;
- plugin folder in release zip remains `node2code/`.

Not checked in container:
- UE4.27 C++ compile;
- live Blueprint editor mutation;
- native C++ ZIP writing inside Unreal Editor.


## 2026-07-08 — Compile fix after C2039 ReplaceFunctionReferences

Issue:
- UE4.27 does not expose `FBlueprintEditorUtils::ReplaceFunctionReferences`.

Fix:
- Replaced it with an internal scanner over the current Blueprint graphs.
- The scanner updates self function calls from old function name to new function name after graph rename.

Limit:
- This updates references inside the same Blueprint only. Cross-Blueprint references are intentionally not mutated for safety.

## 2026-07-08 — Metadata/UX hotfix static check

Changed files:
- `Source/Private/Core/N2CAIExport.cpp`
- `Source/Private/Core/N2CEditorIntegration.cpp`
- `Source/Private/Core/N2CToolbarCommand.cpp`
- `Source/Public/Models/N2CStyle.cpp`
- `Resources/export_arrow_green.png`
- `Resources/import_arrow_red.png`

Static checks performed:
- verified zip package structure remains `node2code/` at archive root;
- verified new icon files exist and are PNG RGBA;
- verified no PowerShell/7z dependency was reintroduced for export zip;
- verified old button labels `Export N2C` / `Import N2C` were replaced in toolbar command definitions;
- verified function export now writes `category` and `local_variables` fields.

Limitations:
- Unreal Engine 4.27 C++ compilation cannot be executed in this packaging environment.
- Final validation must be done by rebuilding the plugin inside the user's UE4.27 project.

## 2026-07-08 — Metadata/UX compilefix static check

User-reported UE4.27 error:

```text
C2039: "bThreadSafe" is not a member of "FKismetUserDeclaredFunctionMetadata"
```

Fix:
- removed access to `MetaData->bThreadSafe`;
- kept JSON key `thread_safe` with safe default value `false`;
- no other metadata export fields were changed.

Still requires:
- final UE4.27 rebuild in user project.
