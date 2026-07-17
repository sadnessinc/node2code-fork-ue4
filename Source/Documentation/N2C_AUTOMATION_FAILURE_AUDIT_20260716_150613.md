# Automation failure audit — 2026-07-16 15:06:13

## Input

Bundle: `N2C_Automation_20260716_150613.zip`

Plugin: Version 194 / `1.2.94-ue427-powershell-strictmode-collection-hardening-candidate`

## Result

- PowerShell syntax preflight: PASS.
- Process runner self-test: PASS.
- Static validation: FAIL before UE build.
- UE build and all runtime stages: NOT RUN.
- Bundle cleanup and failure packaging: PASS.

## Exact failure

```text
N2C_STATIC_VALIDATION_EXCEPTION|line=57|message=Validator collection-count self-test failed for null.
```

## Root cause

`Get-N2CCollectionCount` used `@($Value).Length`. The target Windows PowerShell 5.1 runtime preserved the explicitly bound null value as one counted placeholder in this context. The helper therefore returned `1` for the null self-test.

## Prevention in Version 195

- Null is handled before any enumeration.
- Real `IEnumerable` values are counted explicitly.
- Scalar strings and objects have deterministic cardinality one.
- Pipeline and `Regex.MatchCollection` shapes are included in the runtime self-test.
- Static PowerShell and Python gates reject the Version 194 implementation string.
