# node2code verification workflow — Version 195

`N2C_NEW_NODE_TEST_GATE` is mandatory.

## One-command release gate

```bat
Plugins\node2code\Scripts\RUN_N2C_AUTOMATION_AND_PACK.cmd -EngineRoot "<UE4_ROOT>" -KeepResultDirectory
```

The runner executes 11 stages:

1. static/source/process validation;
2. UE4 Editor target build;
3. 117-case contract Apply;
4. first fresh-process verification;
5. idempotent contract reapply;
6. second fresh-process verification;
7. contract fixture cleanup;
8. legacy 21-case ManualReplay;
9. deferred restore first pass;
10. deferred restore second pass;
11. source-only package.

## Contract case requirements

Every row in `N2C_IMPORT_CONTRACT_MATRIX_V1.json` must contain:

- a unique `id`;
- `expected_apply`;
- `fresh_session_required: true`;
- `reapply_required: true`;
- a valid `N2C_PATCH_V1` patch;
- semantic assertions for every positive case;
- expected report/guard markers for negative cases when applicable.

A positive case passes only when Apply returns success, the Blueprint is not `BS_Error`, save succeeds, semantic assertions pass and no unexpected restore is queued.

A negative case passes only when Apply returns failure, the exact pre/post structural hashes match, semantic assertions remain unchanged and no unexpected restore is queued.

## Persistence phases

### Apply

Create an isolated fixture, apply optional setup, record the exact baseline, run the patch, compile, save and store exact plus semantic structural hashes.

### VerifyFreshFirst

Start a new UE process, load the asset from disk, compile it and require exact structural/hash equality plus semantic assertions.

### Reapply

Start another UE process, verify the prior exact baseline, apply the identical patch and require semantic structural equality. Node/pin GUID churn is ignored. Defaults on pins with actual links are treated as inactive and omitted from the semantic reapply snapshot; duplicates, missing nodes, pin types, link topology, unlinked defaults, variables, functions, interfaces and components remain significant.

### VerifyFreshSecond

Start another UE process, load and compile the post-reapply asset, require exact equality to the state saved by Reapply, then remove the asset.

### Cleanup

Remove state-tracked assets, scan `/Game/N2C_Test/Generated` for any orphan `BP_N2C_Contract_*` assets and remove the shared Struct/DataTable/interface dependencies.

## Extending the importer

A change is incomplete until all of these are updated:

1. importer source;
2. positive contract case;
3. negative no-mutation or rollback case where applicable;
4. action/node/guard coverage map;
5. `N2C_NODE_TEST_REQUIREMENTS_V1.json`;
6. `N2C_AI_JSON_IMPORT_AUTHORING_RULES.md`;
7. release notes/current state;
8. manual JSON when the feature belongs to strict-supported manual scope.

The static validator extracts supported action tokens and guard markers from source and fails if matrix coverage diverges.

## Interactive final acceptance

After all 11 stages pass:

1. create the documented prerequisites under `/Game/N2C_FinalManual`;
2. import `N2C_FINAL_MANUAL_ALL_STRICT_SUPPORTED_V195.json`;
3. approve dry-run;
4. require Apply, compile and save success for all six target Blueprints;
5. close and reopen UE;
6. compile all targets again;
7. import the same JSON a second time;
8. verify no duplicated variables, functions, dispatchers, components, macros, collapsed graphs or graph nodes.

Static validation or source packaging alone is never sufficient for a production claim.


## P0 UI-command requirement for the next release

The next feature release must register remappable Import and Export Editor commands and use their keyboard chords as the primary entry point for real UI acceptance. The driver must validate the active Editor process, expected window identity, timeout and duplicate-window count. Toolbar coordinate clicks may be used only for diagnostics and cannot satisfy the release PASS criterion. See `N2C_P0_EDITOR_COMMAND_SHORTCUTS.md`.

Version 195 evidence predates this requirement and remains valid; this rule applies to the next release gate.

## Snapshot normalization limitation

Version 195 canonicalizes boolean representation and comma-separated numeric tuple text for persistence comparison. The numeric tuple rule is currently broader than proven struct-pin use; future work must restrict it to known struct representations and retain distinct String/Name/Text values such as `1.0,2.00` versus `1,2`.

## Evidence-package rule

Record separate SHA-256 values for the automation bundle, project export, manual JSON and distributed source ZIP. Do not imply that a post-gate handoff or documentation-only repack is byte-identical to the package generated during the gate unless the hashes match exactly.
