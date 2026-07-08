// Copyright (c) 2025 Nick McClure (Protospatial). All Rights Reserved.
// Back2Dead custom extension: safe Blueprint patch importer.

#pragma once

#include "CoreMinimal.h"

class UBlueprint;
class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;
class UFunction;

/**
 * Strict JSON patch importer used by the simplified Blueprint toolbar button "Import N2C".
 *
 * Supported schema: N2C_PATCH_V1
 * Supported safe actions:
 * - add_or_replace_function
 * - replace_function_body
 *
 * This importer is intentionally conservative. Unsupported nodes are not guessed: they are
 * recorded as warnings and, when requested, left as comments instead of causing unsafe graph
 * mutations. Every apply pass creates a .uasset backup before mutating the Blueprint.
 */
class FN2CPatchImporter
{
public:
    /** Validate and optionally apply a patch JSON to a Blueprint. */
    static bool ApplyPatchToBlueprint(UBlueprint* Blueprint, const FString& PatchJson, FString& OutReport);

    /** Validate patch shape without mutating the Blueprint. */
    static bool DryRunPatch(UBlueprint* Blueprint, const FString& PatchJson, FString& OutReport);

private:
    FN2CPatchImporter() = delete;
};
