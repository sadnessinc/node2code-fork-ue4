# Automation failure audit — 2026-07-16 14:35:53

## Input

Bundle: `N2C_Automation_20260716_143553.zip`

Plugin: Version 193 / `1.2.93-ue427-exhaustive-import-contract-matrix-candidate`

## Result

- PowerShell syntax preflight: PASS.
- Process runner self-test: PASS.
- Static validation: FAIL before UE build.
- UE build and all runtime stages: NOT RUN.
- Bundle cleanup and failure packaging: PASS.

## Root cause

`Validate-N2CFiles.ps1` still contained executable direct `.Count` member access. Under `Set-StrictMode -Version 2.0`, Windows PowerShell 5.1 throws `PropertyNotFoundStrict` when a one-element pipeline result is surfaced as a scalar without a real `Count` property. The log therefore reported only `PropertyNotFoundStrict,Validate-N2CFiles.ps1`.

## Prevention in Version 194

- All validator cardinality reads use `Get-N2CCollectionCount`.
- Helper self-tests cover null, scalar and array shapes.
- The validator emits exact line/stack diagnostics for future failures.
- The validator AST and Python audit reject direct `.Count` member access in `Validate-N2CFiles.ps1`.
- Required and optional fields from all validator-owned JSON roots are read through `Get-N2CJsonPropertyValue`; direct JSON member access is rejected by both audits.
