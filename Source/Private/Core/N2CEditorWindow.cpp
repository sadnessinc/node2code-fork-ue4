// Copyright (c) 2025 Nick McClure (Protospatial). All Rights Reserved.

#include "Core/N2CEditorWindow.h"
#include "Core/N2CSettings.h"
#include "Core/N2CEditorIntegration.h"
#include "LLM/N2CLLMModule.h"
#include "Code Editor/Widgets/SN2CCodeEditor.h"
#include "Utils/N2CLogger.h"

#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Images/SThrobber.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboBox.h"
#include "EditorStyleSet.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsPlatformApplicationMisc.h"
#endif

#if PLATFORM_MAC
#include "Mac/MacPlatformApplicationMisc.h"
#endif

#define LOCTEXT_NAMESPACE "NodeToCode"

const FName SN2CEditorWindow::TabId(TEXT("NodeToCodeEditor"));
TWeakPtr<SDockTab> SN2CEditorWindow::ActiveTab;

// ─────────────────────────────────────────────
// Tab management (unchanged from original)
// ─────────────────────────────────────────────

void SN2CEditorWindow::RegisterTabSpawner()
{
    FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
        TabId,
        FOnSpawnTab::CreateStatic(&SN2CEditorWindow::SpawnTab))
        .SetDisplayName(LOCTEXT("TabTitle", "Node to Code"))
        .SetMenuType(ETabSpawnerMenuType::Hidden)
        .SetIcon(FSlateIcon("NodeToCodeStyle", "NodeToCode.TabIcon"));

    FN2CLogger::Get().Log(TEXT("Registered Node to Code tab spawner"), EN2CLogSeverity::Info);
}

void SN2CEditorWindow::UnregisterTabSpawner()
{
    FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(TabId);
    FN2CLogger::Get().Log(TEXT("Unregistered Node to Code tab spawner"), EN2CLogSeverity::Info);
}

TSharedRef<SDockTab> SN2CEditorWindow::SpawnTab(const FSpawnTabArgs& Args)
{
    if (TSharedPtr<SDockTab> ExistingTab = ActiveTab.Pin())
    {
        ExistingTab->DrawAttention();
        return ExistingTab.ToSharedRef();
    }

    TSharedRef<SDockTab> SpawnedTab = SNew(SDockTab)
        .TabRole(ETabRole::NomadTab)
        .OnTabClosed_Static(&SN2CEditorWindow::OnTabClosed);

    ActiveTab = SpawnedTab;

    TSharedRef<SN2CEditorWindow> EditorWindow = SNew(SN2CEditorWindow);
    SpawnedTab->SetContent(EditorWindow);

    return SpawnedTab;
}

void SN2CEditorWindow::OnTabClosed(TSharedRef<SDockTab> ClosedTab)
{
    if (ActiveTab.Pin() == ClosedTab)
    {
        ActiveTab.Reset();
    }
}

void SN2CEditorWindow::CloseTab()
{
    TSharedPtr<SDockTab> Tab = ActiveTab.Pin();
    if (Tab.IsValid())
    {
        Tab->RequestCloseTab();
    }
}

// ─────────────────────────────────────────────
// Construction & destruction
// ─────────────────────────────────────────────

