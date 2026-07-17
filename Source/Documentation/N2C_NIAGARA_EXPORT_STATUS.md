# N2C Niagara Export Status

This document records the current expected behavior of the N2C Niagara exporter/importer in the node2code fork.

## Current default export profile

Default export is compact and AI-readable.

Expected size for a normal 3-emitter Niagara effect is roughly:

```text
300 KB - 600 KB JSON
```

If a normal effect exports as many megabytes, check for accidental deep reflection dumps.

## Main sections

```text
niagara_asset.import_actions
  Main active import/reimport actions.

niagara_asset.import_actions.input_overrides
  Only values that importer can safely recreate.

niagara_asset.import_actions.skipped_input_overrides
  Visible Niagara inputs/trees that exporter can see but importer cannot safely recreate yet.
  This is where FloatFromCurve and curve-driven inputs belong for now.

niagara_asset.niagara_summary.readable_stack
  Main human/AI view of modules and values.

niagara_asset.niagara_summary.local_subobjects
  Compact diagnostics only. Do not dump full graph/node reflection here.
```

## What must be exported

```text
- System State / Emitter State where available
- emitter list and emitter names
- module order per stack
- user parameters
- active input overrides
- skipped/debug curve inputs
- Sprite Renderer properties:
  - FacingMode
  - Alignment
  - SortMode
  - SourceMode
  - SubImageSize
  - ColorBinding
  - SpriteSizeBinding
  - NormalizedAgeBinding
- SubUVAnimation presence and key values
```

## What must not be dumped by default

```text
- full NiagaraGraph reflection
- all NiagaraNode object dumps
- full renderer UObject reflection
- full local object compact_export arrays
- duplicate niagara_summary.emitters/system_scripts deep dumps when readable_stack already contains the readable data
```

## Current known-good behavior

```text
- single selected Niagara export works
- Niagara Editor export button works
- Content Browser folder RMB export works for recursively exporting all UNiagaraSystem assets in selected folders
- compact export keeps normal effects below multi-MB size
- FloatFromCurve/curves are preserved for analysis in skipped_input_overrides
- Sprite Renderer FacingMode is visible in export
- ScaleColor is not required for the current user-edited explosion; do not force-add it
- GravityForce can be present in source assets; do not force-delete it during import unless explicitly required
```

## Current limitation

The exporter can see curve-driven values, but importer does not yet fully recreate them as production-safe Niagara dynamic input nodes.

For now:

```text
FloatFromCurve / curve data = readable/skipped/debug export
literal, parameter, safe dynamic_input_tree functions = active reimport
```

## Next importer improvement target

Add safe reimport support for:

```text
FloatFromCurve
ScaleSpriteSize curve inputs
ScaleColor color/alpha curves
VectorFromCurve
curve data interfaces used by visible Niagara inputs
```

Until this is implemented, these entries must remain in `skipped_input_overrides` and must not be silently omitted.
