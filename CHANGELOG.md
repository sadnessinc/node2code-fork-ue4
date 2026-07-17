## Version 195 — PowerShell StrictMode collection hardening

- Fixed the Version 193 Stage 1 `PropertyNotFoundStrict` failure caused by direct `.Count` member access on a value that Windows PowerShell 5.1 could expose as a scalar.
- Added `Get-N2CCollectionCount` and runtime helper self-tests for null, scalar string, scalar object, empty array, one-element array and multi-element array.
- Removed executable direct `.Count` access from `Validate-N2CFiles.ps1`; an AST gate now rejects reintroduction of that pattern.
- Added structured `N2C_STATIC_VALIDATION_EXCEPTION` diagnostics with source line, position and script stack.
- Made the contract-matrix catalog path version-derived in both PowerShell and Python validators.
- Retained the complete 117-case, four-session import-contract matrix and durable V193-prefixed `import_scope` identities.
- Added failure audit `N2C_AUTOMATION_FAILURE_AUDIT_20260716_143553.md` and updated all current release documentation.
- Passed the complete target-machine 11-stage gate, real six-Blueprint Level Editor import/reimport, fresh compile, project export, and published the Legacy-to-ContractMatrix Keep/Update/Retire decisions.

## Version 193 — exhaustive import-contract matrix and repeat-import identity

- fixes the Version 192 `PropertyNotFoundStrict` failure by reading optional PowerShell/JSON properties through a strict-safe helper;
- adds 117 reusable import contracts mapped to all 38 supported action tokens, 63 constructor/alias families, and 29 runtime guards;
- runs Apply/compile/save, fresh-process verification, identical reapply, second fresh-process verification, and cleanup before the legacy ManualReplay and restore suites;
- gives ordinary `patch_graph` nodes deterministic identities and reuses existing identical edges, preventing duplicate nodes/links on repeat import;
- adds static coverage gates that reject unmapped supported actions, node families, guards, stale release metadata, and unsafe optional-property access;
- updates the AI JSON guide, manual fixture, release workflow, current state, TODO, manifests, and failure audit;
- remains a source-only candidate until the complete 11-stage gate and manual project patch pass on the target UE4.27.2 installation.

# Changelog

## Version 192 — UE4.27 schema-native pin defaults and CDO persistence

- Fixed valid class/object pin literals being rejected because a resolved `DefaultObject` was combined with the same raw path in `DefaultValue`.
- Routed all non-wildcard authored pin defaults through `UEdGraphSchema_K2::GetPinDefaultValuesFromString` and validated the resulting value/object/text tuple.
- Updated RawByte/Rotator persistence regression to verify the generated-class CDO and fresh export rather than post-compile `FBPVariableDescription::DefaultValue`.
- Added positive SpawnActor class-literal coverage, incompatible-class preflight rejection, CDO Rotator verification, validator guards, failure audit, release notes and AI authoring rules.
- Version 192 remains a source-only candidate pending full UE4.27.2 automation and interactive six-Blueprint Apply/compile/save/reopen.

## Version 191 — UE4.27 K2 struct-default format hotfix

- fixes `MakeTransform.Rotation` compile failure caused by assigning named `UScriptStruct::ExportText` to a K2 Rotator pin;
- converts Vector/Rotator ImportText to UE4.27 Blueprint CSV storage (`X,Y,Z` / `Pitch,Yaw,Roll`) for both node pins and member-variable descriptions;
- validates authored pin defaults through `UEdGraphSchema_K2::IsPinDefaultValid` and blocks invalid values before target mutation with `pin_default_invalid`;
- strengthens `ContextualEventGraph` with malformed-Rotator rejection and `RawByteDefaultReopenExport` with persisted K2 Rotator parsing;
- updates the AI authoring guide, manifests, release tooling checks, current-state docs and final manual JSON.

## Version 190 — interactive Apply compile-parity hotfix

- fixes UE4.27 `CreateDelegate` selection loss by deferring signature-aware validation until `OutputDelegate` is connected;
- adds same-patch handler creation/binding compile-save-reopen regression;
- requires `SpawnActorFromClass.SpawnTransform` to be connected and rejects literal/unconnected forms before mutation;
- updates the final manual patch to use `KismetMathLibrary.MakeTransform`;
- removes same-process `OnPreExit` deferred-restore retries so backup replacement occurs only on a fresh startup;
- updates PowerShell/Python validation, authoring rules, manifests, current-state docs and release notes.

## 1.2.89 — Release metadata single-source hotfix

