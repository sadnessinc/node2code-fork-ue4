# Audit of failed final manual import — project export 2026-07-16 03:12:16

## Evidence

The exported project proves that the failed apply was not restored to its pre-apply baseline after restart.

`/Game/N2C_FinalManual/BP_N2C_FinalActor` contains on disk:

- 22 newly requested member variables plus the pre-existing `N2C_OnFinal` dispatcher;
- `N2C_Final_Impure`, `N2C_Final_Pure`, `N2C_Final_MultipleResults`, and `N2C_Final_DelegateHandler`;
- `N2C_FinalMacro`;
- `N2C_FinalCollapsed`;
- the pre-existing `N2C_Box` component.

The EventGraph itself contains only its previous events and the collapsed-graph node. The first 28-node graph action created nodes in memory, then failed while resolving `K2Node_Knot` pins, and the failed graph action did not persist. Earlier actions did persist because the deferred restore did not replace the package.

The other final-manual assets show earlier successful/manual content but no evidence that the later actor graph actions ran after the first failure.

## Root cause of the first apply failure

The JSON used friendly Knot pins `Input` and `Output`. UE4.27 `UK2Node_Knot` canonical internal pins are `InputPin` and `OutputPin`.

## Version 183 controls

- Canonical Knot pins are used in the replacement JSON.
- `Input`/`Output` remain compatibility aliases.
- Every dry run performs a real apply against a transient duplicate Blueprint, including node allocation, pin lookup and `TryCreateConnection`; target mutation starts only after that sandbox passes.
- Deferred restore attempts at startup and clean UE shutdown. A persistent status file carries shutdown results into the next session, which shows a modal success/failure result in interactive Editor sessions.
- A new mandatory `SandboxPinPreflight` regression proves a bad pin is rejected with equal structural hashes and canonical Knot links can be applied.

## Additional JSON audit corrections

The replacement project patch also removes other alias-dependent authoring:

- duplicate `BP_N2C_FinalActor` project entries were merged into one dependency-ordered entry;
- `K2Node_SwitchEnum` uses the real enum output `IE_Pressed` instead of the compatibility alias `Default`;
- `K2Node_GetDataTableRow` uses internal pins `then` and `ReturnValue` instead of friendly labels `RowFound` and `Out Row`;
- every action has unique node IDs and every edge endpoint resolves inside the same action.

These corrections are backed by the Version 183 transient sandbox gate, which is the authoritative protection against future constructor/pin drift.
