# N2C external project macro references

Status: Blueprint Macro Library references are supported and fresh-process verified; cross-Blueprint references owned by another ordinary Blueprint are explicitly blocked on UE4.27.2.

The durable identity and preflight rules are shared with StandardMacros. Existing project Macro Library owners are loaded by exact object path; graph path/name/GUID evidence, graph kind, owner type, tunnel contract, normalized signature, instance pins, wildcard/container types, defaults, and links are validated before mutation. No dependency macro body is copied or mutated.

Positive automation creates a generated project Macro Library and consumer, verifies typed and wildcard/container instances through Apply, compile, save, unload, separate-process reload, recompile, expected-contract compare, persistence compare, and cleanup. Missing asset, missing graph, stale signature, and unsupported graph-kind cases are expected pre-mutation rejects.

UE4.27 does not support a macro instance in Blueprint A referencing a macro graph privately owned by ordinary Blueprint B. A direct attempt fails `SavePackage` with `Graph is linked to private object(s) in an external package`, and `UK2Node_MacroInstance::PostPasteNode` clears cross-owner references unless the owner is `BPTYPE_MacroLibrary`. Supporting this case requires Unreal Engine source/serialization changes or mutating dependency ownership, both outside P0. The stable preflight code is `macro_reference_unsupported`.

Tests: `NodeToCode.Verification.P0ExternalMacros.ProjectLibrary`, `.ProjectBlueprint` (expected engine-limit reject), `.WildcardContainer`, `.MissingAsset`, `.MissingGraph`, `.SignatureMismatch`, and `.NoMutationReject`.

Evidence: targeted log `Saved/NodeToCode/CodexLogs/Verify_NodeToCode.Verification.P0ExternalMacros_20260713_223408.log` and root log `Saved/NodeToCode/CodexLogs/Verify_NodeToCode.Verification.P0_20260713_225520.log`, both exit 0.