void SN2CEditorWindow::Construct(const FArguments& InArgs)
{
    EN2CCodeLanguage Language = GetTargetLanguage();
    FName ThemeName = GetActiveTheme();

    ChildSlot
    [
        SNew(SVerticalBox)

        // ── Toolbar ──
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(0)
        [
            SAssignNew(ToolbarWidget, SBorder)
            .BorderImage(FEditorStyle::GetBrush("ToolPanel.GroupBorder"))
            .Padding(FMargin(4.f, 2.f))
            [
                SNew(SHorizontalBox)

                // Graph selector combo box
                + SHorizontalBox::Slot()
                .AutoWidth()
                .VAlign(VAlign_Center)
                .Padding(2.f, 0.f)
                [
                    SAssignNew(GraphSelector, SComboBox<TSharedPtr<FString>>)
                    .OptionsSource(&GraphNameOptions)
                    .OnSelectionChanged(this, &SN2CEditorWindow::OnGraphSelectionChanged)
                    .OnGenerateWidget(this, &SN2CEditorWindow::GenerateGraphComboItem)
                    .Visibility(EVisibility::Collapsed)
                    [
                        SNew(STextBlock)
                        .Text_Lambda([this]() -> FText
                        {
                            if (GraphNameOptions.IsValidIndex(CurrentGraphIndex) && GraphNameOptions[CurrentGraphIndex].IsValid())
                            {
                                return FText::FromString(*GraphNameOptions[CurrentGraphIndex]);
                            }
                            return FText::GetEmpty();
                        })
                    ]
                ]

                + SHorizontalBox::Slot()
                .AutoWidth()
                .VAlign(VAlign_Center)
                .Padding(4.f, 0.f)
                [
                    SAssignNew(StatusText, STextBlock)
                    .Text(LOCTEXT("Ready", "Ready"))
                    .ColorAndOpacity(FSlateColor(FLinearColor(0.7f, 0.7f, 0.7f)))
                ]

                // Flexible spacer
                + SHorizontalBox::Slot()
                .FillWidth(1.f)
                [
                    SNew(SSpacer)
                ]

                // Copy Header button (C++ only)
                + SHorizontalBox::Slot()
                .AutoWidth()
                .VAlign(VAlign_Center)
                .Padding(2.f, 0.f)
                [
                    SNew(SButton)
                    .Text(LOCTEXT("CopyHeader", "Copy Header"))
                    .ToolTipText(LOCTEXT("CopyHeaderTip", "Copy the header declaration to clipboard"))
                    .OnClicked(this, &SN2CEditorWindow::OnCopyDeclarationClicked)
                    .Visibility_Lambda([this]() -> EVisibility
                    {
                        if (GetTargetLanguage() == EN2CCodeLanguage::Cpp &&
                            CachedResponse.Graphs.IsValidIndex(CurrentGraphIndex) &&
                            !CachedResponse.Graphs[CurrentGraphIndex].Code.GraphDeclaration.IsEmpty())
                        {
                            return EVisibility::Visible;
                        }
                        return EVisibility::Collapsed;
                    })
                ]

                // Copy Code button
                + SHorizontalBox::Slot()
                .AutoWidth()
                .VAlign(VAlign_Center)
                .Padding(2.f, 0.f)
                [
                    SNew(SButton)
                    .Text(LOCTEXT("CopyCode", "Copy Code"))
                    .ToolTipText(LOCTEXT("CopyCodeTip", "Copy the implementation code to clipboard"))
                    .OnClicked(this, &SN2CEditorWindow::OnCopyImplementationClicked)
                ]

                // Open Folder button
                + SHorizontalBox::Slot()
                .AutoWidth()
                .VAlign(VAlign_Center)
                .Padding(2.f, 0.f)
                [
                    SNew(SButton)
                    .Text(LOCTEXT("OpenFolder", "Open Folder"))
                    .ToolTipText(LOCTEXT("OpenFolderTip", "Open the saved translation files in your file explorer"))
                    .OnClicked(this, &SN2CEditorWindow::OnOpenExplorerClicked)
                ]
            ]
        ]

        // ── Main Content (SWidgetSwitcher) ──
        + SVerticalBox::Slot()
        .FillHeight(1.f)
        [
            SAssignNew(PanelSwitcher, SWidgetSwitcher)
            .WidgetIndex(0)

            // [0] Welcome panel
            + SWidgetSwitcher::Slot()
            [
                SNew(SBox)
                .HAlign(HAlign_Center)
                .VAlign(VAlign_Center)
                [
                    SNew(STextBlock)
                    .Text(LOCTEXT("Welcome", "Select Blueprint nodes and click 'Node to Code' to translate them."))
                    .ColorAndOpacity(FSlateColor(FLinearColor(0.5f, 0.5f, 0.5f)))
                    .Justification(ETextJustify::Center)
                ]
            ]

            // [1] Loading panel
            + SWidgetSwitcher::Slot()
            [
                SNew(SBox)
                .HAlign(HAlign_Center)
                .VAlign(VAlign_Center)
                [
                    SNew(SVerticalBox)

                    + SVerticalBox::Slot()
                    .AutoHeight()
                    .HAlign(HAlign_Center)
                    .Padding(0, 0, 0, 8)
                    [
                        SNew(SThrobber)
                    ]

                    + SVerticalBox::Slot()
                    .AutoHeight()
                    .HAlign(HAlign_Center)
                    [
                        SNew(STextBlock)
                        .Text(LOCTEXT("Translating", "Translating Blueprint to code..."))
                    ]
                ]
            ]

            // [2] Results panel
            + SWidgetSwitcher::Slot()
            [
                SNew(SScrollBox)

                // Declaration section (C++ headers)
                + SScrollBox::Slot()
                .Padding(4.f)
                [
                    SAssignNew(DeclarationSection, SVerticalBox)
                    .Visibility(EVisibility::Collapsed)

                    + SVerticalBox::Slot()
                    .AutoHeight()
                    .Padding(4.f, 8.f, 4.f, 2.f)
                    [
                        SNew(STextBlock)
                        .Text(LOCTEXT("DeclarationHeader", "Header / Declaration"))
                        .Font(FCoreStyle::GetDefaultFontStyle("Bold", 11))
                    ]

                    + SVerticalBox::Slot()
                    .AutoHeight()
                    .Padding(4.f, 0.f)
                    [
                        SNew(SBox)
                        .MinDesiredHeight(150.f)
                        [
                            SAssignNew(DeclarationEditor, SN2CCodeEditor)
                            .Text(FText::GetEmpty())
                            .Language(Language)
                            .ThemeName(ThemeName)
                        ]
                    ]

                    + SVerticalBox::Slot()
                    .AutoHeight()
                    .Padding(4.f, 4.f)
                    [
                        SNew(SSeparator)
                    ]
                ]

                // Implementation section
                + SScrollBox::Slot()
                .Padding(4.f)
                [
                    SNew(SVerticalBox)

                    + SVerticalBox::Slot()
                    .AutoHeight()
                    .Padding(4.f, 8.f, 4.f, 2.f)
                    [
                        SNew(STextBlock)
                        .Text(LOCTEXT("ImplementationHeader", "Implementation"))
                        .Font(FCoreStyle::GetDefaultFontStyle("Bold", 11))
                    ]

                    + SVerticalBox::Slot()
                    .AutoHeight()
                    .Padding(4.f, 0.f)
                    [
                        SNew(SBox)
                        .MinDesiredHeight(300.f)
                        [
                            SAssignNew(ImplementationEditor, SN2CCodeEditor)
                            .Text(FText::GetEmpty())
                            .Language(Language)
                            .ThemeName(ThemeName)
                        ]
                    ]
                ]

                // Implementation notes (expandable)
                + SScrollBox::Slot()
                .Padding(4.f)
                [
                    SNew(SExpandableArea)
                    .AreaTitle(LOCTEXT("NotesTitle", "Implementation Notes"))
                    .InitiallyCollapsed(false)
                    .BodyContent()
                    [
                        SNew(SBox)
                        .Padding(8.f)
                        [
                            SAssignNew(NotesText, STextBlock)
                            .Text(FText::GetEmpty())
                            .AutoWrapText(true)
                        ]
                    ]
                ]
            ]

            // [3] Error panel
            + SWidgetSwitcher::Slot()
            [
                SNew(SBox)
                .HAlign(HAlign_Center)
                .VAlign(VAlign_Center)
                .Padding(20.f)
                [
                    SNew(SVerticalBox)

                    + SVerticalBox::Slot()
                    .AutoHeight()
                    .HAlign(HAlign_Center)
                    .Padding(0, 0, 0, 8)
                    [
                        SNew(STextBlock)
                        .Text(LOCTEXT("ErrorTitle", "Translation Failed"))
                        .Font(FCoreStyle::GetDefaultFontStyle("Bold", 14))
                        .ColorAndOpacity(FSlateColor(FLinearColor(0.9f, 0.2f, 0.2f)))
                    ]

                    + SVerticalBox::Slot()
                    .AutoHeight()
                    .HAlign(HAlign_Center)
                    [
                        SAssignNew(ErrorText, STextBlock)
                        .Text(LOCTEXT("ErrorDefault", "An error occurred during translation."))
                        .AutoWrapText(true)
                        .Justification(ETextJustify::Center)
                    ]
                ]
            ]
        ]
    ];

    // Subscribe to LLM module delegates
    UN2CLLMModule* LLMModule = UN2CLLMModule::Get();
    if (LLMModule)
    {
        RequestSentHandle = LLMModule->OnTranslationRequestSentNative.AddRaw(
            this, &SN2CEditorWindow::HandleTranslationRequestSent);
        ResponseReceivedHandle = LLMModule->OnTranslationResponseReceivedNative.AddRaw(
            this, &SN2CEditorWindow::HandleTranslationResponseReceived);

        // If a translation is already in progress, show loading state
        if (LLMModule->GetSystemStatus() == EN2CSystemStatus::Processing)
        {
            SetActivePanel(1);
        }
    }

    FN2CLogger::Get().Log(TEXT("SN2CEditorWindow constructed"), EN2CLogSeverity::Info);
}

