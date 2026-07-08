// Copyright (c) 2025 Nick McClure (Protospatial). All Rights Reserved.

#include "Core/N2CEditorIntegration.h"

#include "BlueprintEditorModes.h"
#include "Core/N2CNodeCollector.h"
#include "Core/N2CAIExport.h"
#include "Core/N2CPatchImporter.h"
#include "BlueprintEditorModule.h"
#include "Code Editor/Models/N2CCodeLanguage.h"
#include "Core/N2CEditorWindow.h"
#include "Core/N2CNodeTranslator.h"
#include "Core/N2CSerializer.h"
#include "Core/N2CSettings.h"
#include "Core/N2CToolbarCommand.h"
#include "LLM/N2CLLMModule.h"
#include "LLM/N2CLLMTypes.h"
#include "Models/N2CStyle.h"
#include "Styling/SlateStyleRegistry.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "LevelEditor.h"
#include "Modules/ModuleManager.h"
#include "Modules/ModuleInterface.h"
#include "Interfaces/IPluginManager.h"
#include "ContentBrowserModule.h"
#include "AssetRegistryModule.h"
#include "IContentBrowserSingleton.h"
#include "Misc/ScopedSlowTask.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Docking/SDockTab.h"
#include "Framework/Docking/TabManager.h"

#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "K2Node.h"
#include "K2Node_FunctionEntry.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "ScopedTransaction.h"
#include "TimerManager.h"
#include "EditorFramework/AssetImportData.h"
#include "UObject/UnrealType.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformApplicationMisc.h"
#include "DesktopPlatformModule.h"
#include "IDesktopPlatform.h"
#include "Misc/MessageDialog.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Policies/PrettyJsonPrintPolicy.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsPlatformApplicationMisc.h"
#endif

#if PLATFORM_MAC
#include "Mac/MacPlatformApplicationMisc.h"
#endif

#ifndef N2C_WITH_BLUEPRINT_ASSIST
#define N2C_WITH_BLUEPRINT_ASSIST 0
#endif

#if N2C_WITH_BLUEPRINT_ASSIST
#include "BlueprintAssistModule.h"
#include "BlueprintAssistTabHandler.h"
#include "BlueprintAssistGraphHandler.h"
#endif

namespace N2CEditorIntegration_Private
{
    static FString SanitizeForFileName(FString In)
    {
        In.ReplaceInline(TEXT("/"), TEXT("_"));
        In.ReplaceInline(TEXT("\\"), TEXT("_"));
        In.ReplaceInline(TEXT(":"), TEXT("_"));
        In.ReplaceInline(TEXT(" "), TEXT("_"));
        return In;
    }


    static FString MakeExportTimestamp()
    {
        const FDateTime Now = FDateTime::Now();
        return FString::Printf(
            TEXT("%04d%02d%02d_%02d%02d%02d"),
            Now.GetYear(), Now.GetMonth(), Now.GetDay(),
            Now.GetHour(), Now.GetMinute(), Now.GetSecond()
        );
    }

    static FString NormalizeReportLineForDialog(FString Line)
    {
        Line.TrimStartAndEndInline();
        if (Line.StartsWith(TEXT("DRY RUN: ")))
        {
            Line = Line.RightChop(9);
        }
        if (Line.StartsWith(TEXT("Mode: dry run")) ||
            Line.Equals(TEXT("Patch processing finished.")) ||
            Line.StartsWith(TEXT("Patch target Blueprint:")) ||
            Line.StartsWith(TEXT("Actions:")) ||
            Line.StartsWith(TEXT("Backup created:")) ||
            Line.StartsWith(TEXT("Compile requested")))
        {
            return TEXT("");
        }
        return Line;
    }

    static bool IsProblemReportLine(const FString& Line)
    {
        const FString Lower = Line.ToLower();
        return Lower.StartsWith(TEXT("error:")) ||
            Lower.Contains(TEXT(" failed")) ||
            Lower.Contains(TEXT("invalid")) ||
            Lower.Contains(TEXT("not found")) ||
            Lower.Contains(TEXT("aborted"));
    }

    static bool IsSkipReportLine(const FString& Line)
    {
        const FString Lower = Line.ToLower();
        return Line.IsEmpty() ||
            Lower.Contains(TEXT("already exists, skipped duplicate")) ||
            Lower.StartsWith(TEXT("using existing function graph:")) ||
            Lower.StartsWith(TEXT("no self function call references required update"));
    }

    static void AddUniqueLine(TArray<FString>& Lines, const FString& Line)
    {
        FString Clean = Line;
        Clean.TrimStartAndEndInline();
        if (!Clean.IsEmpty() && !Lines.Contains(Clean))
        {
            Lines.Add(Clean);
        }
    }

    static FString StripPrefix(FString Line, const TCHAR* Prefix)
    {
        if (Line.StartsWith(Prefix))
        {
            Line = Line.RightChop(FCString::Strlen(Prefix));
            Line.TrimStartAndEndInline();
        }
        return Line;
    }

    static FString FriendlyReportLine(FString Line)
    {
        Line = StripPrefix(Line, TEXT("DRY RUN NEW:"));
        Line = StripPrefix(Line, TEXT("DRY RUN CHANGE:"));
        Line = StripPrefix(Line, TEXT("NEW:"));
        Line = StripPrefix(Line, TEXT("CHANGE:"));
        Line = StripPrefix(Line, TEXT("Added member variable:"));
        Line = StripPrefix(Line, TEXT("Created function graph:"));
        Line = StripPrefix(Line, TEXT("Added local variable:"));
        Line = StripPrefix(Line, TEXT("Renamed function:"));
        Line = StripPrefix(Line, TEXT("Set function category:"));
        Line = StripPrefix(Line, TEXT("Set member variable category:"));
        Line = StripPrefix(Line, TEXT("Updated member variable category:"));
        Line = StripPrefix(Line, TEXT("Updated self function call references:"));
        Line.TrimStartAndEndInline();
        return Line;
    }

    static void SplitReportForDialog(const FString& Report, TArray<FString>& OutNew, TArray<FString>& OutChanged, TArray<FString>& OutProblems, TArray<FString>& OutDetails)
    {
        TArray<FString> Lines;
        Report.ParseIntoArrayLines(Lines, true);
        for (FString Line : Lines)
        {
            Line = NormalizeReportLineForDialog(Line);
            if (IsSkipReportLine(Line))
            {
                continue;
            }

            if (IsProblemReportLine(Line))
            {
                AddUniqueLine(OutProblems, Line);
                continue;
            }

            if (Line.ToLower().StartsWith(TEXT("warning:")))
            {
                AddUniqueLine(OutDetails, Line);
                continue;
            }

            if (Line.StartsWith(TEXT("DRY RUN NEW:")) ||
                Line.StartsWith(TEXT("NEW:")) ||
                Line.StartsWith(TEXT("Added member variable:")) ||
                Line.StartsWith(TEXT("Created function graph:")) ||
                Line.StartsWith(TEXT("Added local variable:")))
            {
                AddUniqueLine(OutNew, FriendlyReportLine(Line));
                continue;
            }

            if (Line.StartsWith(TEXT("DRY RUN CHANGE:")) ||
                Line.StartsWith(TEXT("CHANGE:")) ||
                Line.StartsWith(TEXT("Renamed function:")) ||
                Line.StartsWith(TEXT("Set function category:")) ||
                Line.StartsWith(TEXT("Updated member variable category:")) ||
                Line.StartsWith(TEXT("Set member variable category:")) ||
                Line.StartsWith(TEXT("Updated self function call references:")) ||
                Line.StartsWith(TEXT("Cleared function body")) ||
                Line.StartsWith(TEXT("Added signature input:")) ||
                Line.StartsWith(TEXT("Added signature output:")) ||
                Line.StartsWith(TEXT("Signature input already exists")) ||
                Line.StartsWith(TEXT("Signature output already exists")))
            {
                AddUniqueLine(OutChanged, FriendlyReportLine(Line));
                continue;
            }

            AddUniqueLine(OutDetails, FriendlyReportLine(Line));
        }
    }

    static void AppendSection(FString& Message, const FString& Header, const TArray<FString>& Lines, const FString& EmptyText)
    {
        Message += Header + LINE_TERMINATOR;
        if (Lines.Num() == 0)
        {
            Message += TEXT("- ") + EmptyText + LINE_TERMINATOR;
        }
        else
        {
            for (const FString& Line : Lines)
            {
                Message += TEXT("- ") + Line + LINE_TERMINATOR;
            }
        }
        Message += LINE_TERMINATOR;
    }

    static FString MakePatchValidationDialogText(const FString& BlueprintName, const FString& DryRunReport)
    {
        TArray<FString> NewItems;
        TArray<FString> ChangedItems;
        TArray<FString> Problems;
        TArray<FString> Details;
        SplitReportForDialog(DryRunReport, NewItems, ChangedItems, Problems, Details);

        FString Message;
        Message += TEXT("OK") LINE_TERMINATOR LINE_TERMINATOR;
        Message += FString::Printf(TEXT("Blueprint : %s"), *BlueprintName) + LINE_TERMINATOR LINE_TERMINATOR;
        Message += TEXT("Apply patch now?") LINE_TERMINATOR LINE_TERMINATOR;

        if (Problems.Num() > 0)
        {
            AppendSection(Message, TEXT("Problems:"), Problems, TEXT("none"));
        }

        AppendSection(Message, TEXT("What new:"), NewItems, TEXT("nothing"));
        AppendSection(Message, TEXT("What changed:"), ChangedItems, TEXT("nothing"));

        if (Details.Num() > 0)
        {
            AppendSection(Message, TEXT("Details:"), Details, TEXT("none"));
        }

        return Message;
    }

    static FString MakePatchApplyDialogText(bool bApplied, const FString& BlueprintName, const FString& ApplyReport)
    {
        TArray<FString> NewItems;
        TArray<FString> ChangedItems;
        TArray<FString> Problems;
        TArray<FString> Details;
        SplitReportForDialog(ApplyReport, NewItems, ChangedItems, Problems, Details);

        FString Message;
        Message += bApplied ? TEXT("OK") : TEXT("NOT OK");
        Message += LINE_TERMINATOR LINE_TERMINATOR;
        Message += FString::Printf(TEXT("Blueprint : %s"), *BlueprintName) + LINE_TERMINATOR LINE_TERMINATOR;

        if (!bApplied || Problems.Num() > 0)
        {
            AppendSection(Message, TEXT("Problems:"), Problems, TEXT("none"));
        }

        if (bApplied)
        {
            AppendSection(Message, TEXT("What new:"), NewItems, TEXT("nothing"));
            AppendSection(Message, TEXT("What changed:"), ChangedItems, TEXT("nothing"));
        }
        else
        {
            AppendSection(Message, TEXT("Details:"), Details, TEXT("none"));
        }

        return Message;
    }

    static FString MakePatchFailedDialogText(const FString& BlueprintName, const FString& DryRunReport)
    {
        TArray<FString> NewItems;
        TArray<FString> ChangedItems;
        TArray<FString> Problems;
        TArray<FString> Details;
        SplitReportForDialog(DryRunReport, NewItems, ChangedItems, Problems, Details);

        FString Message;
        Message += TEXT("NOT OK") LINE_TERMINATOR LINE_TERMINATOR;
        Message += FString::Printf(TEXT("Blueprint : %s"), *BlueprintName) + LINE_TERMINATOR LINE_TERMINATOR;

        AppendSection(Message, TEXT("Problems:"), Problems, TEXT("none"));
        if (Details.Num() > 0)
        {
            AppendSection(Message, TEXT("Details:"), Details, TEXT("none"));
        }

        return Message;
    }



    static void ShowN2CNotification(const FString& Message, SNotificationItem::ECompletionState State = SNotificationItem::CS_Success)
    {
        FNotificationInfo Info(FText::FromString(Message));
        Info.bFireAndForget = true;
        Info.FadeInDuration = 0.2f;
        Info.FadeOutDuration = 0.5f;
        Info.ExpireDuration = 5.0f;
        TSharedPtr<SNotificationItem> Notification = FSlateNotificationManager::Get().AddNotification(Info);
        if (Notification.IsValid())
        {
            Notification->SetCompletionState(State);
        }
    }

