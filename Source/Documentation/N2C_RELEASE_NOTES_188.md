# NodeToCode Version 188 — UE4.27 delegate pin import hardening

- Fixed strict sandbox import of `K2Node_CreateDelegate` links by recognizing UE4.27.2 canonical `OutputDelegate`.
- Retained `Delegate` and `Event` as compatibility aliases while canonicalizing shipped fixtures to `OutputDelegate`.
- Extended delegate regression coverage to connect both legacy and canonical source pin names to Add/Remove Delegate nodes.
- Updated AI JSON authoring rules, manual fixture, manifest, changelog, README, release validator, and package integrity manifest.
- Static validation is required before packaging; real UE4.27.2 build and editor automation remain the runtime release gate.