SN2CEditorWindow::~SN2CEditorWindow()
{
    UN2CLLMModule* LLMModule = UN2CLLMModule::Get();
    if (LLMModule)
    {
        LLMModule->OnTranslationRequestSentNative.Remove(RequestSentHandle);
        LLMModule->OnTranslationResponseReceivedNative.Remove(ResponseReceivedHandle);
    }
}

// ─────────────────────────────────────────────
// Delegate handlers
// ─────────────────────────────────────────────

void SN2CEditorWindow::HandleTranslationRequestSent()
{
    SetActivePanel(1); // Loading
    StatusText->SetText(LOCTEXT("StatusTranslating", "Translating..."));
}

void SN2CEditorWindow::HandleTranslationResponseReceived(const FN2CTranslationResponse& Response, bool bSuccess)
{
    if (!bSuccess || Response.Graphs.Num() == 0)
    {
        ErrorText->SetText(LOCTEXT("ErrorNoGraphs", "Translation failed or returned no results.\nCheck the Output Log for details."));
        SetActivePanel(3); // Error
        StatusText->SetText(LOCTEXT("StatusError", "Error"));
        return;
    }

    // Cache the response
    CachedResponse = Response;

    // Rebuild graph name options for the combo box
    GraphNameOptions.Empty();
    for (const FN2CGraphTranslation& Graph : Response.Graphs)
    {
        GraphNameOptions.Add(MakeShareable(new FString(Graph.GraphName)));
    }

    // Pick the best default graph: prefer EventGraph, then Function graphs, then first
    CurrentGraphIndex = 0;
    for (int32 i = 0; i < Response.Graphs.Num(); ++i)
    {
        if (Response.Graphs[i].GraphType == TEXT("EventGraph"))
        {
            CurrentGraphIndex = i;
            break;
        }
    }

    // Show/hide graph selector
    if (GraphSelector.IsValid())
    {
        GraphSelector->SetVisibility(Response.Graphs.Num() > 1 ? EVisibility::Visible : EVisibility::Collapsed);
        GraphSelector->RefreshOptions();
        if (GraphNameOptions.IsValidIndex(CurrentGraphIndex))
        {
            GraphSelector->SetSelectedItem(GraphNameOptions[CurrentGraphIndex]);
        }
    }

    // Update token usage in status
    FString StatusStr;
    if (Response.Usage.InputTokens > 0 || Response.Usage.OutputTokens > 0)
    {
        StatusStr = FString::Printf(TEXT("Tokens: %d in / %d out"), Response.Usage.InputTokens, Response.Usage.OutputTokens);
    }
    else
    {
        StatusStr = TEXT("Translation complete");
    }
    StatusText->SetText(FText::FromString(StatusStr));

    // Populate the UI with the selected default graph
    PopulateUIForGraph(CurrentGraphIndex);
    SetActivePanel(2); // Results

    FN2CLogger::Get().Log(TEXT("Editor window populated with translation results"), EN2CLogSeverity::Info);
}