    static bool SaveTextFile(const FString& Path, const FString& Text, FString& OutError)
    {
        IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
        if (!FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8))
        {
            OutError = FString::Printf(TEXT("Failed to save file: %s"), *Path);
            return false;
        }
        return true;
    }

    static FString MakeSlashIndent(int32 Depth)
    {
        FString Result;
        for (int32 Index = 0; Index < Depth; ++Index)
        {
            Result += TEXT("/");
        }
        return Result;
    }

    static FString GetAssetDiskExtension(const FAssetData& AssetData)
    {
        const FString PackageName = AssetData.PackageName.ToString();
        const FString AssetExtension = FPackageName::GetAssetPackageExtension();
        const FString MapExtension = FPackageName::GetMapPackageExtension();

        const FString AssetFilename = FPackageName::LongPackageNameToFilename(PackageName, AssetExtension);
        if (FPaths::FileExists(AssetFilename))
        {
            return AssetExtension;
        }

        const FString MapFilename = FPackageName::LongPackageNameToFilename(PackageName, MapExtension);
        if (FPaths::FileExists(MapFilename))
        {
            return MapExtension;
        }

        return AssetExtension;
    }

    static FString GetAssetDiskFilename(const FAssetData& AssetData)
    {
        const FString Extension = GetAssetDiskExtension(AssetData);
        return FPackageName::LongPackageNameToFilename(AssetData.PackageName.ToString(), Extension);
    }

    static void GetAllProjectAssets(TArray<FAssetData>& OutAssets)
    {
        OutAssets.Empty();

        FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        AssetRegistryModule.Get().SearchAllAssets(true);

        FARFilter Filter;
        Filter.PackagePaths.Add(FName(TEXT("/Game")));
        Filter.bRecursivePaths = true;

        AssetRegistryModule.Get().GetAssets(Filter, OutAssets);

        OutAssets.Sort([](const FAssetData& A, const FAssetData& B)
        {
            return A.ObjectPath.ToString() < B.ObjectPath.ToString();
        });
    }

    static void GetAllProjectContentPaths(TArray<FString>& OutPaths)
    {
        OutPaths.Empty();

        TArray<FAssetData> Assets;
        GetAllProjectAssets(Assets);

        TSet<FString> UniquePaths;
        UniquePaths.Add(TEXT("/Game"));

        for (const FAssetData& AssetData : Assets)
        {
            FString Path = AssetData.PackagePath.ToString();
            while (Path.StartsWith(TEXT("/Game")))
            {
                UniquePaths.Add(Path);
                const int32 SlashIndex = Path.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
                if (SlashIndex <= 0 || Path == TEXT("/Game"))
                {
                    break;
                }
                Path = Path.Left(SlashIndex);
            }
        }

        OutPaths.Empty(UniquePaths.Num());
        for (const FString& Path : UniquePaths)
        {
            OutPaths.Add(Path);
        }
        OutPaths.Sort();
    }

    static bool PickContentFoldersForN2CExport(TArray<FString>& OutSelectedPaths)
    {
        OutSelectedPaths.Empty();

        TArray<FString> AvailablePaths;
        GetAllProjectContentPaths(AvailablePaths);
        if (AvailablePaths.Num() == 0)
        {
            return false;
        }

        TSet<FString> SelectedPaths;
        bool bConfirmed = false;
        TSharedPtr<SWindow> PickerWindow;

        TSharedRef<SScrollBox> ScrollBox = SNew(SScrollBox);
        for (const FString& Path : AvailablePaths)
        {
            ScrollBox->AddSlot()
            [
                SNew(SCheckBox)
                .OnCheckStateChanged_Lambda([Path, &SelectedPaths](ECheckBoxState NewState)
                {
                    if (NewState == ECheckBoxState::Checked)
                    {
                        SelectedPaths.Add(Path);
                    }
                    else
                    {
                        SelectedPaths.Remove(Path);
                    }
                })
                [
                    SNew(STextBlock).Text(FText::FromString(Path))
                ]
            ];
        }

        PickerWindow = SNew(SWindow)
            .Title(FText::FromString(TEXT("Export N2C from folders")))
            .ClientSize(FVector2D(560.0f, 680.0f))
            .SupportsMinimize(false)
            .SupportsMaximize(false)
            [
                SNew(SVerticalBox)
                + SVerticalBox::Slot()
                .AutoHeight()
                .Padding(12.0f, 10.0f, 12.0f, 4.0f)
                [
                    SNew(STextBlock)
                    .Text(FText::FromString(TEXT("Choose project folders to export. Checked folders are exported recursively.")))
                ]
                + SVerticalBox::Slot()
                .FillHeight(1.0f)
                .Padding(12.0f, 6.0f)
                [
                    SNew(SBox)
                    .MinDesiredHeight(520.0f)
                    [
                        ScrollBox
                    ]
                ]
                + SVerticalBox::Slot()
                .AutoHeight()
                .Padding(12.0f, 8.0f, 12.0f, 12.0f)
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot()
                    .FillWidth(1.0f)
                    [
                        SNew(STextBlock)
                        .Text(FText::FromString(TEXT("Tip: select /Game to export everything under Content.")))
                    ]
                    + SHorizontalBox::Slot()
                    .AutoWidth()
                    .Padding(4.0f, 0.0f)
                    [
                        SNew(SButton)
                        .Text(FText::FromString(TEXT("Export")))
                        .OnClicked_Lambda([&bConfirmed, &PickerWindow]()
                        {
                            bConfirmed = true;
                            if (PickerWindow.IsValid())
                            {
                                PickerWindow->RequestDestroyWindow();
                            }
                            return FReply::Handled();
                        })
                    ]
                    + SHorizontalBox::Slot()
                    .AutoWidth()
                    .Padding(4.0f, 0.0f)
                    [
                        SNew(SButton)
                        .Text(FText::FromString(TEXT("Cancel")))
                        .OnClicked_Lambda([&bConfirmed, &PickerWindow]()
                        {
                            bConfirmed = false;
                            if (PickerWindow.IsValid())
                            {
                                PickerWindow->RequestDestroyWindow();
                            }
                            return FReply::Handled();
                        })
                    ]
                ]
            ];

        FSlateApplication::Get().AddModalWindow(PickerWindow.ToSharedRef(), nullptr);

        if (!bConfirmed || SelectedPaths.Num() == 0)
        {
            return false;
        }

        OutSelectedPaths.Empty(SelectedPaths.Num());
        for (const FString& Path : SelectedPaths)
        {
            OutSelectedPaths.Add(Path);
        }
        OutSelectedPaths.Sort();
        return true;
    }

    static void GetBlueprintAssetsUnderPaths(const TArray<FString>& PackagePaths, TArray<FAssetData>& OutBlueprintAssets)
    {
        OutBlueprintAssets.Empty();
        if (PackagePaths.Num() == 0)
        {
            return;
        }

        FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        AssetRegistryModule.Get().SearchAllAssets(true);

        FARFilter Filter;
        for (const FString& PackagePath : PackagePaths)
        {
            Filter.PackagePaths.Add(FName(*PackagePath));
        }
        Filter.ClassNames.Add(UBlueprint::StaticClass()->GetFName());
        Filter.bRecursivePaths = true;
        Filter.bRecursiveClasses = true;

        AssetRegistryModule.Get().GetAssets(Filter, OutBlueprintAssets);
    }

    static FString NormalizeDiskFilenameForJson(FString Filename)
    {
        Filename.TrimStartAndEndInline();
        if (Filename.IsEmpty())
        {
            return TEXT("");
        }

        if (FPaths::IsRelative(Filename))
        {
            Filename = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), Filename);
        }

        FPaths::NormalizeFilename(Filename);
        return Filename;
    }

    static FString MakePathRelativeToProject(const FString& AbsoluteFilename)
    {
        FString RelativePath = AbsoluteFilename;
        const bool bMadeRelative = FPaths::MakePathRelativeTo(RelativePath, *FPaths::ProjectDir());
        (void)bMadeRelative;
        FPaths::NormalizeFilename(RelativePath);
        return RelativePath;
    }

    static FString MakePathRelativeToContent(const FString& AbsoluteFilename)
    {
        FString RelativePath = AbsoluteFilename;
        const bool bMadeRelative = FPaths::MakePathRelativeTo(RelativePath, *FPaths::ProjectContentDir());
        (void)bMadeRelative;
        FPaths::NormalizeFilename(RelativePath);
        return RelativePath;
    }

    static void AddUniqueSourceFilename(TArray<FString>& InOutFiles, const FString& RawFilename)
    {
        const FString NormalizedFilename = NormalizeDiskFilenameForJson(RawFilename);
        if (!NormalizedFilename.IsEmpty() && !InOutFiles.Contains(NormalizedFilename))
        {
            InOutFiles.Add(NormalizedFilename);
        }
    }

    static void ExtractSourceFilesFromImportData(UAssetImportData* ImportData, TArray<FString>& OutSourceFiles)
    {
        if (!ImportData)
        {
            return;
        }

        TArray<FString> RawFilenames;
        ImportData->ExtractFilenames(RawFilenames);
        for (const FString& RawFilename : RawFilenames)
        {
            AddUniqueSourceFilename(OutSourceFiles, RawFilename);
        }
    }

    static void ExtractSourceFilesFromAssetObject(UObject* AssetObject, TArray<FString>& OutSourceFiles)
    {
        if (!AssetObject)
        {
            return;
        }

        // Common import-data owner pattern for Texture2D, StaticMesh, SkeletalMesh,
        // SoundWave and many other imported asset types in UE4.27.
        for (TFieldIterator<FProperty> PropIt(AssetObject->GetClass()); PropIt; ++PropIt)
        {
            FObjectProperty* ObjectProperty = CastField<FObjectProperty>(*PropIt);
            if (!ObjectProperty || !ObjectProperty->PropertyClass || !ObjectProperty->PropertyClass->IsChildOf(UAssetImportData::StaticClass()))
            {
                continue;
            }

            UAssetImportData* ImportData = Cast<UAssetImportData>(ObjectProperty->GetObjectPropertyValue_InContainer(AssetObject));
            ExtractSourceFilesFromImportData(ImportData, OutSourceFiles);
        }
    }

    static void GetAssetSourceFiles(const FAssetData& AssetData, TArray<FString>& OutSourceFiles)
    {
        OutSourceFiles.Empty();

        UObject* AssetObject = AssetData.GetAsset();
        ExtractSourceFilesFromAssetObject(AssetObject, OutSourceFiles);
        OutSourceFiles.Sort();
    }

    static TSharedPtr<FJsonObject> MakeDiskFileJson(const FString& Kind, const FString& Filename)
    {
        const FString NormalizedFilename = NormalizeDiskFilenameForJson(Filename);

        TSharedPtr<FJsonObject> FileObject = MakeShared<FJsonObject>();
        FileObject->SetStringField(TEXT("kind"), Kind);
        FileObject->SetStringField(TEXT("disk_filename"), NormalizedFilename);
        FileObject->SetStringField(TEXT("disk_extension"), FPaths::GetExtension(NormalizedFilename, true));
        FileObject->SetStringField(TEXT("display_file"), FPaths::GetCleanFilename(NormalizedFilename));
        FileObject->SetBoolField(TEXT("exists"), FPaths::FileExists(NormalizedFilename));
        FileObject->SetStringField(TEXT("relative_to_project"), MakePathRelativeToProject(NormalizedFilename));
        return FileObject;
    }

    static void GetProjectContentDiskFiles(TArray<FString>& OutFiles)
    {
        OutFiles.Empty();

        TArray<FString> FoundFiles;
        IFileManager::Get().FindFilesRecursive(FoundFiles, *FPaths::ProjectContentDir(), TEXT("*.*"), true, false, false);

        for (FString Filename : FoundFiles)
        {
            Filename = NormalizeDiskFilenameForJson(Filename);
            const FString Extension = FPaths::GetExtension(Filename, true).ToLower();

            // For this array keep only actual Unreal package files on disk.
            // Imported source files are exported separately through AssetImportData.
            if (Extension == TEXT(".uasset") || Extension == TEXT(".umap"))
            {
                OutFiles.Add(Filename);
            }
        }

        OutFiles.Sort();
    }

    static void AppendUInt16LE(TArray<uint8>& Bytes, uint16 Value)
    {
        Bytes.Add(static_cast<uint8>(Value & 0xFF));
        Bytes.Add(static_cast<uint8>((Value >> 8) & 0xFF));
    }

    static void AppendUInt32LE(TArray<uint8>& Bytes, uint32 Value)
    {
        Bytes.Add(static_cast<uint8>(Value & 0xFF));
        Bytes.Add(static_cast<uint8>((Value >> 8) & 0xFF));
        Bytes.Add(static_cast<uint8>((Value >> 16) & 0xFF));
        Bytes.Add(static_cast<uint8>((Value >> 24) & 0xFF));
    }

    static void AppendRawBytes(TArray<uint8>& Bytes, const void* Data, int32 Count)
    {
        if (Data && Count > 0)
        {
            Bytes.Append(static_cast<const uint8*>(Data), Count);
        }
    }

    static uint32 ComputeZipCrc32(const uint8* Data, int32 Count)
    {
        uint32 Crc = 0xFFFFFFFFu;
        for (int32 Index = 0; Index < Count; ++Index)
        {
            Crc ^= static_cast<uint32>(Data[Index]);
            for (int32 Bit = 0; Bit < 8; ++Bit)
            {
                const uint32 Mask = (Crc & 1u) ? 0xEDB88320u : 0u;
                Crc = (Crc >> 1) ^ Mask;
            }
        }
        return ~Crc;
    }

    static void GetZipDosDateTime(const FDateTime& DateTime, uint16& OutDosDate, uint16& OutDosTime)
    {
        const int32 Year = FMath::Clamp(DateTime.GetYear(), 1980, 2107);
        const int32 Month = FMath::Clamp(DateTime.GetMonth(), 1, 12);
        const int32 Day = FMath::Clamp(DateTime.GetDay(), 1, 31);
        const int32 Hour = FMath::Clamp(DateTime.GetHour(), 0, 23);
        const int32 Minute = FMath::Clamp(DateTime.GetMinute(), 0, 59);
        const int32 Second = FMath::Clamp(DateTime.GetSecond(), 0, 59);

        OutDosDate = static_cast<uint16>(((Year - 1980) << 9) | (Month << 5) | Day);
        OutDosTime = static_cast<uint16>((Hour << 11) | (Minute << 5) | (Second / 2));
    }

    static FString MakeZipEntryName(const FString& SourceDirFull, const FString& FilePathFull)
    {
        FString NormalizedRoot = SourceDirFull;
        FString NormalizedFile = FilePathFull;
        FPaths::NormalizeFilename(NormalizedRoot);
        FPaths::NormalizeFilename(NormalizedFile);

        if (!NormalizedRoot.EndsWith(TEXT("/")))
        {
            NormalizedRoot += TEXT("/");
        }

        FString EntryName = NormalizedFile;
        if (EntryName.StartsWith(NormalizedRoot))
        {
            EntryName = EntryName.RightChop(NormalizedRoot.Len());
        }
        EntryName.ReplaceInline(TEXT("\\"), TEXT("/"));
        while (EntryName.StartsWith(TEXT("/")))
        {
            EntryName = EntryName.RightChop(1);
        }
        return EntryName;
    }

    struct FZipCentralDirectoryEntry
    {
        FString Name;
        uint32 Crc = 0;
        uint32 CompressedSize = 0;
        uint32 UncompressedSize = 0;
        uint32 LocalHeaderOffset = 0;
        uint16 DosDate = 0;
        uint16 DosTime = 0;
    };

    static bool ZipDirectoryForSharing(const FString& SourceDir, const FString& TargetZipPath, FString& OutReport)
    {
        if (SourceDir.IsEmpty() || TargetZipPath.IsEmpty())
        {
            OutReport += TEXT("ZIP export skipped: empty source or target path.") LINE_TERMINATOR;
            return false;
        }

        if (!FPaths::DirectoryExists(SourceDir))
        {
            OutReport += FString::Printf(TEXT("ZIP export skipped: source directory does not exist: %s"), *SourceDir) + LINE_TERMINATOR;
            return false;
        }

        const FString SourceDirFull = FPaths::ConvertRelativePathToFull(SourceDir);
        const FString TargetZipFull = FPaths::ConvertRelativePathToFull(TargetZipPath);

        TArray<FString> Files;
        IFileManager::Get().FindFilesRecursive(Files, *SourceDirFull, TEXT("*"), true, false, false);
        Files.Sort();

        if (Files.Num() <= 0)
        {
            OutReport += FString::Printf(TEXT("ZIP export skipped: source directory contains no files: %s"), *SourceDirFull) + LINE_TERMINATOR;
            return false;
        }

        IFileManager::Get().MakeDirectory(*FPaths::GetPath(TargetZipFull), true);
        IFileManager::Get().Delete(*TargetZipFull, false, true, true);

        TArray<uint8> ZipBytes;
        TArray<FZipCentralDirectoryEntry> CentralEntries;
        const uint16 GeneralPurposeFlagUtf8 = 0x0800;
        const uint16 StoreMethod = 0;
        const uint16 VersionNeeded = 20;

        for (const FString& File : Files)
        {
            TArray<uint8> FileData;
            if (!FFileHelper::LoadFileToArray(FileData, *File))
            {
                OutReport += FString::Printf(TEXT("WARNING: ZIP skipped unreadable file: %s"), *File) + LINE_TERMINATOR;
                continue;
            }

            if (static_cast<uint64>(FileData.Num()) > 0xFFFFFFFFull || static_cast<uint64>(ZipBytes.Num()) > 0xFFFFFFFFull)
            {
                OutReport += TEXT("WARNING: ZIP export failed: file/archive is too large for simple ZIP32 writer.") LINE_TERMINATOR;
                return false;
            }

            const FString EntryName = MakeZipEntryName(SourceDirFull, FPaths::ConvertRelativePathToFull(File));
            if (EntryName.IsEmpty())
            {
                continue;
            }

            FTCHARToUTF8 EntryNameUtf8(*EntryName);
            if (EntryNameUtf8.Length() <= 0 || EntryNameUtf8.Length() > 65535)
            {
                OutReport += FString::Printf(TEXT("WARNING: ZIP skipped file with invalid entry name length: %s"), *EntryName) + LINE_TERMINATOR;
                continue;
            }

            uint16 DosDate = 0;
            uint16 DosTime = 0;
            const FDateTime FileTime = IFileManager::Get().GetTimeStamp(*File);
            GetZipDosDateTime(FileTime.GetTicks() > 0 ? FileTime : FDateTime::Now(), DosDate, DosTime);

            const uint32 Crc = ComputeZipCrc32(FileData.GetData(), FileData.Num());
            const uint32 Size = static_cast<uint32>(FileData.Num());
            const uint32 LocalHeaderOffset = static_cast<uint32>(ZipBytes.Num());

            // Local file header
            AppendUInt32LE(ZipBytes, 0x04034b50);
            AppendUInt16LE(ZipBytes, VersionNeeded);
            AppendUInt16LE(ZipBytes, GeneralPurposeFlagUtf8);
            AppendUInt16LE(ZipBytes, StoreMethod);
            AppendUInt16LE(ZipBytes, DosTime);
            AppendUInt16LE(ZipBytes, DosDate);
            AppendUInt32LE(ZipBytes, Crc);
            AppendUInt32LE(ZipBytes, Size);
            AppendUInt32LE(ZipBytes, Size);
            AppendUInt16LE(ZipBytes, static_cast<uint16>(EntryNameUtf8.Length()));
            AppendUInt16LE(ZipBytes, 0);
            AppendRawBytes(ZipBytes, EntryNameUtf8.Get(), EntryNameUtf8.Length());
            AppendRawBytes(ZipBytes, FileData.GetData(), FileData.Num());

            FZipCentralDirectoryEntry CentralEntry;
            CentralEntry.Name = EntryName;
            CentralEntry.Crc = Crc;
            CentralEntry.CompressedSize = Size;
            CentralEntry.UncompressedSize = Size;
            CentralEntry.LocalHeaderOffset = LocalHeaderOffset;
            CentralEntry.DosDate = DosDate;
            CentralEntry.DosTime = DosTime;
            CentralEntries.Add(CentralEntry);
        }

        if (CentralEntries.Num() <= 0)
        {
            OutReport += TEXT("WARNING: ZIP export failed: no readable files were added.") LINE_TERMINATOR;
            return false;
        }

        if (CentralEntries.Num() > 65535 || static_cast<uint64>(ZipBytes.Num()) > 0xFFFFFFFFull)
        {
            OutReport += TEXT("WARNING: ZIP export failed: archive is too large for simple ZIP32 writer.") LINE_TERMINATOR;
            return false;
        }

        const uint32 CentralDirectoryOffset = static_cast<uint32>(ZipBytes.Num());

        for (const FZipCentralDirectoryEntry& Entry : CentralEntries)
        {
            FTCHARToUTF8 EntryNameUtf8(*Entry.Name);
            if (EntryNameUtf8.Length() <= 0 || EntryNameUtf8.Length() > 65535)
            {
                continue;
            }

            // Central directory file header
            AppendUInt32LE(ZipBytes, 0x02014b50);
            AppendUInt16LE(ZipBytes, 20);
            AppendUInt16LE(ZipBytes, VersionNeeded);
            AppendUInt16LE(ZipBytes, GeneralPurposeFlagUtf8);
            AppendUInt16LE(ZipBytes, StoreMethod);
            AppendUInt16LE(ZipBytes, Entry.DosTime);
            AppendUInt16LE(ZipBytes, Entry.DosDate);
            AppendUInt32LE(ZipBytes, Entry.Crc);
            AppendUInt32LE(ZipBytes, Entry.CompressedSize);
            AppendUInt32LE(ZipBytes, Entry.UncompressedSize);
            AppendUInt16LE(ZipBytes, static_cast<uint16>(EntryNameUtf8.Length()));
            AppendUInt16LE(ZipBytes, 0);
            AppendUInt16LE(ZipBytes, 0);
            AppendUInt16LE(ZipBytes, 0);
            AppendUInt16LE(ZipBytes, 0);
            AppendUInt32LE(ZipBytes, 0);
            AppendUInt32LE(ZipBytes, Entry.LocalHeaderOffset);
            AppendRawBytes(ZipBytes, EntryNameUtf8.Get(), EntryNameUtf8.Length());
        }

        if (static_cast<uint64>(ZipBytes.Num()) > 0xFFFFFFFFull)
        {
            OutReport += TEXT("WARNING: ZIP export failed: archive is too large for simple ZIP32 writer.") LINE_TERMINATOR;
            return false;
        }

        const uint32 CentralDirectorySize = static_cast<uint32>(ZipBytes.Num()) - CentralDirectoryOffset;

        // End of central directory
        AppendUInt32LE(ZipBytes, 0x06054b50);
        AppendUInt16LE(ZipBytes, 0);
        AppendUInt16LE(ZipBytes, 0);
        AppendUInt16LE(ZipBytes, static_cast<uint16>(CentralEntries.Num()));
        AppendUInt16LE(ZipBytes, static_cast<uint16>(CentralEntries.Num()));
        AppendUInt32LE(ZipBytes, CentralDirectorySize);
        AppendUInt32LE(ZipBytes, CentralDirectoryOffset);
        AppendUInt16LE(ZipBytes, 0);

        if (!FFileHelper::SaveArrayToFile(ZipBytes, *TargetZipFull))
        {
            OutReport += FString::Printf(TEXT("WARNING: ZIP export failed: could not save %s"), *TargetZipFull) + LINE_TERMINATOR;
            return false;
        }

        OutReport += FString::Printf(TEXT("ZIP export saved: %s (%d files)"), *TargetZipFull, CentralEntries.Num()) + LINE_TERMINATOR;
        return true;
    }

    static bool SaveFunctionSplitFiles(const FString& RootJson, const FString& FunctionsDir, FString& OutReport)
    {
        TSharedPtr<FJsonObject> RootObj;
        TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(RootJson);
        if (!FJsonSerializer::Deserialize(Reader, RootObj) || !RootObj.IsValid())
        {
            OutReport += TEXT("Function split skipped: JSON parse failed.") LINE_TERMINATOR;
            return false;
        }

        const TArray<TSharedPtr<FJsonValue>>* Functions = nullptr;
        if (!RootObj->TryGetArrayField(TEXT("functions"), Functions) || !Functions)
        {
            OutReport += TEXT("Function split skipped: functions[] not found.") LINE_TERMINATOR;
            return false;
        }

        IFileManager::Get().MakeDirectory(*FunctionsDir, true);
        int32 SavedCount = 0;
        for (const TSharedPtr<FJsonValue>& Value : *Functions)
        {
            TSharedPtr<FJsonObject> FunctionObj = Value.IsValid() ? Value->AsObject() : nullptr;
            if (!FunctionObj.IsValid())
            {
                continue;
            }

            FString FunctionName;
            if (!FunctionObj->TryGetStringField(TEXT("name"), FunctionName) || FunctionName.IsEmpty())
            {
                FunctionName = FString::Printf(TEXT("Function_%d"), SavedCount + 1);
            }

            TSharedPtr<FJsonObject> Wrapper = MakeShared<FJsonObject>();
            Wrapper->SetStringField(TEXT("schema"), TEXT("N2C_AI_EXPORT_V2_FUNCTION"));
            Wrapper->SetObjectField(TEXT("function"), FunctionObj);

            FString FunctionJson;
            TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&FunctionJson);
            FJsonSerializer::Serialize(Wrapper.ToSharedRef(), Writer);

            FString Error;
            const FString FilePath = FunctionsDir / FString::Printf(TEXT("%03d_%s.json"), SavedCount + 1, *SanitizeForFileName(FunctionName));
            if (SaveTextFile(FilePath, FunctionJson, Error))
            {
                ++SavedCount;
            }
            else
            {
                OutReport += Error + LINE_TERMINATOR;
            }
        }

        OutReport += FString::Printf(TEXT("Function split files saved: %d"), SavedCount) + LINE_TERMINATOR;
        return SavedCount > 0;
    }

    static bool PickJsonFile(FString& OutPath)
    {
        OutPath.Empty();
        IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
        if (!DesktopPlatform)
        {
            return false;
        }

        TArray<FString> OutFiles;
        const void* ParentWindowHandle = nullptr;
        const bool bOpened = DesktopPlatform->OpenFileDialog(
            ParentWindowHandle,
            TEXT("Load N2C patch JSON"),
            FPaths::ProjectSavedDir(),
            TEXT(""),
            TEXT("N2C JSON files (*.json)|*.json|All files (*.*)|*.*"),
            EFileDialogFlags::None,
            OutFiles
        );

        if (bOpened && OutFiles.Num() > 0)
        {
            OutPath = OutFiles[0];
            return true;
        }
        return false;
    }

    static bool TryParseJsonRootObject(const FString& Json, TSharedPtr<FJsonObject>& OutRoot)
    {
        OutRoot.Reset();
        TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
        return FJsonSerializer::Deserialize(Reader, OutRoot) && OutRoot.IsValid();
    }

    static FString GetPatchSchemaSafe(const FString& Json)
    {
        TSharedPtr<FJsonObject> Root;
        if (!TryParseJsonRootObject(Json, Root))
        {
            return TEXT("");
        }
        FString Schema;
        Root->TryGetStringField(TEXT("schema"), Schema);
        return Schema;
    }

    static bool IsProjectPatchJson(const FString& Json)
    {
        return GetPatchSchemaSafe(Json) == TEXT("N2C_PROJECT_PATCH_V1");
    }

    static FString NormalizeBlueprintObjectPath(FString BlueprintPath)
    {
        BlueprintPath.TrimStartAndEndInline();
        if (BlueprintPath.IsEmpty())
        {
            return BlueprintPath;
        }
        if (!BlueprintPath.Contains(TEXT(".")))
        {
            const FString AssetName = FPackageName::GetShortName(BlueprintPath);
            BlueprintPath += TEXT(".") + AssetName;
        }
        return BlueprintPath;
    }

    static UBlueprint* LoadBlueprintForProjectPatchAsset(const TSharedPtr<FJsonObject>& AssetObj, FString& OutBlueprintPath)
    {
        OutBlueprintPath.Empty();
        if (!AssetObj.IsValid())
        {
            return nullptr;
        }

        AssetObj->TryGetStringField(TEXT("blueprint_path"), OutBlueprintPath);
        if (OutBlueprintPath.IsEmpty())
        {
            AssetObj->TryGetStringField(TEXT("asset_path"), OutBlueprintPath);
        }
        if (OutBlueprintPath.IsEmpty())
        {
            AssetObj->TryGetStringField(TEXT("path"), OutBlueprintPath);
        }

        OutBlueprintPath = NormalizeBlueprintObjectPath(OutBlueprintPath);
        if (OutBlueprintPath.IsEmpty())
        {
            return nullptr;
        }
        return LoadObject<UBlueprint>(nullptr, *OutBlueprintPath);
    }

    static bool BuildSingleBlueprintPatchJson(const TSharedPtr<FJsonObject>& AssetObj, UBlueprint* Blueprint, FString& OutJson)
    {
        OutJson.Empty();
        if (!AssetObj.IsValid() || !Blueprint)
        {
            return false;
        }

        TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
        Root->SetStringField(TEXT("schema"), TEXT("N2C_PATCH_V1"));
        Root->SetStringField(TEXT("target_blueprint"), Blueprint->GetName());

        FString Description;
        if (AssetObj->TryGetStringField(TEXT("description"), Description))
        {
            Root->SetStringField(TEXT("description"), Description);
        }

        const TArray<TSharedPtr<FJsonValue>>* Variables = nullptr;
        if (AssetObj->TryGetArrayField(TEXT("variables"), Variables) && Variables)
        {
            Root->SetArrayField(TEXT("variables"), *Variables);
        }
        else if (AssetObj->TryGetArrayField(TEXT("member_variables"), Variables) && Variables)
        {
            Root->SetArrayField(TEXT("variables"), *Variables);
        }

        const TArray<TSharedPtr<FJsonValue>>* Actions = nullptr;
        if (AssetObj->TryGetArrayField(TEXT("actions"), Actions) && Actions)
        {
            Root->SetArrayField(TEXT("actions"), *Actions);
        }
        else
        {
            Root->SetArrayField(TEXT("actions"), TArray<TSharedPtr<FJsonValue>>());
        }

        TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&OutJson);
        return FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
    }

    static void AppendProjectAssetReport(const FString& BlueprintName, const FString& AssetReport, FString& OutReport)
    {
        TArray<FString> Lines;
        AssetReport.ParseIntoArrayLines(Lines, true);
        for (FString Line : Lines)
        {
            Line.TrimStartAndEndInline();
            if (Line.IsEmpty())
            {
                continue;
            }

            const TCHAR* Prefixes[] = {
                TEXT("DRY RUN NEW:"), TEXT("DRY RUN CHANGE:"),
                TEXT("NEW:"), TEXT("CHANGE:"),
                TEXT("ERROR:"), TEXT("WARNING:")
            };

            bool bPrefixed = false;
            for (const TCHAR* Prefix : Prefixes)
            {
                if (Line.StartsWith(Prefix))
                {
                    FString Tail = Line.RightChop(FCString::Strlen(Prefix));
                    Tail.TrimStartAndEndInline();
                    OutReport += FString(Prefix) + TEXT(" ") + BlueprintName + TEXT(" ") + Tail + LINE_TERMINATOR;
                    bPrefixed = true;
                    break;
                }
            }

            if (!bPrefixed)
            {
                OutReport += BlueprintName + TEXT(": ") + Line + LINE_TERMINATOR;
            }
        }
    }

    static bool RunProjectPatch(const FString& PatchJson, bool bDryRun, FString& OutDialogName, FString& OutReport)
    {
        OutDialogName = TEXT("Project patch");
        OutReport.Empty();

        TSharedPtr<FJsonObject> Root;
        if (!TryParseJsonRootObject(PatchJson, Root))
        {
            OutReport += TEXT("ERROR: project patch JSON parse failed.") LINE_TERMINATOR;
            return false;
        }

        FString Schema;
        Root->TryGetStringField(TEXT("schema"), Schema);
        if (Schema != TEXT("N2C_PROJECT_PATCH_V1"))
        {
            OutReport += FString::Printf(TEXT("ERROR: unsupported project patch schema '%s'. Expected N2C_PROJECT_PATCH_V1."), *Schema) + LINE_TERMINATOR;
            return false;
        }

        const TArray<TSharedPtr<FJsonValue>>* Assets = nullptr;
        if (!Root->TryGetArrayField(TEXT("assets"), Assets) || !Assets || Assets->Num() == 0)
        {
            OutReport += TEXT("ERROR: project patch does not contain assets[].") LINE_TERMINATOR;
            return false;
        }

        OutDialogName = FString::Printf(TEXT("Project patch (%d blueprints)"), Assets->Num());
        bool bAllOk = true;

        for (const TSharedPtr<FJsonValue>& Value : *Assets)
        {
            TSharedPtr<FJsonObject> AssetObj = Value.IsValid() ? Value->AsObject() : nullptr;
            FString BlueprintPath;
            UBlueprint* AssetBlueprint = LoadBlueprintForProjectPatchAsset(AssetObj, BlueprintPath);
            if (!AssetBlueprint)
            {
                OutReport += FString::Printf(TEXT("ERROR: blueprint asset not found for project patch: %s"), *BlueprintPath) + LINE_TERMINATOR;
                bAllOk = false;
                continue;
            }

            FString SinglePatchJson;
            if (!BuildSingleBlueprintPatchJson(AssetObj, AssetBlueprint, SinglePatchJson))
            {
                OutReport += FString::Printf(TEXT("ERROR: could not build single patch for: %s"), *AssetBlueprint->GetName()) + LINE_TERMINATOR;
                bAllOk = false;
                continue;
            }

            FString AssetReport;
            const bool bAssetOk = bDryRun
                ? FN2CPatchImporter::DryRunPatch(AssetBlueprint, SinglePatchJson, AssetReport)
                : FN2CPatchImporter::ApplyPatchToBlueprint(AssetBlueprint, SinglePatchJson, AssetReport);
            AppendProjectAssetReport(AssetBlueprint->GetName(), AssetReport, OutReport);
            bAllOk &= bAssetOk;
        }

        return bAllOk;
    }
    struct FN2CAutoFormatTarget
    {
        TWeakObjectPtr<UBlueprint> Blueprint;
        FString BlueprintName;
        FString FunctionName;
    };

    static void AddAutoFormatTarget(TArray<FN2CAutoFormatTarget>& Targets, UBlueprint* Blueprint, const FString& FunctionName)
    {
        if (!Blueprint || FunctionName.IsEmpty())
        {
            return;
        }

        for (const FN2CAutoFormatTarget& Existing : Targets)
        {
            if (Existing.Blueprint.Get() == Blueprint && Existing.FunctionName == FunctionName)
            {
                return;
            }
        }

        FN2CAutoFormatTarget Target;
        Target.Blueprint = Blueprint;
        Target.BlueprintName = Blueprint->GetName();
        Target.FunctionName = FunctionName;
        Targets.Add(Target);
    }

    static void CollectAutoFormatTargetsFromActions(UBlueprint* Blueprint, const TArray<TSharedPtr<FJsonValue>>* Actions, TArray<FN2CAutoFormatTarget>& OutTargets)
    {
        if (!Blueprint || !Actions)
        {
            return;
        }

        for (const TSharedPtr<FJsonValue>& ActionValue : *Actions)
        {
            const TSharedPtr<FJsonObject> ActionObj = ActionValue.IsValid() ? ActionValue->AsObject() : nullptr;
            if (!ActionObj.IsValid())
            {
                continue;
            }

            FString ActionType;
            ActionObj->TryGetStringField(TEXT("type"), ActionType);

            const bool bChangesFunctionBody =
                ActionType == TEXT("add_or_replace_function") ||
                ActionType == TEXT("replace_function_body");

            if (!bChangesFunctionBody)
            {
                continue;
            }

            FString FunctionName;
            if (!ActionObj->TryGetStringField(TEXT("function_name"), FunctionName))
            {
                ActionObj->TryGetStringField(TEXT("name"), FunctionName);
            }

            AddAutoFormatTarget(OutTargets, Blueprint, FunctionName);
        }
    }

    static void CollectAutoFormatTargetsFromPatchJson(const FString& PatchJson, UBlueprint* FallbackBlueprint, TArray<FN2CAutoFormatTarget>& OutTargets)
    {
        OutTargets.Empty();

        TSharedPtr<FJsonObject> Root;
        if (!TryParseJsonRootObject(PatchJson, Root))
        {
            return;
        }

        FString Schema;
        Root->TryGetStringField(TEXT("schema"), Schema);

        if (Schema == TEXT("N2C_PROJECT_PATCH_V1"))
        {
            const TArray<TSharedPtr<FJsonValue>>* Assets = nullptr;
            if (!Root->TryGetArrayField(TEXT("assets"), Assets) || !Assets)
            {
                return;
            }

            for (const TSharedPtr<FJsonValue>& AssetValue : *Assets)
            {
                const TSharedPtr<FJsonObject> AssetObj = AssetValue.IsValid() ? AssetValue->AsObject() : nullptr;
                FString BlueprintPath;
                UBlueprint* AssetBlueprint = LoadBlueprintForProjectPatchAsset(AssetObj, BlueprintPath);
                if (!AssetBlueprint)
                {
                    continue;
                }

                const TArray<TSharedPtr<FJsonValue>>* Actions = nullptr;
                if (AssetObj.IsValid() && AssetObj->TryGetArrayField(TEXT("actions"), Actions))
                {
                    CollectAutoFormatTargetsFromActions(AssetBlueprint, Actions, OutTargets);
                }
            }
            return;
        }

        const TArray<TSharedPtr<FJsonValue>>* Actions = nullptr;
        if (Root->TryGetArrayField(TEXT("actions"), Actions))
        {
            CollectAutoFormatTargetsFromActions(FallbackBlueprint, Actions, OutTargets);
        }
    }

    static UEdGraph* FindFunctionGraphForAutoFormat(UBlueprint* Blueprint, const FString& FunctionName)
    {
        if (!Blueprint || FunctionName.IsEmpty())
        {
            return nullptr;
        }

        for (UEdGraph* Graph : Blueprint->FunctionGraphs)
        {
            if (Graph && Graph->GetName() == FunctionName)
            {
                return Graph;
            }
        }

        return nullptr;
    }

    static UEdGraphNode* FindInitialNodeForBlueprintAssistFormat(UEdGraph* Graph)
    {
        if (!Graph)
        {
            return nullptr;
        }

        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (Node && Node->IsA<UK2Node_FunctionEntry>())
            {
                return Node;
            }
        }

        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (Node && Node->IsValidLowLevelFast(false))
            {
                return Node;
            }
        }

        return nullptr;
    }

    static bool IsBlueprintAssistPluginReady(FString& OutReason)
    {
        const UN2CEditorPreferences* Settings = GetDefault<UN2CEditorPreferences>();
        if (!Settings || !Settings->bAutoFormatImportedFunctionsWithBlueprintAssist)
        {
            OutReason = TEXT("Blueprint Assist auto-format disabled in Node to Code settings.");
            return false;
        }

        const TSharedPtr<IPlugin> BlueprintAssistPlugin = IPluginManager::Get().FindPlugin(TEXT("BlueprintAssist"));
        if (!BlueprintAssistPlugin.IsValid())
        {
            OutReason = TEXT("BlueprintAssist plugin was not found.");
            return false;
        }

        if (!BlueprintAssistPlugin->IsEnabled())
        {
            OutReason = TEXT("BlueprintAssist plugin exists but is not enabled.");
            return false;
        }

#if !N2C_WITH_BLUEPRINT_ASSIST
        OutReason = TEXT("Node to Code was compiled without BlueprintAssist source. Put BlueprintAssist in Project/Plugins or Engine/Plugins/Editor and rebuild the editor.");
        return false;
#else
        if (!FModuleManager::Get().IsModuleLoaded(TEXT("BlueprintAssist")))
        {
            if (!FModuleManager::Get().LoadModulePtr<IModuleInterface>(TEXT("BlueprintAssist")))
            {
                OutReason = TEXT("BlueprintAssist plugin is enabled but the BlueprintAssist module could not be loaded.");
                return false;
            }
        }

        OutReason.Empty();
        return true;
#endif
    }