Fixed the Stage 1 failure `Expected Version 187, got 188`. Current release validation and packaging now derive the version-specific manual filename from `NodeToCode.uplugin`, cross-check all release manifests, and prevent stale copied constants. Canonical CreateDelegate `OutputDelegate` validation remains mandatory.

## 1.2.88 — UE4.27 CreateDelegate pin identity

Added canonical `UK2Node_CreateDelegate.OutputDelegate` resolution with `Delegate`/`Event` compatibility aliases, canonicalized fixtures, and extended delegate edge regressions.

## 1.2.87 — PowerShell mixed edge-schema validator hotfix

- fixes `Validate-N2CFiles.ps1` under `Set-StrictMode`: final manual patches may legally encode graph edges as `from_node_id`/`to_node_id` or function-boundary edges as `from`/`to`;
- adds explicit property normalization and clear errors for missing edge endpoint/pin fields instead of `PropertyNotFoundStrict`;
- validates that the final manual fixture contains and successfully processes both supported edge forms;
- limits diagnostic crash collection to crash directories updated after the current automation run started, preventing stale crash reports from being attached to a later unrelated failure;
- preserves the Version 185 real UE4.27.2 verified baseline and all Version 186 struct-pin changes.

## 1.2.86 — UE4.27 struct pin identity and connected regression

- fixes MakeStruct/BreakStruct pin identity using concrete `StructType->GetFName()` pins;
- retains narrow legacy `ReturnValue`/`Struct` aliases when exactly one struct pin exists;
- corrects the final manual JSON and Select option PinNames;
- strengthens `StructAndDataTable` with connected save/unload/fresh-process persistence;
- records Version 185 as a real 21/21 + restore PASS baseline.

## 1.2.85 — Headless regression harness fix

- Fixes the new Composite canonical-repeat regression to use the same headless-safe apply/compile/direct-save path as the established round-trip verifier.
- Fixes SandboxPinPreflight to call `DryRunPatch` explicitly, then Apply without interactive save, compile, direct-save, unload/reload and assert persisted Knot links.
- Asserts that neither regression increases the pending `.restore` manifest count.
- Records the Version 184 target-machine result: build PASS, 19/21 main tests PASS; GraphBoundaries and SandboxPinPreflight failed because the new tests incorrectly invoked interactive save inside `UE4Editor-Cmd`.

## 1.2.84 — Composite identity idempotency and sandbox logical identity

- Accepts both exact exported and canonical collapsed-graph `bound_graph_identity` values by removing only the unstable `.K2Node_Composite_<n>` object segment before comparison.
- Keeps strict Blueprint path, graph kind, parent graph and collapsed graph name checks; unrelated identities still fail.
- Rejects ambiguous duplicate Composite nodes that own the same collapsed graph name.
- Transient dry-run sandbox now validates graph-boundary metadata against the logical source Blueprint path while allocating nodes in the transient duplicate.
- Adds `NodeToCode.Verification.P0GraphBoundaries.CompositeCanonicalIdentityRepeat` and folds the same save/unload/reload/dry-run/reapply assertion into `NodeToCode.ManualReplay.GraphBoundaries`.
- Updates the mandatory AI authoring rules and final manual JSON to Version 184.

## 1.2.83 — sandbox preflight and restore visibility

- Added transient sandbox dry-run that exercises real node construction, pin resolution and schema connections before target mutation.
- Fixed canonical Knot pins and added a mandatory bad-pin/no-mutation regression.
- Added retrying deferred restore, clean-shutdown restore processing, persistent status, and guaranteed next-startup result UI.
- Added mandatory AI JSON authoring rules, corrected final manual JSON, project-export audit and P1/P4 TODOs.

# Changelog

## 1.2.82 — UE4.27 log audit and release gate candidate

- records the verified Version 181 build/main/restore/package evidence and warning audit;
- validates UE automation `index.json` test counts/states/errors in addition to logs and markers;
- changes patch apply traces from Warning to Display;
- clarifies progress as completed count versus currently running ordinal;
- adds `-KeepResultDirectory`;
- adds `N2C_NEW_NODE_TEST_GATE` and the node/capability regression matrix;
- keeps unsafe same-project parallel execution disabled.

## 1.2.81 — UE4.27 progress self-test and result cleanup hotfix candidate

- Parses complete automation log markers even when the current log chunk has no trailing newline.
- Fixes the progress parser self-test that incorrectly reported `started=1` for a complete final `Test Started` line.
- Verifies diagnostic ZIP entry count, names, sizes and SHA-256 hashes before deleting the source result directory.
- Deletes `Saved/NodeToCode/TestBundles/N2C_Automation_<stamp>/` after a verified ZIP is created; preserves it if ZIP creation or verification fails.
- Adds runtime self-test coverage for verified ZIP cleanup.

