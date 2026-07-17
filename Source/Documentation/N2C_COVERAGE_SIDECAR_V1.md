# N2C coverage sidecar V1

`N2C_COVERAGE_V1` is a read-only report written beside each `N2C_AI_EXPORT_V2` Blueprint JSON as `ExportBaseName.coverage.json`. It is not an executable patch and does not normalize export JSON into `N2C_PATCH_V1`.

The root contains schema/version, plugin and UE target, source schema, asset path/origin, SHA-1 source binding, four safety decisions, strict-Apply decision, status counts, blocker/gap/warning counts, and per-node issues. Issues contain graph/node identity, variant, constructor, required/missing metadata, fixture evidence, loss classification, reason, and strict-block decision.

`FN2CCoverageClassifier` is the single classification core for live sidecars, patch preflight, hard Apply gate, and the P0.1 scanner's project-node decisions. The scanner retains only its origin-specific `dependency_only` rule for Engine graph bodies. `NodeToCode.Verification.P0CoverageParity` verifies representative verified, supported_untested, guarded, partial, unsupported, cosmetic_only, dependency_only, and fail-closed unknown cases.

Strict policy allows only `verified`, `cosmetic_only`, and `dependency_only`. The explicit developer override can admit only `supported_untested` with complete metadata. It never admits guarded, partial, unsupported, unknown, unresolved macro identity, parse/schema failures, or stale evidence. Cosmetic-only nodes warn and are intentionally not sent through runtime constructor resolution.

The importer always reclassifies live `N2C_PATCH_V1`; a sidecar cannot bypass preflight. Durable macro references additionally run live owner/graph/signature resolution before mutation. `N2C_AI_EXPORT_V2` remains descriptive and direct Apply rejects it pending the P3 normalizer.

Evidence: `NodeToCode.Verification.P0CoveragePreflight`, `P0CoverageParity`, and `P0ScannerContract` passed in the 2026-07-13 root suite.