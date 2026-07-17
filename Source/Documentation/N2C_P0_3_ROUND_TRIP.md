> **Historical record.** This file describes an earlier release or evidence run. It is not authoritative for Version 173. See `N2C_CURRENT_STATE.md` and `N2C_DOCUMENT_AUTHORITY.md`.

# N2C P0.3 round-trip verification

Status: implemented and automated for UE4.27.2 on 2026-07-13.

`FN2CRoundTripVerification` uses `N2C_ROUNDTRIP_VERIFY_RESULT_V3`, `N2C_FRESH_VERIFY_REQUEST_V3`, `N2C_FRESH_VERIFY_RESULT_V3`, `N2C_STRUCTURAL_BASELINE_V3`, `N2C_STRUCTURAL_DIFF_V1`, and `N2C_ROUNDTRIP_RUN_MANIFEST_V1`.

The parent pipeline records Parse, Preflight, Backup, Apply, CompileAfterApply, BuildExpectedContract, BuildPersistenceBaseline, Save, CloseEditors, strict Unload, LaunchFreshProcess, ParseChildResult, child-derived reload/compile/snapshot/compare stages, CleanupFixture, and Finalize. A successful result has empty `failed_stage` and `error_code`. Failures name the actual stage and stable code; cleanup/restore evidence is separate.

Strict unload closes editors, clears automation-owned transactions, calls `UPackageTools::UnloadPackages`, performs GC, and requires the weak Blueprint pointer to be invalid before a child process can launch. Generated fixtures live only under `/Game/N2C_Test/Generated/`, are backed up before mutation, and are deleted after each test. Test-only fault flags require `bAutomationOnly`.

The child records ParseRequest, ValidateRequest, ReloadFromDisk, CompileAfterReload, BuildReloadedSnapshot, ExpectedContractCompare, PersistenceCompare, WriteDiff, WriteResult, and Finalize. Parent validation checks schema, run/asset identity, PID, hashes, stage uniqueness/completeness, and verdict consistency. JSON artifacts are written through temporary-file plus rename.

`N2C_STRUCTURAL_BASELINE_V3` is structured by asset, graphs, nodes, pins, variables, functions, macros, dispatchers, and CDO defaults. Canonical ordering ignores layout/transient ordering while preserving semantic identities, map value types, defaults, and links. Expected-contract comparison is distinct from post-Apply versus fresh-reload persistence comparison.

Automation coverage is `NodeToCode.Verification.P0RoundTrip.*`: Success, PureFunction, Containers, CompileFailure, SaveFailure, MissingArtifact, Timeout, ReloadFailure, StructuralMismatch, RejectNoMutation, MalformedChildResult, ChildIdentityMismatch, and Fresh. Evidence: root log `Saved/NodeToCode/CodexLogs/Verify_NodeToCode.Verification.P0_20260713_225520.log`, exit 0.