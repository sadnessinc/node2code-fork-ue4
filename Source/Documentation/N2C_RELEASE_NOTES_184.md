# NodeToCode Version 184 release notes

Version: **184**  
VersionName: **1.2.84-ue427-composite-identity-idempotency-candidate**

## Fixed

- `create_collapsed_graph` and `replace_collapsed_graph` no longer reject a valid existing Composite solely because UE4.27 inserted a generated `K2Node_Composite_<n>` object segment into the BoundGraph path.
- The canonical comparison removes only that unstable segment. Blueprint path, graph kind, graph name and parent graph path are still checked.
- More than one matching Composite/BoundGraph name is rejected with `composite_bound_graph_ambiguous`.
- The transient Blueprint sandbox now validates boundary identities using the original asset's logical path. This prevents false `graph_owner_mismatch` failures caused by `/Engine/Transient` duplicate paths.

## Tests

- Added `NodeToCode.Verification.P0GraphBoundaries.CompositeCanonicalIdentityRepeat`.
- `NodeToCode.ManualReplay.GraphBoundaries` now performs first apply, save, unload/reload, dry-run sandbox, second apply and duplicate-count validation.
- Existing deliberate mismatch test remains required and must still return `composite_bound_graph_mismatch` without mutation.

## Status

Static validation only in the packaging environment. UE4.27 build and automation must be rerun on the target machine before promotion to `verified`.
