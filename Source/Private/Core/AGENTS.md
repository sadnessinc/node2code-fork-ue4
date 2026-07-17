# Core importer/exporter rules

These rules refine the repository-level `AGENTS.md` for `Source/Private/Core`.

## Ownership

- `N2CPatchImporter.cpp`: Blueprint patch parsing, preflight, constructors, mutation, compile and rollback.
- `N2CAIExport.cpp`: Blueprint/Niagara/Enum/Struct export and constructor metadata.
- `N2CEditorIntegration.cpp`: UI, project archives, import/export dialogs, backup/restore and editor lifecycle.

## Constructor work

Before editing a constructor:

1. identify the exact JSON contract and current guard;
2. find the UE4.27 class declaration, allocation/reconstruction implementation and editor action/factory;
3. confirm the symbol is public/exported and the module dependency is correct;
4. set node-specific state in UE4.27 order before final pin allocation;
5. add pre-mutation validation;
6. add PASS and/or EXPECTED_REJECT automation coverage.

## Mutation safety

- parse and validate the complete action before clearing a graph;
- resolve assets/types/properties before mutation;
- remove partially created nodes on constructor failure;
- do not use broad `ReconstructNode()` as a repair guess;
- preserve raw pin names and reciprocal links;
- do not invent wildcard types during post-import repair;
- a rejected high-risk action must leave no mutation.

## Export/import synchronization

When constructor behavior changes, synchronize the exporter contract, importer guard, regression fixture, compatibility metadata, `N2C_CURRENT_STATE.md` and `N2C_TODO.md`. Do not update release version metadata until build/automation evidence exists.

## Large-file discipline

`N2CPatchImporter.cpp`, `N2CAIExport.cpp` and `N2CEditorIntegration.cpp` are large. Search for the exact helper/entry point and keep the edit local. Do not refactor unrelated regions during a constructor fix.
