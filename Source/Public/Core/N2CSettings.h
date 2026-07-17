// Copyright (c) 2025 Nick McClure (Protospatial). All Rights Reserved.
// Back2Dead/node2code fork: direct-codegen settings removed.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "Models/N2CLogging.h"
#include "N2CSettings.generated.h"

/** Clean visible editor preferences for the Back2Dead Node2Code fork. */
UCLASS(Config = EditorPerProjectUserSettings, DefaultConfig, meta = (Category = "Plugins", DisplayName = "Node2Code"))
class NODETOCODE_API UN2CEditorPreferences : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    virtual FName GetContainerName() const override { return TEXT("Editor"); }
    virtual FName GetCategoryName() const override { return TEXT("Plugins"); }
    virtual FName GetSectionName() const override { return TEXT("Node2Code"); }
    virtual FText GetSectionText() const override { return NSLOCTEXT("NodeToCode", "Node2CodeEditorPreferencesSection", "Node2Code"); }

    UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Blueprint Assistant Extension",
        meta = (DisplayName = "Auto Format Imported Functions",
                ToolTip = "If enabled, Node2Code checks whether Blueprint Assist exists and is enabled after import, then formats every function graph changed by the patch."))
    bool bAutoFormatImportedFunctionsWithBlueprintAssist = false;
};

/**
 * Legacy settings object kept only for source compatibility with the remaining exporter/importer code.
 * It is not registered as the visible settings page and no longer contains provider/API/model/codegen options.
 */
UCLASS(Config = NodeToCode, DefaultConfig, meta = (Category = "Plugins", DisplayName = "Node2Code Legacy"))
class NODETOCODE_API UN2CSettings : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    UN2CSettings();

    virtual FName GetContainerName() const override { return TEXT("Editor"); }
    virtual FName GetCategoryName() const override { return TEXT("Plugins"); }
    virtual FName GetSectionName() const override { return TEXT("Node2CodeLegacy"); }
    virtual FText GetSectionText() const override;

    UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Node2Code | Logging")
    EN2CLogSeverity MinSeverity = EN2CLogSeverity::Info;

    // Used by the old raw JSON collector path only. Kept so FN2CNodeTranslator compiles.
    UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Node2Code | Legacy Export", meta=(ClampMin="0", ClampMax="5", UIMin="0", UIMax="5"))
    int32 TranslationDepth = 0;

#if WITH_EDITOR
    virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

    EN2CLogSeverity GetMinLogSeverity() const { return MinSeverity; }
};
