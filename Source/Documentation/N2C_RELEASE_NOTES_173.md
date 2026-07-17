# Version 173 release notes

Release: **`1.2.73-ue427-full-review-refactor-candidate`**

## Audit findings corrected

- The input Version 172 archive contained `Binaries/` and `Intermediate/` despite being described as source-only; generated DLL/PDB/OBJ/LIB/EXP files are excluded from Version 173.
- Version 172 had extensive `NodeToCode.Verification.*` coverage but no required ManualReplay namespace or dedicated restore second pass.
- Raw Byte and enum-backed Byte were both treated as requiring a subtype object; raw `uint8` was therefore skipped.
- Member-variable semantic validation was not always selected by the public preflight path.
- Existing wrappers lacked timeouts, process-tree termination, robust multi-process restore sequencing, unified result ZIP and SHA-256.

## Code changes

- Raw Byte/enum distinction and `enum_member_type_unresolved` guard.
- Member-variable actions participate in live semantic preflight.
- Test-only post-mutation failure hook.
- Test-only headless restore queue and diagnostic formatter wrappers.
- AIModule is an explicit module dependency for class-specific AI/BT Blueprint tests.
- Removed an empty module preprocessor block and an unused Build.cs include-path array.

## Tests

- Added 20 main ManualReplay cases with deterministic per-case PASS/FAIL log markers; FlowAndArrays and contextual EventGraph now use connected executable paths.
- Added isolated restore first/second passes.
- Added raw Byte positive and missing-enum negative fixtures.
- Added class-specific Widget, AIController, BTTask, BTService and BTDecorator Blueprint persistence tests.

## Refactoring status

The release consolidates validation and automation contracts but intentionally avoids a risky rewrite of the giant importer/editor-integration files before a real UE4.27 verification baseline. Further service extraction remains P1.

## Verification status

Static source/package validation passed and is recorded in `N2C_STATIC_AUDIT_173.json`. UE4.27.2 was not available in the archive-generation environment. Build and Editor automation are **NOT RUN**; the release remains a candidate.