#if N2C_WITH_BLUEPRINT_ASSIST
    static bool TryQueueBlueprintAssistFormat(UEdGraph* Graph, FString& OutLine)
    {
        if (!Graph)
        {
            OutLine = TEXT("WARNING: Blueprint Assist auto-format skipped: graph is null.");
            return false;
        }

        // Opening a graph and giving Blueprint Assist one tick is not always enough.
        // This function is called several times from timers below. On every attempt we
        // re-focus the graph and force BA to process the foreground tab before formatting.
        FKismetEditorUtilities::BringKismetToFocusAttentionOnObject(Graph);

        if (FSlateApplication::IsInitialized())
        {
            if (TSharedPtr<SDockTab> ActiveTab = FGlobalTabmanager::Get()->GetActiveTab())
            {
                FBATabHandler::Get().ProcessTab(ActiveTab);
            }
        }

        TSharedPtr<FBAGraphHandler> GraphHandler = FBATabHandler::Get().GetActiveGraphHandler();
        if (!GraphHandler.IsValid())
        {
            OutLine = FString::Printf(TEXT("WARNING: Blueprint Assist auto-format skipped: active BA graph handler not found for %s."), *Graph->GetName());
            return false;
        }

        if (GraphHandler->GetFocusedEdGraph() != Graph)
        {
            OutLine = FString::Printf(
                TEXT("WARNING: Blueprint Assist auto-format skipped: focused BA graph is '%s', expected '%s'."),
                GraphHandler->GetFocusedEdGraph() ? *GraphHandler->GetFocusedEdGraph()->GetName() : TEXT("None"),
                *Graph->GetName()
            );
            return false;
        }

        TSet<UEdGraphNode*> NodesToFormat;
        TSet<const UEdGraphNode*> NodesToSelect;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (Node && Node->IsValidLowLevelFast(false))
            {
                NodesToFormat.Add(Node);
                NodesToSelect.Add(Node);
            }
        }

        if (NodesToFormat.Num() == 0)
        {
            OutLine = FString::Printf(TEXT("WARNING: Blueprint Assist auto-format skipped: graph has no nodes: %s"), *Graph->GetName());
            return false;
        }

        UEdGraphNode* InitialNode = FindInitialNodeForBlueprintAssistFormat(Graph);
        if (!InitialNode)
        {
            OutLine = FString::Printf(TEXT("WARNING: Blueprint Assist auto-format skipped: no initial node found for %s."), *Graph->GetName());
            return false;
        }

        // Match the manual workflow as closely as possible: Ctrl+A -> F.
        // UEdGraph::SelectNodeSet updates graph selection without needing private SGraphEditor headers.
        Graph->SelectNodeSet(NodesToSelect);

        FEdGraphFormatterParameters FormatterParameters;
        FormatterParameters.NodesToFormat = NodesToFormat.Array();

        TSharedPtr<FScopedTransaction> Transaction = MakeShareable(new FScopedTransaction(NSLOCTEXT("NodeToCode", "N2CBlueprintAssistAutoFormat", "N2C Blueprint Assist Auto Format")));
        GraphHandler->AddPendingFormatNodes(InitialNode, Transaction, FormatterParameters);

        // Force BA to process the queue now. If node sizes are not cached yet, BA will
        // cache them and a later scheduled attempt will finish the format.
        GraphHandler->UpdateNodesRequiringFormatting();

        Graph->NotifyGraphChanged();
        Graph->MarkPackageDirty();

        OutLine = FString::Printf(TEXT("CHANGE: Blueprint Assist auto-format queued: %s (%d nodes, Ctrl+A -> F emulation)"), *Graph->GetName(), NodesToSelect.Num());
        return true;
    }
