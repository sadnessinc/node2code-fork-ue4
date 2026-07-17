# NodeToCode Version 187 release notes

Version: **187**  
VersionName: **1.2.87-ue427-powershell-json-edge-schema-hotfix-candidate**

## Fixed

The Version 186 Windows PowerShell static validator failed before build with `PropertyNotFoundStrict`. The final manual JSON intentionally contains two valid edge representations:

- normal graph actions: `from_node_id` / `to_node_id`;
- function and macro boundary actions: `from` / `to`.

`Validate-N2CFiles.ps1` accessed `from_node_id` directly under `Set-StrictMode`, so boundary edges without that property caused a runtime exception. Version 187 resolves either representation through explicit `PSObject.Properties` lookup, requires endpoint and pin fields, verifies referenced node ids, and reports a readable schema error instead of an automatic-variable/property exception.

## Diagnostic bundle hygiene

Crash directories are now collected only when their `LastWriteTimeUtc` is at or after the current runner start time (with a small filesystem timestamp tolerance). The former eight-hour window could attach an unrelated earlier Editor crash to a later static-validation failure.

## Status

Version 185 remains the latest real target-machine verified baseline. Version 187 changes PowerShell automation and packaging metadata only; the full runner must pass again before this source is renamed `verified`.
