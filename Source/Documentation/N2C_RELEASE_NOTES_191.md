# NodeToCode Version 191 release notes

Version name: `1.2.91-ue427-k2-struct-default-format-hotfix-candidate`

## Fixed

Version 190 treated `UScriptStruct::ExportText` as a universal Blueprint default representation. That is incorrect for UE4.27 K2 custom-format structs. `FRotator` pins and Blueprint variable descriptions require CSV values accepted by `FDefaultValueHelper`, while named text such as `(Pitch=...,Yaw=...,Roll=...)` is serialized ImportText.

Version 191 now:

- parses named or CSV Vector/Rotator JSON values;
- stores them as K2 CSV before node-pin or member-variable assignment;
- validates authored pin defaults with `UEdGraphSchema_K2::IsPinDefaultValid`;
- rejects malformed defaults before mutation with `pin_default_invalid`;
- verifies persisted Rotator CSV after fresh-process reopen;
- retains named ImportText as the friendly AI JSON input format.

## Mandatory tests

- `NodeToCode.ManualReplay.ContextualEventGraph`: named MakeTransform defaults, compile/save/reopen, malformed Rotator pin rejection, and unlinked SpawnTransform rejection.
- `NodeToCode.ManualReplay.RawByteDefaultReopenExport`: raw byte plus Rotator member default, persisted K2 CSV, fresh-process snapshot equality and export.

## Proof status

Static validation passes are necessary but not sufficient. Run the complete UE4.27.2 automation and the interactive six-Blueprint patch before promotion.