#endif

    static void QueueBlueprintAssistAutoFormatIfEnabled(const TArray<FN2CAutoFormatTarget>& Targets, FString& InOutReport)
    {
        if (Targets.Num() == 0)
        {
            return;
        }

        FString Reason;
        if (!IsBlueprintAssistPluginReady(Reason))
        {
            if (GetDefault<UN2CEditorPreferences>() && GetDefault<UN2CEditorPreferences>()->bAutoFormatImportedFunctionsWithBlueprintAssist)
            {
                InOutReport += FString::Printf(TEXT("WARNING: Blueprint Assist auto-format skipped: %s"), *Reason) + LINE_TERMINATOR;
            }
            return;
        }

#if N2C_WITH_BLUEPRINT_ASSIST
        TArray<FN2CAutoFormatTarget> TargetsCopy = Targets;
        auto DoFormat = [TargetsCopy](const TCHAR* AttemptName)
        {
            for (const FN2CAutoFormatTarget& Target : TargetsCopy)
            {
                UBlueprint* Blueprint = Target.Blueprint.Get();
                if (!Blueprint)
                {
                    continue;
                }

                UEdGraph* Graph = FindFunctionGraphForAutoFormat(Blueprint, Target.FunctionName);
                if (!Graph)
                {
                    const FString Line = FString::Printf(TEXT("WARNING: Blueprint Assist auto-format %s skipped: function graph not found: %s.%s"), AttemptName, *Target.BlueprintName, *Target.FunctionName);
                    FN2CLogger::Get().Log(Line, EN2CLogSeverity::Warning);
                    continue;
                }

                FString Line;
                TryQueueBlueprintAssistFormat(Graph, Line);
                if (!Line.IsEmpty())
                {
                    const FString AttemptLine = FString::Printf(TEXT("%s [%s]"), *Line, AttemptName);
                    FN2CLogger::Get().Log(AttemptLine, Line.StartsWith(TEXT("WARNING:")) ? EN2CLogSeverity::Warning : EN2CLogSeverity::Info);
                }
            }
        };

        if (GEditor)
        {
            GEditor->GetTimerManager()->SetTimerForNextTick(FTimerDelegate::CreateLambda([DoFormat]()
            {
                DoFormat(TEXT("attempt 1/4 next tick"));
            }));

            FTimerHandle Attempt2Handle;
            GEditor->GetTimerManager()->SetTimer(Attempt2Handle, FTimerDelegate::CreateLambda([DoFormat]()
            {
                DoFormat(TEXT("attempt 2/4 0.25s"));
            }), 0.25f, false);

            FTimerHandle Attempt3Handle;
            GEditor->GetTimerManager()->SetTimer(Attempt3Handle, FTimerDelegate::CreateLambda([DoFormat]()
            {
                DoFormat(TEXT("attempt 3/4 0.75s"));
            }), 0.75f, false);

            FTimerHandle Attempt4Handle;
            GEditor->GetTimerManager()->SetTimer(Attempt4Handle, FTimerDelegate::CreateLambda([DoFormat]()
            {
                DoFormat(TEXT("attempt 4/4 1.50s"));
            }), 1.50f, false);

            for (const FN2CAutoFormatTarget& Target : Targets)
            {
                InOutReport += FString::Printf(TEXT("CHANGE: Blueprint Assist auto-format scheduled: %s.%s"), *Target.BlueprintName, *Target.FunctionName) + LINE_TERMINATOR;
            }
        }
#endif
    }

}

FN2CEditorIntegration& FN2CEditorIntegration::Get()
{
    static FN2CEditorIntegration Instance;
    return Instance;
}

void FN2CEditorIntegration::ExecuteCopyJsonForEditor(TWeakPtr<FBlueprintEditor> InEditor)
{
    FN2CLogger::Get().Log(TEXT("ExecuteCopyJsonForEditor ALL GRAPHS called"), EN2CLogSeverity::Info);

    TSharedPtr<FBlueprintEditor> Editor = InEditor.Pin();
    if (!Editor.IsValid())
    {
        FN2CLogger::Get().LogError(TEXT("Invalid Blueprint Editor pointer"));
        return;
    }

    UBlueprint* BlueprintObj = Editor->GetBlueprintObj();
    if (!BlueprintObj)
    {
        FN2CLogger::Get().LogError(TEXT("Could not get Blueprint object from editor"));
        return;
    }

    const FString BlueprintName = BlueprintObj->GetName();

    FN2CLogger::Get().Log(
        FString::Printf(TEXT("Exporting ALL graphs for Blueprint: %s"), *BlueprintName),
        EN2CLogSeverity::Info
    );

    TArray<UEdGraph*> GraphsToExport;
    TSet<UEdGraph*> SeenGraphs;

    auto AddGraphArray = [&GraphsToExport, &SeenGraphs](const TArray<UEdGraph*>& SourceGraphs)
    {
        for (UEdGraph* Graph : SourceGraphs)
        {
            if (!Graph || SeenGraphs.Contains(Graph))
            {
                continue;
            }

            SeenGraphs.Add(Graph);
            GraphsToExport.Add(Graph);
        }
    };

    AddGraphArray(BlueprintObj->UbergraphPages);
    AddGraphArray(BlueprintObj->FunctionGraphs);
    AddGraphArray(BlueprintObj->MacroGraphs);
    AddGraphArray(BlueprintObj->DelegateSignatureGraphs);

    if (GraphsToExport.Num() <= 0)
    {
        FN2CLogger::Get().LogError(TEXT("No graphs found in Blueprint"));
        return;
    }

    FN2CLogger::Get().Log(
        FString::Printf(TEXT("Found %d graphs in Blueprint: %s"), GraphsToExport.Num(), *BlueprintName),
        EN2CLogSeverity::Info
    );

    FN2CNodeCollector& Collector = FN2CNodeCollector::Get();

    TArray<UK2Node*> AllCollectedNodes;
    TSet<UK2Node*> SeenNodes;

    for (UEdGraph* Graph : GraphsToExport)
    {
        if (!Graph)
        {
            continue;
        }

        TArray<UK2Node*> GraphNodes;

        FN2CLogger::Get().Log(
            FString::Printf(TEXT("Collecting graph: %s"), *Graph->GetName()),
            EN2CLogSeverity::Info
        );

        if (Collector.CollectNodesFromGraph(Graph, GraphNodes))
        {
            for (UK2Node* Node : GraphNodes)
            {
                if (!Node || SeenNodes.Contains(Node))
                {
                    continue;
                }

                SeenNodes.Add(Node);
                AllCollectedNodes.Add(Node);
            }

            FN2CLogger::Get().Log(
                FString::Printf(TEXT("Graph collected: %s, nodes: %d"), *Graph->GetName(), GraphNodes.Num()),
                EN2CLogSeverity::Info
            );
        }
        else
        {
            FN2CLogger::Get().LogWarning(
                FString::Printf(TEXT("Failed to collect graph: %s"), *Graph->GetName())
            );
        }
    }

    if (AllCollectedNodes.Num() <= 0)
    {
        FN2CLogger::Get().LogError(TEXT("No nodes collected from Blueprint graphs"));
        return;
    }

    FN2CLogger::Get().Log(
        FString::Printf(TEXT("ALL GRAPHS node collection complete: %d unique nodes"), AllCollectedNodes.Num()),
        EN2CLogSeverity::Info
    );

    FN2CNodeTranslator& Translator = FN2CNodeTranslator::Get();

    if (!Translator.GenerateN2CStruct(AllCollectedNodes))
    {
        FN2CLogger::Get().LogError(TEXT("Failed to translate all Blueprint graph nodes"));
        return;
    }

    const FN2CBlueprint& N2CBlueprint = FN2CNodeTranslator::Get().GetN2CBlueprint();

    if (!N2CBlueprint.IsValid())
    {
        FN2CLogger::Get().LogError(TEXT("Generated all-graphs Blueprint JSON is invalid"));
        return;
    }

    FN2CSerializer::SetPrettyPrint(true);
    FString JsonOutput = FN2CSerializer::ToJson(N2CBlueprint);

    if (JsonOutput.IsEmpty())
    {
        FN2CLogger::Get().LogError(TEXT("JSON serialization failed"));
        return;
    }

    FPlatformApplicationMisc::ClipboardCopy(*JsonOutput);

    const FString SaveDir = FPaths::ProjectSavedDir() / TEXT("NodeToCode/AllBlueprintExports");
    IFileManager::Get().MakeDirectory(*SaveDir, true);

    FString SafeBlueprintName = BlueprintName;
    SafeBlueprintName = SafeBlueprintName.Replace(TEXT("/"), TEXT("_"));
    SafeBlueprintName = SafeBlueprintName.Replace(TEXT("\\"), TEXT("_"));
    SafeBlueprintName = SafeBlueprintName.Replace(TEXT(":"), TEXT("_"));

    const FString Timestamp = FString::Printf(TEXT("%lld"), FDateTime::Now().GetTicks());

    const FString SavePath =
        SaveDir / FString::Printf(TEXT("N2C_ALL_%s_%s.json"), *SafeBlueprintName, *Timestamp);

    const bool bSaved = FFileHelper::SaveStringToFile(
        JsonOutput,
        *SavePath,
        FFileHelper::EEncodingOptions::ForceUTF8
    );

    if (bSaved)
    {
        FN2CLogger::Get().Log(
            FString::Printf(TEXT("ALL Blueprint JSON saved: %s"), *SavePath),
            EN2CLogSeverity::Info
        );
    }
    else
    {
        FN2CLogger::Get().LogError(
            FString::Printf(TEXT("Failed to save ALL Blueprint JSON: %s"), *SavePath)
        );
    }

    FNotificationInfo Info(
        FText::FromString(
            FString::Printf(TEXT("All Blueprint JSON exported: %d graphs, %d nodes"), GraphsToExport.Num(), AllCollectedNodes.Num())
        )
    );

    Info.bFireAndForget = true;
    Info.FadeInDuration = 0.2f;
    Info.FadeOutDuration = 0.5f;
    Info.ExpireDuration = 3.0f;
    FSlateNotificationManager::Get().AddNotification(Info);
}


