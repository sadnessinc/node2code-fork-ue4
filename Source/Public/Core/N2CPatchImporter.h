// Copyright (c) 2025 Nick McClure (Protospatial). All Rights Reserved.
// Back2Dead custom extension: safe Blueprint patch importer.

#pragma once

#include "CoreMinimal.h"
#include "Core/N2CCoverage.h"

class UBlueprint;
class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;
class UFunction;

/**
 * Strict JSON patch importer used by the simplified Blueprint toolbar button "Import N2C".
 *
 * Supported schema: N2C_PATCH_V1.  P0.2 runs coverage and metadata preflight before a
 * transaction. Developer override is explicit per operation and only relaxes
 * supported_untested verification gaps.
 */
class FN2CPatchImporter
{
public:
    /** Validate and optionally apply a patch JSON to a Blueprint. */
    static bool ApplyPatchToBlueprint(UBlueprint* Blueprint, const FString& PatchJson, FString& OutReport, bool bDeveloperOverride = false, bool bCompileAndSave = true);

    /** Validate patch shape without mutating the Blueprint. */
    static bool DryRunPatch(UBlueprint* Blueprint, const FString& PatchJson, FString& OutReport, bool bDeveloperOverride = false);

    /** Coverage-only preflight for UI, logs and narrow automation. */
    static bool PreflightPatch(UBlueprint* Blueprint, const FString& PatchJson, bool bDeveloperOverride, FN2CPreflightResult& OutResult, FString& OutReport);

private:
    FN2CPatchImporter() = delete;
};