// ─────────────────────────────────────────────
// UI population
// ─────────────────────────────────────────────

void SN2CEditorWindow::PopulateUIForGraph(int32 GraphIndex)
{
    if (!CachedResponse.Graphs.IsValidIndex(GraphIndex))
    {
        return;
    }

    CurrentGraphIndex = GraphIndex;
    const FN2CGraphTranslation& Graph = CachedResponse.Graphs[GraphIndex];

    // Update language on editors if it changed
    EN2CCodeLanguage Language = GetTargetLanguage();
    if (DeclarationEditor.IsValid())
    {
        DeclarationEditor->SetLanguage(Language);
    }
    if (ImplementationEditor.IsValid())
    {
        ImplementationEditor->SetLanguage(Language);
    }

    // Declaration (C++ header)
    bool bHasDeclaration = !Graph.Code.GraphDeclaration.IsEmpty();
    if (DeclarationSection.IsValid())
    {
        DeclarationSection->SetVisibility(bHasDeclaration ? EVisibility::Visible : EVisibility::Collapsed);
    }
    if (bHasDeclaration && DeclarationEditor.IsValid())
    {
        DeclarationEditor->SetText(FText::FromString(Graph.Code.GraphDeclaration));
    }

    // Implementation
    if (ImplementationEditor.IsValid())
    {
        ImplementationEditor->SetText(FText::FromString(Graph.Code.GraphImplementation));
    }

    // Notes
    if (NotesText.IsValid())
    {
        NotesText->SetText(FText::FromString(
            Graph.Code.ImplementationNotes.IsEmpty()
                ? TEXT("No implementation notes provided.")
                : Graph.Code.ImplementationNotes));
    }
}

