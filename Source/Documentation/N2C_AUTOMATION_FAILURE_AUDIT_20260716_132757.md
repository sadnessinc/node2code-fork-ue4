# Automation failure audit — 2026-07-16 13:27:57

Plugin: Version 192 / `1.2.92-ue427-schema-native-pin-defaults-hotfix-candidate`

## Result

The run stopped at static validation. UE build and Editor tests were not started.

```text
PropertyNotFoundStrict,Validate-N2CFiles.ps1
Property "release_claim_allowed" was not found on the JSON object.
```

## Root cause

The node capability matrix intentionally omits `release_claim_allowed` on some rows. `Validate-N2CFiles.ps1` read the property directly while `Set-StrictMode -Version 2.0` was active. Optional schema fields therefore behaved as mandatory PowerShell properties.

## Fix

Version 193 resolves optional fields through `Get-N2CJsonPropertyValue` and supplies explicit defaults. Static validation also validates the full import-contract matrix so a newly added importer action/guard cannot be released without a mapped runtime case.

## Safety impact

No Blueprint or project asset was opened or modified. The runner correctly returned failure, created the diagnostic ZIP and removed the unpacked result directory after ZIP verification.
