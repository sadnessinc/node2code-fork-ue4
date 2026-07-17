# UE4.27 source-reference workflow

Use exact UE4.27.2 engine and supplied BlueprintGraph/Kismet/reference-plugin source before changing importer constructors, pin names, reconstruction order or editor lifecycle code.

## Review sequence

1. Find the UE4.27 class declaration and module/API export status.
2. Read its constructor, `AllocateDefaultPins`, reconstruction and expansion code.
3. Record exact internal pin names and required identity fields.
4. Confirm public/exported versus private/`NO_API` status.
5. Verify `NodeToCode.Build.cs` dependencies.
6. Keep unsupported private utilities guarded; never import a UE5-only assumption.
7. Apply `N2C_NEW_NODE_TEST_GATE`: update `N2C_NODE_TEST_REQUIREMENTS_V1.json` and add a mandatory test with Apply, compile, save, unload, fresh-process reopen and persisted semantic assertions.
8. Add missing-identity/no-mutation or verified rollback coverage where applicable.
9. Update coverage claims only after the regression exists and passes.

For future Animation/AnimGraph work, use the relevant UE4.27 animation editor source and dedicated AnimBlueprint/AnimGraph fixtures. EventGraph tests are not evidence for AnimGraph fidelity.

A static source check is not a C++ build. Run `Scripts/RUN_N2C_AUTOMATION_AND_PACK.cmd` in the real project before promotion.