void SN2CEditorWindow::SetActivePanel(int32 PanelIndex)
{
    if (PanelSwitcher.IsValid())
    {
        PanelSwitcher->SetActiveWidgetIndex(PanelIndex);
    }
}

// ─────────────────────────────────────────────
// Button callbacks
// ─────────────────────────────────────────────

FReply SN2CEditorWindow::OnCopyDeclarationClicked()
{
    if (CachedResponse.Graphs.IsValidIndex(CurrentGraphIndex))
    {
        const FString& Declaration = CachedResponse.Graphs[CurrentGraphIndex].Code.GraphDeclaration;
        if (!Declaration.IsEmpty())
        {
            FPlatformApplicationMisc::ClipboardCopy(*Declaration);

            FNotificationInfo Info(LOCTEXT("DeclCopied", "Header declaration copied to clipboard"));
            Info.bFireAndForget = true;
            Info.ExpireDuration = 2.0f;
            FSlateNotificationManager::Get().AddNotification(Info);
        }
    }
    return FReply::Handled();
}

FReply SN2CEditorWindow::OnCopyImplementationClicked()
{
    if (CachedResponse.Graphs.IsValidIndex(CurrentGraphIndex))
    {
        const FString& Implementation = CachedResponse.Graphs[CurrentGraphIndex].Code.GraphImplementation;
        if (!Implementation.IsEmpty())
        {
            FPlatformApplicationMisc::ClipboardCopy(*Implementation);

            FNotificationInfo Info(LOCTEXT("ImplCopied", "Implementation code copied to clipboard"));
            Info.bFireAndForget = true;
            Info.ExpireDuration = 2.0f;
            FSlateNotificationManager::Get().AddNotification(Info);
        }
    }
    return FReply::Handled();
}

FReply SN2CEditorWindow::OnOpenExplorerClicked()
{
    UN2CLLMModule* LLMModule = UN2CLLMModule::Get();
    if (LLMModule)
    {
        bool bSuccess = false;
        LLMModule->OpenTranslationFolder(bSuccess);
    }
    return FReply::Handled();
}

// ─────────────────────────────────────────────
// Combo box
// ─────────────────────────────────────────────

void SN2CEditorWindow::OnGraphSelectionChanged(TSharedPtr<FString> NewValue, ESelectInfo::Type SelectInfo)
{
    if (!NewValue.IsValid())
    {
        return;
    }

    int32 NewIndex = GraphNameOptions.IndexOfByPredicate([&NewValue](const TSharedPtr<FString>& Item)
    {
        return Item.IsValid() && *Item == *NewValue;
    });

    if (NewIndex != INDEX_NONE)
    {
        PopulateUIForGraph(NewIndex);
    }
}

TSharedRef<SWidget> SN2CEditorWindow::GenerateGraphComboItem(TSharedPtr<FString> InItem)
{
    return SNew(STextBlock)
        .Text(InItem.IsValid() ? FText::FromString(*InItem) : FText::GetEmpty());
}

// ─────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────

EN2CCodeLanguage SN2CEditorWindow::GetTargetLanguage() const
{
    const UN2CSettings* Settings = GetDefault<UN2CSettings>();
    return Settings ? Settings->TargetLanguage : EN2CCodeLanguage::Cpp;
}

FName SN2CEditorWindow::GetActiveTheme() const
{
    return FN2CEditorIntegration::Get().GetDefaultTheme(GetTargetLanguage());
}

#undef LOCTEXT_NAMESPACE
