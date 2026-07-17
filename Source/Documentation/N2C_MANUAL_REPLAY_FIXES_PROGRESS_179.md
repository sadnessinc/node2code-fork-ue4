# Version 179 — ManualReplay fixes and live progress

## Evidence used

Diagnostic bundle `N2C_Automation_20260716_010509.zip` showed a successful Version 178 build and two failed main tests. UE exited normally after the queue; exit code 255 reflected failed automation tests.

## Fixed cases

### ContextualEventGraph

The positive InputKey fixture now serializes all required UE4.27 identity/state fields:

```json
"shift": false,
"ctrl": false,
"alt": false,
"cmd": false,
"consume_input": true,
"execute_when_paused": false,
"override_parent_binding": true
```

### RawByteDefaultReopenExport

Round-trip persistence already passed. The exporter now resolves member defaults from `Blueprint->GeneratedClass` and its CDO, finds the reflected property by variable name, and calls `ExportTextItem(..., PPF_SerializedAsImportText)`. This matches the structural verifier and preserves raw `uint8` default `37` after reopen.

The test now logs category, subtype and exported default when it fails.

## Live progress

`Invoke-N2CFullValidation.ps1` reads appended UE log bytes while the process runs. It tracks `Test Started` and `Test Completed` records and renders an ASCII bar, completed count, current ordinal/name and elapsed time. The mechanism works for the 20-case main stage and one-case restore stages.

## Diagnostics

- Failed case names take priority over the normal engine shutdown line.
- Failure records are stored as `PSCustomObject`, so the final UI keeps the original stage log path rather than falling back to the later package log.
