# NodeToCode release notes — Version 194

Version: `1.2.94-ue427-powershell-strictmode-collection-hardening-candidate`

## Fixed

The Version 193 automation stopped at Stage 1 before Unreal Build. Windows PowerShell 5.1 raised `PropertyNotFoundStrict` while evaluating a direct `.Count` member on a value that had collapsed from a one-element pipeline to a scalar object. The previous optional-JSON accessor fix did not eliminate this second StrictMode failure class.

Version 194:

- routes every collection cardinality check in `Validate-N2CFiles.ps1` through `Get-N2CCollectionCount`;
- validates null, scalar, empty, single-item and multi-item inputs at validator startup;
- emits `N2C_STATIC_VALIDATION_EXCEPTION` with line, position and stack for any future terminating error;
- parses its own PowerShell AST and rejects executable direct `.Count` member access in the validator;
- routes all release-manifest and fixture JSON fields through `Get-N2CJsonPropertyValue`, including fields that are currently required, so a missing field produces a contextual schema error instead of `PropertyNotFoundStrict`;
- rejects future direct member access on known `ConvertFrom-Json` root variables;
- mirrors that rule in the cross-platform Python release audit;
- derives the current contract-catalog filename from `NodeToCode.uplugin`;
- preserves the 117-case import matrix, four-session persistence/reapply protocol, 21 legacy ManualReplay cases and two-process restore tests.

## Manual JSON identity

The manual JSON is now named `N2C_FINAL_MANUAL_ALL_STRICT_SUPPORTED_V194.json`, but its existing `V193/...` `import_scope` strings remain unchanged intentionally. Those strings are durable graph-import identities. Changing them solely because the plugin version changed would defeat idempotent reapply and could duplicate nodes.

## Validation status

Cross-platform static source, JSON, manifest, documentation and package validation pass locally. A Windows PowerShell 5.1 Stage 1 run, UE4.27.2 build, full 11-stage contract automation and interactive manual Apply are still required before production status can be claimed.
