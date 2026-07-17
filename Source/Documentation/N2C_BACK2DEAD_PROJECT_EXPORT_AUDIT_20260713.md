> **Historical record.** This file describes an earlier release or evidence run. It is not authoritative for Version 173. See `N2C_CURRENT_STATE.md` and `N2C_DOCUMENT_AUTHORITY.md`.

# Back2Dead full project export audit — 2026-07-13

Input: `N2C_Project_20260713_232723.zip`  
Engine: UE4.27.2  
Plugin descriptor during export: `170 / 1.2.70-ue427-p1-enum-verified`

## Integrity

- ZIP method: store/no compression.
- Archive size: 279,117,570 bytes (about 266.19 MiB).
- Entries / JSON files: 1,362.
- JSON parse errors: 0.
- Duplicate archive paths: 0.
- Manifest: `N2C_PROJECT_EXPORT_V2`.
- Exported: 303/303 supported assets, skipped 0.
- Blueprints: 156.
- Niagara Systems: 82.
- Enums: 37.
- Structs: 28.

## Why the archive grew

The prior verified export contained 1,124 files: 156 Blueprint roots, 902 function sidecars, 37 Enums, 28 Structs and one manifest. It contained no Niagara Systems and no per-Blueprint coverage files.

The new export contains exactly 238 additional files:

- 82 Niagara System JSON files — 23.78 MiB;
- 156 `N2C_COVERAGE_V1` sidecars — 15.88 MiB.

These new files add about 39.66 MiB by themselves. The remaining observed growth versus the owner's older archive comes from richer P0-era metadata in the existing Blueprint/root/function JSON records. A byte-exact attribution of that residual requires the old ZIP itself, but the project asset counts and Blueprint/function counts are unchanged. The growth is exporter-format growth, not evidence that Back2Dead content changed.

Current size composition:

- Blueprint root JSON: 142.70 MiB;
- function sidecars: 83.48 MiB;
- coverage sidecars: 15.88 MiB;
- Niagara JSON: 23.78 MiB;
- Enum, Struct and manifest: about 0.35 MiB.

## New P0 blocker found in real project data

### Struct nodes — 461

- `K2Node_BreakStruct`: 399, all guarded for missing `struct_path`;
- `K2Node_MakeStruct`: 30, all guarded for missing `struct_path`;
- `K2Node_SetFieldsInStruct`: 32, all guarded for missing `struct_path`.

### Enum-related nodes — 195

- `K2Node_EnumEquality`: 94, guarded for missing `enum_path`;
- `K2Node_SwitchEnum`: 57, guarded for missing `enum_path`;
- `K2Node_EnumInequality`: 15, guarded for missing `enum_path`;
- `K2Node_GetEnumeratorNameAsString`: 19, unsupported;
- `K2Node_ForEachElementInEnum`: 7, unsupported;
- `K2Node_CastByteToEnum`: 2, unsupported;
- `K2Node_EnumLiteral`: 1, unsupported.

### DataTable nodes — 26

- `K2Node_GetDataTableRow`: 26, all guarded for missing `data_table_path` and `row_struct_path`.

Total: **682 runtime-relevant instances across 42 Blueprint assets**. They are used in core Back2Dead Blueprints and are now an explicit P0 release blocker.

## Decision

- Ordinary-Blueprint cross-owner macros remain an expected reject imposed by stock UE4.27 and do not block release.
- P0 is not complete until Struct/Enum/DataTable specialised nodes have durable export identity, exact constructors, no-mutation rejects, fresh-process persistence fixtures and zero related blockers in a new Back2Dead coverage export.