void FN2CEditorIntegration::RegisterLevelEditorToolbar()
{
    FN2CLogger::Get().Log(TEXT("Registering project-level N2C toolbar buttons"), EN2CLogSeverity::Info);

    if (!LevelEditorCommandList.IsValid())
    {
        LevelEditorCommandList = MakeShareable(new FUICommandList);
    }

    LevelEditorCommandList->MapAction(
        FN2CToolbarCommand::Get().ProjectImportCommand,
        FExecuteAction::CreateRaw(this, &FN2CEditorIntegration::ExecuteBack2DeadProjectImportFromEditor),
        FCanExecuteAction::CreateLambda([]() { return true; })
    );

    LevelEditorCommandList->MapAction(
        FN2CToolbarCommand::Get().ExportAllCommand,
        FExecuteAction::CreateRaw(this, &FN2CEditorIntegration::ExecuteBack2DeadExportAllBlueprints),
        FCanExecuteAction::CreateLambda([]() { return true; })
    );

    FLevelEditorModule& LevelEditorModule = FModuleManager::LoadModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));

    LevelEditorToolbarExtender = MakeShareable(new FExtender);
    LevelEditorToolbarExtender->AddToolBarExtension(
        "Settings",
        EExtensionHook::After,
        LevelEditorCommandList,
        FToolBarExtensionDelegate::CreateRaw(this, &FN2CEditorIntegration::FillLevelEditorToolbar)
    );

    LevelEditorModule.GetToolBarExtensibilityManager()->AddExtender(LevelEditorToolbarExtender);
}

void FN2CEditorIntegration::FillLevelEditorToolbar(FToolBarBuilder& Builder)
{
    Builder.AddSeparator();

    Builder.AddToolBarButton(
        FN2CToolbarCommand::Get().ProjectImportCommand,
        NAME_None,
        FN2CToolbarCommand::CommandLabel_ProjectImport,
        FN2CToolbarCommand::CommandTooltip_ProjectImport,
        FSlateIcon(FName("NodeToCodeStyle"), FName("NodeToCode.ImportButton"), FName("NodeToCode.ImportButton.Small"))
    );

    // Main export button: exports ALL Blueprints, with its own confirmation dialog.
    Builder.AddToolBarButton(
        FN2CToolbarCommand::Get().ExportAllCommand,
        NAME_None,
        FN2CToolbarCommand::CommandLabel_ExportAll,
        FN2CToolbarCommand::CommandTooltip_ExportAll,
        FSlateIcon(FName("NodeToCodeStyle"), FName("NodeToCode.ExportButton"), FName("NodeToCode.ExportButton.Small"))
    );

    // Adjacent dropdown: secondary project export actions only.
    Builder.AddComboButton(
        FUIAction(),
        FOnGetContent::CreateRaw(this, &FN2CEditorIntegration::MakeExportAllDropdownMenu),
        FText::FromString(TEXT("Export Options")),
        FText::FromString(TEXT("Export Blueprints from selected folders or export a JSON file list for the project.")),
        FSlateIcon(FName("NodeToCodeStyle"), FName("NodeToCode.ExportButton"), FName("NodeToCode.ExportButton.Small")),
        true
    );
}

TSharedRef<SWidget> FN2CEditorIntegration::MakeExportAllDropdownMenu()
{
    FMenuBuilder MenuBuilder(true, LevelEditorCommandList);

    MenuBuilder.BeginSection(TEXT("N2CProjectExportOptions"), FText::FromString(TEXT("NodeToCode Export Options")));

    MenuBuilder.AddMenuEntry(
        FText::FromString(TEXT("Export Blueprints from folders...")),
        FText::FromString(TEXT("Choose one or more /Game folders and export Blueprints from them recursively into one N2C ZIP archive.")),
        FSlateIcon(FName("NodeToCodeStyle"), FName("NodeToCode.ExportButton"), FName("NodeToCode.ExportButton.Small")),
        FUIAction(FExecuteAction::CreateRaw(this, &FN2CEditorIntegration::ExecuteBack2DeadExportBlueprintsFromFolderPicker))
    );

    MenuBuilder.AddMenuEntry(
        FText::FromString(TEXT("Export project file list")),
        FText::FromString(TEXT("Export one AI-friendly JSON with UE asset metadata, content files and import source files.")),
        FSlateIcon(FName("NodeToCodeStyle"), FName("NodeToCode.ExportButton"), FName("NodeToCode.ExportButton.Small")),
        FUIAction(FExecuteAction::CreateRaw(this, &FN2CEditorIntegration::ExecuteBack2DeadExportProjectFileList))
    );

    MenuBuilder.EndSection();
    return MenuBuilder.MakeWidget();
}

void FN2CEditorIntegration::Initialize()
{
    // Register commands
    FN2CToolbarCommand::Register();

    // Register tab spawner
    SN2CEditorWindow::RegisterTabSpawner();

    // Register main editor toolbar buttons for project-level patch import / full Blueprint export.
    RegisterLevelEditorToolbar();

    // Register Content Browser right-click export for selected Blueprints.
    RegisterContentBrowserAssetContextMenu();

    // Subscribe to asset editor opened events
    if (GEditor)
    {
        UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
        if (AssetEditorSubsystem)
        {
            AssetEditorSubsystem->OnAssetOpenedInEditor().AddRaw(this, &FN2CEditorIntegration::HandleAssetEditorOpened);
            FN2CLogger::Get().Log(TEXT("N2C Editor Integration: Subscribed to OnAssetOpenedInEditor via AssetEditorSubsystem"), EN2CLogSeverity::Info);
        }
    }

    FN2CLogger::Get().Log(TEXT("N2C Editor Integration initialized"), EN2CLogSeverity::Info);
}

void FN2CEditorIntegration::Shutdown()
{
    // Unregister tab spawner
    SN2CEditorWindow::UnregisterTabSpawner();

    // Clear editor command lists
    EditorCommandLists.Empty();
    LevelEditorCommandList.Reset();
    LevelEditorToolbarExtender.Reset();

    // Unsubscribe from Content Browser context menu extension.
    if (ContentBrowserAssetExtenderDelegateHandle.IsValid() && FModuleManager::Get().IsModuleLoaded(TEXT("ContentBrowser")))
    {
        FContentBrowserModule& ContentBrowserModule = FModuleManager::GetModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
        TArray<FContentBrowserMenuExtender_SelectedAssets>& Extenders = ContentBrowserModule.GetAllAssetViewContextMenuExtenders();
        Extenders.RemoveAll([this](const FContentBrowserMenuExtender_SelectedAssets& Delegate)
        {
            return Delegate.GetHandle() == ContentBrowserAssetExtenderDelegateHandle;
        });
        ContentBrowserAssetExtenderDelegateHandle.Reset();
    }

    // Unsubscribe from asset editor events
    if (GEditor)
    {
        UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
        if (AssetEditorSubsystem)
        {
            AssetEditorSubsystem->OnAssetOpenedInEditor().RemoveAll(this);
        }
    }

    FN2CLogger::Get().Log(TEXT("N2C Editor Integration shutdown"), EN2CLogSeverity::Info);
}


TSharedPtr<FBlueprintEditor> FN2CEditorIntegration::GetBlueprintEditorFromTab() const
{
    // Mark as deprecated
    FN2CLogger::Get().LogWarning(TEXT("GetBlueprintEditorFromTab is deprecated - editors should be accessed directly"));
    return nullptr;
}

void FN2CEditorIntegration::HandleAssetEditorOpened(UObject* Asset, IAssetEditorInstance* EditorInstance)
{
    if (!Asset || !EditorInstance)
    {
        return;
    }

    // Check if the asset is a Blueprint or a child class of Blueprint
    UBlueprint* OpenedBlueprint = Cast<UBlueprint>(Asset);
    if (!OpenedBlueprint)
    {
        return; // Not a Blueprint, so ignore
    }

    // Convert the EditorInstance to the correct type
    FBlueprintEditor* BlueprintEditorPtr = static_cast<FBlueprintEditor*>(EditorInstance);
    if (!BlueprintEditorPtr)
    {
        return;
    }

    // Convert to SharedPtr so it matches our existing RegisterToolbarForEditor() signature
    TSharedPtr<FBlueprintEditor> BlueprintEditorShared = StaticCastSharedRef<FBlueprintEditor>(BlueprintEditorPtr->AsShared());
    if (BlueprintEditorShared.IsValid())
    {
        // Check if we already have this editor registered
        TWeakPtr<FBlueprintEditor> WeakEditor(BlueprintEditorShared);
        if (!EditorCommandLists.Contains(WeakEditor))
        {
            FString BlueprintPath = OpenedBlueprint->GetPathName();
            
            FN2CLogger::Get().Log(
                FString::Printf(TEXT("Registering toolbar for Blueprint Editor: %s"), 
                *BlueprintPath), 
                EN2CLogSeverity::Info
            );
            
            RegisterToolbarForEditor(BlueprintEditorShared);
        }
        else
        {
            FN2CLogger::Get().Log(
                TEXT("Blueprint Editor already registered"), 
                EN2CLogSeverity::Debug
            );
        }
    }
}


void FN2CEditorIntegration::RegisterToolbarForEditor(TSharedPtr<FBlueprintEditor> InEditor)
{
    FN2CLogger::Get().Log(TEXT("Starting toolbar registration for editor"), EN2CLogSeverity::Info);

    if (!InEditor.IsValid())
    {
        FN2CLogger::Get().LogError(TEXT("Invalid editor pointer provided to RegisterToolbarForEditor"));
        return;
    }

    // Get Blueprint name for context
    FString BlueprintName = TEXT("Unknown");
    if (InEditor->GetBlueprintObj())
    {
        BlueprintName = InEditor->GetBlueprintObj()->GetName();
    }

    // Check if we already have a command list for this editor
    TWeakPtr<FBlueprintEditor> WeakEditor(InEditor);
    if (EditorCommandLists.Contains(WeakEditor))
    {
        FN2CLogger::Get().Log(
            FString::Printf(TEXT("Editor already has command list registered: %s"), *BlueprintName),
            EN2CLogSeverity::Warning
        );
        return;
    }

    // Create command list for this editor
    TSharedPtr<FUICommandList> CommandList = MakeShareable(new FUICommandList);
    
    FN2CLogger::Get().Log(
        FString::Printf(TEXT("Created command list for Blueprint: %s"), *BlueprintName), 
        EN2CLogSeverity::Info
    );

    // Map the simplified Back2Dead Export command
    CommandList->MapAction(
        FN2CToolbarCommand::Get().ExportCommand,
        FExecuteAction::CreateLambda([this, WeakEditor, BlueprintName]()
        {
            FN2CLogger::Get().Log(
                FString::Printf(TEXT("Back2Dead AI export triggered for Blueprint: %s"), *BlueprintName),
                EN2CLogSeverity::Info
            );
            ExecuteBack2DeadExportForEditor(WeakEditor);
        }),
        FCanExecuteAction::CreateLambda([WeakEditor]()
        {
            TSharedPtr<FBlueprintEditor> Editor = WeakEditor.Pin();
            return Editor.IsValid() && Editor->GetCurrentMode() == FBlueprintEditorApplicationModes::StandardBlueprintEditorMode;
        })
    );

    // Map the simplified Back2Dead Import command
    CommandList->MapAction(
        FN2CToolbarCommand::Get().ImportCommand,
        FExecuteAction::CreateLambda([this, WeakEditor, BlueprintName]()
        {
            FN2CLogger::Get().Log(
                FString::Printf(TEXT("Back2Dead patch import triggered for Blueprint: %s"), *BlueprintName),
                EN2CLogSeverity::Info
            );
            ExecuteBack2DeadImportForEditor(WeakEditor);
        }),
        FCanExecuteAction::CreateLambda([WeakEditor]()
        {
            TSharedPtr<FBlueprintEditor> Editor = WeakEditor.Pin();
            return Editor.IsValid() && Editor->GetCurrentMode() == FBlueprintEditorApplicationModes::StandardBlueprintEditorMode;
        })
    );

    // Store in our map
    EditorCommandLists.Add(WeakEditor, CommandList);
    FN2CLogger::Get().Log(
        FString::Printf(TEXT("Added command list to map for Blueprint: %s"), *BlueprintName),
        EN2CLogSeverity::Info
    );

    // Add toolbar extension
    TSharedPtr<FExtender> ToolbarExtender = MakeShareable(new FExtender);
    ToolbarExtender->AddToolBarExtension(
        "Asset",
        EExtensionHook::After,
        CommandList,
        FToolBarExtensionDelegate::CreateLambda([CommandList](FToolBarBuilder& Builder)
        {
            Builder.AddSeparator();

            // Verify style is loaded and log the path for debugging
            const FSlateBrush* TestBrush = FSlateStyleRegistry::FindSlateStyle("NodeToCodeStyle") ?
                FSlateStyleRegistry::FindSlateStyle("NodeToCodeStyle")->GetBrush("NodeToCode.ToolbarButton") : nullptr;
            FN2CLogger::Get().Log(
                FString::Printf(TEXT("Toolbar icon brush: %s"), TestBrush ? *TestBrush->GetResourceName().ToString() : TEXT("NOT FOUND")),
                EN2CLogSeverity::Info);

            Builder.AddToolBarButton(
                FN2CToolbarCommand::Get().ExportCommand,
                NAME_None,
                FN2CToolbarCommand::CommandLabel_Export,
                FN2CToolbarCommand::CommandTooltip_Export,
                FSlateIcon(FName("NodeToCodeStyle"), FName("NodeToCode.ExportButton"), FName("NodeToCode.ExportButton.Small"))
            );

            Builder.AddToolBarButton(
                FN2CToolbarCommand::Get().ImportCommand,
                NAME_None,
                FN2CToolbarCommand::CommandLabel_Import,
                FN2CToolbarCommand::CommandTooltip_Import,
                FSlateIcon(FName("NodeToCodeStyle"), FName("NodeToCode.ImportButton"), FName("NodeToCode.ImportButton.Small"))
            );
        })
    );

    // Add the extender to this specific editor
    InEditor->AddToolbarExtender(ToolbarExtender);

    InEditor->RegenerateMenusAndToolbars();
    
    FN2CLogger::Get().Log(
        FString::Printf(TEXT("Completed toolbar registration for Blueprint: %s"), *BlueprintName), 
        EN2CLogSeverity::Info
    );
}