## 1.2.80 — UE4.27 PowerShell progress parser hotfix candidate

- fixed the `$total:` parse error in the live progress line;
- added CMD-level PowerShell syntax preflight before the orchestrator;
- added static detection of unsafe variable-colon interpolation;
- preserved Version 179 ManualReplay fixes and progress UX.

## 1.2.79 — UE4.27 ManualReplay fixes and progress candidate

- fixed ContextualEventGraph InputKey persisted modifier metadata;
- exported Blueprint member defaults from the generated CDO;
- added live Editor-test progress and current case display;
- reported concrete failed cases and preserved the correct failed-stage log path.

## 1.2.78 — UE4.27 PowerShell process audit candidate

- replaced unreliable `Start-Process -PassThru` child exit-code handling with one `ProcessStartInfo` runner;
- added mandatory self-tests for exit 0/7, stdout/stderr, quoting and timeout;
- fixed remaining `$Matches` use, stale export acceptance and standalone false-PASS logic;
- added StrictMode/parser/automatic-variable checks for every PowerShell script;
- removed and forbids `__pycache__`, `.pyc` and `.pyo`;
- writes diagnostic ZIP entries with forward slashes and preserves the result directory if ZIP finalization fails;
- no importer/exporter C++ behavior changed.

## 1.2.77 — UE4.27 PowerShell runtime hotfix candidate

- replaced the illegal `$pid = ...` assignment with `$processId = ...`; PowerShell variables are case-insensitive and `$PID` is read-only;
- replaced brittle literal checks for `-NoOpenResult` and `-NoPause` with AST validation of the actual script parameters;
- added static rejection of assignments to known read-only PowerShell automatic variables;
- improved friendly stage errors by surfacing the final non-empty child stderr/stdout line;
- renamed other risky automatic-variable locals (`$args`, `$matches`) to ordinary names;
- no importer/exporter C++ behavior changed.

## 1.2.76 — UE4.27 PowerShell parser hotfix candidate

- fixed Windows PowerShell parser failure at `Write-Host "$DisplayName: $resultText"` by using the delimited form `${DisplayName}`;
- added a real `System.Management.Automation.Language.Parser` pass over every `.ps1` file to static validation;
- preserved the Version 175 process exit-code, ZIP separator, console UX, pause and Explorer-opening fixes;
- no importer/exporter C++ behavior changed.

## 1.2.75 — UE4.27 automation runner UX fix candidate

- fixed false Build failure caused by reading an empty PowerShell 5.1 process `ExitCode` without `WaitForExit()`;
- replaced Windows-dependent ZIP entry creation with explicit forward-slash entries;
- added readable six-stage console progress, exact stopped stage, detail/log/result paths, and final Success/Error output;
- CMD now waits for a key before closing;
- result ZIP is automatically selected in a new Windows Explorer window;
- added `-NoOpenResult` for headless use;
- no importer/exporter C++ behavior changed.

## 1.2.74 — UE4.27 automation runner hotfix candidate

- fixed immediate orchestration failure when an installed/previously built plugin contains normal top-level `Binaries/` or `Intermediate/`;
- separated working-tree validation from strict source-package hygiene;
- retained strict removal and ZIP verification of build outputs in the packaging stage;
- no importer or runtime behavior was changed.


## 1.2.73 — UE4.27 full review/refactor candidate

- Added ManualReplay regression and separate-process deferred restore automation.
- Added raw Byte/uint8 import and export persistence coverage.
- Added strict missing-enum rejection.
- Added real post-mutation Undo/structural-equality tests.
- Added one-CMD build/test/package orchestration and source-only manifest validation.
- Removed generated binaries/intermediates from the release.

## 1.2.72 — UE4.27 editor import hotfix

Historical importer hotfix: verified rollback, pre-apply backup, queued disk restore, strict edge errors, UDS field matching, linked DataTable row typing and diagnostics.

## UE4.27 CreateDelegate pin identity (Version 188)

`K2Node_CreateDelegate` uses the canonical internal output pin name `OutputDelegate` (the editor displays it as **Event**). Author JSON edges as `CreateDelegate.OutputDelegate -> AddDelegate.Delegate` or `RemoveDelegate.Delegate`. The importer also accepts legacy `Delegate`/`Event` aliases for compatibility, but exports, fixtures and documentation use `OutputDelegate`. Strict transient sandbox preflight must reject genuinely missing pins before target mutation.
