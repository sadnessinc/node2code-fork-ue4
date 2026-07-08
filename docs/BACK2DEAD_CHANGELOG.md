# Back2Dead fork changelog

## 2026-07-08 — safety-first Blueprint export/import MVP

Added:
- `N2C_AI_EXPORT_V2` AI-friendly Blueprint export.
- Per-function JSON split files.
- Safe read-only selected Niagara reflection export.
- `N2C_PATCH_V1` strict Blueprint patch importer.
- Replace/add function graph action support.
- Backup-before-mutation flow.
- Dry-run before apply from toolbar.
- Two-button Blueprint toolbar: **Export N2C** and **Import N2C**.
- Fork attribution documentation for Nick McClure / Protospatial and sadnessinc.

Changed:
- Blueprint toolbar no longer exposes old Node to Code actions in this fork.
- Exec pins now serialize `type = Exec` for JSON round-trip safety.
- Build dependencies include `ContentBrowser` and `DesktopPlatform`.

Removed from Blueprint toolbar:
- old translate action;
- old dropdown;
- old raw JSON export button;
- old editor window button.

Not removed from source:
- original LLM/provider code remains for compatibility with the original plugin source tree.
- original README content remains, with fork note added at the top.

Safety limitations:
- Niagara import/mutation is not implemented.
- Complex Blueprint node types are intentionally not guessed.
- Unreal Engine compilation was not run in this packaging environment; verify inside UE4.27 after installing the plugin.

## Safety pass 2

- Hardened `N2C_PATCH_V1` validation.
- Changed `replace_function_body` to preserve the existing graph/signature nodes.
- Added duplicate signature pin protection.
- Added optional `target_blueprint` mismatch guard.
- Added simple `pin_defaults` support.
- Clarified documentation and check report.

## 2026-07-08 — UE4.27 compile hotfix

- Fixed `N2CAIExport.cpp` Content Browser selection code to avoid direct `IContentBrowserSingleton` usage that can fail as an incomplete type in UE4.27 toolchains.
- Fixed `N2CPatchImporter.cpp` `FBlueprintEditorUtils::AddFunctionGraph` call by explicitly specifying the template signature type (`UFunction`) instead of relying on `nullptr` template inference.
- Release zip root folder renamed to `node2code/` for easier drop-in install as `Project/Plugins/node2code`.


## 2026-07-08 local variable importer hotfix

- Added `local_variables` / `locals` support to N2C_PATCH_V1 function actions.
- Re-importing the same patch skips duplicate local variables.
- Signature input/output additions now report added pins in the patch report.

## Local Variables Compile Hotfix 2

- Fixed UE4.27 compile error: `UEdGraph::LocalVariables` does not exist.
- Local variable duplicate detection now uses `FBlueprintEditorUtils::FindLocalVariable(Blueprint, Graph, Name, &FunctionEntry)`.
- Added `DoesSupportLocalVariables(Graph)` guard before calling `AddLocalVariable`.
- `AddLocalVariable` return value is now checked and reported.

## Back2Dead/sadnessinc varfolders hotfix

Added:
- automatic ZIP packaging after `Export N2C`;
- patch action `add_member_variables` / `add_variables`;
- root-level `variables[]` support;
- patch action `rename_function`;
- patch action `set_function_category` / `move_function_to_category`;
- `category` / `folder` support on function patch actions.

Safety:
- rename validates source/destination before mutation;
- member variables are duplicate-safe using `FindNewVariableIndex`;
- function/variable categories use UE editor helper APIs;
- project backup still happens before mutation.


## 2026-07-08 — UE4.27 rename_function compile fix

- Removed unsupported `FBlueprintEditorUtils::ReplaceFunctionReferences` call. This API is not available in UE4.27.
- Added local `ReplaceSelfFunctionCallReferences` fallback:
  - scans FunctionGraphs, UbergraphPages and MacroGraphs;
  - finds `UK2Node_CallFunction` nodes with the old self-member function name;
  - rewrites them to the new self-member function name;
  - reconstructs updated call nodes.
- `rename_function` now remains UE4.27-safe while still updating calls inside the current Blueprint.

## 2026-07-08 — Export crash / native ZIP hotfix

- Fixed UE4.27 breakpoint/assert during AI-friendly Blueprint export when a member variable has no `tooltip` metadata.
  - `FBPVariableDescription::GetMetaData()` is no longer called blindly.
  - Metadata is read by scanning `MetaDataArray` safely.
- Replaced PowerShell `Compress-Archive` ZIP packaging with a native simple ZIP32 writer.
  - Export now writes `.zip` directly from C++ without depending on PowerShell/7z/shell extensions.
  - ZIP uses store/no-compression mode for safety and predictable behavior.
- Normalized imported member variable defaults for Integer/Byte/Boolean variables.
  - Example: `7.0` becomes `7` for Integer variables to avoid Blueprint compiler warnings.

## 2026-07-08 — Export metadata / UX / toolbar hotfix

Added:
- function category export via Blueprint graph function metadata;
- function local variables export through `local_variables[]`;
- readable export naming: `N2C_<BlueprintName>_<YYYYMMDD_HHMMSS>.json/.zip`;
- clearer import dialogs with top-level `OK` / `NOT OK`, `Warnings`, and `What was done` sections;
- toolbar labels changed to `Export N2C` and `Import N2C`;
- separate toolbar icons: green outgoing arrow for export, red incoming arrow for import.

Notes:
- export still writes a folder and a zip; the zip path uses the same readable timestamp as the folder.
- UE4.27 compile must still be verified inside Unreal Editor.

## 2026-07-08 — UE4.27 function metadata compile fix

- Fixed UE4.27 compile error: `FKismetUserDeclaredFunctionMetadata::bThreadSafe` does not exist in UE4.27.
- `thread_safe` remains in exported JSON as a stable schema key, but is exported as `false` instead of reading a missing engine field.
- Function category/local_variables export remains enabled.
