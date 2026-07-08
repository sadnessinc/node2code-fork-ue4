# NodeToCode UE4 — Back2Dead / sadnessinc fork notes

This repository is a fork/custom branch of the original **Node to Code** plugin by **Nick McClure / Protospatial**.

Original project:
- Author: Nick McClure / Protospatial
- Original plugin: Node to Code
- License: Apache License 2.0, kept in `LICENSE`

Back2Dead fork/customization:
- Fork/customization owner: **sadnessinc**
- Purpose: safe AI-friendly Blueprint export and strict patch-based Blueprint import for the Back2Dead UE4.27 Blueprint workflow.
- Main goal: reduce manual Blueprint rewriting by exporting readable Blueprint JSON and importing controlled `N2C_PATCH_V1` patches.

Important license/attribution note:
- Do not remove the original copyright/license headers.
- Keep the Apache-2.0 `LICENSE` file when publishing this fork.
- Keep this fork note so GitHub users can distinguish original Node to Code features from Back2Dead-specific changes.

---

## What changed in this fork

Blueprint Editor toolbar was simplified to exactly two buttons:

1. **Export N2C**
   - Exports the currently opened Blueprint as `N2C_AI_EXPORT_V2`.
   - Saves the full JSON export.
   - Copies the full JSON export to clipboard.
   - Saves per-function split JSON files into a `functions/` subfolder.
   - Tries to export selected Niagara assets from Content Browser through safe read-only reflection if any Niagara assets are selected.

2. **Import N2C**
   - Reads `N2C_PATCH_V1` from clipboard first.
   - If clipboard is invalid, opens a `.json` file picker.
   - Runs dry-run validation first.
   - Shows a confirmation dialog.
   - Creates a `.uasset` backup before mutating the Blueprint.
   - Applies supported patch actions.
   - Requests Blueprint compile after patch apply.

Removed from the Blueprint toolbar:
- old direct translate button;
- old dropdown menu;
- old “open Node to Code editor” toolbar entry;
- old raw JSON button.

The old source files remain in the plugin for compatibility, but they are not exposed in the Blueprint toolbar in this fork.

The export button exports only the current Blueprint and, optionally, selected Niagara assets. It does not dump unrelated Blueprints, unrelated AI systems, or LLM/provider data.

---

## New / modified source files

### `Source/Public/Core/N2CAIExport.h`
Declares the AI-friendly exporter.

Main methods:
- `BuildBlueprintAIJson`
- `SaveJsonToFile`
- `BuildSelectedNiagaraAIJson`

### `Source/Private/Core/N2CAIExport.cpp`
Builds `N2C_AI_EXPORT_V2` JSON.

Export contains:
- metadata;
- Blueprint variables;
- functions / event graphs / macros / delegate graphs;
- nodes;
- pins;
- pin types;
- readable exec/data edges;
- simple `linear_flow` traversal;
- enum internal/display names when available.

Niagara export is read-only reflection only. It does **not** mutate Niagara graphs and does **not** try to reconstruct Niagara internals.

### `Source/Public/Core/N2CPatchImporter.h`
Declares the strict patch importer for `N2C_PATCH_V1`.

Main methods:
- `DryRunPatch`
- `ApplyPatchToBlueprint`

### `Source/Private/Core/N2CPatchImporter.cpp`
Implements conservative Blueprint patch import.

Supported actions:
- `add_or_replace_function`
- `replace_function_body`

Supported node creation MVP:
- function entry;
- function result / return;
- branch;
- call function;
- variable get;
- variable set;
- exec/data edge connection by loose pin names.

Unsupported nodes are not guessed. They are reported as warnings instead of unsafe graph mutation.

### `Source/Public/Core/N2CToolbarCommand.h`
Toolbar command list was simplified to:
- `ExportCommand`
- `ImportCommand`

### `Source/Private/Core/N2CToolbarCommand.cpp`
Registers only:
- `Export N2C`
- `Import N2C`

### `Source/Private/Core/N2CEditorIntegration.cpp`
The Blueprint toolbar now displays exactly two buttons.

Added methods:
- `ExecuteBack2DeadExportForEditor`
- `ExecuteBack2DeadImportForEditor`

### `Source/Private/Core/N2CSerializer.cpp`
Changed JSON serialization for pins:
- Every pin now writes the `type` field, including `Exec` pins.

Reason:
- `FromJson` expects the `type` field.
- Omitting type for Exec pins breaks round-trip readers and patch tooling.

### `Source/NodeToCode.Build.cs`
Added dependencies:
- `ContentBrowser`
- `DesktopPlatform`

They are required for selected Niagara asset export and patch `.json` file picker.

---

## Export output location

Blueprint export button writes to:

```text
Saved/NodeToCode/AIExports/N2C_AI_<BlueprintName>_<Timestamp>/
```

Inside:

```text
N2C_AI_<BlueprintName>_<Timestamp>.json
functions/
    001_<FunctionName>.json
    002_<FunctionName>.json
    ...
N2C_AI_SELECTED_NIAGARA_<Timestamp>.json    // only if Niagara assets were selected
```

Full Blueprint JSON is also copied to clipboard.

---

## Import safety rules

Before modifying a Blueprint, the importer must:

1. Parse JSON.
2. Check `schema == "N2C_PATCH_V1"`.
3. Check `actions[]`.
4. Run dry-run.
5. Ask user confirmation.
6. Create `.uasset` backup.
7. Start scoped transaction.
8. Apply supported actions.
9. Mark Blueprint structurally modified.
10. Request Blueprint compile.

Backups are written to:

```text
Saved/NodeToCode/Backups/<BlueprintName>_<Timestamp>.uasset
```

If backup fails, patch apply is aborted before mutation.

---

## Current limitations

This fork is intentionally conservative.

Patch importer MVP does **not** fully support:
- timelines;
- latent actions;
- custom event dispatcher binding;
- complex macro internals;
- widget designer layout;
- AnimGraph reconstruction;
- Behavior Tree asset reconstruction;
- Niagara graph import/mutation.

Niagara support is export-only and read-only.

The importer can create common Blueprint function scaffolds and simple logic, but complex functions may still need manual cleanup after import.

---

## Recommended GitHub description

```text
NodeToCode UE4 Back2Dead fork by sadnessinc — AI-friendly Blueprint export and safety-first JSON patch import for UE4.27 Blueprint workflows. Based on Node to Code by Nick McClure / Protospatial, Apache-2.0.
```

---

## Safety revision after code review

The second safety pass changed the patch importer behavior:

- `replace_function_body` preserves the existing function graph and signature nodes instead of deleting the whole graph.
- Dry-run validates patch structure deeply before the user confirmation dialog.
- Apply repeats validation before creating backup and before mutation.
- Existing signature pins are not duplicated during repeated imports.
- Optional `target_blueprint` can be used as a guard against applying a patch to the wrong Blueprint.
- `pin_defaults` lets patches set simple default values on Return/Branch/CallFunction pins.

This keeps the importer conservative. Unsupported node types fail validation instead of being guessed.


## 2026-07-08 local variable importer hotfix

- Added `local_variables` / `locals` support to N2C_PATCH_V1 function actions.
- Re-importing the same patch skips duplicate local variables.
- Signature input/output additions now report added pins in the patch report.
