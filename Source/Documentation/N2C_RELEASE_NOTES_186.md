# NodeToCode Version 186 release notes

Version: **186**  
VersionName: **1.2.86-ue427-struct-pin-identity-candidate**

## Fixed

A real project-wide transient dry run rejected:

```text
MakeStruct.ReturnValue -> SetFields.StructRef
```

UE4.27 `UK2Node_MakeStruct` creates its struct output with `StructType->GetFName()`. `UK2Node_BreakStruct` uses the same concrete name for its struct input. Only `UK2Node_SetFieldsInStruct` uses `StructRef` and `StructOut`.

The importer now accepts legacy `ReturnValue`/`Struct` aliases only when exactly one compatible struct pin exists. This preserves older generated patches without making the alias ambiguous. New JSON must use the exported canonical PinName.

## Manual JSON corrections

- `MakeStruct.ReturnValue` → `MakeStruct.ST_N2C_Final`;
- `BreakStruct.Struct` → `BreakStruct.ST_N2C_Final`;
- `Select.A/B` → `Select.Option 0/Option 1`.

## Regression coverage

`NodeToCode.ManualReplay.StructAndDataTable` now verifies:

1. a canonical connected `MakeStruct → SetFieldsInStruct → BreakStruct` pipeline;
2. `StructRef`/`StructOut` identities;
3. exact generated struct-name pins;
4. compile, direct save, unload and fresh-process persistence;
5. legacy Make/Break aliases as a compatibility path.

## Status

Static validation may pass in the build environment, but Version 186 remains `candidate` until the full target-machine runner passes 21/21 main cases and both restore processes.