void FN2CEditorIntegration::ExecuteBack2DeadExportForEditor(TWeakPtr<FBlueprintEditor> InEditor)
{
    using namespace N2CEditorIntegration_Private;

    TSharedPtr<FBlueprintEditor> Editor = InEditor.Pin();
    if (!Editor.IsValid())
    {
        FN2CLogger::Get().LogError(TEXT("Back2Dead export failed: invalid Blueprint Editor pointer"));
        ShowN2CNotification(TEXT("NodeToCode export failed: invalid Blueprint editor"), SNotificationItem::CS_Fail);
        return;
    }

    UBlueprint* BlueprintObj = Editor->GetBlueprintObj();
    if (!BlueprintObj)
    {
        FN2CLogger::Get().LogError(TEXT("Back2Dead export failed: Blueprint object is null"));
        ShowN2CNotification(TEXT("NodeToCode export failed: Blueprint object is null"), SNotificationItem::CS_Fail);
        return;
    }

    FString JsonOutput;
    FString Error;
    if (!FN2CAIExport::BuildBlueprintAIJson(BlueprintObj, JsonOutput, Error))
    {
        FN2CLogger::Get().LogError(FString::Printf(TEXT("Back2Dead export failed: %s"), *Error));
        ShowN2CNotification(FString::Printf(TEXT("NodeToCode export failed: %s"), *Error), SNotificationItem::CS_Fail);
        return;
    }

    const FString SafeBlueprintName = SanitizeForFileName(BlueprintObj->GetName());
    const FString Timestamp = MakeExportTimestamp();
    const FString ExportDir = FPaths::ProjectSavedDir() / TEXT("NodeToCode/AIExports") / FString::Printf(TEXT("N2C_%s_%s"), *SafeBlueprintName, *Timestamp);
    const FString MainPath = ExportDir / FString::Printf(TEXT("N2C_%s_%s.json"), *SafeBlueprintName, *Timestamp);

    if (!FN2CAIExport::SaveJsonToFile(JsonOutput, MainPath, Error))
    {
        FN2CLogger::Get().LogError(FString::Printf(TEXT("Back2Dead export save failed: %s"), *Error));
        ShowN2CNotification(FString::Printf(TEXT("NodeToCode export save failed: %s"), *Error), SNotificationItem::CS_Fail);
        return;
    }

    FString Report;
    SaveFunctionSplitFiles(JsonOutput, ExportDir / TEXT("functions"), Report);

    FString NiagaraJson;
    FString NiagaraError;
    if (FN2CAIExport::BuildSelectedNiagaraAIJson(NiagaraJson, NiagaraError))
    {
        FString NiagaraSaveError;
        const FString NiagaraPath = ExportDir / FString::Printf(TEXT("N2C_SELECTED_NIAGARA_%s.json"), *Timestamp);
        SaveTextFile(NiagaraPath, NiagaraJson, NiagaraSaveError);
        if (NiagaraSaveError.IsEmpty())
        {
            Report += FString::Printf(TEXT("Selected Niagara reflection export saved: %s"), *NiagaraPath) + LINE_TERMINATOR;
        }
        else
        {
            Report += NiagaraSaveError + LINE_TERMINATOR;
        }
    }
    else
    {
        Report += TEXT("Niagara export skipped: no Niagara assets selected in Content Browser.") LINE_TERMINATOR;
    }

    const FString ZipPath = ExportDir + TEXT(".zip");
    ZipDirectoryForSharing(ExportDir, ZipPath, Report);

    FN2CLogger::Get().Log(FString::Printf(TEXT("Back2Dead AI export saved: %s\n%s"), *MainPath, *Report), EN2CLogSeverity::Info);
    if (FPaths::FileExists(ZipPath))
    {
        ShowN2CNotification(FString::Printf(TEXT("N2C export ZIP ready: %s"), *ZipPath), SNotificationItem::CS_Success);
    }
    else
    {
        ShowN2CNotification(FString::Printf(TEXT("N2C export ready: %s"), *MainPath), SNotificationItem::CS_Success);
    }
}

void FN2CEditorIntegration::ExecuteBack2DeadImportForEditor(TWeakPtr<FBlueprintEditor> InEditor)
{
    using namespace N2CEditorIntegration_Private;

    TSharedPtr<FBlueprintEditor> Editor = InEditor.Pin();
    if (!Editor.IsValid())
    {
        ShowN2CNotification(TEXT("NodeToCode import failed: invalid Blueprint editor"), SNotificationItem::CS_Fail);
        return;
    }

    UBlueprint* BlueprintObj = Editor->GetBlueprintObj();
    if (!BlueprintObj)
    {
        ShowN2CNotification(TEXT("NodeToCode import failed: Blueprint object is null"), SNotificationItem::CS_Fail);
        return;
    }

    FString PatchJson;
    FPlatformApplicationMisc::ClipboardPaste(PatchJson);

    FString DryRunReport;
    FString DialogBlueprintName = BlueprintObj->GetName();
    auto DryRunAnyPatch = [&](const FString& InPatchJson) -> bool
    {
        DryRunReport.Empty();
        DialogBlueprintName = BlueprintObj->GetName();
        if (IsProjectPatchJson(InPatchJson))
        {
            return RunProjectPatch(InPatchJson, true, DialogBlueprintName, DryRunReport);
        }
        return FN2CPatchImporter::DryRunPatch(BlueprintObj, InPatchJson, DryRunReport);
    };

    bool bPatchLooksValid = !PatchJson.IsEmpty() && DryRunAnyPatch(PatchJson);

    if (!bPatchLooksValid)
    {
        FString PatchPath;
        if (!PickJsonFile(PatchPath))
        {
            FN2CLogger::Get().LogWarning(FString::Printf(TEXT("Back2Dead import cancelled. Clipboard dry-run report:\n%s"), *DryRunReport));
            ShowN2CNotification(TEXT("N2C import cancelled: patch JSON was not selected"), SNotificationItem::CS_Fail);
            return;
        }

        if (!FFileHelper::LoadFileToString(PatchJson, *PatchPath))
        {
            ShowN2CNotification(FString::Printf(TEXT("N2C import failed: could not read patch JSON: %s"), *PatchPath), SNotificationItem::CS_Fail);
            return;
        }

        bPatchLooksValid = DryRunAnyPatch(PatchJson);
    }

    if (!bPatchLooksValid)
    {
        FN2CLogger::Get().LogError(FString::Printf(TEXT("Back2Dead import dry run failed:\n%s"), *DryRunReport));
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(MakePatchFailedDialogText(DialogBlueprintName, DryRunReport)));
        return;
    }

    const EAppReturnType::Type Confirm = FMessageDialog::Open(
        EAppMsgType::YesNo,
        FText::FromString(MakePatchValidationDialogText(DialogBlueprintName, DryRunReport))
    );

    if (Confirm != EAppReturnType::Yes)
    {
        ShowN2CNotification(TEXT("N2C import cancelled by user"), SNotificationItem::CS_None);
        return;
    }

    TArray<FN2CAutoFormatTarget> AutoFormatTargets;
    CollectAutoFormatTargetsFromPatchJson(PatchJson, BlueprintObj, AutoFormatTargets);

    FString ApplyReport;
    FString ApplyDialogName = DialogBlueprintName;
    const bool bApplied = IsProjectPatchJson(PatchJson)
        ? RunProjectPatch(PatchJson, false, ApplyDialogName, ApplyReport)
        : FN2CPatchImporter::ApplyPatchToBlueprint(BlueprintObj, PatchJson, ApplyReport);

    if (bApplied)
    {
        QueueBlueprintAssistAutoFormatIfEnabled(AutoFormatTargets, ApplyReport);
    }

    FN2CLogger::Get().Log(FString::Printf(TEXT("Back2Dead import apply report:\n%s"), *ApplyReport), bApplied ? EN2CLogSeverity::Info : EN2CLogSeverity::Error);

    FMessageDialog::Open(
        EAppMsgType::Ok,
        FText::FromString(MakePatchApplyDialogText(bApplied, ApplyDialogName, ApplyReport))
    );

    ShowN2CNotification(bApplied ? TEXT("N2C import finished") : TEXT("N2C import finished with errors"), bApplied ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
}


void FN2CEditorIntegration::ExecuteBack2DeadProjectImportFromEditor()
{
    using namespace N2CEditorIntegration_Private;

    FString PatchJson;
    FPlatformApplicationMisc::ClipboardPaste(PatchJson);

    FString DryRunReport;
    FString DialogName = TEXT("Project patch");

    auto DryRunProjectPatch = [&](const FString& InPatchJson) -> bool
    {
        DryRunReport.Empty();
        DialogName = TEXT("Project patch");
        if (!IsProjectPatchJson(InPatchJson))
        {
            DryRunReport += TEXT("ERROR: project toolbar import expects N2C_PROJECT_PATCH_V1. Use the Blueprint Editor Import N2C button for a single current-Blueprint patch.") LINE_TERMINATOR;
            return false;
        }
        return RunProjectPatch(InPatchJson, true, DialogName, DryRunReport);
    };

    bool bPatchLooksValid = !PatchJson.IsEmpty() && DryRunProjectPatch(PatchJson);

    if (!bPatchLooksValid)
    {
        FString PatchPath;
        if (!PickJsonFile(PatchPath))
        {
            FN2CLogger::Get().LogWarning(FString::Printf(TEXT("Project N2C import cancelled. Dry-run report:\n%s"), *DryRunReport));
            ShowN2CNotification(TEXT("N2C project import cancelled: patch JSON was not selected"), SNotificationItem::CS_Fail);
            return;
        }

        if (!FFileHelper::LoadFileToString(PatchJson, *PatchPath))
        {
            ShowN2CNotification(FString::Printf(TEXT("N2C project import failed: could not read JSON: %s"), *PatchPath), SNotificationItem::CS_Fail);
            return;
        }

        bPatchLooksValid = DryRunProjectPatch(PatchJson);
    }

    if (!bPatchLooksValid)
    {
        FN2CLogger::Get().LogError(FString::Printf(TEXT("Project N2C import dry run failed:\n%s"), *DryRunReport));
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(MakePatchFailedDialogText(DialogName, DryRunReport)));
        return;
    }

    const EAppReturnType::Type Confirm = FMessageDialog::Open(
        EAppMsgType::YesNo,
        FText::FromString(MakePatchValidationDialogText(DialogName, DryRunReport))
    );

    if (Confirm != EAppReturnType::Yes)
    {
        ShowN2CNotification(TEXT("N2C project import cancelled by user"), SNotificationItem::CS_None);
        return;
    }

    TArray<FN2CAutoFormatTarget> AutoFormatTargets;
    CollectAutoFormatTargetsFromPatchJson(PatchJson, nullptr, AutoFormatTargets);

    FString ApplyReport;
    FString ApplyDialogName = DialogName;
    const bool bApplied = RunProjectPatch(PatchJson, false, ApplyDialogName, ApplyReport);

    if (bApplied)
    {
        QueueBlueprintAssistAutoFormatIfEnabled(AutoFormatTargets, ApplyReport);
    }

    FN2CLogger::Get().Log(FString::Printf(TEXT("Project N2C import apply report:\n%s"), *ApplyReport), bApplied ? EN2CLogSeverity::Info : EN2CLogSeverity::Error);

    FMessageDialog::Open(
        EAppMsgType::Ok,
        FText::FromString(MakePatchApplyDialogText(bApplied, ApplyDialogName, ApplyReport))
    );

    ShowN2CNotification(bApplied ? TEXT("N2C project import finished") : TEXT("N2C project import finished with errors"), bApplied ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
}

void FN2CEditorIntegration::RegisterContentBrowserAssetContextMenu()
{
    if (ContentBrowserAssetExtenderDelegateHandle.IsValid())
    {
        return;
    }

    FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
    TArray<FContentBrowserMenuExtender_SelectedAssets>& Extenders = ContentBrowserModule.GetAllAssetViewContextMenuExtenders();
    Extenders.Add(FContentBrowserMenuExtender_SelectedAssets::CreateRaw(this, &FN2CEditorIntegration::OnExtendContentBrowserAssetSelectionMenu));
    ContentBrowserAssetExtenderDelegateHandle = Extenders.Last().GetHandle();

    FN2CLogger::Get().Log(TEXT("N2C Content Browser context menu registered"), EN2CLogSeverity::Info);
}

TSharedRef<FExtender> FN2CEditorIntegration::OnExtendContentBrowserAssetSelectionMenu(const TArray<FAssetData>& SelectedAssets)
{
    TSharedRef<FExtender> Extender = MakeShareable(new FExtender);

    bool bHasBlueprint = false;
    for (const FAssetData& AssetData : SelectedAssets)
    {
        UClass* AssetClass = AssetData.GetClass();
        const bool bClassLooksLikeBlueprint = AssetClass && AssetClass->IsChildOf(UBlueprint::StaticClass());
        const FString AssetClassName = AssetData.AssetClass.ToString();
        const bool bNameLooksLikeBlueprint = AssetClassName == TEXT("Blueprint") || AssetClassName.EndsWith(TEXT("Blueprint"));
        if (bClassLooksLikeBlueprint || bNameLooksLikeBlueprint)
        {
            bHasBlueprint = true;
            break;
        }
    }

    if (!bHasBlueprint)
    {
        return Extender;
    }

    Extender->AddMenuExtension(
        TEXT("GetAssetActions"),
        EExtensionHook::After,
        nullptr,
        FMenuExtensionDelegate::CreateRaw(this, &FN2CEditorIntegration::AddContentBrowserN2CMenuEntry, SelectedAssets)
    );

    return Extender;
}

void FN2CEditorIntegration::AddContentBrowserN2CMenuEntry(FMenuBuilder& MenuBuilder, TArray<FAssetData> SelectedAssets)
{
    MenuBuilder.BeginSection(TEXT("N2CExport"), FText::FromString(TEXT("NodeToCode")));
    MenuBuilder.AddMenuEntry(
        FText::FromString(TEXT("Export N2C")),
        FText::FromString(TEXT("Export selected Blueprint assets into one N2C ZIP archive.")),
        FSlateIcon(FName("NodeToCodeStyle"), FName("NodeToCode.ExportButton"), FName("NodeToCode.ExportButton.Small")),
        FUIAction(FExecuteAction::CreateRaw(this, &FN2CEditorIntegration::ExecuteBack2DeadExportSelectedBlueprints, SelectedAssets))
    );
    MenuBuilder.EndSection();
}

bool FN2CEditorIntegration::ExportBlueprintAssetsToSingleArchive(const TArray<FAssetData>& BlueprintAssets, const FString& ExportLabel, FString& OutZipPath, FString& OutReport)
{
    using namespace N2CEditorIntegration_Private;

    OutZipPath.Empty();
    OutReport.Empty();

    if (BlueprintAssets.Num() <= 0)
    {
        OutReport += TEXT("ERROR: no Blueprint assets were provided for export.") LINE_TERMINATOR;
        return false;
    }

    const FString Timestamp = MakeExportTimestamp();
    const FString SafeLabel = SanitizeForFileName(ExportLabel.IsEmpty() ? TEXT("Blueprints") : ExportLabel);
    const FString ExportRootDir = FPaths::ProjectSavedDir() / TEXT("NodeToCode/ProjectExports") / FString::Printf(TEXT("N2C_%s_%s"), *SafeLabel, *Timestamp);
    const FString BlueprintsDir = ExportRootDir / TEXT("blueprints");

    IFileManager::Get().DeleteDirectory(*ExportRootDir, false, true);
    IFileManager::Get().MakeDirectory(*BlueprintsDir, true);

    TArray<TSharedPtr<FJsonValue>> ManifestAssets;
    TSet<FName> SeenPackages;
    int32 ExportedCount = 0;
    int32 SkippedCount = 0;

    FScopedSlowTask SlowTask(BlueprintAssets.Num(), FText::FromString(FString::Printf(TEXT("Exporting %d Blueprint(s) to N2C..."), BlueprintAssets.Num())));
    SlowTask.MakeDialog(true);

    for (const FAssetData& AssetData : BlueprintAssets)
    {
        SlowTask.EnterProgressFrame(1.0f, FText::FromName(AssetData.AssetName));
        if (SlowTask.ShouldCancel())
        {
            OutReport += TEXT("WARNING: export cancelled by user.") LINE_TERMINATOR;
            break;
        }

        if (SeenPackages.Contains(AssetData.PackageName))
        {
            continue;
        }
        SeenPackages.Add(AssetData.PackageName);

        UObject* LoadedAsset = AssetData.GetAsset();
        UBlueprint* Blueprint = Cast<UBlueprint>(LoadedAsset);
        if (!Blueprint)
        {
            ++SkippedCount;
            OutReport += FString::Printf(TEXT("WARNING: skipped non-Blueprint asset: %s"), *AssetData.ObjectPath.ToString()) + LINE_TERMINATOR;
            continue;
        }

        FString BlueprintJson;
        FString Error;
        if (!FN2CAIExport::BuildBlueprintAIJson(Blueprint, BlueprintJson, Error))
        {
            ++SkippedCount;
            OutReport += FString::Printf(TEXT("ERROR: failed to export Blueprint %s: %s"), *Blueprint->GetPathName(), *Error) + LINE_TERMINATOR;
            continue;
        }

        const FString SafeBlueprintPath = SanitizeForFileName(Blueprint->GetPathName()).Replace(TEXT("."), TEXT("_"));
        const FString BlueprintDir = BlueprintsDir / SafeBlueprintPath;
        const FString SafeBlueprintName = SanitizeForFileName(Blueprint->GetName());
        const FString MainRelativePath = FString::Printf(TEXT("blueprints/%s/N2C_%s_%s.json"), *SafeBlueprintPath, *SafeBlueprintName, *Timestamp);
        const FString MainPath = ExportRootDir / MainRelativePath;

        FString SaveError;
        if (!SaveTextFile(MainPath, BlueprintJson, SaveError))
        {
            ++SkippedCount;
            OutReport += SaveError + LINE_TERMINATOR;
            continue;
        }

        FString FunctionReport;
        SaveFunctionSplitFiles(BlueprintJson, BlueprintDir / TEXT("functions"), FunctionReport);

        TSharedPtr<FJsonObject> ManifestAsset = MakeShared<FJsonObject>();
        ManifestAsset->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
        ManifestAsset->SetStringField(TEXT("blueprint_path"), Blueprint->GetPathName());
        ManifestAsset->SetStringField(TEXT("asset_object_path"), AssetData.ObjectPath.ToString());
        ManifestAsset->SetStringField(TEXT("json"), MainRelativePath);
        ManifestAsset->SetStringField(TEXT("functions_dir"), FString::Printf(TEXT("blueprints/%s/functions"), *SafeBlueprintPath));
        ManifestAssets.Add(MakeShared<FJsonValueObject>(ManifestAsset));

        ++ExportedCount;
        OutReport += FString::Printf(TEXT("Exported Blueprint: %s"), *Blueprint->GetPathName()) + LINE_TERMINATOR;
    }

    TSharedPtr<FJsonObject> Manifest = MakeShared<FJsonObject>();
    Manifest->SetStringField(TEXT("schema"), TEXT("N2C_PROJECT_EXPORT_V1"));
    Manifest->SetStringField(TEXT("export_kind"), TEXT("BlueprintProjectArchive"));
    Manifest->SetStringField(TEXT("export_label"), ExportLabel);
    Manifest->SetStringField(TEXT("timestamp"), Timestamp);
    Manifest->SetNumberField(TEXT("source_asset_count"), BlueprintAssets.Num());
    Manifest->SetNumberField(TEXT("exported_blueprint_count"), ExportedCount);
    Manifest->SetNumberField(TEXT("skipped_asset_count"), SkippedCount);
    Manifest->SetArrayField(TEXT("assets"), ManifestAssets);

    FString ManifestJson;
    TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&ManifestJson);
    FJsonSerializer::Serialize(Manifest.ToSharedRef(), Writer);

    FString ManifestError;
    SaveTextFile(ExportRootDir / TEXT("N2C_PROJECT_MANIFEST.json"), ManifestJson, ManifestError);
    if (!ManifestError.IsEmpty())
    {
        OutReport += ManifestError + LINE_TERMINATOR;
    }

    if (ExportedCount <= 0)
    {
        OutReport += TEXT("ERROR: export finished with zero exported Blueprints.") LINE_TERMINATOR;
        return false;
    }

    OutZipPath = ExportRootDir + TEXT(".zip");
    ZipDirectoryForSharing(ExportRootDir, OutZipPath, OutReport);

    if (!FPaths::FileExists(OutZipPath))
    {
        OutReport += FString::Printf(TEXT("ERROR: ZIP file was not created: %s"), *OutZipPath) + LINE_TERMINATOR;
        return false;
    }

    OutReport += FString::Printf(TEXT("Project N2C export complete: %d Blueprint(s), %d skipped asset(s)."), ExportedCount, SkippedCount) + LINE_TERMINATOR;
    return true;
}

