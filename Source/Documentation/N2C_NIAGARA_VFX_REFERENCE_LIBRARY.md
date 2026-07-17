# N2C Niagara VFX Reference Library

This document defines how exported Niagara effects are used as reference material for future N2C JSON generation.

## Reference workflow

A good VFX reference folder should contain:

```text
Examples/NiagaraVFX/<EffectName>/
  export.zip or export.json
  preview.gif
  contact_sheet.png
  notes.md
```

The exporter output is used as the technical source of truth:

```text
- emitter count and emitter roles
- module stack order
- spawn model: SpawnRate / SpawnBurst
- location model: SphereLocation / other location modules
- movement modules: CurlNoiseForce, Drag, VortexVelocity, WindForce, PointAttractionForce
- color and fade modules
- sprite renderer properties, especially FacingMode and SubImageSize
- curve-driven values exported into skipped_input_overrides/debug when not reimportable yet
```

The GIF/contact-sheet preview is used only for visual classification and sanity checks.

## Current reference pack: MagicalStaffAuraVFX

Source archive used as the first reference pack:

```text
N2C_FolderNiagara__Game_Test_vfx_20260710_012416.zip
```

The pack contains ten staff-aura style Niagara systems:

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

Use them as working references for dark-fantasy aura, weapon, buff/debuff and magic-impact VFX.

## General pattern found in the pack

Most effects use a three-layer layout:

```text
1. Thematic particle/sprite layer
   Examples: butterflies, leaves, bats, skulls, crystals, sparks, rays.

2. Volume/atmosphere layer
   Examples: smoke, mist, flame flipbook, air, poison cloud.

3. Shape/motion layer
   Examples: ring, branch, blood trail, orbiting sparkles, vortex or directional drift.
```

Common module stack:

```text
Emitter Update:
  EmitterState
  SpawnRate or SpawnBurst_Instantaneous

Particle Spawn:
  InitializeParticle
  SphereLocation

Particle Update:
  CurlNoiseForce
  Drag
  FloatFromCurve
  PointAttractionForce
  ScaleColor
  ScaleSpriteSize
  SolveForcesAndVelocity
  SubUVAnimation when using flipbooks
  VortexVelocity / WindForce when orbit or drift is needed
```

Renderer baseline:

```text
Sprite Renderer
FacingMode = FaceCamera
SubImageSize = 1x1 for normal sprites
SubImageSize = 6x6 or 8x8 for flipbook smoke/flame/mist
```

## Effect-specific usage notes

```text
FireStaffAura
  Use as reference for fire aura / burning weapon / magical flame.
  Typical layers: sparkles + flames + smoke.

MagmaStaffAura
  Use as reference for hot core + sparks + smoke, and for impact/explosion direction.

ElectricStaffAura
  Special reference for lightning/beam VFX.
  Uses more emitters than the others and contains beam/lightning-style layers.

NatureStaffAura
  Use as reference for organic magic: air + branches + leaves.

PoisonStaffAura
  Use as dark-fantasy poison reference: smoke + particles + skull motifs.

VampiricStaffAura
  Use as vampire/necro reference: mist + bats + blood/sharp silhouettes.

IceStaffAura
  Use as cold magic reference: snow/crystal/storm layers.

LightStaffAura
  Use as holy/light reference: sun/core + ring + particles.

MysticStaffAura
  Use as generic arcane/mystic reference: sprites + fire/mist.

ButterflyStaffAura
  Use as gentle magical aura reference: butterflies + rays + smoke/mist.
```

## What is already safe to use for generation

The following details are reliably useful as references from the exported JSON:

```text
- emitter names and count
- module order in readable_stack
- spawn rates and burst counts
- SphereLocation radius
- InitializeParticle lifetime / size / color values
- renderer FacingMode
- renderer SubImageSize
- SubUVAnimation presence
- movement module presence and rough values
- curve-driven module presence, even when stored as skipped/debug
```

## Current limitation

Curve nodes are visible but not fully reimportable yet:

```text
FloatFromCurve
ScaleSpriteSize curve
ScaleColor curve
VectorFromCurve
some color conversion dynamic inputs
```

These must be exported into:

```text
niagara_asset.import_actions.skipped_input_overrides
```

Do not silently drop them. Until importer support is added, use them as reference/debug data, not as active reimport actions.

## Generation rules learned from the pack

```text
1. Do not put all visual logic into one emitter.
2. For aura effects, prefer 3 layers: atmosphere + particles + theme silhouette/shape.
3. For explosion/impact effects, prefer 3 layers: core burst + smoke + sparks/debris.
4. For lightning, use multiple specialized layers and beam/jitter patterns when available.
5. Always preserve Sprite Renderer FacingMode in export.
6. For flipbooks, export SubImageSize and SubUVAnimation.
7. Curves are essential for production look; if not reimportable, keep them in skipped/debug export.
8. Generate pseudo-sim GIF/contact-sheet for every newly generated effect JSON before sending it.
```
