// Copyright (c) 2025 Nick McClure (Protospatial). All Rights Reserved.
// Back2Dead custom extension: AI-friendly Blueprint/Niagara export.

#pragma once

#include "CoreMinimal.h"

class UBlueprint;
class UObject;

/**
 * AI-oriented exporter used by the simplified Blueprint toolbar button "Export N2C".
 *
 * Output schema: N2C_AI_EXPORT_V2
 * - splits Blueprint graphs into functions/macros/events;
 * - keeps raw node/pin/link data;
 * - adds readable exec/data links and a simple linear flow;
 * - exports selected Niagara assets through read-only deep reflection + typed Niagara summaries without mutating them.
 */
class FN2CAIExport
{
public:
    /** Build AI-friendly JSON for a Blueprint. */
    static bool BuildBlueprintAIJson(UBlueprint* Blueprint, FString& OutJson, FString& OutError);

    /** Build AI-friendly JSON for one Niagara asset, including typed Niagara summary when possible. */
    static bool BuildNiagaraAssetAIJson(UObject* NiagaraAsset, FString& OutJson, FString& OutError);

    /** Build AI-friendly JSON for one UEnum / UUserDefinedEnum asset. */
    static bool BuildEnumAssetAIJson(UObject* EnumAsset, FString& OutJson, FString& OutError);

    /** Build AI-friendly JSON for one UScriptStruct / UUserDefinedStruct asset. */
    static bool BuildStructAssetAIJson(UObject* StructAsset, FString& OutJson, FString& OutError);

    /** Save JSON to a file and copy it to clipboard. */
    static bool SaveJsonToFile(const FString& Json, const FString& TargetPath, FString& OutError);

    /** Export selected Niagara assets from Content Browser into the same AI-friendly style. Safe read-only reflection + typed Niagara summary only. */
    static bool BuildSelectedNiagaraAIJson(FString& OutJson, FString& OutError);

private:
    FN2CAIExport() = delete;
};
