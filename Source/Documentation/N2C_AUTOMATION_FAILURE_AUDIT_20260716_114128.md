# Automation failure audit — 2026-07-16 11:41:28

## Result

The runner passed PowerShell syntax preflight and the child-process self-test, then stopped at Stage 1 static validation. No UE build or Editor process was started.

## Exact cause

`NodeToCode.uplugin` reported Version 188, while `Scripts/Codex/Validate-N2CFiles.ps1` still contained a literal `-ne 187` check and the Version 187 name. The failure was deterministic and correctly packaged into `N2C_Automation_20260716_114128.zip`.

## Corrective action in Version 189

- descriptor-driven current version and filename resolution;
- semantic `Version`/`VersionName` consistency check;
- cross-checks for manual JSON, ManualReplay manifest and node matrix;
- canonical CreateDelegate pin validation;
- root/fixture JSON identity check;
- current release notes and static-audit presence checks.

This failure is unrelated to UE4.27 compilation or the CreateDelegate graph connection itself because execution never reached Stage 2.