void FN2CEditorIntegration::ExecuteBack2DeadExportSelectedBlueprints(TArray<FAssetData> SelectedAssets)
{
    using namespace N2CEditorIntegration_Private;

    FString ZipPath;
    FString Report;
    const bool bOk = ExportBlueprintAssetsToSingleArchive(SelectedAssets, TEXT("SelectedBlueprints"), ZipPath, Report);

    FN2CLogger::Get().Log(FString::Printf(TEXT("Selected Blueprint N2C export report:\n%s"), *Report), bOk ? EN2CLogSeverity::Info : EN2CLogSeverity::Error);

    if (bOk)
    {
        ShowN2CNotification(FString::Printf(TEXT("Selected Blueprint N2C ZIP ready: %s"), *ZipPath), SNotificationItem::CS_Success);
    }
    else
    {
        const FString Message = FString(TEXT("NOT OK")) + LINE_TERMINATOR + LINE_TERMINATOR
            + TEXT("Selected Blueprint export failed.") + LINE_TERMINATOR + LINE_TERMINATOR
            + TEXT("Problems:") + LINE_TERMINATOR
            + Report;
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Message));
        ShowN2CNotification(TEXT("Selected Blueprint N2C export failed"), SNotificationItem::CS_Fail);
    }
}

void FN2CEditorIntegration::ExecuteBack2DeadExportBlueprintsFromFolderPicker()
{
    using namespace N2CEditorIntegration_Private;

    TArray<FString> SelectedPaths;
    if (!PickContentFoldersForN2CExport(SelectedPaths))
    {
        ShowN2CNotification(TEXT("Folder N2C export cancelled: no folders selected"), SNotificationItem::CS_None);
        return;
    }

    TArray<FAssetData> BlueprintAssets;
    GetBlueprintAssetsUnderPaths(SelectedPaths, BlueprintAssets);

    if (BlueprintAssets.Num() <= 0)
    {
        FString PathList;
        for (const FString& Path : SelectedPaths)
        {
            PathList += TEXT("- ") + Path + LINE_TERMINATOR;
        }
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(FString(TEXT("NOT OK")) + LINE_TERMINATOR + LINE_TERMINATOR
            + TEXT("No Blueprint assets found in selected folders.") + LINE_TERMINATOR + LINE_TERMINATOR
            + TEXT("Folders:") + LINE_TERMINATOR + PathList));
        ShowN2CNotification(TEXT("Folder N2C export failed: no Blueprints found"), SNotificationItem::CS_Fail);
        return;
    }

    FString ZipPath;
    FString Report;
    const bool bOk = ExportBlueprintAssetsToSingleArchive(BlueprintAssets, TEXT("FolderBlueprints"), ZipPath, Report);

    FN2CLogger::Get().Log(FString::Printf(TEXT("Folder Blueprint N2C export report:\n%s"), *Report), bOk ? EN2CLogSeverity::Info : EN2CLogSeverity::Error);

    if (bOk)
    {
        const FString Message = FString(TEXT("OK")) + LINE_TERMINATOR + LINE_TERMINATOR
            + TEXT("Folder Blueprint export finished.") + LINE_TERMINATOR + LINE_TERMINATOR
            + FString::Printf(TEXT("Folders: %d"), SelectedPaths.Num()) + LINE_TERMINATOR
            + FString::Printf(TEXT("Blueprints: %d"), BlueprintAssets.Num()) + LINE_TERMINATOR
            + FString::Printf(TEXT("ZIP: %s"), *ZipPath);

        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Message));
        ShowN2CNotification(FString::Printf(TEXT("Folder Blueprint N2C ZIP ready: %s"), *ZipPath), SNotificationItem::CS_Success);
    }
    else
    {
        const FString Message = FString(TEXT("NOT OK")) + LINE_TERMINATOR + LINE_TERMINATOR
            + TEXT("Folder Blueprint export failed.") + LINE_TERMINATOR + LINE_TERMINATOR
            + TEXT("Problems:") + LINE_TERMINATOR
            + Report;
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Message));
        ShowN2CNotification(TEXT("Folder Blueprint N2C export failed"), SNotificationItem::CS_Fail);
    }
}

void FN2CEditorIntegration::ExecuteBack2DeadExportProjectFileList()
{
    using namespace N2CEditorIntegration_Private;

    TArray<FAssetData> Assets;
    GetAllProjectAssets(Assets);

    if (Assets.Num() <= 0)
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(TEXT("NOT OK") LINE_TERMINATOR LINE_TERMINATOR TEXT("No assets found under /Game.")));
        ShowN2CNotification(TEXT("Project file list export failed: no assets found"), SNotificationItem::CS_Fail);
        return;
    }

    TArray<FString> ContentDiskFiles;
    GetProjectContentDiskFiles(ContentDiskFiles);

    const FString Timestamp = MakeExportTimestamp();
    const FString ExportRootDir = FPaths::ProjectSavedDir() / TEXT("NodeToCode/ProjectExports") / FString::Printf(TEXT("N2C_ProjectFileList_%s"), *Timestamp);
    IFileManager::Get().DeleteDirectory(*ExportRootDir, false, true);
    IFileManager::Get().MakeDirectory(*ExportRootDir, true);

    TArray<TSharedPtr<FJsonValue>> JsonAssets;
    TArray<TSharedPtr<FJsonValue>> JsonContentDiskFiles;
    TArray<TSharedPtr<FJsonValue>> JsonSourceFilesUnique;
    TArray<TSharedPtr<FJsonValue>> JsonFlatFiles;

    TMap<FString, int32> AssetClassCounts;
    TMap<FString, int32> FlatExtensionCounts;

    TArray<FString> SourceFileKeys;
    TMap<FString, FString> SourceFilenameByKey;
    TMap<FString, TSet<FString>> SourceOwnersByKey;

    TArray<FString> FlatFileKeys;
    TMap<FString, FString> FlatFilenameByKey;
    TMap<FString, FString> FlatPrimaryKindByKey;
    TMap<FString, TSet<FString>> FlatKindsByKey;
    TMap<FString, TSet<FString>> FlatOwnersByKey;
    TMap<FString, TSet<FString>> FlatClassesByKey;

    auto NormalizeKey = [](const FString& Filename) -> FString
    {
        FString Key = Filename.ToLower();
        FPaths::NormalizeFilename(Key);
        return Key;
    };

    auto IncrementCounter = [](TMap<FString, int32>& Counter, const FString& Key)
    {
        int32& Count = Counter.FindOrAdd(Key.IsEmpty() ? FString(TEXT("unknown")) : Key);
        ++Count;
    };

    auto MakeStringArray = [](const TArray<FString>& Strings) -> TArray<TSharedPtr<FJsonValue>>
    {
        TArray<TSharedPtr<FJsonValue>> JsonValues;
        for (const FString& Value : Strings)
        {
            JsonValues.Add(MakeShared<FJsonValueString>(Value));
        }
        return JsonValues;
    };

    auto MakeStringSetArray = [&MakeStringArray](const TSet<FString>& StringSet) -> TArray<TSharedPtr<FJsonValue>>
    {
        TArray<FString> Values = StringSet.Array();
        Values.Sort();
        return MakeStringArray(Values);
    };

    auto RegisterFlatFile = [&](const FString& Kind, const FString& RawFilename, const FString& OwnerAsset, const FString& ClassName)
    {
        const FString NormalizedFilename = NormalizeDiskFilenameForJson(RawFilename);
        if (NormalizedFilename.IsEmpty())
        {
            return;
        }

        const FString Key = NormalizeKey(NormalizedFilename);
        if (!FlatFilenameByKey.Contains(Key))
        {
            FlatFilenameByKey.Add(Key, NormalizedFilename);
            FlatPrimaryKindByKey.Add(Key, Kind);
            FlatFileKeys.Add(Key);
        }

        FlatKindsByKey.FindOrAdd(Key).Add(Kind);
        if (!OwnerAsset.IsEmpty())
        {
            FlatOwnersByKey.FindOrAdd(Key).Add(OwnerAsset);
        }
        if (!ClassName.IsEmpty())
        {
            FlatClassesByKey.FindOrAdd(Key).Add(ClassName);
        }
    };

    for (const FAssetData& AssetData : Assets)
    {
        const FString DiskFilename = GetAssetDiskFilename(AssetData);
        const FString DiskExtension = GetAssetDiskExtension(AssetData);
        const FString ObjectPath = AssetData.ObjectPath.ToString();
        const FString ClassName = AssetData.AssetClass.ToString();

        IncrementCounter(AssetClassCounts, ClassName);
        RegisterFlatFile(TEXT("project_asset_file"), DiskFilename, ObjectPath, ClassName);

        TArray<FString> SourceFiles;
        GetAssetSourceFiles(AssetData, SourceFiles);

        TArray<TSharedPtr<FJsonValue>> JsonAssetSourceFiles;
        for (const FString& SourceFilenameRaw : SourceFiles)
        {
            const FString SourceFilename = NormalizeDiskFilenameForJson(SourceFilenameRaw);
            if (SourceFilename.IsEmpty())
            {
                continue;
            }

            const FString SourceKey = NormalizeKey(SourceFilename);
            if (!SourceFilenameByKey.Contains(SourceKey))
            {
                SourceFilenameByKey.Add(SourceKey, SourceFilename);
                SourceFileKeys.Add(SourceKey);
            }

            SourceOwnersByKey.FindOrAdd(SourceKey).Add(ObjectPath);
            RegisterFlatFile(TEXT("source_file"), SourceFilename, ObjectPath, ClassName);

            TSharedPtr<FJsonObject> SourceObject = MakeDiskFileJson(TEXT("source_file"), SourceFilename);
            SourceObject->SetStringField(TEXT("owner_asset"), ObjectPath);
            SourceObject->SetStringField(TEXT("owner_asset_name"), AssetData.AssetName.ToString());
            SourceObject->SetStringField(TEXT("owner_asset_class"), ClassName);
            JsonAssetSourceFiles.Add(MakeShared<FJsonValueObject>(SourceObject));
        }

        TSharedPtr<FJsonObject> AssetObject = MakeShared<FJsonObject>();
        AssetObject->SetStringField(TEXT("asset_name"), AssetData.AssetName.ToString());
        AssetObject->SetStringField(TEXT("class_name"), ClassName);
        AssetObject->SetStringField(TEXT("package_name"), AssetData.PackageName.ToString());
        AssetObject->SetStringField(TEXT("package_path"), AssetData.PackagePath.ToString());
        AssetObject->SetStringField(TEXT("object_path"), ObjectPath);
        AssetObject->SetStringField(TEXT("project_file"), DiskFilename);
        AssetObject->SetStringField(TEXT("project_file_extension"), DiskExtension);
        AssetObject->SetStringField(TEXT("display_file"), AssetData.AssetName.ToString() + DiskExtension);
        AssetObject->SetStringField(TEXT("relative_to_project"), MakePathRelativeToProject(DiskFilename));
        AssetObject->SetStringField(TEXT("relative_to_content"), MakePathRelativeToContent(DiskFilename));
        AssetObject->SetNumberField(TEXT("source_file_count"), SourceFiles.Num());
        AssetObject->SetArrayField(TEXT("source_files"), JsonAssetSourceFiles);
        JsonAssets.Add(MakeShared<FJsonValueObject>(AssetObject));
    }

    for (const FString& ContentDiskFileRaw : ContentDiskFiles)
    {
        const FString ContentDiskFile = NormalizeDiskFilenameForJson(ContentDiskFileRaw);
        if (ContentDiskFile.IsEmpty())
        {
            continue;
        }

        TSharedPtr<FJsonObject> ContentFileObject = MakeDiskFileJson(TEXT("content_disk_file"), ContentDiskFile);
        ContentFileObject->SetStringField(TEXT("relative_to_content"), MakePathRelativeToContent(ContentDiskFile));
        JsonContentDiskFiles.Add(MakeShared<FJsonValueObject>(ContentFileObject));

        RegisterFlatFile(TEXT("content_disk_file"), ContentDiskFile, TEXT(""), TEXT(""));
    }

    SourceFileKeys.Sort();
    for (const FString& SourceKey : SourceFileKeys)
    {
        const FString SourceFilename = SourceFilenameByKey.FindRef(SourceKey);
        TSharedPtr<FJsonObject> SourceObject = MakeDiskFileJson(TEXT("source_file"), SourceFilename);
        const TSet<FString>* Owners = SourceOwnersByKey.Find(SourceKey);
        if (Owners)
        {
            SourceObject->SetNumberField(TEXT("owner_asset_count"), Owners->Num());
            SourceObject->SetArrayField(TEXT("owner_assets"), MakeStringSetArray(*Owners));
        }
        else
        {
            SourceObject->SetNumberField(TEXT("owner_asset_count"), 0);
            SourceObject->SetArrayField(TEXT("owner_assets"), TArray<TSharedPtr<FJsonValue>>());
        }
        JsonSourceFilesUnique.Add(MakeShared<FJsonValueObject>(SourceObject));
    }

    FlatFileKeys.Sort();
    for (const FString& FlatKey : FlatFileKeys)
    {
        const FString FlatFilename = FlatFilenameByKey.FindRef(FlatKey);
        const FString PrimaryKind = FlatPrimaryKindByKey.FindRef(FlatKey);
        TSharedPtr<FJsonObject> FlatObject = MakeDiskFileJson(PrimaryKind, FlatFilename);

        const TSet<FString>* Kinds = FlatKindsByKey.Find(FlatKey);
        if (Kinds)
        {
            FlatObject->SetArrayField(TEXT("kinds"), MakeStringSetArray(*Kinds));
        }

        const TSet<FString>* Owners = FlatOwnersByKey.Find(FlatKey);
        if (Owners)
        {
            FlatObject->SetNumberField(TEXT("owner_asset_count"), Owners->Num());
            FlatObject->SetArrayField(TEXT("owner_assets"), MakeStringSetArray(*Owners));
        }
        else
        {
            FlatObject->SetNumberField(TEXT("owner_asset_count"), 0);
            FlatObject->SetArrayField(TEXT("owner_assets"), TArray<TSharedPtr<FJsonValue>>());
        }

        const TSet<FString>* Classes = FlatClassesByKey.Find(FlatKey);
        if (Classes)
        {
            FlatObject->SetArrayField(TEXT("asset_classes"), MakeStringSetArray(*Classes));
        }
        else
        {
            FlatObject->SetArrayField(TEXT("asset_classes"), TArray<TSharedPtr<FJsonValue>>());
        }

        IncrementCounter(FlatExtensionCounts, FPaths::GetExtension(FlatFilename, true).ToLower());
        JsonFlatFiles.Add(MakeShared<FJsonValueObject>(FlatObject));
    }

    auto MakeCounterObject = [](const TMap<FString, int32>& Counter) -> TSharedPtr<FJsonObject>
    {
        TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
        TArray<FString> Keys;
        Counter.GetKeys(Keys);
        Keys.Sort();
        for (const FString& Key : Keys)
        {
            Object->SetNumberField(Key, Counter.FindRef(Key));
        }
        return Object;
    };

    TSharedPtr<FJsonObject> SummaryObject = MakeShared<FJsonObject>();
    SummaryObject->SetNumberField(TEXT("asset_count"), Assets.Num());
    SummaryObject->SetNumberField(TEXT("content_disk_file_count"), JsonContentDiskFiles.Num());
    SummaryObject->SetNumberField(TEXT("unique_source_file_count"), JsonSourceFilesUnique.Num());
    SummaryObject->SetNumberField(TEXT("flat_file_count"), JsonFlatFiles.Num());
    SummaryObject->SetObjectField(TEXT("asset_class_counts"), MakeCounterObject(AssetClassCounts));
    SummaryObject->SetObjectField(TEXT("flat_file_extension_counts"), MakeCounterObject(FlatExtensionCounts));

    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("schema"), TEXT("N2C_PROJECT_FILE_LIST_V3"));
    Root->SetStringField(TEXT("export_kind"), TEXT("ProjectAssetAndSourceFileList"));
    Root->SetStringField(TEXT("root"), TEXT("/Game"));
    Root->SetStringField(TEXT("timestamp"), Timestamp);
    Root->SetStringField(TEXT("project_root"), NormalizeDiskFilenameForJson(FPaths::ProjectDir()));
    Root->SetStringField(TEXT("content_root"), NormalizeDiskFilenameForJson(FPaths::ProjectContentDir()));
    Root->SetStringField(TEXT("format_note"), TEXT("assets[] contains UE asset metadata; content_disk_files[] contains .uasset/.umap files on disk; source_files[] contains unique import sources from AssetImportData; flat_files[] contains unique disk files without duplicates."));
    Root->SetNumberField(TEXT("asset_count"), Assets.Num());
    Root->SetNumberField(TEXT("content_disk_file_count"), JsonContentDiskFiles.Num());
    Root->SetNumberField(TEXT("unique_source_file_count"), JsonSourceFilesUnique.Num());
    Root->SetNumberField(TEXT("flat_file_count"), JsonFlatFiles.Num());
    Root->SetObjectField(TEXT("summary"), SummaryObject);
    Root->SetArrayField(TEXT("assets"), JsonAssets);
    Root->SetArrayField(TEXT("content_disk_files"), JsonContentDiskFiles);
    Root->SetArrayField(TEXT("source_files"), JsonSourceFilesUnique);
    Root->SetArrayField(TEXT("flat_files"), JsonFlatFiles);

    FString JsonText;
    TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&JsonText);
    FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);

    FString Report;
    FString SaveError;
    SaveTextFile(ExportRootDir / TEXT("N2C_PROJECT_FILE_LIST.json"), JsonText, SaveError);
    if (!SaveError.IsEmpty())
    {
        Report += SaveError + LINE_TERMINATOR;
    }

    const FString ZipPath = ExportRootDir + TEXT(".zip");
    ZipDirectoryForSharing(ExportRootDir, ZipPath, Report);

    FN2CLogger::Get().Log(FString::Printf(TEXT("Project file list N2C export report:\n%s"), *Report), Report.Contains(TEXT("ERROR:")) ? EN2CLogSeverity::Error : EN2CLogSeverity::Info);

    if (FPaths::FileExists(ZipPath) && !Report.Contains(TEXT("ERROR:")))
    {
        const FString Message = FString(TEXT("OK")) + LINE_TERMINATOR + LINE_TERMINATOR
            + TEXT("Project file list export finished.") + LINE_TERMINATOR + LINE_TERMINATOR
            + FString::Printf(TEXT("Assets: %d"), Assets.Num()) + LINE_TERMINATOR
            + FString::Printf(TEXT("Content disk files: %d"), JsonContentDiskFiles.Num()) + LINE_TERMINATOR
            + FString::Printf(TEXT("Source files: %d"), JsonSourceFilesUnique.Num()) + LINE_TERMINATOR
            + FString::Printf(TEXT("Unique flat files: %d"), JsonFlatFiles.Num()) + LINE_TERMINATOR
            + FString::Printf(TEXT("ZIP: %s"), *ZipPath);
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Message));
        ShowN2CNotification(FString::Printf(TEXT("Project file list JSON ZIP ready: %s"), *ZipPath), SNotificationItem::CS_Success);
    }
    else
    {
        const FString Message = FString(TEXT("NOT OK")) + LINE_TERMINATOR + LINE_TERMINATOR
            + TEXT("Project file list export failed.") + LINE_TERMINATOR + LINE_TERMINATOR
            + TEXT("Problems:") + LINE_TERMINATOR
            + Report;
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Message));
        ShowN2CNotification(TEXT("Project file list export failed"), SNotificationItem::CS_Fail);
    }
}

