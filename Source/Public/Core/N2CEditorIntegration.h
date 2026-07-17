// Copyright (c) 2025 Nick McClure (Protospatial). All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "BlueprintEditorModule.h"
#include "BlueprintEditor.h"
#include "Utils/N2CLogger.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "AssetData.h"
#include "Widgets/SWidget.h"
#include "UObject/WeakObjectPtr.h"

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


private:
    /** Constructor */
    FN2CEditorIntegration() = default;

public:
    /** Get Blueprint Editor from active tab */
    TSharedPtr<FBlueprintEditor> GetBlueprintEditorFromTab() const;

    /** Register for Blueprint Editor callbacks */
    void RegisterBlueprintEditorCallback();

    /** Headless full-project export for automation; selects every supported asset kind under /Game. */
    bool ExportAllProjectAssetsForAutomation(FString& OutZipPath, FString& OutReport);

#if WITH_DEV_AUTOMATION_TESTS
    /** Queue a real deferred disk restore without opening a modal confirmation dialog. */
    bool QueueBackupRestoreForAutomation(
        UObject* Asset,
        const FString& BackupPath,
        FString& OutManifestPath,
        FString& OutReport);

    /** Read deterministic pending/applied restore diagnostics for automation assertions. */
    FString GetPendingRestoreStatusForAutomation() const;

    /** Convert one structured N2C diagnostic into the user-facing dialog wording. */
    FString FormatDiagnosticForAutomation(const FString& DiagnosticLine) const;
#endif

