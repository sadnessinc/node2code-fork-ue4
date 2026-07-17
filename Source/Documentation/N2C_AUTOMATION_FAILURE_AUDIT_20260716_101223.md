# Version 186 automation failure audit — 2026-07-16 10:12:23

## Result

The run stopped in Stage 1 static validation before UE build. PowerShell parser preflight and process-runner self-test passed.

## Root cause

`N2C_FINAL_MANUAL_ALL_STRICT_SUPPORTED_V186.json` intentionally contains two legal edge encodings:

- `from_node_id` / `to_node_id` for normal graph patches;
- `from` / `to` for function-boundary actions.

`Validate-N2CFiles.ps1` directly evaluated `$manualEdge.from_node_id` under `Set-StrictMode`. Function-boundary edges do not expose that property, causing `PropertyNotFoundStrict`. This was a validator implementation defect, not invalid import JSON and not a C++/UE build failure.

## Stale crash attachment

The diagnostic ZIP also contained a UE crash directory updated at approximately 07:07 UTC, while this automation run began at 07:12 UTC. The old runner collected every crash directory from the prior eight hours, so that crash did not belong to this Stage 1 failure. Version 187 only collects crash directories updated after the current runner start time.

## Fix

Version 187 resolves both endpoint forms through explicit `PSObject.Properties` lookup, validates required pins and node references, requires both forms in the authoritative fixture, and emits readable schema errors.
