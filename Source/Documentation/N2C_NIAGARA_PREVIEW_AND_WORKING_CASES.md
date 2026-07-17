# N2C Niagara Preview / GIF validation and current working cases

## Always generate visual preview artifacts

For every non-trivial Niagara import JSON created by ChatGPT / N2C assistant, generate two local preview artifacts before sending the JSON:

```text
1. pseudo-sim GIF
2. contact-sheet PNG
```

The preview is **not** a real Unreal/Niagara renderer. It is a deterministic CPU-side approximation used to catch obvious design mistakes before the user imports the JSON into UE4.27:

```text
- particles start from the intended radius/point
- radial velocity points in the expected direction
- gravity is not accidentally pulling the whole effect down
- drag/curl/fade timing is plausible
- smoke/sparks/core layers are visually separated
- alpha/color values are not zero
```

The current Python preview model should approximate these Niagara concepts:

```text
SphereLocation        -> random points in a sphere/shell
AddVelocityFromPoint  -> radial velocity away from origin
GravityForce          -> constant acceleration
Drag                  -> exponential/linear damping approximation
CurlNoiseForce        -> smooth pseudo-noise displacement/acceleration
Color                 -> color over normalized age
Scale Sprite Size     -> size over normalized age
```

Minimum expected files for effect-delivery:

```text
N2C_<effect_name>_pseudo_sim.gif
N2C_<effect_name>_pseudo_sim_contact_sheet.png
N2C_<effect_name>_validation.json
```

If the pseudo-sim does not look roughly correct, do not send the JSON as final. Adjust the JSON first.

## Current working Niagara import/export behavior

The following cases are confirmed by user round-trips during the Niagara import/export work:

```text
- Plugin can compile after v26 link fix: no direct call to FNiagaraUserRedirectionParameterStore::RecreateRedirections.
- Export/import buttons are usable from Niagara Editor.
- Single-emitter point burst can be imported and made visible.
- User parameters for BurstStrength and StartRadius can drive AddVelocityFromPoint.Velocity Strength and SphereLocation.Sphere Radius.
- Extra emitters can be duplicated to produce a three-emitter system.
- Three-emitter setup direction is valid enough for iteration: core burst + smoke + sparks.
- ScaleColor should not be used in this effect because it can wash/whiten the result.
- GravityForce is risky in this asset: do not force-delete it; do not rely on enabling it for the final look.
- Sprite renderer FacingMode matters and must be exported for audit.
```

## Current exporter rule for full Niagara export

Active/reimportable formulas belong in:

```text
niagara_asset.import_actions.input_overrides
```

Visible but not yet safely reimportable graph shapes, such as `FloatFromCurve` and curve-node inputs, must not be silently dropped. Export them into:

```text
niagara_asset.import_actions.skipped_input_overrides
niagara_asset.niagara_summary.local_subobjects
```

This keeps the export complete for inspection while avoiding unsafe reimport of curve nodes until the importer explicitly supports them.

## Known good module patterns from current user-edited effect

The user-edited first emitter contains useful patterns that must be preserved by export:

```text
Color001.Color:
  Lerp Linear Colors
    StartColor = User.N2C_ColorHot
    EndColor   = User.N2C_ColorSmoke
    LerpFactor = Particles.NormalizedAge

Color.Color:
  Lerp Linear Colors / Float from Curve pattern exists in the stack and must be exported for debug if it is not reimportable.

Scale Sprite Size:
  Uniform Curve Sprite Scale driven by Particles.NormalizedAge.
```

When `FloatFromCurve` cannot be recreated safely, export it as debug/non-reimportable rather than omitting it.

---

## v31 Export Size Policy

Default Niagara export must stay readable and compact.

Do not dump full NiagaraGraph / NiagaraNode / Renderer reflection into `niagara_summary.local_subobjects` by default. It makes normal exports too large and mostly duplicates `readable_stack` and `import_actions`.

Current rule:

```text
readable_stack                 = main human/AI view
import_actions.input_overrides = active/reimportable dynamic inputs
import_actions.skipped_input_overrides = visible but not safely reimportable dynamic inputs
niagara_summary.local_subobjects = compact diagnostics only
```

For `FloatFromCurve` / curve inputs that are not safely reimportable yet, export the curve information in `skipped_input_overrides[*].tree...node_input_debug.data_interface`, not by dumping the entire graph.


## v32 export size policy

Default Niagara exports must stay compact. Do not export deep duplicate `niagara_summary.system_scripts` or `niagara_summary.emitters` arrays in normal AI-facing exports. The source of truth for reading is `niagara_summary.readable_stack`, while curve/dynamic-input debug that cannot yet be safely reimported belongs in `import_actions.skipped_input_overrides` and compact `niagara_summary.local_subobjects`.

Expected size for the current three-emitter explosion export is roughly a few hundred KB, not multi-MB.


## v34 Reference library status

The first reference pack is documented in:

```text
Source/Documentation/N2C_NIAGARA_VFX_REFERENCE_LIBRARY.md
Source/Documentation/N2C_MagicalStaffAuraVFX_reference_breakdown.md
```

Reference source:

```text
N2C_FolderNiagara__Game_Test_vfx_20260710_012416.zip
```

It contains ten staff aura effects and should be used as a reference library for future N2C Niagara generation:

```text
ButterflyStaffAura
ElectricStaffAura
FireStaffAura
IceStaffAura
LightStaffAura
MagmaStaffAura
MysticStaffAura
NatureStaffAura
PoisonStaffAura
VampiricStaffAura
```

Current export status is documented in:

```text
Source/Documentation/N2C_NIAGARA_EXPORT_STATUS.md
```

Important rule: reference exports may contain `FloatFromCurve` and curve data in `skipped_input_overrides`. This is acceptable for reference/analysis, but not yet fully reimportable.
