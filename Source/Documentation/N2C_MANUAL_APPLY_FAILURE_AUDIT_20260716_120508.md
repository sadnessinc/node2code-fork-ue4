# Interactive final-project Apply failure audit — 2026-07-16 12:05

## Result

Dry-run passed, but real Apply compiled `BP_N2C_FinalActor` with `BS_Error`. The plugin correctly refused to save that Blueprint and queued its pre-apply backup for deferred restore.

## Compiler errors

- `Spawn Transform` on the expanded begin/finish spawn calls was a literal on a by-reference parameter and had no source link.
- `Create Event` had no selected function name.
- `K2Node_CreateDelegate_OutputDelegate` had an unknown delegate type because the output signature was not stabilized.
- `N2C_Rotator` produced `Can't parse default value`, proving that raw struct text was not canonicalized through the UE4.27 property serializer.

## Root causes

The final manual JSON encoded `SpawnTransform` as a pin default. That shape can pass node allocation and edge sandbox checks but cannot compile in UE4.27. The importer also invoked `UK2Node_CreateDelegate::HandleAnyChangeWithoutNotifying()` before its output pin was connected; UE4.27 intentionally clears an invalid unlinked selection. Member struct defaults were copied directly into `FBPVariableDescription::DefaultValue` without first proving that UE4.27 could import and re-export them in canonical serialized form.

## Restore observation

The backup queue was correct, but the same process also retried it from `OnPreExit`. At that point the package file could still be held by UE, so deletion failed. The manifest was not lost and remained available for the next startup.

## Version 190 prevention

- preflight requires a linked `SpawnTransform`;
- final fixture uses `MakeTransform`;
- CreateDelegate validation occurs after edge creation;
- exact same-patch delegate and linked-spawn compile/reopen regressions are mandatory;
- struct member defaults are canonicalized with `UScriptStruct::ImportText/ExportText`; invalid text is rejected before mutation;
- deferred restore executes only at startup in a fresh process.
