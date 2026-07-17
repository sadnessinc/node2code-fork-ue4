# Manual dry-run audit — MakeStruct/SetFields pin identity

## Observed result

The Version 185 manual project patch was rejected before target mutation with:

```text
MakeStruct.ReturnValue -> SetFields.StructRef
```

The transient sandbox and all five other Blueprint entries behaved correctly. No target Blueprint mutation occurred in this attempt.

## UE4.27 source result

`UK2Node_MakeStruct::AllocateDefaultPins()` creates the output pin with `StructType->GetFName()`.

`UK2Node_BreakStruct::AllocateDefaultPins()` creates the input pin with `StructType->GetFName()`.

`UK2Node_SetFieldsInStruct::AllocateDefaultPins()` creates `StructRef` and `StructOut`.

For `ST_N2C_Final`, the canonical chain is:

```text
MakeStruct.ST_N2C_Final -> SetFields.StructRef
SetFields.StructOut -> BreakStruct.ST_N2C_Final
```

## Root cause

The manually authored fixture used a common function-return convention (`ReturnValue`) for a node that does not use that convention. Existing automated struct tests checked node presence but did not connect Make/SetFields/Break, so the missing edge identity was not exercised.

## Corrective action

Version 186 updates the importer compatibility alias, canonical fixture, AI rules, static gate and connected fresh-process regression.
