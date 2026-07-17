# node2code architecture map — Version 173

## Runtime shape

```text
Toolbar / Content Browser / automation
                ↓
          import service
                ↓
 strict coverage + semantic preflight
                ↓
 transaction + apply constructors
                ↓
 graph repair → compile → save
          ↙                 ↘
 verified rollback          commit
          ↓
 queued disk restore when structural equality fails
```

## Module entry

- `Source/Private/NodeToCode.cpp` — startup/shutdown.
- `Source/NodeToCode.Build.cs` — Editor module dependencies, including BlueprintGraph, UMG, Niagara and AIModule.
- `NodeToCode.uplugin` — release metadata and Win64 Editor module declaration.

## Blueprint importer

- Public API: `Source/Public/Core/N2CPatchImporter.h`.
- Implementation: `Source/Private/Core/N2CPatchImporter.cpp`.
- Main responsibilities: JSON parsing, action validation, coverage/semantic preflight, type/identity resolution, node construction, reconstruction, pin resolution, edge creation, compile/save orchestration and verified rollback.
- Shared pin aliases and high-risk context checks must stay consistent with `N2CCoverage`.
- Raw `PC_Byte` is valid without a subtype only for explicit raw Byte declarations. Enum-backed Byte requires `UEnum`.

## Structural verification

- `N2CStructuralSnapshot.*` builds stable Blueprint state and compares persistence/rollback contracts.
- `N2CRoundTripVerification.*` performs apply, compile, save, strict unload, child `UE4Editor-Cmd`, reopen, recompile, expected-contract comparison and cleanup.

## Restore and UI

- `N2CEditorIntegration.cpp` owns toolbar/content-browser actions, user-facing diagnostics, backup selection, pending restore manifests and startup application.
- Automation-only wrappers are guarded by `WITH_DEV_AUTOMATION_TESTS` and do not expose fault behavior in normal builds.
- Pending restore folders live under the project `Saved/NodeToCode/Backups` tree, never inside the source plugin.

## Export

- `N2CAIExport.cpp` exports Blueprint/Enum/Struct/Niagara JSON.
- Raw Byte exports with category `byte`, its numeric default and no enum subtype object.

## Automation

- `N2CVerificationTests.cpp` contains legacy verification and the Version 173 ManualReplay suite.
- `Source/Tests/Fixtures/ManualReplay/N2C_MANUAL_REPLAY_MANIFEST_V1.json` is an inventory, not PASS evidence.
- `Scripts/RUN_N2C_AUTOMATION_AND_PACK.cmd` invokes `Scripts/Codex/Invoke-N2CFullValidation.ps1`.

## Refactoring boundary

The Version 173 refactor is intentionally local. The two largest Editor/importer translation units remain large; a broad architectural rewrite is deferred until this candidate passes a real UE4.27 baseline.
