# NodeToCode Version 175 release notes

VersionName: `1.2.75-ue427-automation-runner-ux-fix-candidate`

This source-only candidate fixes the two automation infrastructure failures exposed by the first real Version 174 Windows run. The UE4.27 C++ build log completed successfully, but PowerShell recorded an empty child-process exit code and the source packager rejected its own backslash ZIP entry names.

## Fixed

- reliable PowerShell 5.1 child-process exit codes through explicit `WaitForExit()` and `Refresh()`;
- deterministic forward-slash ZIP entries and normalized verification;
- package-stage errors are recorded as package failures instead of generic orchestrator failures;
- readable stage progress and exact failure context;
- final Success/Error message with result/log path;
- pause before CMD closes;
- automatic Explorer selection of the diagnostic result ZIP;
- optional `-NoPause -NoOpenResult` for headless execution.

No importer/exporter C++ behavior changed from Version 174.

## Status

Static source validation is required before packaging. Full UE4.27 main automation and both restore passes remain to be rerun; this release is not marked verified.
