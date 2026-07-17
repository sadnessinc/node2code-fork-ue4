# N2C automation runner hotfix — Version 174

## Defect

Version 173 ran strict source-package hygiene against the installed project plugin before Build. After any successful UE build, the installed plugin legitimately contains top-level `Binaries/` and `Intermediate/`, so every later automation run stopped before static validation, Build, or Editor tests. Deleting those directories only worked until another build recreated them.

## Correction

`Invoke-N2CFullValidation.ps1` now invokes `Validate-N2CFiles.ps1 -AllowBuildProducts`. In this working-tree mode:

- top-level `Binaries/` and `Intermediate/` and their generated binary files are allowed;
- `Saved/`, `.vs/`, `DerivedDataCache/`, and generated binaries outside those build-output roots remain rejected;
- required files, descriptor version, JSON, fixtures, test registrations, importer markers, and secret checks are still enforced.

Strict source-only hygiene is not weakened. `Package-N2CPlugin.ps1` copies only the explicit source distribution allowlist, removes forbidden generated content from staging, and verifies every ZIP entry and manifest hash. Calling `Validate-N2CFiles.ps1` without `-AllowBuildProducts` also retains strict source-only behavior.

## Evidence boundary

The change is statically validated only in the packaging environment. UE4.27.2 Build and Editor automation must be run by the user.
