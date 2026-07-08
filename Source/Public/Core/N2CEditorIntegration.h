// Copyright (c) 2025 Nick McClure (Protospatial). All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "BlueprintEditorModule.h"
#include "BlueprintEditor.h"
#include "Code Editor/Models/N2CCodeLanguage.h"
#include "Utils/N2CLogger.h"
#include "LLM/IN2CLLMService.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "AssetData.h"
#include "Widgets/SWidget.h"

/**
 * @class FN2CEditorIntegration
 * @brief Handles integration with the Blueprint Editor
 *
 * Manages Blueprint Editor toolbar extensions and provides
 * access to the active Blueprint Editor instance.
 */
class FN2CEditorIntegration
{
public:
    static FN2CEditorIntegration& Get();

    /** Initialize integration with Blueprint Editor */
    void Initialize();

    /** Cleanup integration */
    void Shutdown();

    /** Get available themes for a language */
    TArray<FName> GetAvailableThemes(EN2CCodeLanguage Language) const;

    /** Get the default theme for a language */
    FName GetDefaultTheme(EN2CCodeLanguage Language) const;

private:
    /** Constructor */
    FN2CEditorIntegration() = default;

public:
    /** Get Blueprint Editor from active tab */
    TSharedPtr<FBlueprintEditor> GetBlueprintEditorFromTab() const;

    /** Register for Blueprint Editor callbacks */
    void RegisterBlueprintEditorCallback();

private:
    /** Map of Blueprint Editor instances to their command lists */
    TMap<TWeakPtr<FBlueprintEditor>, TSharedPtr<FUICommandList>> EditorCommandLists;

    /** Level Editor toolbar command list and extender for project-level actions. */
    TSharedPtr<FUICommandList> LevelEditorCommandList;
    TSharedPtr<FExtender> LevelEditorToolbarExtender;
    FDelegateHandle ContentBrowserAssetExtenderDelegateHandle;

    /** Register toolbar for a specific Blueprint Editor */
    void RegisterToolbarForEditor(TSharedPtr<FBlueprintEditor> InEditor);

    /** Register project-level N2C buttons in the main Level Editor toolbar. */
    void RegisterLevelEditorToolbar();

    /** Fill project-level N2C toolbar section. */
    void FillLevelEditorToolbar(FToolBarBuilder& Builder);

    /** Dropdown menu for project export actions. */
    TSharedRef<SWidget> MakeExportAllDropdownMenu();

    /** Legacy action kept for compatibility with old source; no longer exposed in the toolbar. */
    void ExecuteCollectNodesForEditor(TWeakPtr<FBlueprintEditor> InEditor);

    /** Legacy raw JSON export kept for compatibility; no longer exposed in the toolbar. */
    void ExecuteCopyJsonForEditor(TWeakPtr<FBlueprintEditor> InEditor);

    /** Button 1: AI-friendly export for current Blueprint. */
    void ExecuteBack2DeadExportForEditor(TWeakPtr<FBlueprintEditor> InEditor);

    /** Button 2: load N2C_PATCH_V1 from clipboard/file and apply it safely. */
    void ExecuteBack2DeadImportForEditor(TWeakPtr<FBlueprintEditor> InEditor);

    /** Main editor toolbar: import N2C_PROJECT_PATCH_V1 without requiring a Blueprint editor tab. */
    void ExecuteBack2DeadProjectImportFromEditor();

    /** Main editor toolbar: export all project Blueprint assets into one ZIP. */
    void ExecuteBack2DeadExportAllBlueprints();

    /** Main editor toolbar dropdown: export Blueprint assets from selected /Game folders into one ZIP. */
    void ExecuteBack2DeadExportBlueprintsFromFolderPicker();

    /** Main editor toolbar dropdown: export a text/JSON list of all project assets. */
    void ExecuteBack2DeadExportProjectFileList();

    /** Content Browser context menu: export selected Blueprint assets into one ZIP. */
    void ExecuteBack2DeadExportSelectedBlueprints(TArray<FAssetData> SelectedAssets);

    /** Register Content Browser right-click menu action for selected Blueprint assets. */
    void RegisterContentBrowserAssetContextMenu();

    /** Extend Content Browser right-click menu for selected Blueprint assets. */
    TSharedRef<FExtender> OnExtendContentBrowserAssetSelectionMenu(const TArray<FAssetData>& SelectedAssets);

    /** Add actual Content Browser menu entry. */
    void AddContentBrowserN2CMenuEntry(FMenuBuilder& MenuBuilder, TArray<FAssetData> SelectedAssets);

    /** Shared export implementation for all/selected Blueprint assets. */
    bool ExportBlueprintAssetsToSingleArchive(const TArray<FAssetData>& BlueprintAssets, const FString& ExportLabel, FString& OutZipPath, FString& OutReport);
    
    /** Handle asset editor opened callback */
    void HandleAssetEditorOpened(UObject* Asset, IAssetEditorInstance* EditorInstance);

};
