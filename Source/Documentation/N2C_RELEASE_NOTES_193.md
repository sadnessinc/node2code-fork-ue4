# NodeToCode release notes — Version 193

Version: `1.2.93-ue427-exhaustive-import-contract-matrix-candidate`

## Immediate failure fixed

Version 192 failed at stage 1 before UE build. `Validate-N2CFiles.ps1` accessed `release_claim_allowed` directly on capability rows where that optional property was absent. PowerShell `Set-StrictMode -Version 2.0` converted the absent property into `PropertyNotFoundStrict`.

Version 193 routes optional JSON access through `Get-N2CJsonPropertyValue` and applies an explicit false default. The same safe access pattern is used for positive/negative test arrays and fresh-process flags.

## New import-contract release gate

A new 117-case matrix replaces one-off regression growth with a reusable contract system. It covers every action token currently recognized by the importer, each mapped node-constructor family and every current preflight/runtime guard code.

Coverage totals:

- supported actions: 38;
- node families: 63;
- guards: 29;
- negative no-mutation cases: 36;
- runtime case executions before cleanup: 468;
- cleanup test: 1;
- legacy ManualReplay cases: 21;
- deferred restore cases: 2.

Every contract has a semantic expected result. Positive cases compile and save. Negative cases must fail without exact structural mutation and without an unexpected restore manifest.

## Fresh-process and repeat-import proof

The runner now has 11 stages. Contract cases are applied in one UE process, verified in a new process, reapplied in a third process and verified again in a fourth process. A fifth process removes fixtures. The legacy suite and restore suite then run.

The reapply phase compares a semantic structural snapshot that ignores only unstable node/pin GUID fields. It detects duplicated nodes, graph/link/default changes, signature changes, variables, functions, dispatchers, components and interfaces.

## Idempotent graph node identity

`patch_graph` and its aliases now assign a deterministic `NodeGuid` using:

- target graph path;
- explicit `import_scope` / `action_id` when present, otherwise a stable action signature;
- authored node `id`.

The same patch therefore reuses its imported node and existing connection rather than creating duplicates. The AI guide now requires stable unique node IDs and recommends explicit import scopes for long-lived actions.

## Matrix families

- variables: scalar, raw Byte, Enum, Struct, object/class/soft refs, Array/Set/Map, defaults, change, rename/delete, Get/Set, Get-to-function and failures;
- functions: pure/impure, parameters, outputs, locals, flags, categories, replace/rename/delete and invalid metadata;
- graphs: flow, arrays, calls, casts, Select, Knot, input nodes, SpawnActor, CreateWidget, events, parent calls, interface message and edge failures;
- delegates: dispatcher lifecycle, Create/Add/Call/Remove/Clear and ComponentBoundEvent;
- graph boundaries: macro and collapsed graph create/replace/delete;
- specialized: Struct, Enum and literal/linked DataTable;
- SCS, interfaces, Widget, AIController and Behavior Tree Blueprint subclasses;
- guarded/deferred node paths are tested as safe pre-mutation rejects.

## Proof status

Static source/package validation passes locally. Version 193 remains a candidate until the target machine completes the 11-stage UE4.27.2 run and the interactive six-Blueprint manual JSON succeeds through restart and repeated import.
