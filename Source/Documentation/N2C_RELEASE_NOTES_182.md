# NodeToCode Version 182 release notes

Version: **182**  
VersionName: **`1.2.82-ue427-log-audit-release-gate-candidate`**

- Recorded and audited the real Version 181 PASS bundle: build PASS, 20/20 main tests, both restore passes and package PASS.
- Added report-level validation of UE `index.json` alongside exit-code/log/case-marker checks.
- Changed patch-apply trace diagnostics from Warning to Display.
- Clarified progress as `Completed N/total | Running M/total` and added a finalizing state.
- Added `-KeepResultDirectory` for Codex-friendly direct access to logs and reports.
- Added `N2C_NEW_NODE_TEST_GATE` and `N2C_NODE_TEST_REQUIREMENTS_V1.json`.
- Kept AnimGraph and full Niagara graph claims deferred until dedicated mandatory regressions exist.
- Did not enable unsafe same-project multi-process parallel testing.

Version 182 must be rerun on UE4.27.2 before replacing `candidate` with `verified`.

## Progress output contract

The runner prints a permanent `Completed {N}/{Total}: {Case} [{Result}]` line for every finished test and keeps a separate live `Running {N}/{Total}: {Case}` line. Fast tests therefore remain visible even when several complete between one-second log polls.
