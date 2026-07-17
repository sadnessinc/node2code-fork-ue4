> **Historical evidence.** This file records an earlier UE4.27 verification run. It does not prove Version 173; use `N2C_CURRENT_STATE.md` and `N2C_FULL_REVIEW_173.md` for current status.

# N2C StandardMacros durable references

Status: supported and fresh-process verified for UE4.27.2.

The canonical asset is `/Engine/EditorBlueprintResources/StandardMacros.StandardMacros`. Engine macro graph bodies remain `dependency_only`; the consumer `UK2Node_MacroInstance` is exported as a durable reference rather than a copied body.

`FN2CMacroReference` records owner/Blueprint/graph paths, graph name and GUID evidence, owner package/type, dependency origin, graph kind, normalized SHA-1 signature, tunnel signature, instance pin contract, wildcard pin types, and resolved wildcard type. Preflight loads the exact owner and graph and blocks missing owner, missing graph, unsupported owner/kind, or signature mismatch before mutation. Import reconstructs the exact instance and then restores wildcard/container types, defaults, and links.

Verified real graphs: `DoOnce` (simple exec), `ForLoop` (loop), and `ForEachLoop` connected to typed `MakeArray<int>` (wildcard/container persistence). Tests: `NodeToCode.Verification.P0StandardMacros.Simple`, `.Loop`, `.WildcardContainer`, `.MissingOwner`, `.MissingGraph`, and `.SignatureMismatch`.

Evidence: targeted log `Saved/NodeToCode/CodexLogs/Verify_NodeToCode.Verification.P0StandardMacros_20260713_222442.log` and root log `Saved/NodeToCode/CodexLogs/Verify_NodeToCode.Verification.P0_20260713_225520.log`, both exit 0.