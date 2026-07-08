// Copyright (c) 2025 Nick McClure (Protospatial). All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Docking/SDockTab.h"
#include "LLM/N2CLLMTypes.h"
#include "Models/N2CTranslation.h"
#include "Code Editor/Models/N2CCodeLanguage.h"

class SN2CCodeEditor;
class SWidgetSwitcher;
class STextBlock;

class SN2CEditorWindow : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SN2CEditorWindow)
    {}
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);
    virtual ~SN2CEditorWindow();

    /** Register the tab spawner */
    static void RegisterTabSpawner();

    /** Unregister the tab spawner */
    static void UnregisterTabSpawner();

    /** Create and show the window as a dockable tab */
    static TSharedRef<SDockTab> SpawnTab(const FSpawnTabArgs& Args);

    /** Handler for when the tab is closed */
    static void OnTabClosed(TSharedRef<SDockTab> ClosedTab);

    /** Get the tab identifier */
    static const FName TabId;

    /** Check if the tab is currently open */
    static bool IsTabOpen() { return ActiveTab.IsValid(); }

    /** Close the existing tab so it can be re-spawned in a different tab well */
    static void CloseTab();

private:
    // --- Delegate handlers ---
    void HandleTranslationRequestSent();
    void HandleTranslationResponseReceived(const FN2CTranslationResponse& Response, bool bSuccess);

    // --- UI population ---
    void PopulateUIForGraph(int32 GraphIndex);
    void SetActivePanel(int32 PanelIndex);

    // --- Button callbacks ---
    FReply OnCopyDeclarationClicked();
    FReply OnCopyImplementationClicked();
    FReply OnOpenExplorerClicked();

    // --- Combo box ---
    void OnGraphSelectionChanged(TSharedPtr<FString> NewValue, ESelectInfo::Type SelectInfo);
    TSharedRef<SWidget> GenerateGraphComboItem(TSharedPtr<FString> InItem);

    // --- Helpers ---
    EN2CCodeLanguage GetTargetLanguage() const;
    FName GetActiveTheme() const;

    // --- State ---
    int32 CurrentGraphIndex = 0;
    FN2CTranslationResponse CachedResponse;
    FDelegateHandle RequestSentHandle;
    FDelegateHandle ResponseReceivedHandle;

    // --- Widget references ---
    TSharedPtr<SWidgetSwitcher> PanelSwitcher;
    TSharedPtr<SN2CCodeEditor> DeclarationEditor;
    TSharedPtr<SN2CCodeEditor> ImplementationEditor;
    TSharedPtr<STextBlock> StatusText;
    TSharedPtr<STextBlock> NotesText;
    TSharedPtr<STextBlock> ErrorText;
    TSharedPtr<SWidget> DeclarationSection;
    TSharedPtr<SWidget> ToolbarWidget;
    TSharedPtr<SComboBox<TSharedPtr<FString>>> GraphSelector;
    TArray<TSharedPtr<FString>> GraphNameOptions;

    /** The currently active tab */
    static TWeakPtr<SDockTab> ActiveTab;
};
