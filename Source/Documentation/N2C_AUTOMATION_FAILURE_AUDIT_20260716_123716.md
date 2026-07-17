# Version 190 automation failure audit — 2026-07-16 12:37

## Result

Build passed. Main ManualReplay completed 21 cases with 19 PASS and 2 FAIL: `ContextualEventGraph` and `RawByteDefaultReopenExport`. Restore stages were correctly skipped after main failure; packaging still passed.

## Root cause

The Version 190 canonicalizer parsed struct text correctly, then wrote `UScriptStruct::ExportText` output back into Blueprint storage. For Rotator this produced `(Pitch=...,Yaw=...,Roll=...)`. UE4.27 `UEdGraphSchema_K2` validates Rotator pins through `FDefaultValueHelper`, whose accepted K2 form is three comma-separated floats.

`ContextualEventGraph` therefore failed compile on `MakeTransform.Rotation`. `RawByteDefaultReopenExport` compiled with a warning for the Rotator member default, then failed fresh-process persistence comparison because the default did not survive correctly. The raw byte implementation itself was not the cause.

## Prevention

Version 191 separates serialized struct ImportText from K2 default storage, validates every authored non-wildcard pin default before mutation, and adds positive/negative regressions for the exact failure.
