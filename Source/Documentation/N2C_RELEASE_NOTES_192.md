# NodeToCode release notes — Version 192

Version: `1.2.92-ue427-schema-native-pin-defaults-hotfix-candidate`

## Runtime failures corrected

The Version 191 target-machine run built successfully but finished 19/21 ManualReplay cases.

### Schema-native pin defaults

`ContextualEventGraph` was blocked during transient sandbox preflight on the valid class literal `/Script/Engine.Actor`. Version 191 resolved the class into `DefaultObject` but also left the raw path in `DefaultValue`; UE4.27 rejects a class pin when a string value is present.

Version 192 converts every authored non-wildcard pin default with `UEdGraphSchema_K2::GetPinDefaultValuesFromString`, validates the resulting `DefaultValue` / `DefaultObject` / `DefaultTextValue` tuple with `IsPinDefaultValid`, then assigns that tuple and calls `PinDefaultValueChanged`.

This systematically covers scalar, enum, struct, object, class, soft-reference and text defaults instead of special-casing only Rotator/Vector strings. The audit also migrated literal GetDataTableRow object pins and Tunnel boundary defaults away from direct field writes.

### Compiled member-default verification

`RawByteDefaultReopenExport` was a false-negative. Round-trip, compile, save, strict unload, fresh-process reopen, structural comparison, raw Byte export and Rotator export all passed. The test failed only because it required `FBPVariableDescription::DefaultValue` to remain populated after compilation.

Version 192 verifies the persisted Rotator on the generated-class CDO through the reflected `FStructProperty`, and retains fresh export as user-facing persistence proof. Descriptor text is diagnostic only.

## Regression additions

- positive `SpawnActorFromClass.Class=/Script/Engine.Actor` through schema conversion;
- negative incompatible `UserWidget` class on SpawnActor, rejected before mutation;
- CDO-backed Rotator member-default assertion after fresh-process reopen;
- static guards requiring schema conversion markers and prohibiting raw final-manual class/default regressions;
- updated AI JSON guide documenting object/class/text pin storage and post-compile CDO authority.

## Proof status

Version 189 remains the latest complete target-machine automation PASS. Version 192 is source-only until the full UE4.27.2 harness and the interactive six-Blueprint final patch both pass.