private:
    /** Map of Blueprint Editor instances to their command lists */
    TMap<TWeakPtr<FBlueprintEditor>, TSharedPtr<FUICommandList>> EditorCommandLists;

    /** Keep non-Blueprint asset editor toolbar command lists alive after registering per-editor extenders. */
    TArray<TSharedPtr<FUICommandList>> AssetEditorCommandLists;

    /** Level Editor toolbar command list and extender for project-level actions. */
    TSharedPtr<FUICommandList> LevelEditorCommandList;
    TSharedPtr<FExtender> LevelEditorToolbarExtender;
    FDelegateHandle ContentBrowserAssetExtenderDelegateHandle;
    FDelegateHandle ContentBrowserFolderExtenderDelegateHandle;

    /** Register toolbar for a specific Blueprint Editor */
    void RegisterToolbarForEditor(TSharedPtr<FBlueprintEditor> InEditor);

    /** Register N2C export/import toolbar buttons for a specific Niagara editor toolkit. */
    void RegisterToolbarForNiagaraEditor(UObject* NiagaraAsset, IAssetEditorInstance* EditorInstance);

    /** Register N2C export/import toolbar buttons for a specific Enum editor toolkit. */
    void RegisterToolbarForEnumEditor(UObject* EnumAsset, IAssetEditorInstance* EditorInstance);

    /** Register N2C export/import toolbar buttons for a specific Struct editor toolkit. */
    void RegisterToolbarForStructEditor(UObject* StructAsset, IAssetEditorInstance* EditorInstance);

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

    /** Main editor toolbar: open project export picker and export selected asset types into one ZIP. */
    void ExecuteBack2DeadExportProject();

    /** Legacy name kept for old command mappings; now redirects to ExecuteBack2DeadExportProject. */
    void ExecuteBack2DeadExportAllBlueprints();

    /** Main editor toolbar dropdown: export Blueprint assets from selected /Game folders into one ZIP. */
    void ExecuteBack2DeadExportBlueprintsFromFolderPicker();

    /** Main editor toolbar dropdown: export a text/JSON list of all project assets. */
    void ExecuteBack2DeadExportProjectFileList();

    /** Restore latest N2C backup for an asset. */
    void ExecuteRestoreLatestBackupForAsset(TWeakObjectPtr<UObject> Asset);

    /** Restore a user-selected N2C backup for an asset. */
    void ExecuteRestoreChosenBackupForAsset(TWeakObjectPtr<UObject> Asset);

    /** Backup restore dropdown for supported asset editors. */
    TSharedRef<SWidget> MakeRestoreBackupDropdownMenu(TWeakObjectPtr<UObject> Asset);

    /** Show queued/applied N2C deferred restore status. */
    void ExecuteShowPendingRestoreStatus();

    /** Cancel all queued N2C deferred restores before UE restart. */
    void ExecuteCancelPendingRestore();

    /** Cancel queued N2C deferred restore for one asset before UE restart. */
    void ExecuteCancelPendingRestoreForAsset(TWeakObjectPtr<UObject> Asset);

    /** Content Browser context menu: restore latest N2C backup for selected assets. */
    void ExecuteRestoreLatestBackupForSelectedAssets(TArray<FAssetData> SelectedAssets);

    /** Content Browser context menu: choose an N2C backup and restore it to the selected asset. */
    void ExecuteRestoreChosenBackupForSelectedAsset(TArray<FAssetData> SelectedAssets);

    /** Content Browser context menu: export selected Blueprint assets into one ZIP. */
    void ExecuteBack2DeadExportSelectedBlueprints(TArray<FAssetData> SelectedAssets);

    /** Content Browser folder context menu: export supported assets recursively after asking which types to include. */
    void ExecuteBack2DeadExportSelectedFoldersWithPicker(TArray<FString> SelectedPaths);

    /** Content Browser context menu: export selected assets after asking which supported types to include. */
    void ExecuteBack2DeadExportSelectedAssetsWithPicker(TArray<FAssetData> SelectedAssets);

    /** Content Browser context menu: export selected Niagara assets into one ZIP. */
    void ExecuteBack2DeadExportSelectedNiagara(TArray<FAssetData> SelectedAssets);

    /** Content Browser/editor: export selected Enum assets into one ZIP. */
    void ExecuteBack2DeadExportSelectedEnums(TArray<FAssetData> SelectedAssets);

    /** Content Browser/editor: export selected Struct assets into one ZIP. */
    void ExecuteBack2DeadExportSelectedStructs(TArray<FAssetData> SelectedAssets);

    /** Content Browser/editor: import N2C enum values into selected UserDefinedEnum assets. */
    void ExecuteBack2DeadImportSelectedEnums(TArray<FAssetData> SelectedAssets);

    /** Content Browser/editor: import N2C struct fields into selected UserDefinedStruct assets. */
    void ExecuteBack2DeadImportSelectedStructs(TArray<FAssetData> SelectedAssets);

    /** Content Browser context menu: import supported N2C Niagara parameter values into selected NiagaraSystem assets. */
    void ExecuteBack2DeadImportSelectedNiagara(TArray<FAssetData> SelectedAssets);

    /** Content Browser folder context menu: export all NiagaraSystem assets under selected folders recursively. */
    void ExecuteBack2DeadExportNiagaraFromSelectedFolders(TArray<FString> SelectedPaths);

    /** Niagara editor toolbar: export currently edited Niagara asset into one ZIP. */
    void ExecuteBack2DeadExportNiagaraEditor(TWeakObjectPtr<UObject> NiagaraAsset);

    /** Niagara editor toolbar: import supported N2C Niagara parameter values into the currently edited NiagaraSystem. */
    void ExecuteBack2DeadImportNiagaraEditor(TWeakObjectPtr<UObject> NiagaraAsset);

    /** Enum editor toolbar: import N2C enum values into the currently edited UserDefinedEnum. */
    void ExecuteBack2DeadImportEnumEditor(TWeakObjectPtr<UObject> EnumAsset);

    /** Struct editor toolbar: import N2C struct fields into the currently edited UserDefinedStruct. */
    void ExecuteBack2DeadImportStructEditor(TWeakObjectPtr<UObject> StructAsset);

    /** Register Content Browser right-click menu action for selected Blueprint/Niagara assets. */
    void RegisterContentBrowserAssetContextMenu();

    /** Register Content Browser right-click menu action for selected folders. */
    void RegisterContentBrowserFolderContextMenu();

    /** Extend Content Browser right-click menu for selected Blueprint/Niagara assets. */
    TSharedRef<FExtender> OnExtendContentBrowserAssetSelectionMenu(const TArray<FAssetData>& SelectedAssets);

    /** Extend Content Browser right-click menu for selected folders. */
    TSharedRef<FExtender> OnExtendContentBrowserFolderSelectionMenu(const TArray<FString>& SelectedPaths);

    /** Add actual Content Browser menu entry. */
    void AddContentBrowserN2CMenuEntry(FMenuBuilder& MenuBuilder, TArray<FAssetData> SelectedAssets);

    /** Add actual Content Browser folder menu entry. */
    void AddContentBrowserFolderN2CMenuEntry(FMenuBuilder& MenuBuilder, TArray<FString> SelectedPaths);

    /** Shared export implementation for all/selected Blueprint assets. */
    bool ExportBlueprintAssetsToSingleArchive(const TArray<FAssetData>& BlueprintAssets, const FString& ExportLabel, FString& OutZipPath, FString& OutReport);

    /** Shared export implementation for selected Niagara assets. */
    bool ExportNiagaraAssetsToSingleArchive(const TArray<FAssetData>& NiagaraAssets, const FString& ExportLabel, FString& OutZipPath, FString& OutReport);

    /** Shared export implementation for selected Enum assets. */
    bool ExportEnumAssetsToSingleArchive(const TArray<FAssetData>& EnumAssets, const FString& ExportLabel, FString& OutZipPath, FString& OutReport);

    /** Shared export implementation for selected Struct assets. */
    bool ExportStructAssetsToSingleArchive(const TArray<FAssetData>& StructAssets, const FString& ExportLabel, FString& OutZipPath, FString& OutReport);

    /** Shared export implementation for mixed Blueprint/NiagaraSystem/Enum/Struct archives. */
    bool ExportMixedAssetsToSingleArchive(const TArray<FAssetData>& BlueprintAssets, const TArray<FAssetData>& NiagaraAssets, const TArray<FAssetData>& EnumAssets, const TArray<FAssetData>& StructAssets, const FString& ExportLabel, FString& OutZipPath, FString& OutReport);
    
    /** Handle asset editor opened callback */
    void HandleAssetEditorOpened(UObject* Asset, IAssetEditorInstance* EditorInstance);

};