void FN2CEditorIntegration::ExecuteBack2DeadExportAllBlueprints()
{
    using namespace N2CEditorIntegration_Private;

    const EAppReturnType::Type Confirm = FMessageDialog::Open(
        EAppMsgType::YesNo,
        FText::FromString(
            TEXT("Are you sure to export ALL blueprints from project?") LINE_TERMINATOR
            TEXT("It may take a long time.")
        )
    );

    if (Confirm != EAppReturnType::Yes)
    {
        ShowN2CNotification(TEXT("Export All N2C cancelled"), SNotificationItem::CS_None);
        return;
    }

    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    AssetRegistryModule.Get().SearchAllAssets(true);

    FARFilter Filter;
    Filter.PackagePaths.Add(FName(TEXT("/Game")));
    Filter.ClassNames.Add(UBlueprint::StaticClass()->GetFName());
    Filter.bRecursivePaths = true;
    Filter.bRecursiveClasses = true;

    TArray<FAssetData> BlueprintAssets;
    AssetRegistryModule.Get().GetAssets(Filter, BlueprintAssets);

    if (BlueprintAssets.Num() <= 0)
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(TEXT("NOT OK") LINE_TERMINATOR LINE_TERMINATOR TEXT("No Blueprint assets found under /Game.")));
        ShowN2CNotification(TEXT("Export All N2C failed: no Blueprints found"), SNotificationItem::CS_Fail);
        return;
    }

    FString ZipPath;
    FString Report;
    const bool bOk = ExportBlueprintAssetsToSingleArchive(BlueprintAssets, TEXT("AllBlueprints"), ZipPath, Report);

    FN2CLogger::Get().Log(FString::Printf(TEXT("Export All N2C report:\n%s"), *Report), bOk ? EN2CLogSeverity::Info : EN2CLogSeverity::Error);

    if (bOk)
    {
        const FString Message = FString(TEXT("OK")) + LINE_TERMINATOR + LINE_TERMINATOR
            + TEXT("Export All N2C finished.") + LINE_TERMINATOR + LINE_TERMINATOR
            + FString::Printf(TEXT("Blueprints: %d"), BlueprintAssets.Num()) + LINE_TERMINATOR
            + FString::Printf(TEXT("ZIP: %s"), *ZipPath);

        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Message));
        ShowN2CNotification(FString::Printf(TEXT("Export All N2C ZIP ready: %s"), *ZipPath), SNotificationItem::CS_Success);
    }
    else
    {
        const FString Message = FString(TEXT("NOT OK")) + LINE_TERMINATOR + LINE_TERMINATOR
            + TEXT("Export All N2C failed.") + LINE_TERMINATOR + LINE_TERMINATOR
            + TEXT("Problems:") + LINE_TERMINATOR
            + Report;
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Message));
        ShowN2CNotification(TEXT("Export All N2C failed"), SNotificationItem::CS_Fail);
    }
}

TArray<FName> FN2CEditorIntegration::GetAvailableThemes(EN2CCodeLanguage Language) const
{
    TArray<FName> ThemeNames;
    const UN2CSettings* Settings = GetDefault<UN2CSettings>();
    
    switch (Language)
    {
        case EN2CCodeLanguage::Cpp:
            Settings->CPPThemes.Themes.GetKeys(ThemeNames);
            break;
        case EN2CCodeLanguage::Python:
            Settings->PythonThemes.Themes.GetKeys(ThemeNames);
            break;
        case EN2CCodeLanguage::JavaScript:
            Settings->JavaScriptThemes.Themes.GetKeys(ThemeNames);
            break;
        case EN2CCodeLanguage::CSharp:
            Settings->CSharpThemes.Themes.GetKeys(ThemeNames);
            break;
        case EN2CCodeLanguage::Swift:
            Settings->SwiftThemes.Themes.GetKeys(ThemeNames);
            break;
    }
    
    return ThemeNames;
}

FName FN2CEditorIntegration::GetDefaultTheme(EN2CCodeLanguage Language) const
{
    return TEXT("Unreal Engine");
}

void FN2CEditorIntegration::ExecuteCollectNodesForEditor(TWeakPtr<FBlueprintEditor> InEditor)
{
    // Check if translation is already in progress
    UN2CLLMModule* LLMModule = UN2CLLMModule::Get();
    if (LLMModule && LLMModule->GetSystemStatus() == EN2CSystemStatus::Processing)
    {
        FN2CLogger::Get().LogWarning(TEXT("Translation already in progress, please wait"));
        return;
    }

    FN2CLogger::Get().Log(TEXT("ExecuteCollectNodesForEditor called"), EN2CLogSeverity::Debug);

    // Get the editor pointer BEFORE invoking the tab, because TryInvokeTab
    // can shift focus to a different editor/tab and change which graph is "focused"
    TSharedPtr<FBlueprintEditor> Editor = InEditor.Pin();
    if (!Editor.IsValid())
    {
        FN2CLogger::Get().LogError(TEXT("Invalid Blueprint Editor pointer"));
        return;
    }
    FN2CLogger::Get().Log(TEXT("Successfully obtained Blueprint Editor pointer"), EN2CLogSeverity::Info);

    // Capture the focused graph BEFORE opening the results tab
    UEdGraph* FocusedGraph = Editor->GetFocusedGraph();
    if (!FocusedGraph)
    {
        FN2CLogger::Get().LogError(TEXT("No focused graph in Blueprint Editor"));
        return;
    }

    // Close any existing tab first so TryInvokeTab re-spawns it in the
    // currently active window (the one where the toolbar button was clicked),
    // rather than activating it wherever it was previously docked
    if (SN2CEditorWindow::IsTabOpen())
    {
        SN2CEditorWindow::CloseTab();
    }
    FGlobalTabmanager::Get()->TryInvokeTab(SN2CEditorWindow::TabId);
    FN2CLogger::Get().Log(TEXT("Node to Code tab spawned in active window"), EN2CLogSeverity::Debug);

    FString GraphName = FocusedGraph->GetName();
    FString BlueprintName = TEXT("Unknown");
    if (UBlueprint* Blueprint = Cast<UBlueprint>(FocusedGraph->GetOuter()))
    {
        BlueprintName = Blueprint->GetName();
    }
    FN2CLogger::Get().Log(
        FString::Printf(TEXT("Found focused graph: %s in Blueprint: %s"), 
        *GraphName, *BlueprintName), 
        EN2CLogSeverity::Info
    );

    // Get collector instance
    FN2CNodeCollector& Collector = FN2CNodeCollector::Get();
    
    // Collect nodes using the specific editor
    TArray<UK2Node*> CollectedNodes;
    if (Collector.CollectNodesFromGraph(FocusedGraph, CollectedNodes))
    {
        FString Context = FString::Printf(TEXT("Collected %d nodes"), CollectedNodes.Num());
        FN2CLogger::Get().Log(TEXT("Node collection successful"), EN2CLogSeverity::Info, Context);
        
        // Get translator instance                                                                                                                                                                        
        FN2CNodeTranslator& Translator = FN2CNodeTranslator::Get();

        // Generate N2CStruct from collected nodes
        if (Translator.GenerateN2CStruct(CollectedNodes))
        {
            FN2CLogger::Get().Log(TEXT("Node translation successful"), EN2CLogSeverity::Info);

            // Get the Blueprint structure
            const FN2CBlueprint& Blueprint = FN2CNodeTranslator::Get().GetN2CBlueprint();
            
            // Validate the generated Blueprint
            if (Blueprint.IsValid())
            {
                FN2CLogger::Get().Log(TEXT("Node translation validation successful"), EN2CLogSeverity::Info);

                // Serialize to JSON with pretty printing enabled                                                                                                                                             
                FN2CSerializer::SetPrettyPrint(false);
                FString JsonOutput = FN2CSerializer::ToJson(Blueprint);                                                                                                                                       
                                                                                                                                                                                                           
                // Log the JSON output                                                                                                                                                                        
                if (!JsonOutput.IsEmpty())                                                                                                                                                                    
                {                                                                                                                                                                                             
                    FN2CLogger::Get().Log(TEXT("JSON Output:"), EN2CLogSeverity::Debug);                                                                                                                       
                    FN2CLogger::Get().Log(JsonOutput, EN2CLogSeverity::Debug);
                    
                    if (LLMModule->Initialize())
                    {
                        // Send JSON to LLM service                                                                                                                                                        
                        LLMModule->ProcessN2CJson(JsonOutput, FOnLLMResponseReceived::CreateLambda(
                            [](const FString& Response)                                                                                                                                                   
                            {                                                                                                                                                                             
                                FN2CLogger::Get().Log(FString::Printf(TEXT("LLM Response:\n\n%s"), *Response), EN2CLogSeverity::Debug);                                                                                                      
                            
                                // Create translation response struct
                                FN2CTranslationResponse TranslationResponse;
                            
                                // Get active service's response parser
                                TScriptInterface<IN2CLLMService> ActiveService = UN2CLLMModule::Get()->GetActiveService();
                                if (ActiveService.GetInterface())
                                {
                                    UN2CResponseParserBase* Parser = ActiveService->GetResponseParser();
                                    if (Parser)
                                    {
                                        if (Parser->ParseLLMResponse(Response, TranslationResponse))
                                        {
                                            // Log successful parsing
                                            FN2CLogger::Get().Log(TEXT("Successfully parsed LLM response"), EN2CLogSeverity::Info);
                                        }
                                        else
                                        {
                                            FN2CLogger::Get().LogError(TEXT("Failed to parse LLM response"));
                                        }
                                    }
                                    else
                                    {
                                        FN2CLogger::Get().LogError(TEXT("No response parser available"));
                                    }
                                }
                                else
                                {
                                    FN2CLogger::Get().LogError(TEXT("No active LLM service"));
                                }
                            }));
                    }
                    else
                    {
                        FN2CLogger::Get().LogError(TEXT("Failed to initialize LLM Module"));
                    }
                }
                else
                {
                    FN2CLogger::Get().LogError(TEXT("JSON serialization failed"));
                }
            }
            else
            {
                FN2CLogger::Get().LogError(TEXT("Node translation validation failed"));
            }
        }
        else
        {
            FN2CLogger::Get().LogError(TEXT("Failed to translate nodes"));
        }
    }
}
