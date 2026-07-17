# Automation failure audit — 2026-07-16 12:59:08 / Version 191

Bundle: `N2C_Automation_20260716_125908.zip`

## Stage result

- PowerShell syntax preflight: PASS
- static validation: PASS
- `testEditor` build: PASS
- Main ManualReplay: 19/21 PASS
- failed: `ContextualEventGraph`, `RawByteDefaultReopenExport`
- restore stages: correctly skipped because the main suite failed
- source package: PASS

## ContextualEventGraph

The positive SpawnActor case failed in transient sandbox preflight:

```text
N2C_RUNTIME_GUARD|code=pin_default_invalid|node=K2Node_SpawnActorFromClass|pin=Class|value=/Script/Engine.Actor
String NewDefaultValue '/Script/Engine.Actor' specified on class pin 'Class'
```

The path itself was valid. The importer had resolved the class into `DefaultObject` but still validated/retained the same path as `DefaultValue`. UE4.27 class pins require the resolved object and an empty string value. The Version 191 broad validator therefore rejected a valid positive fixture.

Corrective action: delegate authored-default parsing to `UEdGraphSchema_K2::GetPinDefaultValuesFromString`, validate the converted tuple, then assign all three default fields.

## RawByteDefaultReopenExport

The round-trip report itself passed all stages. Export proved:

```text
raw default = 37
rotator export = (Pitch=10.000000,Yaw=20.000000,Roll=30.000000)
```

The failure was only:

```text
stored_k2_rotator=0
stored_rotator=
```

`FBPVariableDescription::DefaultValue` may be cleared after compile. It is not the authoritative runtime storage of a compiled member. Corrective action: read the generated-class CDO through the reflected property, then also verify fresh export.

## Similar-path audit

The fix was applied to the shared pin-default pipeline rather than only SpawnActor. JSON fixtures and source were audited for object/class/text/default writes. Literal GetDataTableRow table pins and Tunnel boundary defaults were migrated to the same schema-native conversion; validators now reject the old direct-write patterns. Current authoring rules require schema-native conversion and semantic persistence checks.
