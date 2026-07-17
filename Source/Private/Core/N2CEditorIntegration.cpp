// Copyright (c) 2025 Nick McClure (Protospatial). All Rights Reserved.

#include "Core/N2CEditorIntegration.h"

#include "BlueprintEditorModes.h"
#include "Core/N2CNodeCollector.h"
#include "Core/N2CAIExport.h"
#include "Core/N2CCoverage.h"
#include "Core/N2CPatchImporter.h"
#include "BlueprintEditorModule.h"
#include "Core/N2CNodeTranslator.h"
#include "Core/N2CSerializer.h"
#include "Core/N2CSettings.h"
#include "Core/N2CToolbarCommand.h"
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
#include "HAL/PlatformFilemanager.h"
#include "Framework/Application/SlateApplication.h"
#include "Containers/Ticker.h"
#include "Widgets/SWindow.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Docking/SDockTab.h"
#include "Framework/Docking/TabManager.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "FileHelpers.h"
#include "PackageTools.h"
#include "UObject/GarbageCollection.h"
#include "Editor.h"

#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraParameterStore.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSourceBase.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"
#include "NiagaraUserRedirectionParameterStore.h"
#include "NiagaraScriptSource.h"
#include "EdGraphSchema_Niagara.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraNodeCustomHlsl.h"
#include "EdGraph/EdGraphNodeUtils.h"
#include "EdGraphUtilities.h"

#ifndef N2C_WITH_NIAGARA_PRIVATE_GRAPH_API
#define N2C_WITH_NIAGARA_PRIVATE_GRAPH_API 0
#endif

#if N2C_WITH_NIAGARA_PRIVATE_GRAPH_API
#include "NiagaraNodeParameterMapSet.h"
#include "NiagaraNodeParameterMapGet.h"
#endif

#include "Engine/Blueprint.h"
#include "Engine/UserDefinedEnum.h"
#include "Engine/UserDefinedStruct.h"
#include "UserDefinedStructure/UserDefinedStructEditorData.h"
#include "EdGraph/EdGraph.h"
#include "K2Node.h"
#include "K2Node_FunctionEntry.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/EnumEditorUtils.h"
#include "Kismet2/StructureEditorUtils.h"
#include "EdGraphSchema_K2.h"
#include "ScopedTransaction.h"
#include "TimerManager.h"
#include "EditorFramework/AssetImportData.h"
#include "UObject/UnrealType.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/App.h"
#include "Misc/CoreDelegates.h"
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
#include <initializer_list>

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
    // Forward declarations: Niagara import helpers are intentionally kept in this file,
    // but several passes use them before their definitions below. Keep declarations here
    // to avoid C3861 in non-unity/regular UE4 builds.
    static bool N2CReorderNiagaraModulesForUsage(UEdGraph* Graph, ENiagaraScriptUsage Usage, const TArray<FString>& DesiredOrder, FString& OutReason);
    static TSharedPtr<FJsonObject> GetNiagaraImportActionsObject(const TSharedPtr<FJsonObject>& Root);

    static FString SanitizeForFileName(FString In)
    {
        // Windows forbids these characters in file names: < > : " / \ | ? *
        // Previous builds only replaced /, \, :, and spaces, so Blueprint graphs
        // named like "CanMove?" were present in the main JSON but their split
        // functions/*.json sidecar file failed to save. Keep this sanitizer broad
        // because these exports are commonly moved between Windows and Unix hosts.
        In.ReplaceInline(TEXT("/"), TEXT("_"));
        In.ReplaceInline(TEXT("\\"), TEXT("_"));
        In.ReplaceInline(TEXT(":"), TEXT("_"));
        In.ReplaceInline(TEXT("*"), TEXT("_"));
        In.ReplaceInline(TEXT("?"), TEXT("_"));
        In.ReplaceInline(TEXT("\""), TEXT("_"));
        In.ReplaceInline(TEXT("<"), TEXT("_"));
        In.ReplaceInline(TEXT(">"), TEXT("_"));
        In.ReplaceInline(TEXT("|"), TEXT("_"));
        In.ReplaceInline(TEXT("\t"), TEXT("_"));
        In.ReplaceInline(TEXT("\r"), TEXT("_"));
        In.ReplaceInline(TEXT("\n"), TEXT("_"));
        In.ReplaceInline(TEXT(" "), TEXT("_"));
        while (In.Contains(TEXT("__")))
        {
            In.ReplaceInline(TEXT("__"), TEXT("_"));
        }
        In.TrimStartAndEndInline();
        while (In.StartsWith(TEXT(".")) || In.StartsWith(TEXT("_")))
        {
            In.RightChopInline(1, false);
        }
        while (In.EndsWith(TEXT(".")) || In.EndsWith(TEXT("_")))
        {
            In.LeftChopInline(1, false);
        }
        if (In.IsEmpty())
        {
            In = TEXT("Asset");
        }
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

    static FString GetDiagnosticField(const FString& Line, const FString& Key)
    {
        TArray<FString> Parts;
        Line.ParseIntoArray(Parts, TEXT("|"), true);
        const FString Prefix = Key + TEXT("=");
        for (FString Part : Parts)
        {
            Part.TrimStartAndEndInline();
            if (Part.StartsWith(Prefix))
            {
                return Part.RightChop(Prefix.Len());
            }
        }
        return FString();
    }

    static FString HumanizeN2CDiagnostic(const FString& Line)
    {
        if (Line.StartsWith(TEXT("N2C_COVERAGE_BLOCKER|")))
        {
            const FString NodeClass = GetDiagnosticField(Line, TEXT("node_class"));
            const FString Reason = GetDiagnosticField(Line, TEXT("reason"));
            return FString::Printf(
                TEXT("Node '%s' is blocked by import coverage%s%s."),
                NodeClass.IsEmpty() ? TEXT("<unknown>") : *NodeClass,
                Reason.IsEmpty() ? TEXT("") : TEXT(": "),
                Reason.IsEmpty() ? TEXT("") : *Reason);
        }

        if (Line.StartsWith(TEXT("N2C_PREFLIGHT_GUARD|")) ||
            Line.StartsWith(TEXT("N2C_RUNTIME_GUARD|")))
        {
            const FString Code = GetDiagnosticField(Line, TEXT("code"));
            if (Code == TEXT("unsupported_local_default"))
            {
                return TEXT("Default values for local function variables are not supported yet. Remove default/default_value or assign the value inside the function graph.");
            }
            if (Code == TEXT("struct_member_mismatch"))
            {
                return FString::Printf(
                    TEXT("A requested struct field could not be resolved: %s. Friendly name, internal name and persistent GUID were checked."),
                    *GetDiagnosticField(Line, TEXT("member")));
            }
            if (Code == TEXT("datatable_linked_output_untyped"))
            {
                return TEXT("Linked GetDataTableRow must connect Out Row/ReturnValue to a typed row-struct pin so UE4.27 can preserve the row type.");
            }
            if (Code == TEXT("datatable_row_type_not_persistent"))
            {
                return TEXT("GetDataTableRow lost its row-struct type during UE4.27 connection reconstruction.");
            }
            if (Code == TEXT("sandbox_apply_failed"))
            {
                return TEXT("Transient dry-run sandbox found a node, pin or connection that UE4.27 cannot create. The target Blueprint was not modified.");
            }
            if (Code == TEXT("sandbox_duplicate_failed"))
            {
                return TEXT("Strict dry run could not create a transient Blueprint sandbox. The target Blueprint was not modified.");
            }
            if (Code == TEXT("enum_member_type_unresolved"))
            {
                return FString::Printf(
                    TEXT("Enum-backed Byte variable '%s' requires a resolvable UEnum subtype. Use type=byte without an enum identity for raw uint8."),
                    *GetDiagnosticField(Line, TEXT("variable")));
            }
            if (Code == TEXT("edge_node_missing"))
            {
                return FString::Printf(
                    TEXT("A runtime edge references a node that was not created: %s -> %s."),
                    *GetDiagnosticField(Line, TEXT("from")),
                    *GetDiagnosticField(Line, TEXT("to")));
            }
            if (Code == TEXT("edge_pin_missing"))
            {
                return FString::Printf(
                    TEXT("A runtime edge references a pin that does not exist: %s -> %s."),
                    *GetDiagnosticField(Line, TEXT("from")),
                    *GetDiagnosticField(Line, TEXT("to")));
            }
            if (Code == TEXT("edge_connection_failed"))
            {
                return FString::Printf(
                    TEXT("UE4.27 rejected a requested runtime edge: %s -> %s."),
                    *GetDiagnosticField(Line, TEXT("from")),
                    *GetDiagnosticField(Line, TEXT("to")));
            }
            return Code.IsEmpty()
                ? TEXT("Import validation failed. See technical details in the log.")
                : FString::Printf(TEXT("Import validation failed: %s."), *Code);
        }

        if (Line.StartsWith(TEXT("N2C_ROLLBACK_FALLBACK|")))
        {
            return TEXT("In-memory rollback could not be verified, so the pre-apply backup was queued for automatic disk restore on the next UE startup. Close UE without saving this Blueprint.");
        }

        if (Line.StartsWith(TEXT("N2C_ROLLBACK_RESULT|")))
        {
            const FString Result = GetDiagnosticField(Line, TEXT("result"));
            if (Result == TEXT("PASS"))
            {
                return TEXT("The failed patch was rolled back and the Blueprint structure matches the pre-apply snapshot.");
            }
            const FString Fallback = GetDiagnosticField(Line, TEXT("fallback"));
            return Fallback == TEXT("queued")
                ? TEXT("In-memory rollback could not be verified. A pre-apply backup is queued for automatic restore on the next UE startup. Close UE without saving this Blueprint.")
                : TEXT("Automatic rollback could not be verified. Do not save the current Blueprint; restore the pre-apply backup listed in the technical details.");
        }

        if (Line.StartsWith(TEXT("N2C_PREFLIGHT_RESULT|")) &&
            GetDiagnosticField(Line, TEXT("allowed")) == TEXT("0"))
        {
            return TEXT("Strict preflight rejected this patch before Blueprint mutation.");
        }

        return FString();
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

            if (Line.StartsWith(TEXT("N2C_")))
            {
                AddUniqueLine(OutDetails, Line);
                const FString FriendlyDiagnostic = HumanizeN2CDiagnostic(Line);
                if (!FriendlyDiagnostic.IsEmpty())
                {
                    AddUniqueLine(OutProblems, FriendlyDiagnostic);
                }
                continue;
            }

            if (IsProblemReportLine(Line))
            {
                AddUniqueLine(OutProblems, FriendlyReportLine(Line));
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
                Line.StartsWith(TEXT("Graph patch summary:")) ||
                Line.StartsWith(TEXT("Graph edge connected:")) ||
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

    static void AppendSectionIfAny(FString& Message, const FString& Header, const TArray<FString>& Lines)
    {
        if (Lines.Num() == 0)
        {
            return;
        }

        Message += Header + LINE_TERMINATOR;
        for (const FString& Line : Lines)
        {
            Message += TEXT("- ") + Line + LINE_TERMINATOR;
        }
        Message += LINE_TERMINATOR;
    }

    static void SplitApplyResultForDialog(const FString& Report, TArray<FString>& OutProblems, TArray<FString>& OutWarnings)
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

            if (Line.StartsWith(TEXT("N2C_")))
            {
                const FString FriendlyDiagnostic = HumanizeN2CDiagnostic(Line);
                if (!FriendlyDiagnostic.IsEmpty())
                {
                    AddUniqueLine(OutProblems, FriendlyDiagnostic);
                }
                continue;
            }

            if (IsProblemReportLine(Line))
            {
                AddUniqueLine(OutProblems, FriendlyReportLine(Line));
                continue;
            }

            if (Line.ToLower().StartsWith(TEXT("warning:")))
            {
                AddUniqueLine(OutWarnings, FriendlyReportLine(Line));
            }
        }
    }

    static FString MakePatchValidationDialogText(const FString& BlueprintName, const FString& DryRunReport)
    {
        TArray<FString> NewItems;
        TArray<FString> ChangedItems;
        TArray<FString> Problems;
        TArray<FString> Details;
        SplitReportForDialog(DryRunReport, NewItems, ChangedItems, Problems, Details);

        TArray<FString> Warnings;
        for (const FString& Detail : Details)
        {
            if (Detail.ToLower().StartsWith(TEXT("warning:")))
            {
                AddUniqueLine(Warnings, FriendlyReportLine(Detail));
            }
        }

        FString Message;
        Message += TEXT("OK") LINE_TERMINATOR LINE_TERMINATOR;
        Message += FString::Printf(TEXT("Blueprint : %s"), *BlueprintName) + LINE_TERMINATOR LINE_TERMINATOR;
        Message += TEXT("This is a pre-apply import check.") LINE_TERMINATOR;
        Message += TEXT("Changes have NOT been applied yet.") LINE_TERMINATOR;
        Message += TEXT("If you click OK, the plugin will create a backup, apply the patch, compile the Blueprint, and roll back on error.") LINE_TERMINATOR LINE_TERMINATOR;

        AppendSectionIfAny(Message, TEXT("What will be added:"), NewItems);
        AppendSectionIfAny(Message, TEXT("What will be changed:"), ChangedItems);
        AppendSectionIfAny(Message, TEXT("Warnings / safe-skips:"), Warnings);
        AppendSectionIfAny(Message, TEXT("Problems:"), Problems);

        if (NewItems.Num() == 0 && ChangedItems.Num() == 0 && Warnings.Num() == 0 && Problems.Num() == 0)
        {
            Message += TEXT("No explicit changes were found in this patch.") LINE_TERMINATOR LINE_TERMINATOR;
        }

        return Message;
    }

    static FString MakePatchApplyDialogText(bool bApplied, const FString& BlueprintName, const FString& ApplyReport, const FString& PlannedReport = TEXT(""))
    {
        TArray<FString> Problems;
        TArray<FString> Warnings;
        SplitApplyResultForDialog(ApplyReport, Problems, Warnings);

        FString Message;
        Message += bApplied ? TEXT("OK") : TEXT("NOT OK");
        Message += LINE_TERMINATOR LINE_TERMINATOR;
        Message += FString::Printf(TEXT("Blueprint : %s"), *BlueprintName) + LINE_TERMINATOR LINE_TERMINATOR;
        if (bApplied)
        {
            Message += TEXT("Applied successfully.");
        }
        else if (ApplyReport.Contains(TEXT("N2C_APPLY_BLOCKED_BEFORE_MUTATION")))
        {
            Message += TEXT("Not applied. Strict validation stopped the patch before Blueprint mutation.");
        }
        else if (ApplyReport.Contains(TEXT("N2C_ROLLBACK_RESULT|result=PASS")))
        {
            Message += TEXT("Apply failed. Changes were rolled back and the pre-apply Blueprint structure was verified.");
        }
        else if (ApplyReport.Contains(TEXT("N2C_ROLLBACK_RESULT|result=FAIL")) &&
                 ApplyReport.Contains(TEXT("N2C_ROLLBACK_FALLBACK|result=QUEUED")))
        {
            Message += TEXT("Apply failed. In-memory rollback could not be verified, so the pre-apply backup was queued for automatic restore on the next UE startup. Close UE without saving this Blueprint.");
        }
        else if (ApplyReport.Contains(TEXT("N2C_ROLLBACK_RESULT|result=FAIL")))
        {
            Message += TEXT("Apply failed. Automatic rollback could not be verified. Do not save this Blueprint; restore the pre-apply backup listed in the technical details.");
        }
        else
        {
            Message += TEXT("Not applied. Check the errors below; rollback status was not reported.");
        }
        Message += LINE_TERMINATOR LINE_TERMINATOR;

        AppendSectionIfAny(Message, TEXT("Errors:"), Problems);
        AppendSectionIfAny(Message, TEXT("Warnings:"), Warnings);

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

    static bool SaveBlueprintCoverageSidecar(UBlueprint* Blueprint, const FString& SourceJson, const FString& MainJsonPath, FString& OutCoverageJson, FString& OutCoveragePath, FString& OutReport)
    {
        OutCoverageJson.Empty();
        OutCoveragePath = FPaths::GetPath(MainJsonPath) / (FPaths::GetBaseFilename(MainJsonPath) + TEXT(".coverage.json"));
        FString PrimaryReason;
        if (!FN2CCoverageClassifier::BuildBlueprintSidecar(Blueprint, SourceJson, OutCoverageJson, PrimaryReason))
        {
            OutReport += FString::Printf(TEXT("ERROR: could not build coverage sidecar for %s: %s"), Blueprint ? *Blueprint->GetPathName() : TEXT("<null>"), *PrimaryReason) + LINE_TERMINATOR;
            return false;
        }
        FString Error;
        if (!SaveTextFile(OutCoveragePath, OutCoverageJson, Error))
        {
            OutReport += Error + LINE_TERMINATOR;
            return false;
        }
        FN2CLogger::Get().Log(FString::Printf(TEXT("N2C_COVERAGE_RESULT|asset=%s|source_schema=N2C_AI_EXPORT_V2|sidecar=%s|primary_reason=%s"), Blueprint ? *Blueprint->GetPathName() : TEXT("<null>"), *OutCoveragePath, *PrimaryReason), EN2CLogSeverity::Info);
        OutReport += FString::Printf(TEXT("Coverage sidecar: %s"), *OutCoveragePath) + LINE_TERMINATOR;
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


    static bool GetAssetPackageFilename(UObject* Asset, FString& OutFilename, FString& OutError)
    {
        OutFilename.Empty();
        OutError.Empty();
        if (!Asset || !Asset->GetOutermost())
        {
            OutError = TEXT("Invalid asset/package.");
            return false;
        }

        const FString PackageName = Asset->GetOutermost()->GetName();
        if (!FPackageName::DoesPackageExist(PackageName, nullptr, &OutFilename) || OutFilename.IsEmpty())
        {
            OutError = FString::Printf(TEXT("Asset package file was not found for %s. Save the asset first, then retry."), *PackageName);
            return false;
        }
        return true;
    }

    static FString MakeAssetBackupPrefix(UObject* Asset)
    {
        if (!Asset || !Asset->GetOutermost())
        {
            return TEXT("InvalidAsset");
        }
        FString PackageName = Asset->GetOutermost()->GetName();
        PackageName.ReplaceInline(TEXT("/"), TEXT("_"));
        PackageName.ReplaceInline(TEXT("."), TEXT("_"));
        return SanitizeForFileName(PackageName);
    }

    static void PruneBackupsForAsset(UObject* Asset, int32 MaxBackupsToKeep, FString& InOutReport);

    static bool BackupAssetPackage(UObject* Asset, FString& OutBackupPath, FString& OutReport)
    {
        OutBackupPath.Empty();
        FString SourceFilename;
        FString Error;
        if (!GetAssetPackageFilename(Asset, SourceFilename, Error))
        {
            OutReport += FString::Printf(TEXT("ERROR: cannot create N2C backup: %s"), *Error) + LINE_TERMINATOR;
            return false;
        }

        const FString BackupDir = FPaths::ProjectSavedDir() / TEXT("NodeToCode/Backups");
        IFileManager::Get().MakeDirectory(*BackupDir, true);

        const FString Extension = FPaths::GetExtension(SourceFilename, true);
        const FString SafeObjectName = SanitizeForFileName(Asset ? Asset->GetName() : TEXT("Asset"));
        const FString Timestamp = MakeExportTimestamp();
        // Keep backup filenames aligned with normal N2C export filenames:
        // N2C_<AssetName>_<YYYYMMDD_HHMMSS>.uasset
        OutBackupPath = BackupDir / FString::Printf(TEXT("N2C_%s_%s%s"), *SafeObjectName, *Timestamp, *Extension);

        const int32 CopyResult = IFileManager::Get().Copy(*OutBackupPath, *SourceFilename, true, true);
        if (CopyResult == COPY_OK)
        {
            OutReport += FString::Printf(TEXT("Backup created: %s"), *OutBackupPath) + LINE_TERMINATOR;
            PruneBackupsForAsset(Asset, 10, OutReport);
            return true;
        }

        OutReport += FString::Printf(TEXT("ERROR: failed to create backup from %s to %s (copy result %d)"), *SourceFilename, *OutBackupPath, CopyResult) + LINE_TERMINATOR;
        return false;
    }

    static bool SaveAssetPackageNoPrompt(UObject* Asset, FString& OutReport)
    {
        if (!Asset || !Asset->GetOutermost())
        {
            OutReport += TEXT("WARNING: N2C auto-save skipped: invalid asset/package.") LINE_TERMINATOR;
            return false;
        }

        Asset->MarkPackageDirty();
        TArray<UPackage*> PackagesToSave;
        PackagesToSave.Add(Asset->GetOutermost());
        const FEditorFileUtils::EPromptReturnCode SaveResult = FEditorFileUtils::PromptForCheckoutAndSave(PackagesToSave, false, false);
        const bool bSaved = (SaveResult == FEditorFileUtils::PR_Success);
        OutReport += bSaved
            ? FString::Printf(TEXT("Saved asset package after N2C apply: %s"), *Asset->GetOutermost()->GetName()) + LINE_TERMINATOR
            : FString::Printf(TEXT("WARNING: N2C auto-save failed or was skipped for package: %s"), *Asset->GetOutermost()->GetName()) + LINE_TERMINATOR;
        return bSaved;
    }

    static void FindBackupFilesForAsset(UObject* Asset, TArray<FString>& OutBackupFiles)
    {
        OutBackupFiles.Empty();
        if (!Asset)
        {
            return;
        }

        const FString BackupDir = FPaths::ProjectSavedDir() / TEXT("NodeToCode/Backups");
        TArray<FString> AllFiles;
        IFileManager::Get().FindFilesRecursive(AllFiles, *BackupDir, TEXT("*.uasset"), true, false);

        const FString SafeObjectName = SanitizeForFileName(Asset->GetName());
        const FString SafePackagePrefix = MakeAssetBackupPrefix(Asset);
        for (const FString& File : AllFiles)
        {
            const FString NormalizedFile = File.Replace(TEXT("\\"), TEXT("/"));
            if (NormalizedFile.Contains(TEXT("/RestoreRollback/")) ||
                NormalizedFile.Contains(TEXT("/PendingRestore/")) ||
                NormalizedFile.Contains(TEXT("/PendingRestoreApplied/")))
            {
                continue;
            }

            const FString Base = FPaths::GetBaseFilename(File);
            if (Base.StartsWith(TEXT("N2C_") + SafeObjectName + TEXT("_")) ||
                Base.StartsWith(SafeObjectName + TEXT("_")) ||
                Base.StartsWith(SafePackagePrefix + TEXT("__")) ||
                Base.Contains(TEXT("__") + SafeObjectName + TEXT("__")))
            {
                OutBackupFiles.Add(File);
            }
        }

        OutBackupFiles.Sort([](const FString& A, const FString& B)
        {
            return IFileManager::Get().GetTimeStamp(*A) > IFileManager::Get().GetTimeStamp(*B);
        });
    }

    static void PruneBackupsForAsset(UObject* Asset, int32 MaxBackupsToKeep, FString& InOutReport)
    {
        if (!Asset || MaxBackupsToKeep <= 0)
        {
            return;
        }

        TArray<FString> BackupFiles;
        FindBackupFilesForAsset(Asset, BackupFiles);
        if (BackupFiles.Num() <= MaxBackupsToKeep)
        {
            return;
        }

        int32 DeletedCount = 0;
        for (int32 Index = MaxBackupsToKeep; Index < BackupFiles.Num(); ++Index)
        {
            const FString& BackupPath = BackupFiles[Index];
            const FString NormalizedFile = BackupPath.Replace(TEXT("\\"), TEXT("/"));
            if (NormalizedFile.Contains(TEXT("/RestoreRollback/")) ||
                NormalizedFile.Contains(TEXT("/PendingRestore/")) ||
                NormalizedFile.Contains(TEXT("/PendingRestoreApplied/")) ||
                NormalizedFile.Contains(TEXT("/PendingRestoreCancelled/")))
            {
                continue;
            }

            if (IFileManager::Get().Delete(*BackupPath, false, true, true))
            {
                ++DeletedCount;
            }
            else
            {
                InOutReport += FString::Printf(TEXT("WARNING: backup retention could not delete old backup: %s"), *BackupPath) + LINE_TERMINATOR;
            }
        }

        if (DeletedCount > 0)
        {
            InOutReport += FString::Printf(TEXT("Backup retention: kept last %d backup(s), deleted %d old backup(s) for %s."), MaxBackupsToKeep, DeletedCount, *Asset->GetName()) + LINE_TERMINATOR;
        }
    }

    static bool PickBackupFileForAsset(UObject* Asset, FString& OutBackupPath)
    {
        OutBackupPath.Empty();
        IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
        if (!DesktopPlatform)
        {
            return false;
        }

        const FString BackupDir = FPaths::ProjectSavedDir() / TEXT("NodeToCode/Backups");
        TArray<FString> OutFiles;
        const bool bOpened = DesktopPlatform->OpenFileDialog(
            nullptr,
            FString::Printf(TEXT("Choose N2C backup for %s"), Asset ? *Asset->GetName() : TEXT("asset")),
            BackupDir,
            TEXT(""),
            TEXT("Unreal asset backups (*.uasset)|*.uasset|All files (*.*)|*.*"),
            EFileDialogFlags::None,
            OutFiles
        );

        if (bOpened && OutFiles.Num() > 0)
        {
            OutBackupPath = OutFiles[0];
            return true;
        }
        return false;
    }

    static FString GetPendingRestoreDir()
    {
        return FPaths::ProjectSavedDir() / TEXT("NodeToCode/Backups/PendingRestore");
    }

    static FString GetPendingRestoreAppliedDir()
    {
        return FPaths::ProjectSavedDir() / TEXT("NodeToCode/Backups/PendingRestoreApplied");
    }

    static FString GetRestoreRollbackDir()
    {
        return FPaths::ProjectSavedDir() / TEXT("NodeToCode/Backups/RestoreRollback");
    }

    static bool LoadPendingRestoreManifest(const FString& ManifestPath, TMap<FString, FString>& OutFields, FString& OutError);

    static FString GetPendingRestoreCancelledDir()
    {
        return FPaths::ProjectSavedDir() / TEXT("NodeToCode/Backups/PendingRestoreCancelled");
    }

    static FString FileStateForDiagnostics(const FString& Path)
    {
        IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
        if (Path.IsEmpty())
        {
            return TEXT("<empty>");
        }
        const bool bExists = PlatformFile.FileExists(*Path);
        const bool bReadOnly = bExists ? PlatformFile.IsReadOnly(*Path) : false;
        const int64 Size = bExists ? PlatformFile.FileSize(*Path) : -1;
        return FString::Printf(
            TEXT("path=%s exists=%s read_only=%s size=%lld"),
            *Path,
            bExists ? TEXT("yes") : TEXT("no"),
            bReadOnly ? TEXT("yes") : TEXT("no"),
            Size);
    }

    static FString BuildPendingRestoreDiagnostics(const FString& ManifestPath, const TMap<FString, FString>& Fields)
    {
        FString Text;
        Text += TEXT("Diagnostics:") LINE_TERMINATOR;
        Text += TEXT("- manifest: ") + FileStateForDiagnostics(ManifestPath) + LINE_TERMINATOR;
        Text += TEXT("- target: ") + FileStateForDiagnostics(Fields.FindRef(TEXT("TargetFilename"))) + LINE_TERMINATOR;
        Text += TEXT("- pending_backup_copy: ") + FileStateForDiagnostics(Fields.FindRef(TEXT("PendingBackupCopy"))) + LINE_TERMINATOR;
        Text += TEXT("- original_backup: ") + FileStateForDiagnostics(Fields.FindRef(TEXT("OriginalBackupPath"))) + LINE_TERMINATOR;
        Text += TEXT("- rollback: ") + FileStateForDiagnostics(Fields.FindRef(TEXT("RollbackPath"))) + LINE_TERMINATOR;
        Text += FString::Printf(TEXT("- package: %s%s"), *Fields.FindRef(TEXT("PackageName")), LINE_TERMINATOR);
        Text += FString::Printf(TEXT("- asset_path: %s%s"), *Fields.FindRef(TEXT("AssetPathName")), LINE_TERMINATOR);
        return Text;
    }

    static void GetPendingRestoreManifests(TArray<FString>& OutManifestFiles)
    {
        OutManifestFiles.Empty();
        const FString PendingDir = GetPendingRestoreDir();
        IFileManager::Get().FindFilesRecursive(OutManifestFiles, *PendingDir, TEXT("*.restore"), true, false);
        OutManifestFiles.Sort([](const FString& A, const FString& B)
        {
            return IFileManager::Get().GetTimeStamp(*A) > IFileManager::Get().GetTimeStamp(*B);
        });
    }

    static FString BuildPendingRestoreStatusReport()
    {
        TArray<FString> ManifestFiles;
        GetPendingRestoreManifests(ManifestFiles);

        TArray<FString> AppliedFiles;
        IFileManager::Get().FindFilesRecursive(AppliedFiles, *GetPendingRestoreAppliedDir(), TEXT("*.done"), true, false);
        AppliedFiles.Sort([](const FString& A, const FString& B)
        {
            return IFileManager::Get().GetTimeStamp(*A) > IFileManager::Get().GetTimeStamp(*B);
        });

        FString Report;
        Report += TEXT("N2C Pending Restore Status") LINE_TERMINATOR LINE_TERMINATOR;
        Report += FString::Printf(TEXT("Queued restores: %d%s"), ManifestFiles.Num(), LINE_TERMINATOR);
        for (const FString& ManifestPath : ManifestFiles)
        {
            TMap<FString, FString> Fields;
            FString Error;
            if (LoadPendingRestoreManifest(ManifestPath, Fields, Error))
            {
                Report += FString::Printf(TEXT("- %s%s  target=%s%s  backup=%s%s  manifest=%s%s"),
                    *Fields.FindRef(TEXT("PackageName")), LINE_TERMINATOR,
                    *Fields.FindRef(TEXT("TargetFilename")), LINE_TERMINATOR,
                    *Fields.FindRef(TEXT("OriginalBackupPath")), LINE_TERMINATOR,
                    *ManifestPath, LINE_TERMINATOR);
            }
            else
            {
                Report += FString::Printf(TEXT("- BROKEN manifest: %s%s  %s%s"), *ManifestPath, LINE_TERMINATOR, *Error, LINE_TERMINATOR);
            }
        }

        Report += LINE_TERMINATOR;
        Report += FString::Printf(TEXT("Applied/failed startup records: %d%s"), AppliedFiles.Num(), LINE_TERMINATOR);
        const int32 MaxAppliedToShow = FMath::Min(AppliedFiles.Num(), 20);
        for (int32 Index = 0; Index < MaxAppliedToShow; ++Index)
        {
            Report += FString::Printf(TEXT("- %s%s"), *AppliedFiles[Index], LINE_TERMINATOR);
        }
        if (AppliedFiles.Num() > MaxAppliedToShow)
        {
            Report += FString::Printf(TEXT("- ... %d more%s"), AppliedFiles.Num() - MaxAppliedToShow, LINE_TERMINATOR);
        }
        return Report;
    }

    static bool ManifestMatchesAssetFilter(const TMap<FString, FString>& Fields, UObject* AssetFilter)
    {
        if (!AssetFilter || !AssetFilter->GetOutermost())
        {
            return true;
        }
        const FString PackageName = AssetFilter->GetOutermost()->GetName();
        const FString AssetPathName = AssetFilter->GetPathName();
        return Fields.FindRef(TEXT("PackageName")) == PackageName || Fields.FindRef(TEXT("AssetPathName")) == AssetPathName;
    }

    static bool CancelPendingRestoreManifests(UObject* AssetFilter, FString& OutReport)
    {
        OutReport.Empty();
        TArray<FString> ManifestFiles;
        GetPendingRestoreManifests(ManifestFiles);
        if (ManifestFiles.Num() <= 0)
        {
            OutReport = TEXT("No pending restore manifests were queued.");
            return true;
        }

        const FString CancelledDir = GetPendingRestoreCancelledDir();
        IFileManager::Get().MakeDirectory(*CancelledDir, true);

        int32 Cancelled = 0;
        int32 Failed = 0;
        for (const FString& ManifestPath : ManifestFiles)
        {
            TMap<FString, FString> Fields;
            FString Error;
            if (!LoadPendingRestoreManifest(ManifestPath, Fields, Error))
            {
                ++Failed;
                OutReport += FString::Printf(TEXT("ERROR: cannot cancel broken manifest: %s (%s)"), *ManifestPath, *Error) + LINE_TERMINATOR;
                continue;
            }

            if (!ManifestMatchesAssetFilter(Fields, AssetFilter))
            {
                continue;
            }

            const FString Stamp = MakeExportTimestamp();
            const FString Base = FPaths::GetBaseFilename(ManifestPath);
            const FString CancelledManifestPath = CancelledDir / FString::Printf(TEXT("%s__cancelled_%s.restore"), *Base, *Stamp);
            const FString PendingBackupCopy = Fields.FindRef(TEXT("PendingBackupCopy"));
            bool bOk = IFileManager::Get().Move(*CancelledManifestPath, *ManifestPath, true, true, true, true);
            if (!PendingBackupCopy.IsEmpty() && FPaths::FileExists(PendingBackupCopy))
            {
                const FString CancelledBackupPath = CancelledDir / FPaths::GetCleanFilename(PendingBackupCopy);
                bOk &= IFileManager::Get().Move(*CancelledBackupPath, *PendingBackupCopy, true, true, true, true);
            }

            if (bOk)
            {
                ++Cancelled;
                OutReport += FString::Printf(TEXT("Cancelled pending restore: %s"), *Fields.FindRef(TEXT("PackageName"))) + LINE_TERMINATOR;
            }
            else
            {
                ++Failed;
                OutReport += FString::Printf(TEXT("ERROR: failed to move pending restore into cancelled folder: %s"), *ManifestPath) + LINE_TERMINATOR;
                OutReport += BuildPendingRestoreDiagnostics(ManifestPath, Fields);
            }
        }

        if (Cancelled == 0 && Failed == 0)
        {
            OutReport += TEXT("No pending restore matched the selected asset.") LINE_TERMINATOR;
        }
        OutReport = FString::Printf(TEXT("Cancelled: %d%sFailed: %d%s%s"), Cancelled, LINE_TERMINATOR, Failed, LINE_TERMINATOR, *OutReport);
        return Failed == 0;
    }

    static FString MakePendingRestoreManifestText(
        const FString& TargetFilename,
        const FString& PendingBackupCopy,
        const FString& OriginalBackupPath,
        const FString& RollbackPath,
        const FString& PackageName,
        const FString& AssetPathName)
    {
        FString Text;
        Text += FString::Printf(TEXT("TargetFilename=%s%s"), *TargetFilename, LINE_TERMINATOR);
        Text += FString::Printf(TEXT("PendingBackupCopy=%s%s"), *PendingBackupCopy, LINE_TERMINATOR);
        Text += FString::Printf(TEXT("OriginalBackupPath=%s%s"), *OriginalBackupPath, LINE_TERMINATOR);
        Text += FString::Printf(TEXT("RollbackPath=%s%s"), *RollbackPath, LINE_TERMINATOR);
        Text += FString::Printf(TEXT("PackageName=%s%s"), *PackageName, LINE_TERMINATOR);
        Text += FString::Printf(TEXT("AssetPathName=%s%s"), *AssetPathName, LINE_TERMINATOR);
        Text += FString::Printf(TEXT("CreatedAt=%s%s"), *FDateTime::Now().ToString(), LINE_TERMINATOR);
        return Text;
    }

    static bool LoadPendingRestoreManifest(const FString& ManifestPath, TMap<FString, FString>& OutFields, FString& OutError)
    {
        OutFields.Empty();
        OutError.Empty();

        TArray<FString> Lines;
        if (!FFileHelper::LoadFileToStringArray(Lines, *ManifestPath))
        {
            OutError = FString::Printf(TEXT("could not read pending restore manifest: %s"), *ManifestPath);
            return false;
        }

        for (const FString& RawLine : Lines)
        {
            FString Line = RawLine.TrimStartAndEnd();
            if (Line.IsEmpty() || Line.StartsWith(TEXT("#")))
            {
                continue;
            }

            FString Key;
            FString Value;
            if (Line.Split(TEXT("="), &Key, &Value))
            {
                OutFields.Add(Key.TrimStartAndEnd(), Value.TrimStartAndEnd());
            }
        }

        const TCHAR* RequiredKeys[] = {
            TEXT("TargetFilename"),
            TEXT("PendingBackupCopy"),
            TEXT("PackageName")
        };

        for (const TCHAR* RequiredKey : RequiredKeys)
        {
            if (!OutFields.Contains(RequiredKey) || OutFields[RequiredKey].IsEmpty())
            {
                OutError = FString::Printf(TEXT("pending restore manifest is missing required field '%s': %s"), RequiredKey, *ManifestPath);
                return false;
            }
        }

        return true;
    }

    static bool IsAssetQueuedForPendingRestore(UObject* Asset, FString& OutManifestPath)
    {
        OutManifestPath.Empty();
        if (!Asset || !Asset->GetOutermost())
        {
            return false;
        }

        const FString PackageName = Asset->GetOutermost()->GetName();
        const FString AssetPathName = Asset->GetPathName();
        const FString PendingDir = GetPendingRestoreDir();

        TArray<FString> ManifestFiles;
        GetPendingRestoreManifests(ManifestFiles);
        for (const FString& ManifestPath : ManifestFiles)
        {
            TMap<FString, FString> Fields;
            FString Error;
            if (!LoadPendingRestoreManifest(ManifestPath, Fields, Error))
            {
                continue;
            }

            const FString ManifestPackageName = Fields.FindRef(TEXT("PackageName"));
            const FString ManifestAssetPathName = Fields.FindRef(TEXT("AssetPathName"));
            if (ManifestPackageName == PackageName || ManifestAssetPathName == AssetPathName)
            {
                OutManifestPath = ManifestPath;
                return true;
            }
        }

        return false;
    }

    static void CloseEditorsForAssetSafe(UObject* Asset)
    {
        if (!Asset || !GEditor)
        {
            return;
        }

        UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
        if (AssetEditorSubsystem)
        {
            AssetEditorSubsystem->CloseAllEditorsForAsset(Asset);
        }
    }


    static void ScheduleClosePendingRestoreAssetEditor(UObject* Asset, const FString& ManifestPath)
    {
        if (!Asset)
        {
            return;
        }

        const FString AssetPathName = Asset->GetPathName();
        TWeakObjectPtr<UObject> WeakAsset(Asset);

        // NOTE FOR FUTURE FIXES:
        // Do NOT call CloseAllEditorsForAsset() or FMessageDialog::Open() directly inside
        // AssetEditorSubsystem::OnAssetOpenedInEditor. UE4.27 editor toolkits and plugins
        // such as BlueprintAssist can still be finishing their own open callbacks; destroying
        // the asset window or blocking with a modal dialog in that same stack can leave invalid
        // shared pointers and trigger SharedPointer.h IsValid() asserts after the user clicks OK.
        // Always defer pending-restore lock closure through the core ticker and use a non-modal
        // notification/log message here.
        FTicker::GetCoreTicker().AddTicker(
            FTickerDelegate::CreateLambda([WeakAsset, AssetPathName, ManifestPath](float DeltaTime)
            {
                UObject* AssetToClose = WeakAsset.Get();
                if (!AssetToClose)
                {
                    AssetToClose = FindObject<UObject>(nullptr, *AssetPathName);
                }

                if (AssetToClose)
                {
                    CloseEditorsForAssetSafe(AssetToClose);
                }

                const FString Notice = FString::Printf(
                    TEXT("N2C pending restore lock: %s is queued for restore. Restart UE before opening it."),
                    *AssetPathName);
                ShowN2CNotification(Notice, SNotificationItem::CS_Fail);
                FN2CLogger::Get().LogWarning(FString::Printf(
                    TEXT("N2C pending restore blocked asset open. Asset=%s Manifest=%s"),
                    *AssetPathName,
                    *ManifestPath));

                return false;
            }),
            0.1f);
    }

    static bool ApplyPendingRestoreManifest(const FString& ManifestPath, FString& OutReport)
    {
        OutReport.Empty();

        TMap<FString, FString> Fields;
        FString Error;
        if (!LoadPendingRestoreManifest(ManifestPath, Fields, Error))
        {
            OutReport = FString::Printf(TEXT("ERROR: %s"), *Error);
            return false;
        }

        const FString TargetFilename = Fields.FindRef(TEXT("TargetFilename"));
        const FString PendingBackupCopy = Fields.FindRef(TEXT("PendingBackupCopy"));
        const FString OriginalBackupPath = Fields.FindRef(TEXT("OriginalBackupPath"));
        const FString RollbackPath = Fields.FindRef(TEXT("RollbackPath"));
        const FString PackageName = Fields.FindRef(TEXT("PackageName"));

        IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
        if (!PlatformFile.FileExists(*PendingBackupCopy))
        {
            OutReport = FString::Printf(TEXT("ERROR: pending restore backup copy is missing for %s: %s"), *PackageName, *PendingBackupCopy) + LINE_TERMINATOR + BuildPendingRestoreDiagnostics(ManifestPath, Fields);
            return false;
        }

        PlatformFile.SetReadOnly(*TargetFilename, false);

        if (PlatformFile.FileExists(*TargetFilename))
        {
            if (!PlatformFile.DeleteFile(*TargetFilename))
            {
                OutReport = FString::Printf(
                    TEXT("ERROR: pending restore could not delete target file for %s. Close every UE instance and retry by restarting the editor.%s%s"),
                    *PackageName,
                    LINE_TERMINATOR,
                    *BuildPendingRestoreDiagnostics(ManifestPath, Fields));
                return false;
            }
        }

        if (!PlatformFile.CopyFile(*TargetFilename, *PendingBackupCopy))
        {
            if (!RollbackPath.IsEmpty() && PlatformFile.FileExists(*RollbackPath) && !PlatformFile.FileExists(*TargetFilename))
            {
                PlatformFile.CopyFile(*TargetFilename, *RollbackPath);
            }

            OutReport = FString::Printf(
                TEXT("ERROR: pending restore could not copy backup to target for %s. Target: %s Backup copy: %s Original backup: %s%s%s"),
                *PackageName,
                *TargetFilename,
                *PendingBackupCopy,
                *OriginalBackupPath,
                LINE_TERMINATOR,
                *BuildPendingRestoreDiagnostics(ManifestPath, Fields));
            return false;
        }

        TArray<FString> FilesToScan;
        FilesToScan.Add(TargetFilename);
        FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        AssetRegistryModule.Get().ScanFilesSynchronous(FilesToScan, true);

        const FString AppliedDir = GetPendingRestoreAppliedDir();
        IFileManager::Get().MakeDirectory(*AppliedDir, true);
        const FString AppliedManifest = AppliedDir / (FPaths::GetBaseFilename(ManifestPath) + TEXT(".done"));
        IFileManager::Get().Move(*AppliedManifest, *ManifestPath, true, true, true, true);

        OutReport = FString::Printf(
            TEXT("OK: pending restore applied for %s. Target: %s Backup: %s"),
            *PackageName,
            *TargetFilename,
            *OriginalBackupPath);
        return true;
    }

    static FString GetPersistentPendingRestoreStatusPath()
    {
        return GetPendingRestoreAppliedDir() / TEXT("N2C_LAST_PENDING_RESTORE_STATUS.txt");
    }

    static bool LoadPendingRestorePersistentStatus(FString& OutStatus, bool& bOutShowOnNextStartup)
    {
        OutStatus.Empty();
        bOutShowOnNextStartup = false;
        const FString StatusPath = GetPersistentPendingRestoreStatusPath();
        if (!FFileHelper::LoadFileToString(OutStatus, *StatusPath))
        {
            return false;
        }
        bOutShowOnNextStartup = OutStatus.Contains(TEXT("ShowOnNextStartup=1"));
        return true;
    }

    static void AcknowledgePendingRestorePersistentStatus()
    {
        const FString StatusPath = GetPersistentPendingRestoreStatusPath();
        FString Status;
        if (FFileHelper::LoadFileToString(Status, *StatusPath))
        {
            Status.ReplaceInline(TEXT("ShowOnNextStartup=1"), TEXT("ShowOnNextStartup=0"));
            FFileHelper::SaveStringToFile(Status, *StatusPath);
        }
    }

    struct FN2CStartupRestoreState
    {
        int32 AppliedCount = 0;
        int32 FailedCount = 0;
        FString Report;
        FString ReportPath;
        bool bHadWork = false;
    };

    static FN2CStartupRestoreState GStartupRestoreState;

    static void ProcessPendingBackupRestores(bool bShowOnNextStartup = false)
    {
        TArray<FString> ManifestFiles;
        GetPendingRestoreManifests(ManifestFiles);
        if (ManifestFiles.Num() <= 0)
        {
            return;
        }

        GStartupRestoreState.bHadWork = true;
        FString Report;
        int32 AppliedCount = 0;
        int32 FailedCount = 0;
        for (const FString& ManifestPath : ManifestFiles)
        {
            FString ItemReport;
            if (ApplyPendingRestoreManifest(ManifestPath, ItemReport))
            {
                ++AppliedCount;
                FN2CLogger::Get().Log(ItemReport, EN2CLogSeverity::Info);
            }
            else
            {
                ++FailedCount;
                FN2CLogger::Get().Log(ItemReport, EN2CLogSeverity::Error);
            }
            Report += ItemReport + LINE_TERMINATOR;
        }

        if (AppliedCount > 0 || FailedCount > 0)
        {
            GStartupRestoreState.AppliedCount += AppliedCount;
            GStartupRestoreState.FailedCount += FailedCount;
            GStartupRestoreState.Report += Report;

            const FString ReportDir = GetPendingRestoreAppliedDir();
            IFileManager::Get().MakeDirectory(*ReportDir, true);
            const FString StartupReportPath = ReportDir / FString::Printf(TEXT("N2C_pending_restore_startup_report_%s.txt"), *MakeExportTimestamp());
            FFileHelper::SaveStringToFile(Report, *StartupReportPath);
            GStartupRestoreState.ReportPath = StartupReportPath;

            TArray<FString> RemainingManifests;
            GetPendingRestoreManifests(RemainingManifests);
            const bool bFinalSuccess = RemainingManifests.Num() == 0 && GStartupRestoreState.AppliedCount > 0;
            const FString PersistentStatus = FString::Printf(
                TEXT("FinalResult=%s%sApplied=%d%sFailedAttempts=%d%sPending=%d%sShowOnNextStartup=%d%sReport=%s%s%s"),
                bFinalSuccess ? TEXT("PASS") : TEXT("FAIL"), LINE_TERMINATOR,
                GStartupRestoreState.AppliedCount, LINE_TERMINATOR,
                GStartupRestoreState.FailedCount, LINE_TERMINATOR,
                RemainingManifests.Num(), LINE_TERMINATOR,
                bShowOnNextStartup ? 1 : 0, LINE_TERMINATOR,
                *StartupReportPath, LINE_TERMINATOR,
                *GStartupRestoreState.Report);
            FFileHelper::SaveStringToFile(PersistentStatus, *GetPersistentPendingRestoreStatusPath());
        }
    }

    static void SchedulePendingRestoreStartupSummary()
    {
        FTicker::GetCoreTicker().AddTicker(
            FTickerDelegate::CreateLambda([](float DeltaTime)
            {
                // Retry once after the editor has finished startup. The first pass can
                // run before every package/registry subsystem is settled on some UE4.27
                // projects. Pending manifests remain locked until this retry succeeds.
                TArray<FString> RemainingBeforeRetry;
                GetPendingRestoreManifests(RemainingBeforeRetry);
                if (RemainingBeforeRetry.Num() > 0)
                {
                    ProcessPendingBackupRestores();
                }

                TArray<FString> RemainingAfterRetry;
                GetPendingRestoreManifests(RemainingAfterRetry);

                FString PreviousStatus;
                bool bShowPreviousStatus = false;
                LoadPendingRestorePersistentStatus(PreviousStatus, bShowPreviousStatus);
                if (!GStartupRestoreState.bHadWork && RemainingAfterRetry.Num() == 0 && !bShowPreviousStatus)
                {
                    return false;
                }

                const bool bFinalSuccess = RemainingAfterRetry.Num() == 0 &&
                    (GStartupRestoreState.AppliedCount > 0 || PreviousStatus.Contains(TEXT("FinalResult=PASS")));
                const FString Result = bFinalSuccess ? TEXT("OK") : TEXT("NOT OK");
                FString Message = FString::Printf(
                    TEXT("%s%s%sN2C deferred restore startup result%s%sApplied this startup: %d%sFailed attempts: %d%sStill pending: %d%s"),
                    *Result, LINE_TERMINATOR, LINE_TERMINATOR, LINE_TERMINATOR, LINE_TERMINATOR,
                    GStartupRestoreState.AppliedCount, LINE_TERMINATOR,
                    GStartupRestoreState.FailedCount, LINE_TERMINATOR,
                    RemainingAfterRetry.Num(), LINE_TERMINATOR);
                if (!GStartupRestoreState.ReportPath.IsEmpty())
                {
                    Message += FString::Printf(TEXT("Report: %s%s"), *GStartupRestoreState.ReportPath, LINE_TERMINATOR);
                }
                else if (bShowPreviousStatus)
                {
                    Message += FString::Printf(TEXT("Previous shutdown restore status:%s%s%s"), LINE_TERMINATOR, *PreviousStatus, LINE_TERMINATOR);
                }
                Message += bFinalSuccess
                    ? TEXT("The queued backup restore has been processed. You may open the asset now.")
                    : TEXT("The restore is still pending or failed. Do not save or open the affected asset. Close other UE instances and restart again; use N2C Pending Restore Status for exact paths.");

                if (IsRunningCommandlet() || FApp::IsUnattended())
                {
                    FN2CLogger::Get().Log(Message, bFinalSuccess ? EN2CLogSeverity::Info : EN2CLogSeverity::Error);
                    return false;
                }

                if (!FSlateApplication::IsInitialized())
                {
                    return true;
                }

                ShowN2CNotification(
                    FString::Printf(TEXT("N2C restore: %d applied, %d failed attempts, %d pending"),
                        GStartupRestoreState.AppliedCount,
                        GStartupRestoreState.FailedCount,
                        RemainingAfterRetry.Num()),
                    bFinalSuccess ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
                FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Message));
                if (bShowPreviousStatus)
                {
                    AcknowledgePendingRestorePersistentStatus();
                }
                return false;
            }),
            1.5f);
    }

    static bool QueueBackupRestoreToAsset(
        UObject* Asset,
        const FString& BackupPath,
        bool bRequireConfirmation,
        FString* OutManifestPath,
        FString& OutReport)
    {
        OutReport.Empty();
        if (!Asset)
        {
            OutReport = TEXT("ERROR: invalid target asset.");
            return false;
        }
        if (!FPaths::FileExists(BackupPath))
        {
            OutReport = FString::Printf(TEXT("ERROR: backup file does not exist: %s"), *BackupPath);
            return false;
        }

        UPackage* TargetPackage = Asset->GetOutermost();
        if (!TargetPackage)
        {
            OutReport = TEXT("ERROR: invalid target package.");
            return false;
        }

        const FString AssetPathName = Asset->GetPathName();
        const FString PackageName = TargetPackage->GetName();

        FString TargetFilename;
        FString Error;
        if (!GetAssetPackageFilename(Asset, TargetFilename, Error))
        {
            OutReport = FString::Printf(TEXT("ERROR: cannot restore backup: %s"), *Error);
            return false;
        }

        if (bRequireConfirmation)
        {
            const EAppReturnType::Type Confirm = FMessageDialog::Open(
                EAppMsgType::YesNo,
                FText::FromString(FString::Printf(
                    TEXT("Queue N2C backup restore?\n\nAsset:\n%s\n\nBackup:\n%s\n\nThe restore will be applied on the next UE editor startup before the asset is opened. This avoids unsafe live-unload crashes for Blueprints with broken graph links.\n\nAfter clicking Yes, close UE and reopen the project."),
                    *AssetPathName, *BackupPath))
            );
            if (Confirm != EAppReturnType::Yes)
            {
                OutReport = TEXT("Restore cancelled by user.");
                return false;
            }
        }

        IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
        PlatformFile.SetReadOnly(*TargetFilename, false);

        const FString PendingDir = GetPendingRestoreDir();
        const FString RollbackDir = GetRestoreRollbackDir();
        IFileManager::Get().MakeDirectory(*PendingDir, true);
        IFileManager::Get().MakeDirectory(*RollbackDir, true);

        const FString SafePackageName = SanitizeForFileName(PackageName);
        const FString SafeObjectName = SanitizeForFileName(Asset->GetName());
        const FString Timestamp = MakeExportTimestamp();
        const FString Extension = FPaths::GetExtension(TargetFilename, true);

        const FString PendingBackupCopy = PendingDir / FString::Printf(
            TEXT("N2C_%s_%s_pending_restore_backup%s"),
            *SafeObjectName,
            *Timestamp,
            *Extension);

        const FString RollbackPath = RollbackDir / FString::Printf(
            TEXT("N2C_%s_%s_before_queued_restore%s"),
            *SafeObjectName,
            *Timestamp,
            *Extension);

        bool bRollbackSaved = false;
        if (PlatformFile.FileExists(*TargetFilename))
        {
            bRollbackSaved = PlatformFile.CopyFile(*RollbackPath, *TargetFilename);
        }

        if (!PlatformFile.CopyFile(*PendingBackupCopy, *BackupPath))
        {
            OutReport = FString::Printf(
                TEXT("ERROR: failed to copy selected backup into pending restore folder. Backup: %s Pending copy: %s%s%s"),
                *BackupPath,
                *PendingBackupCopy,
                bRollbackSaved ? LINE_TERMINATOR TEXT("Current file rollback copy: ") : TEXT(""),
                bRollbackSaved ? *RollbackPath : TEXT(""));
            return false;
        }

        const FString ManifestPath = PendingDir / FString::Printf(TEXT("%s__%s.restore"), *SafePackageName, *Timestamp);
        const FString ManifestText = MakePendingRestoreManifestText(
            TargetFilename,
            PendingBackupCopy,
            BackupPath,
            bRollbackSaved ? RollbackPath : TEXT(""),
            PackageName,
            AssetPathName);

        if (!FFileHelper::SaveStringToFile(ManifestText, *ManifestPath))
        {
            OutReport = FString::Printf(
                TEXT("ERROR: failed to write pending restore manifest: %s"),
                *ManifestPath);
            return false;
        }
        if (OutManifestPath)
        {
            *OutManifestPath = ManifestPath;
        }

        // Fool-proof mode: after restore is queued, close the editor for this asset immediately.
        // Until UE restarts and applies the pending restore, HandleAssetEditorOpened will close it again if the user reopens it.
        CloseEditorsForAssetSafe(Asset);

        OutReport = FString::Printf(
            TEXT("OK: N2C backup restore was queued.\n\nTarget: %s\nBackup: %s\nPending manifest: %s\n\nThe asset editor was closed and this asset is now locked by N2C pending restore. Close UE and reopen the project. The plugin will apply this restore on startup before the asset is opened.%s%s"),
            *TargetFilename,
            *BackupPath,
            *ManifestPath,
            bRollbackSaved ? LINE_TERMINATOR TEXT("Current pre-restore file rollback: ") : TEXT(""),
            bRollbackSaved ? *RollbackPath : TEXT(""));
        return true;
    }

    static bool RestoreBackupFileToAsset(UObject* Asset, const FString& BackupPath, FString& OutReport)
    {
        return QueueBackupRestoreToAsset(Asset, BackupPath, true, nullptr, OutReport);
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

    static bool NormalizeContentBrowserPackagePath(FString InPath, FString& OutPath)
    {
        InPath.TrimStartAndEndInline();
        if (InPath.IsEmpty())
        {
            return false;
        }

        InPath.ReplaceInline(TEXT("\\"), TEXT("/"));
        if (InPath.StartsWith(TEXT("Content/")))
        {
            InPath = FString(TEXT("/Game/")) + InPath.RightChop(8);
        }
        else if (InPath == TEXT("Content"))
        {
            InPath = TEXT("/Game");
        }

        if (!InPath.StartsWith(TEXT("/")))
        {
            InPath = FString(TEXT("/")) + InPath;
        }

        OutPath = InPath;
        return true;
    }

    static void GetAssetsUnderPaths(const TArray<FString>& PackagePaths, TArray<FAssetData>& OutAssets)
    {
        OutAssets.Empty();
        if (PackagePaths.Num() == 0)
        {
            return;
        }

        FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        AssetRegistryModule.Get().SearchAllAssets(true);

        FARFilter Filter;
        for (const FString& RawPackagePath : PackagePaths)
        {
            FString NormalizedPath;
            if (NormalizeContentBrowserPackagePath(RawPackagePath, NormalizedPath))
            {
                Filter.PackagePaths.AddUnique(FName(*NormalizedPath));
            }
        }

        if (Filter.PackagePaths.Num() == 0)
        {
            return;
        }

        Filter.bRecursivePaths = true;
        Filter.bRecursiveClasses = true;

        AssetRegistryModule.Get().GetAssets(Filter, OutAssets);
        OutAssets.Sort([](const FAssetData& A, const FAssetData& B)
        {
            return A.ObjectPath.ToString() < B.ObjectPath.ToString();
        });
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


    static void GetNiagaraAssetsUnderPaths(const TArray<FString>& PackagePaths, TArray<FAssetData>& OutNiagaraAssets)
    {
        OutNiagaraAssets.Empty();
        if (PackagePaths.Num() == 0)
        {
            return;
        }

        FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        AssetRegistryModule.Get().SearchAllAssets(true);

        FARFilter Filter;
        for (FString PackagePath : PackagePaths)
        {
            PackagePath.TrimStartAndEndInline();
            if (PackagePath.IsEmpty())
            {
                continue;
            }

            // Content Browser path-view extenders normally provide paths like /Game/Foo.
            // Keep a small normalization fallback for copied paths like Content/Foo.
            PackagePath.ReplaceInline(TEXT("\\"), TEXT("/"));
            if (PackagePath.StartsWith(TEXT("Content/")))
            {
                PackagePath = FString(TEXT("/Game/")) + PackagePath.RightChop(8);
            }
            else if (PackagePath == TEXT("Content"))
            {
                PackagePath = TEXT("/Game");
            }

            if (!PackagePath.StartsWith(TEXT("/")))
            {
                PackagePath = FString(TEXT("/")) + PackagePath;
            }

            Filter.PackagePaths.AddUnique(FName(*PackagePath));
        }

        if (Filter.PackagePaths.Num() == 0)
        {
            return;
        }

        // Export Niagara Systems from selected folders recursively.  Other Niagara helper
        // assets/scripts may exist next to the system, but N2C JSON export is centered on
        // UNiagaraSystem assets that can be opened/tested as final effects.
        Filter.ClassNames.Add(UNiagaraSystem::StaticClass()->GetFName());
        Filter.bRecursivePaths = true;
        Filter.bRecursiveClasses = true;

        AssetRegistryModule.Get().GetAssets(Filter, OutNiagaraAssets);
        OutNiagaraAssets.Sort([](const FAssetData& A, const FAssetData& B)
        {
            return A.ObjectPath.ToString() < B.ObjectPath.ToString();
        });
    }

    static bool IsBlueprintAssetData(const FAssetData& AssetData)
    {
        UClass* AssetClass = AssetData.GetClass();
        const bool bClassLooksLikeBlueprint = AssetClass && AssetClass->IsChildOf(UBlueprint::StaticClass());
        const FString AssetClassName = AssetData.AssetClass.ToString();
        const bool bNameLooksLikeBlueprint = AssetClassName == TEXT("Blueprint") || AssetClassName.EndsWith(TEXT("Blueprint"));
        return bClassLooksLikeBlueprint || bNameLooksLikeBlueprint;
    }

    static bool IsNiagaraAssetData(const FAssetData& AssetData)
    {
        UClass* AssetClass = AssetData.GetClass();
        const bool bClassLooksLikeNiagaraSystem = AssetClass && AssetClass->IsChildOf(UNiagaraSystem::StaticClass());
        const FString AssetClassName = AssetData.AssetClass.ToString();
        const bool bNameLooksLikeNiagaraSystem = AssetClassName == TEXT("NiagaraSystem");
        return bClassLooksLikeNiagaraSystem || bNameLooksLikeNiagaraSystem;
    }

    static bool IsEnumAssetData(const FAssetData& AssetData)
    {
        UClass* AssetClass = AssetData.GetClass();
        const bool bClassLooksLikeEnum = AssetClass && AssetClass->IsChildOf(UEnum::StaticClass());
        const FString AssetClassName = AssetData.AssetClass.ToString();
        const bool bNameLooksLikeEnum = AssetClassName == TEXT("UserDefinedEnum") || AssetClassName == TEXT("Enum") || AssetClassName.EndsWith(TEXT("Enum"));
        return bClassLooksLikeEnum || bNameLooksLikeEnum;
    }

    static bool IsStructAssetData(const FAssetData& AssetData)
    {
        UClass* AssetClass = AssetData.GetClass();
        const bool bClassLooksLikeStruct = AssetClass && AssetClass->IsChildOf(UScriptStruct::StaticClass());
        const FString AssetClassName = AssetData.AssetClass.ToString();
        const bool bNameLooksLikeStruct = AssetClassName == TEXT("UserDefinedStruct") || AssetClassName == TEXT("ScriptStruct") || AssetClassName.EndsWith(TEXT("Struct"));
        return bClassLooksLikeStruct || bNameLooksLikeStruct;
    }

    static bool IsNiagaraObjectAsset(const UObject* Asset)
    {
        return Asset && Asset->IsA<UNiagaraSystem>();
    }

    static bool IsEnumObjectAsset(const UObject* Asset)
    {
        return Asset && Asset->IsA<UEnum>();
    }

    static bool IsStructObjectAsset(const UObject* Asset)
    {
        return Asset && Asset->IsA<UScriptStruct>();
    }

    static int32 CountNiagaraAssets(const TArray<FAssetData>& Assets)
    {
        int32 Count = 0;
        for (const FAssetData& AssetData : Assets)
        {
            if (IsNiagaraAssetData(AssetData))
            {
                ++Count;
            }
        }
        return Count;
    }

    static int32 CountEnumAssets(const TArray<FAssetData>& Assets)
    {
        int32 Count = 0;
        for (const FAssetData& AssetData : Assets)
        {
            if (IsEnumAssetData(AssetData))
            {
                ++Count;
            }
        }
        return Count;
    }

    static int32 CountStructAssets(const TArray<FAssetData>& Assets)
    {
        int32 Count = 0;
        for (const FAssetData& AssetData : Assets)
        {
            if (IsStructAssetData(AssetData))
            {
                ++Count;
            }
        }
        return Count;
    }


    struct FN2CExportKindSelection
    {
        bool bBlueprint = false;
        bool bNiagaraSystem = false;
        bool bEnum = false;
        bool bStruct = false;

        bool HasAnySelected() const
        {
            return bBlueprint || bNiagaraSystem || bEnum || bStruct;
        }
    };

    static void AddExportKindCheckBox(TSharedRef<SVerticalBox> Box, const FString& Label, const FString& Tooltip, bool bAvailable, bool& bValue)
    {
        if (!bAvailable)
        {
            bValue = false;
        }

        bool* ValuePtr = &bValue;

        Box->AddSlot()
        .AutoHeight()
        .Padding(0.0f, 4.0f)
        [
            SNew(SCheckBox)
            .IsEnabled(bAvailable)
            .IsChecked(bValue ? ECheckBoxState::Checked : ECheckBoxState::Unchecked)
            .ToolTipText(FText::FromString(Tooltip))
            .OnCheckStateChanged_Lambda([ValuePtr](ECheckBoxState NewState)
            {
                if (ValuePtr)
                {
                    *ValuePtr = (NewState == ECheckBoxState::Checked);
                }
            })
            [
                SNew(STextBlock)
                .Text(FText::FromString(bAvailable ? Label : (Label + TEXT(" (not found)"))))
            ]
        ];
    }

    static bool PickExportKindsForN2C(const FString& WindowTitle, const FString& Description, const FN2CExportKindSelection& AvailableKinds, FN2CExportKindSelection& InOutSelection)
    {
        bool bConfirmed = false;
        TSharedPtr<SWindow> PickerWindow;

        TSharedRef<SVerticalBox> KindBox = SNew(SVerticalBox);
        AddExportKindCheckBox(KindBox, TEXT("Blueprint"), TEXT("Export Blueprint assets."), AvailableKinds.bBlueprint, InOutSelection.bBlueprint);
        AddExportKindCheckBox(KindBox, TEXT("Niagara System"), TEXT("Export Niagara System assets only."), AvailableKinds.bNiagaraSystem, InOutSelection.bNiagaraSystem);
        AddExportKindCheckBox(KindBox, TEXT("Enum"), TEXT("Export User Defined Enum assets."), AvailableKinds.bEnum, InOutSelection.bEnum);
        AddExportKindCheckBox(KindBox, TEXT("Struct"), TEXT("Export User Defined Struct assets."), AvailableKinds.bStruct, InOutSelection.bStruct);

        PickerWindow = SNew(SWindow)
            .Title(FText::FromString(WindowTitle))
            .ClientSize(FVector2D(460.0f, 260.0f))
            .SupportsMinimize(false)
            .SupportsMaximize(false)
            [
                SNew(SVerticalBox)
                + SVerticalBox::Slot()
                .AutoHeight()
                .Padding(12.0f, 10.0f, 12.0f, 8.0f)
                [
                    SNew(STextBlock)
                    .Text(FText::FromString(Description))
                    .AutoWrapText(true)
                ]
                + SVerticalBox::Slot()
                .FillHeight(1.0f)
                .Padding(18.0f, 4.0f, 12.0f, 4.0f)
                [
                    KindBox
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
                        .Text(FText::FromString(TEXT("Choose at least one type.")))
                    ]
                    + SHorizontalBox::Slot()
                    .AutoWidth()
                    .Padding(4.0f, 0.0f)
                    [
                        SNew(SButton)
                        .Text(FText::FromString(TEXT("Export")))
                        .OnClicked_Lambda([&bConfirmed, &PickerWindow, &InOutSelection]()
                        {
                            if (!InOutSelection.HasAnySelected())
                            {
                                FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(TEXT("Select at least one asset type to export.")));
                                return FReply::Handled();
                            }
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
        return bConfirmed && InOutSelection.HasAnySelected();
    }

    static void FilterAssetsForN2CExportKinds(const TArray<FAssetData>& SourceAssets, const FN2CExportKindSelection& Selection, TArray<FAssetData>& OutBlueprintAssets, TArray<FAssetData>& OutNiagaraAssets, TArray<FAssetData>& OutEnumAssets, TArray<FAssetData>& OutStructAssets)
    {
        OutBlueprintAssets.Empty();
        OutNiagaraAssets.Empty();
        OutEnumAssets.Empty();
        OutStructAssets.Empty();

        for (const FAssetData& AssetData : SourceAssets)
        {
            if (Selection.bBlueprint && IsBlueprintAssetData(AssetData))
            {
                OutBlueprintAssets.Add(AssetData);
            }
            else if (Selection.bNiagaraSystem && IsNiagaraAssetData(AssetData))
            {
                OutNiagaraAssets.Add(AssetData);
            }
            else if (Selection.bEnum && IsEnumAssetData(AssetData))
            {
                OutEnumAssets.Add(AssetData);
            }
            else if (Selection.bStruct && IsStructAssetData(AssetData))
            {
                OutStructAssets.Add(AssetData);
            }
        }
    }

    static FN2CExportKindSelection MakeExportAvailabilityFromAssets(const TArray<FAssetData>& Assets)
    {
        FN2CExportKindSelection Availability;
        for (const FAssetData& AssetData : Assets)
        {
            Availability.bBlueprint |= IsBlueprintAssetData(AssetData);
            Availability.bNiagaraSystem |= IsNiagaraAssetData(AssetData);
            Availability.bEnum |= IsEnumAssetData(AssetData);
            Availability.bStruct |= IsStructAssetData(AssetData);
        }
        return Availability;
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


    static void DeleteExportWorkDirAfterZip(const FString& ExportRootDir, const FString& ZipPath, FString& Report)
    {
        if (ExportRootDir.IsEmpty() || ZipPath.IsEmpty())
        {
            return;
        }
        if (!FPaths::FileExists(ZipPath))
        {
            Report += FString::Printf(TEXT("WARNING: export work directory kept because ZIP was not created: %s"), *ExportRootDir) + LINE_TERMINATOR;
            return;
        }
        if (!FPaths::DirectoryExists(ExportRootDir))
        {
            return;
        }
        if (IFileManager::Get().DeleteDirectory(*ExportRootDir, false, true))
        {
            Report += FString::Printf(TEXT("Cleaned export work directory after ZIP: %s"), *ExportRootDir) + LINE_TERMINATOR;
        }
        else
        {
            Report += FString::Printf(TEXT("WARNING: could not delete export work directory after ZIP: %s"), *ExportRootDir) + LINE_TERMINATOR;
        }
    }

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
        int32 FunctionIndex = 0;
        for (const TSharedPtr<FJsonValue>& Value : *Functions)
        {
            ++FunctionIndex;
            TSharedPtr<FJsonObject> FunctionObj = Value.IsValid() ? Value->AsObject() : nullptr;
            if (!FunctionObj.IsValid())
            {
                continue;
            }

            FString FunctionName;
            if (!FunctionObj->TryGetStringField(TEXT("name"), FunctionName) || FunctionName.IsEmpty())
            {
                FunctionName = FString::Printf(TEXT("Function_%d"), FunctionIndex);
            }

            TSharedPtr<FJsonObject> Wrapper = MakeShared<FJsonObject>();
            Wrapper->SetStringField(TEXT("schema"), TEXT("N2C_AI_EXPORT_V2_FUNCTION"));
            Wrapper->SetObjectField(TEXT("function"), FunctionObj);

            FString FunctionJson;
            TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&FunctionJson);
            FJsonSerializer::Serialize(Wrapper.ToSharedRef(), Writer);

            FString Error;
            const FString FilePath = FunctionsDir / FString::Printf(TEXT("%03d_%s.json"), FunctionIndex, *SanitizeForFileName(FunctionName));
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

    static bool PickNiagaraImportFile(FString& OutPath)
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
            TEXT("Import N2C ZIP/JSON"),
            FPaths::ProjectSavedDir() / TEXT("NodeToCode/ProjectExports"),
            TEXT(""),
            TEXT("N2C files (*.zip;*.json)|*.zip;*.json|N2C ZIP archives (*.zip)|*.zip|JSON files (*.json)|*.json|All files (*.*)|*.*"),
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

    static uint16 ReadUInt16LE(const TArray<uint8>& Bytes, int64 Offset)
    {
        if (!Bytes.IsValidIndex(Offset + 1))
        {
            return 0;
        }
        return static_cast<uint16>(Bytes[Offset]) | (static_cast<uint16>(Bytes[Offset + 1]) << 8);
    }

    static uint32 ReadUInt32LE(const TArray<uint8>& Bytes, int64 Offset)
    {
        if (!Bytes.IsValidIndex(Offset + 3))
        {
            return 0;
        }
        return static_cast<uint32>(Bytes[Offset]) |
            (static_cast<uint32>(Bytes[Offset + 1]) << 8) |
            (static_cast<uint32>(Bytes[Offset + 2]) << 16) |
            (static_cast<uint32>(Bytes[Offset + 3]) << 24);
    }

    static FString Utf8BytesToString(const uint8* Data, int32 NumBytes)
    {
        if (!Data || NumBytes <= 0)
        {
            return TEXT("");
        }
        FUTF8ToTCHAR Converter(reinterpret_cast<const ANSICHAR*>(Data), NumBytes);
        return FString(Converter.Length(), Converter.Get());
    }

    static bool ExtractStoredZipEntries(const FString& ZipPath, TMap<FString, TArray<uint8>>& OutEntries, FString& OutError)
    {
        OutEntries.Empty();
        OutError.Empty();

        TArray<uint8> ZipBytes;
        if (!FFileHelper::LoadFileToArray(ZipBytes, *ZipPath))
        {
            OutError = FString::Printf(TEXT("Could not read ZIP file: %s"), *ZipPath);
            return false;
        }

        int64 Offset = 0;
        while (Offset + 4 <= ZipBytes.Num())
        {
            const uint32 Signature = ReadUInt32LE(ZipBytes, Offset);
            if (Signature == 0x02014b50u || Signature == 0x06054b50u)
            {
                break; // central directory / end of central directory
            }
            if (Signature != 0x04034b50u)
            {
                OutError = FString::Printf(TEXT("Unsupported ZIP layout at byte %lld. Only N2C store-method ZIP archives are supported."), Offset);
                return false;
            }
            if (Offset + 30 > ZipBytes.Num())
            {
                OutError = TEXT("Invalid ZIP local file header.");
                return false;
            }

            const uint16 Flags = ReadUInt16LE(ZipBytes, Offset + 6);
            const uint16 Method = ReadUInt16LE(ZipBytes, Offset + 8);
            const uint32 CompressedSize = ReadUInt32LE(ZipBytes, Offset + 18);
            const uint32 UncompressedSize = ReadUInt32LE(ZipBytes, Offset + 22);
            const uint16 NameLen = ReadUInt16LE(ZipBytes, Offset + 26);
            const uint16 ExtraLen = ReadUInt16LE(ZipBytes, Offset + 28);

            if ((Flags & 0x0008u) != 0)
            {
                OutError = TEXT("Unsupported ZIP data descriptor flag. Use N2C-exported ZIP archives or import the JSON file directly.");
                return false;
            }
            if (Method != 0)
            {
                OutError = FString::Printf(TEXT("Unsupported ZIP compression method %d. N2C ZIP import currently supports store/no-compression archives only."), static_cast<int32>(Method));
                return false;
            }
            if (CompressedSize != UncompressedSize)
            {
                OutError = TEXT("Unsupported ZIP entry: compressed/uncompressed sizes differ for a store-method entry.");
                return false;
            }

            const int64 NameOffset = Offset + 30;
            const int64 DataOffset = NameOffset + NameLen + ExtraLen;
            const int64 DataEnd = DataOffset + CompressedSize;
            if (NameLen <= 0 || DataOffset < 0 || DataEnd > ZipBytes.Num())
            {
                OutError = TEXT("Invalid ZIP entry offsets.");
                return false;
            }

            FString EntryName = Utf8BytesToString(ZipBytes.GetData() + NameOffset, NameLen);
            EntryName.ReplaceInline(TEXT("\\"), TEXT("/"));

            TArray<uint8> EntryData;
            EntryData.Append(ZipBytes.GetData() + DataOffset, static_cast<int32>(CompressedSize));
            OutEntries.Add(EntryName, MoveTemp(EntryData));

            Offset = DataEnd;
        }

        if (OutEntries.Num() <= 0)
        {
            OutError = TEXT("ZIP archive did not contain any readable entries.");
            return false;
        }
        return true;
    }

    static bool LoadNiagaraJsonFromZip(const FString& ZipPath, FString& OutJson, FString& OutJsonEntry, FString& OutError)
    {
        OutJson.Empty();
        OutJsonEntry.Empty();
        OutError.Empty();

        TMap<FString, TArray<uint8>> Entries;
        if (!ExtractStoredZipEntries(ZipPath, Entries, OutError))
        {
            return false;
        }

        FString PreferredJsonEntry;
        if (const TArray<uint8>* ManifestBytes = Entries.Find(TEXT("N2C_PROJECT_MANIFEST.json")))
        {
            FString ManifestJson;
            FFileHelper::BufferToString(ManifestJson, ManifestBytes->GetData(), ManifestBytes->Num());
            TSharedPtr<FJsonObject> Manifest;
            if (TryParseJsonRootObject(ManifestJson, Manifest))
            {
                const TArray<TSharedPtr<FJsonValue>>* Assets = nullptr;
                if (Manifest->TryGetArrayField(TEXT("assets"), Assets))
                {
                    for (const TSharedPtr<FJsonValue>& AssetValue : *Assets)
                    {
                        TSharedPtr<FJsonObject> AssetObj = AssetValue.IsValid() ? AssetValue->AsObject() : nullptr;
                        FString JsonPath;
                        if (AssetObj.IsValid() && AssetObj->TryGetStringField(TEXT("json"), JsonPath) && Entries.Contains(JsonPath))
                        {
                            PreferredJsonEntry = JsonPath;
                            break;
                        }
                    }
                }
            }
        }

        if (PreferredJsonEntry.IsEmpty())
        {
            TArray<FString> JsonEntries;
            Entries.GetKeys(JsonEntries);
            JsonEntries.Sort();
            for (const FString& EntryName : JsonEntries)
            {
                if (EntryName.EndsWith(TEXT(".json")) && EntryName != TEXT("N2C_PROJECT_MANIFEST.json"))
                {
                    PreferredJsonEntry = EntryName;
                    break;
                }
            }
        }

        if (PreferredJsonEntry.IsEmpty())
        {
            OutError = TEXT("ZIP archive contains no Niagara asset JSON. Expected N2C_PROJECT_MANIFEST.json plus niagara/.../N2C_*.json.");
            return false;
        }

        const TArray<uint8>* JsonBytes = Entries.Find(PreferredJsonEntry);
        if (!JsonBytes)
        {
            OutError = FString::Printf(TEXT("Manifest points to missing JSON entry: %s"), *PreferredJsonEntry);
            return false;
        }

        FFileHelper::BufferToString(OutJson, JsonBytes->GetData(), JsonBytes->Num());
        OutJsonEntry = PreferredJsonEntry;
        return !OutJson.IsEmpty();
    }

    static bool LoadNiagaraImportJsonFromFile(const FString& ImportPath, FString& OutJson, FString& OutSourceLabel, FString& OutError)
    {
        OutJson.Empty();
        OutSourceLabel.Empty();
        OutError.Empty();

        const FString Extension = FPaths::GetExtension(ImportPath).ToLower();
        if (Extension == TEXT("zip"))
        {
            FString EntryName;
            if (!LoadNiagaraJsonFromZip(ImportPath, OutJson, EntryName, OutError))
            {
                return false;
            }
            OutSourceLabel = FString::Printf(TEXT("%s :: %s"), *ImportPath, *EntryName);
            return true;
        }

        if (!FFileHelper::LoadFileToString(OutJson, *ImportPath))
        {
            OutError = FString::Printf(TEXT("Could not read JSON file: %s"), *ImportPath);
            return false;
        }
        OutSourceLabel = ImportPath;
        return true;
    }

    static bool GetJsonNumberFieldAny(const TSharedPtr<FJsonObject>& Obj, const TCHAR* FieldName, double& OutValue)
    {
        return Obj.IsValid() && Obj->TryGetNumberField(FieldName, OutValue);
    }

    static bool GetJsonVectorField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* FieldName, TArray<double>& OutComponents)
    {
        OutComponents.Empty();
        if (!Obj.IsValid())
        {
            return false;
        }

        static const TCHAR* Names4[] = { TEXT("x"), TEXT("y"), TEXT("z"), TEXT("w") };
        static const TCHAR* ColorNames[] = { TEXT("r"), TEXT("g"), TEXT("b"), TEXT("a") };

        const bool bColor = FCString::Strcmp(FieldName, TEXT("linear_color")) == 0;
        const TCHAR** Names = bColor ? ColorNames : Names4;

        // Import JSON generated by the exporter uses nested typed objects, for example:
        //   { "linear_color": { "r": 1, "g": 0.2, "b": 0.05, "a": 1 } }
        // Hand-authored AI patches often use the shorter flat form:
        //   { "r": 1, "g": 0.2, "b": 0.05, "a": 1 }
        // Support both forms so colors/vectors do not silently stay at old values like A=0.
        TSharedPtr<FJsonObject> VecObj = Obj;
        if (Obj->HasTypedField<EJson::Object>(FieldName))
        {
            VecObj = Obj->GetObjectField(FieldName);
        }

        for (int32 Index = 0; Index < 4; ++Index)
        {
            double Component = 0.0;
            if (VecObj.IsValid() && VecObj->TryGetNumberField(Names[Index], Component))
            {
                OutComponents.Add(Component);
            }
            else if (Index < 2)
            {
                return false;
            }
        }
        return OutComponents.Num() >= 2;
    }

    template<typename T>
    static void MakeValueBytes(const T& Value, TArray<uint8>& OutBytes)
    {
        OutBytes.SetNumUninitialized(sizeof(T));
        FMemory::Memcpy(OutBytes.GetData(), &Value, sizeof(T));
    }

    static bool TryBuildNiagaraParameterBytes(const FNiagaraVariable& Variable, const TSharedPtr<FJsonObject>& ValueObj, TArray<uint8>& OutBytes, FString& OutReason)
    {
        OutBytes.Empty();
        OutReason.Empty();

        if (!Variable.IsValid())
        {
            OutReason = TEXT("invalid Niagara variable");
            return false;
        }
        if (!ValueObj.IsValid())
        {
            OutReason = TEXT("missing JSON value object");
            return false;
        }
        if (Variable.IsDataInterface() || Variable.IsUObject())
        {
            OutReason = TEXT("object/data-interface parameter import is not supported yet");
            return false;
        }

        const FNiagaraTypeDefinition& TypeDef = Variable.GetType();
        double NumberValue = 0.0;

        if (TypeDef == FNiagaraTypeDefinition::GetFloatDef())
        {
            if (!GetJsonNumberFieldAny(ValueObj, TEXT("float"), NumberValue))
            {
                FString Display;
                if (ValueObj->TryGetStringField(TEXT("display"), Display) || ValueObj->TryGetStringField(TEXT("as_string"), Display))
                {
                    NumberValue = FCString::Atod(*Display);
                }
                else
                {
                    OutReason = TEXT("missing float value");
                    return false;
                }
            }
            const float TypedValue = static_cast<float>(NumberValue);
            MakeValueBytes(TypedValue, OutBytes);
            return true;
        }

        if (TypeDef == FNiagaraTypeDefinition::GetIntDef())
        {
            if (!GetJsonNumberFieldAny(ValueObj, TEXT("int32"), NumberValue))
            {
                FString Display;
                if (ValueObj->TryGetStringField(TEXT("display"), Display) || ValueObj->TryGetStringField(TEXT("as_string"), Display))
                {
                    NumberValue = FCString::Atod(*Display);
                }
                else
                {
                    OutReason = TEXT("missing int32 value");
                    return false;
                }
            }
            const int32 TypedValue = static_cast<int32>(NumberValue);
            MakeValueBytes(TypedValue, OutBytes);
            return true;
        }

        if (TypeDef == FNiagaraTypeDefinition::GetBoolDef())
        {
            FNiagaraBool TypedValue;
            if (GetJsonNumberFieldAny(ValueObj, TEXT("raw_bool_int"), NumberValue))
            {
                TypedValue.SetRawValue(static_cast<int32>(NumberValue));
            }
            else
            {
                bool bValue = false;
                if (!ValueObj->TryGetBoolField(TEXT("bool"), bValue))
                {
                    FString Display;
                    if (ValueObj->TryGetStringField(TEXT("display"), Display) || ValueObj->TryGetStringField(TEXT("as_string"), Display))
                    {
                        bValue = Display.ToBool();
                    }
                    else
                    {
                        OutReason = TEXT("missing bool value");
                        return false;
                    }
                }
                TypedValue.SetValue(bValue);
            }
            MakeValueBytes(TypedValue, OutBytes);
            return true;
        }

        TArray<double> Components;
        if (TypeDef == FNiagaraTypeDefinition::GetVec2Def())
        {
            if (!GetJsonVectorField(ValueObj, TEXT("vector2"), Components))
            {
                OutReason = TEXT("missing vector2 value");
                return false;
            }
            const FVector2D TypedValue(static_cast<float>(Components[0]), static_cast<float>(Components[1]));
            MakeValueBytes(TypedValue, OutBytes);
            return true;
        }

        if (TypeDef == FNiagaraTypeDefinition::GetVec3Def())
        {
            if (!GetJsonVectorField(ValueObj, TEXT("vector3"), Components))
            {
                OutReason = TEXT("missing vector3 value");
                return false;
            }
            const FVector TypedValue(static_cast<float>(Components[0]), static_cast<float>(Components[1]), static_cast<float>(Components.Num() > 2 ? Components[2] : 0.0));
            MakeValueBytes(TypedValue, OutBytes);
            return true;
        }

        if (TypeDef == FNiagaraTypeDefinition::GetVec4Def())
        {
            if (!GetJsonVectorField(ValueObj, TEXT("vector4"), Components))
            {
                OutReason = TEXT("missing vector4 value");
                return false;
            }
            const FVector4 TypedValue(
                static_cast<float>(Components[0]),
                static_cast<float>(Components[1]),
                static_cast<float>(Components.Num() > 2 ? Components[2] : 0.0),
                static_cast<float>(Components.Num() > 3 ? Components[3] : 0.0));
            MakeValueBytes(TypedValue, OutBytes);
            return true;
        }

        if (TypeDef == FNiagaraTypeDefinition::GetColorDef())
        {
            if (!GetJsonVectorField(ValueObj, TEXT("linear_color"), Components))
            {
                OutReason = TEXT("missing linear_color value");
                return false;
            }
            const FLinearColor TypedValue(
                static_cast<float>(Components[0]),
                static_cast<float>(Components[1]),
                static_cast<float>(Components.Num() > 2 ? Components[2] : 0.0),
                static_cast<float>(Components.Num() > 3 ? Components[3] : 1.0));
            MakeValueBytes(TypedValue, OutBytes);
            return true;
        }

        if (TypeDef == FNiagaraTypeDefinition::GetQuatDef())
        {
            if (!GetJsonVectorField(ValueObj, TEXT("quat"), Components))
            {
                OutReason = TEXT("missing quat value");
                return false;
            }
            const FQuat TypedValue(
                static_cast<float>(Components[0]),
                static_cast<float>(Components[1]),
                static_cast<float>(Components.Num() > 2 ? Components[2] : 0.0),
                static_cast<float>(Components.Num() > 3 ? Components[3] : 1.0));
            MakeValueBytes(TypedValue, OutBytes);
            return true;
        }

        OutReason = FString::Printf(TEXT("unsupported Niagara value type: %s"), *TypeDef.GetName());
        return false;
    }

    static FString NiagaraValueDisplay(const TSharedPtr<FJsonObject>& ValueObj)
    {
        if (!ValueObj.IsValid())
        {
            return TEXT("<missing>");
        }
        FString Display;
        if (ValueObj->TryGetStringField(TEXT("display"), Display) || ValueObj->TryGetStringField(TEXT("as_string"), Display))
        {
            return Display;
        }
        double NumberValue = 0.0;
        if (ValueObj->TryGetNumberField(TEXT("float"), NumberValue) || ValueObj->TryGetNumberField(TEXT("int32"), NumberValue))
        {
            return FString::SanitizeFloat(NumberValue);
        }
        bool bBool = false;
        if (ValueObj->TryGetBoolField(TEXT("bool"), bBool))
        {
            return bBool ? TEXT("true") : TEXT("false");
        }
        return TEXT("<structured value>");
    }

    static void CollectNiagaraParameterJsonValuesRecursive(const TSharedPtr<FJsonObject>& Obj, TMap<FString, TSharedPtr<FJsonObject>>& OutValues)
    {
        if (!Obj.IsValid())
        {
            return;
        }

        FString FullName;
        if (Obj->TryGetStringField(TEXT("full_name"), FullName) && !FullName.IsEmpty() && Obj->HasTypedField<EJson::Object>(TEXT("value")))
        {
            OutValues.FindOrAdd(FullName) = Obj->GetObjectField(TEXT("value"));
        }

        if (Obj->HasTypedField<EJson::Object>(TEXT("variable")) && Obj->HasTypedField<EJson::Object>(TEXT("decoded_value")))
        {
            TSharedPtr<FJsonObject> VariableObj = Obj->GetObjectField(TEXT("variable"));
            FString VariableName;
            if (VariableObj.IsValid() && VariableObj->TryGetStringField(TEXT("name"), VariableName) && !VariableName.IsEmpty())
            {
                OutValues.FindOrAdd(VariableName) = Obj->GetObjectField(TEXT("decoded_value"));
            }
        }

        if (Obj->HasTypedField<EJson::Object>(TEXT("user_variable")) && Obj->HasTypedField<EJson::Object>(TEXT("decoded_value")))
        {
            TSharedPtr<FJsonObject> VariableObj = Obj->GetObjectField(TEXT("user_variable"));
            FString VariableName;
            if (VariableObj.IsValid() && VariableObj->TryGetStringField(TEXT("name"), VariableName) && !VariableName.IsEmpty())
            {
                OutValues.FindOrAdd(VariableName) = Obj->GetObjectField(TEXT("decoded_value"));
            }
        }

        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Obj->Values)
        {
            if (Pair.Key == TEXT("local_subobjects") || Pair.Key == TEXT("full_reflection"))
            {
                continue;
            }
            if (!Pair.Value.IsValid())
            {
                continue;
            }
            if (Pair.Value->Type == EJson::Object)
            {
                CollectNiagaraParameterJsonValuesRecursive(Pair.Value->AsObject(), OutValues);
            }
            else if (Pair.Value->Type == EJson::Array)
            {
                for (const TSharedPtr<FJsonValue>& ChildValue : Pair.Value->AsArray())
                {
                    if (ChildValue.IsValid() && ChildValue->Type == EJson::Object)
                    {
                        CollectNiagaraParameterJsonValuesRecursive(ChildValue->AsObject(), OutValues);
                    }
                }
            }
        }
    }

    static void CollectNiagaraExplicitParameterValueActions(const TSharedPtr<FJsonObject>& Actions, const TCHAR* ArrayName, TMap<FString, TSharedPtr<FJsonObject>>& OutValues)
    {
        if (!Actions.IsValid() || !ArrayName || !Actions->HasTypedField<EJson::Array>(ArrayName))
        {
            return;
        }

        for (const TSharedPtr<FJsonValue>& Value : Actions->GetArrayField(ArrayName))
        {
            TSharedPtr<FJsonObject> Action = Value.IsValid() ? Value->AsObject() : nullptr;
            if (!Action.IsValid())
            {
                continue;
            }

            FString Name;
            if ((!Action->TryGetStringField(TEXT("full_name"), Name) || Name.IsEmpty())
                && (!Action->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
                && (!Action->TryGetStringField(TEXT("parameter"), Name) || Name.IsEmpty()))
            {
                continue;
            }

            TSharedPtr<FJsonObject> ValueObj = nullptr;
            if (Action->HasTypedField<EJson::Object>(TEXT("value")))
            {
                ValueObj = Action->GetObjectField(TEXT("value"));
            }
            else if (Action->HasTypedField<EJson::Object>(TEXT("default_value")))
            {
                ValueObj = Action->GetObjectField(TEXT("default_value"));
            }
            else if (Action->HasTypedField<EJson::Object>(TEXT("decoded_value")))
            {
                ValueObj = Action->GetObjectField(TEXT("decoded_value"));
            }
            else
            {
                ValueObj = Action;
            }

            if (ValueObj.IsValid())
            {
                OutValues.FindOrAdd(Name) = ValueObj;
            }
        }
    }

    static void CollectNiagaraImportValues(const TSharedPtr<FJsonObject>& Root, TMap<FString, TSharedPtr<FJsonObject>>& OutValues)
    {
        OutValues.Empty();
        if (!Root.IsValid() || !Root->HasTypedField<EJson::Object>(TEXT("niagara_asset")))
        {
            return;
        }

        TSharedPtr<FJsonObject> NiagaraAsset = Root->GetObjectField(TEXT("niagara_asset"));
        if (!NiagaraAsset.IsValid())
        {
            return;
        }

        // v21: allow JSON to provide explicit RapidIteration/System parameter values in import_actions.
        // This is the safe visible fallback for simple constants; dynamic_input_tree is still used only
        // when an actual nested Niagara function tree is required.
        TSharedPtr<FJsonObject> Actions = GetNiagaraImportActionsObject(Root);
        CollectNiagaraExplicitParameterValueActions(Actions, TEXT("parameter_values"), OutValues);
        CollectNiagaraExplicitParameterValueActions(Actions, TEXT("rapid_iteration_parameters"), OutValues);
        CollectNiagaraExplicitParameterValueActions(Actions, TEXT("direct_parameter_values"), OutValues);

        if (!NiagaraAsset->HasTypedField<EJson::Object>(TEXT("niagara_summary")))
        {
            return;
        }

        TSharedPtr<FJsonObject> Summary = NiagaraAsset->GetObjectField(TEXT("niagara_summary"));
        if (Summary->HasTypedField<EJson::Object>(TEXT("readable_stack")))
        {
            CollectNiagaraParameterJsonValuesRecursive(Summary->GetObjectField(TEXT("readable_stack")), OutValues);
        }
        if (Summary->HasTypedField<EJson::Object>(TEXT("exposed_parameters")))
        {
            CollectNiagaraParameterJsonValuesRecursive(Summary->GetObjectField(TEXT("exposed_parameters")), OutValues);
        }
        if (Summary->HasTypedField<EJson::Array>(TEXT("system_scripts")))
        {
            for (const TSharedPtr<FJsonValue>& ScriptValue : Summary->GetArrayField(TEXT("system_scripts")))
            {
                if (ScriptValue.IsValid() && ScriptValue->Type == EJson::Object)
                {
                    CollectNiagaraParameterJsonValuesRecursive(ScriptValue->AsObject(), OutValues);
                }
            }
        }
        if (Summary->HasTypedField<EJson::Array>(TEXT("emitters")))
        {
            for (const TSharedPtr<FJsonValue>& EmitterValue : Summary->GetArrayField(TEXT("emitters")))
            {
                if (EmitterValue.IsValid() && EmitterValue->Type == EJson::Object)
                {
                    CollectNiagaraParameterJsonValuesRecursive(EmitterValue->AsObject(), OutValues);
                }
            }
        }
    }


    static TSharedPtr<FJsonObject> GetNiagaraImportActionsObject(const TSharedPtr<FJsonObject>& Root)
    {
        if (!Root.IsValid())
        {
            return nullptr;
        }
        if (Root->HasTypedField<EJson::Object>(TEXT("niagara_import_actions")))
        {
            return Root->GetObjectField(TEXT("niagara_import_actions"));
        }
        if (Root->HasTypedField<EJson::Object>(TEXT("niagara_asset")))
        {
            TSharedPtr<FJsonObject> NiagaraAsset = Root->GetObjectField(TEXT("niagara_asset"));
            if (NiagaraAsset.IsValid() && NiagaraAsset->HasTypedField<EJson::Object>(TEXT("import_actions")))
            {
                return NiagaraAsset->GetObjectField(TEXT("import_actions"));
            }
        }
        return nullptr;
    }

    static int32 CountNiagaraImportActions(const TSharedPtr<FJsonObject>& Root)
    {
        TSharedPtr<FJsonObject> Actions = GetNiagaraImportActionsObject(Root);
        if (!Actions.IsValid())
        {
            return 0;
        }

        int32 Count = 0;
        const TCHAR* ArrayNames[] = { TEXT("user_parameters"), TEXT("remove_user_parameters"), TEXT("emitters"), TEXT("modules"), TEXT("input_overrides"), TEXT("parameter_values"), TEXT("rapid_iteration_parameters"), TEXT("direct_parameter_values") };
        for (const TCHAR* ArrayName : ArrayNames)
        {
            if (Actions->HasTypedField<EJson::Array>(ArrayName))
            {
                Count += Actions->GetArrayField(ArrayName).Num();
            }
        }
        bool bClearUserParameters = false;
        if (Actions->TryGetBoolField(TEXT("clear_user_parameters"), bClearUserParameters) && bClearUserParameters)
        {
            ++Count;
        }
        bool bClearN2CUserParameters = false;
        if (Actions->TryGetBoolField(TEXT("clear_n2c_user_parameters"), bClearN2CUserParameters) && bClearN2CUserParameters)
        {
            ++Count;
        }
        return Count;
    }

    static bool TryNiagaraTypeFromString(const FString& TypeStringIn, FNiagaraTypeDefinition& OutType)
    {
        FString TypeString = TypeStringIn;
        TypeString.TrimStartAndEndInline();
        TypeString.ToLowerInline();

        if (TypeString == TEXT("float") || TypeString == TEXT("double") || TypeString == TEXT("real"))
        {
            OutType = FNiagaraTypeDefinition::GetFloatDef();
            return true;
        }
        if (TypeString == TEXT("int") || TypeString == TEXT("int32") || TypeString == TEXT("integer"))
        {
            OutType = FNiagaraTypeDefinition::GetIntDef();
            return true;
        }
        if (TypeString == TEXT("bool") || TypeString == TEXT("boolean"))
        {
            OutType = FNiagaraTypeDefinition::GetBoolDef();
            return true;
        }
        if (TypeString == TEXT("vec2") || TypeString == TEXT("vector2") || TypeString == TEXT("vector2d"))
        {
            OutType = FNiagaraTypeDefinition::GetVec2Def();
            return true;
        }
        if (TypeString == TEXT("vec3") || TypeString == TEXT("vector3") || TypeString == TEXT("vector") || TypeString == TEXT("position"))
        {
            OutType = FNiagaraTypeDefinition::GetVec3Def();
            return true;
        }
        if (TypeString == TEXT("vec4") || TypeString == TEXT("vector4"))
        {
            OutType = FNiagaraTypeDefinition::GetVec4Def();
            return true;
        }
        if (TypeString == TEXT("color") || TypeString == TEXT("linear_color") || TypeString == TEXT("linearcolor"))
        {
            OutType = FNiagaraTypeDefinition::GetColorDef();
            return true;
        }
        if (TypeString == TEXT("quat") || TypeString == TEXT("quaternion"))
        {
            OutType = FNiagaraTypeDefinition::GetQuatDef();
            return true;
        }

        return false;
    }

    static TSharedPtr<FJsonObject> GetNiagaraActionValueObject(const TSharedPtr<FJsonObject>& Action)
    {
        if (!Action.IsValid())
        {
            return nullptr;
        }
        if (Action->HasTypedField<EJson::Object>(TEXT("default_value")))
        {
            return Action->GetObjectField(TEXT("default_value"));
        }
        if (Action->HasTypedField<EJson::Object>(TEXT("value")))
        {
            return Action->GetObjectField(TEXT("value"));
        }
        return Action;
    }

    static bool NiagaraEmitterHandleExistsByName(UNiagaraSystem* System, const FName& Name)
    {
        if (!System)
        {
            return false;
        }
        const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();
        for (const FNiagaraEmitterHandle& Handle : Handles)
        {
            if (Handle.GetName() == Name || FName(*Handle.GetUniqueInstanceName()) == Name)
            {
                return true;
            }
        }
        return false;
    }

    static bool ResolveNiagaraScriptUsageFromStage(const FString& StageIn, ENiagaraScriptUsage& OutUsage, bool& bOutSystemStage)
    {
        FString Stage = StageIn;
        Stage.TrimStartAndEndInline();
        Stage.ToLowerInline();
        Stage.ReplaceInline(TEXT(" "), TEXT(""));
        Stage.ReplaceInline(TEXT("_"), TEXT(""));
        Stage.ReplaceInline(TEXT("-"), TEXT(""));

        bOutSystemStage = false;
        if (Stage == TEXT("systemspawn") || Stage == TEXT("systemspawnscript"))
        {
            OutUsage = ENiagaraScriptUsage::SystemSpawnScript;
            bOutSystemStage = true;
            return true;
        }
        if (Stage == TEXT("systemupdate") || Stage == TEXT("systemupdatescript"))
        {
            OutUsage = ENiagaraScriptUsage::SystemUpdateScript;
            bOutSystemStage = true;
            return true;
        }
        if (Stage == TEXT("emitterspawn") || Stage == TEXT("emitterspawnscript"))
        {
            OutUsage = ENiagaraScriptUsage::EmitterSpawnScript;
            return true;
        }
        if (Stage == TEXT("emitterupdate") || Stage == TEXT("emitterupdatescript"))
        {
            OutUsage = ENiagaraScriptUsage::EmitterUpdateScript;
            return true;
        }
        if (Stage == TEXT("particlespawn") || Stage == TEXT("particlespawnscript") || Stage == TEXT("spawn"))
        {
            // v23: Use ParticleSpawnScript for AddModuleIfMissing compatibility.
            // Existing UE4.27 graphs may expose the output as ParticleSpawnScriptInterpolated;
            // N2CNiagaraScriptUsageMatches treats both Particle Spawn usages as equivalent
            // for find/remove/reorder, so this remains safe for existing stacks.
            OutUsage = ENiagaraScriptUsage::ParticleSpawnScript;
            return true;
        }
        if (Stage == TEXT("particleupdate") || Stage == TEXT("particleupdatescript") || Stage == TEXT("update"))
        {
            OutUsage = ENiagaraScriptUsage::ParticleUpdateScript;
            return true;
        }
        return false;
    }

    static UNiagaraScriptSourceBase* GetNiagaraSourceForSystemStage(UNiagaraSystem* System, ENiagaraScriptUsage Usage)
    {
        if (!System)
        {
            return nullptr;
        }
        UNiagaraScript* Script = nullptr;
        if (Usage == ENiagaraScriptUsage::SystemSpawnScript)
        {
            Script = System->GetSystemSpawnScript();
        }
        else if (Usage == ENiagaraScriptUsage::SystemUpdateScript)
        {
            Script = System->GetSystemUpdateScript();
        }
        return Script ? Script->GetLatestSource() : nullptr;
    }

    static void CollectTargetNiagaraEmitters(UNiagaraSystem* System, const TSharedPtr<FJsonObject>& Action, TArray<UNiagaraEmitter*>& OutEmitters, FString& OutLabel)
    {
        OutEmitters.Empty();
        OutLabel = TEXT("all emitters");
        if (!System)
        {
            return;
        }

        FString EmitterName;
        int32 EmitterIndex = INDEX_NONE;
        bool bAll = false;
        if (Action.IsValid())
        {
            Action->TryGetStringField(TEXT("emitter"), EmitterName);
            if (EmitterName.IsEmpty())
            {
                Action->TryGetStringField(TEXT("emitter_name"), EmitterName);
            }
            double IndexNumber = -1.0;
            if (Action->TryGetNumberField(TEXT("emitter_index"), IndexNumber))
            {
                EmitterIndex = static_cast<int32>(IndexNumber);
            }
            FString Mode;
            if (Action->TryGetStringField(TEXT("target"), Mode) || Action->TryGetStringField(TEXT("emitter_target"), Mode))
            {
                FString RawMode = Mode;
                Mode.ToLowerInline();
                bAll = Mode == TEXT("all") || Mode == TEXT("all_emitters");

                // Older Niagara import_actions exported emitter names in "target".
                // Treat non-special target values as emitter names so actions exported for
                // N2C_ExtraEmitter001 do not silently fall back to emitter index 0.
                if (!bAll && EmitterName.IsEmpty() && !RawMode.IsEmpty()
                    && Mode != TEXT("system") && Mode != TEXT("first") && Mode != TEXT("first_emitter"))
                {
                    EmitterName = RawMode;
                }
            }
        }

        const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();
        if (bAll)
        {
            for (const FNiagaraEmitterHandle& Handle : Handles)
            {
                if (UNiagaraEmitter* Emitter = Handle.GetInstance())
                {
                    OutEmitters.Add(Emitter);
                }
            }
            OutLabel = TEXT("all emitters");
            return;
        }

        if (EmitterIndex >= 0 && Handles.IsValidIndex(EmitterIndex))
        {
            if (UNiagaraEmitter* Emitter = Handles[EmitterIndex].GetInstance())
            {
                OutEmitters.Add(Emitter);
                OutLabel = FString::Printf(TEXT("emitter_index=%d"), EmitterIndex);
                return;
            }
        }

        if (!EmitterName.IsEmpty())
        {
            for (const FNiagaraEmitterHandle& Handle : Handles)
            {
                if (Handle.GetName().ToString() == EmitterName || Handle.GetUniqueInstanceName() == EmitterName)
                {
                    if (UNiagaraEmitter* Emitter = Handle.GetInstance())
                    {
                        OutEmitters.Add(Emitter);
                        OutLabel = FString::Printf(TEXT("emitter=%s"), *EmitterName);
                        return;
                    }
                }
            }
        }

        if (Handles.Num() > 0 && Handles[0].GetInstance())
        {
            OutEmitters.Add(Handles[0].GetInstance());
            OutLabel = TEXT("first emitter");
        }
    }

    static FString N2CNormalizeNiagaraUserParameterShortName(FString Name)
    {
        Name.TrimStartAndEndInline();
        if (Name.StartsWith(TEXT("User.")))
        {
            Name = Name.Mid(5);
        }
        return Name;
    }

    static void N2CCollectNiagaraUserParameterRemovalNames(const TSharedPtr<FJsonObject>& Actions, TSet<FString>& OutNames)
    {
        OutNames.Empty();
        if (!Actions.IsValid() || !Actions->HasTypedField<EJson::Array>(TEXT("remove_user_parameters")))
        {
            return;
        }

        for (const TSharedPtr<FJsonValue>& Value : Actions->GetArrayField(TEXT("remove_user_parameters")))
        {
            FString Name;
            if (!Value.IsValid())
            {
                continue;
            }
            if (Value->Type == EJson::String)
            {
                Name = Value->AsString();
            }
            else if (Value->Type == EJson::Object)
            {
                TSharedPtr<FJsonObject> Obj = Value->AsObject();
                if (Obj.IsValid())
                {
                    Obj->TryGetStringField(TEXT("name"), Name);
                    if (Name.IsEmpty())
                    {
                        Obj->TryGetStringField(TEXT("parameter"), Name);
                    }
                }
            }

            Name = N2CNormalizeNiagaraUserParameterShortName(Name);
            if (!Name.IsEmpty())
            {
                OutNames.Add(Name);
            }
        }
    }

    static int32 ApplyNiagaraUserParameterActions(UNiagaraSystem* System, const TSharedPtr<FJsonObject>& Root, bool bDryRun, FString& OutReport, int32& OutUnsupported, int32& OutUnchanged)
    {
        TSharedPtr<FJsonObject> Actions = GetNiagaraImportActionsObject(Root);
        if (!System || !Actions.IsValid())
        {
            return 0;
        }

        int32 Changed = 0;
        FNiagaraUserRedirectionParameterStore& Store = System->GetExposedParameters();

        // v25: Niagara tests accumulated dozens of stale User.N2C_* parameters while iterating.
        // Allow import JSON to intentionally clean the exposed user parameter store before adding
        // the small set of parameters that the current effect actually needs.
        bool bClearAllUserParameters = false;
        bool bClearN2CUserParameters = false;
        Actions->TryGetBoolField(TEXT("clear_user_parameters"), bClearAllUserParameters);
        Actions->TryGetBoolField(TEXT("remove_all_user_parameters"), bClearAllUserParameters);
        Actions->TryGetBoolField(TEXT("clear_n2c_user_parameters"), bClearN2CUserParameters);

        TSet<FString> ExplicitRemoveNames;
        N2CCollectNiagaraUserParameterRemovalNames(Actions, ExplicitRemoveNames);

        if (bClearAllUserParameters || bClearN2CUserParameters || ExplicitRemoveNames.Num() > 0)
        {
            const int32 CleanupChangedBefore = Changed;
            TArray<FNiagaraVariable> ExistingUserParameters;
            Store.GetUserParameters(ExistingUserParameters);
            for (const FNiagaraVariable& ExistingVariable : ExistingUserParameters)
            {
                FString ShortName = N2CNormalizeNiagaraUserParameterShortName(ExistingVariable.GetName().ToString());
                const bool bRemove = bClearAllUserParameters
                    || (bClearN2CUserParameters && ShortName.StartsWith(TEXT("N2C_")))
                    || ExplicitRemoveNames.Contains(ShortName);

                if (!bRemove)
                {
                    continue;
                }

                if (bDryRun)
                {
                    OutReport += FString::Printf(TEXT("ACTION: remove user parameter User.%s"), *ShortName) + LINE_TERMINATOR;
                }
                else
                {
                    Store.RemoveParameter(ExistingVariable);
                    OutReport += FString::Printf(TEXT("Applied: removed user parameter User.%s"), *ShortName) + LINE_TERMINATOR;
                }
                ++Changed;
            }
            if (!bDryRun && Changed > CleanupChangedBefore)
            {
                // UE4.27 declares the Niagara user-redirection refresh helper,
                // but it is not exported for external plugin linkage in the tested Editor build.
                // Calling it compiles but fails at link time (LNK2019). Removing parameters
                // through the public parameter store API is enough for this importer pass;
                // the owning Niagara system is marked changed/dirty later in the import flow.
            }
        }

        if (!Actions->HasTypedField<EJson::Array>(TEXT("user_parameters")))
        {
            return Changed;
        }

        for (const TSharedPtr<FJsonValue>& Value : Actions->GetArrayField(TEXT("user_parameters")))
        {
            TSharedPtr<FJsonObject> Action = Value.IsValid() ? Value->AsObject() : nullptr;
            if (!Action.IsValid())
            {
                continue;
            }

            FString Name;
            FString TypeString;
            Action->TryGetStringField(TEXT("name"), Name);
            Action->TryGetStringField(TEXT("type"), TypeString);
            if (Name.IsEmpty() || TypeString.IsEmpty())
            {
                ++OutUnsupported;
                OutReport += TEXT("WARNING: user parameter action missing name/type.") LINE_TERMINATOR;
                continue;
            }
            if (!Name.StartsWith(TEXT("User.")))
            {
                Name = TEXT("User.") + Name;
            }

            FNiagaraTypeDefinition TypeDef;
            if (!TryNiagaraTypeFromString(TypeString, TypeDef))
            {
                ++OutUnsupported;
                OutReport += FString::Printf(TEXT("WARNING: unsupported user parameter type for %s: %s"), *Name, *TypeString) + LINE_TERMINATOR;
                continue;
            }

            FNiagaraVariable Variable(TypeDef, FName(*Name));
            const bool bAlreadyExists = Store.IndexOf(Variable) != INDEX_NONE;
            TSharedPtr<FJsonObject> ValueObj = GetNiagaraActionValueObject(Action);
            TArray<uint8> NewBytes;
            FString Reason;
            const bool bHasValue = TryBuildNiagaraParameterBytes(Variable, ValueObj, NewBytes, Reason);
            if (!bHasValue && Action->HasTypedField<EJson::Object>(TEXT("default_value")))
            {
                ++OutUnsupported;
                OutReport += FString::Printf(TEXT("WARNING: could not build default value for user parameter %s: %s"), *Name, *Reason) + LINE_TERMINATOR;
                continue;
            }

            if (bAlreadyExists && bHasValue)
            {
                const uint8* CurrentData = Store.GetParameterData(Variable);
                if (CurrentData && NewBytes.Num() == Variable.GetSizeInBytes() && FMemory::Memcmp(CurrentData, NewBytes.GetData(), NewBytes.Num()) == 0)
                {
                    ++OutUnchanged;
                    continue;
                }
            }
            else if (bAlreadyExists)
            {
                ++OutUnchanged;
                continue;
            }

            if (bDryRun)
            {
                OutReport += FString::Printf(TEXT("ACTION: %s user parameter %s (%s)%s%s"),
                    bAlreadyExists ? TEXT("update") : TEXT("add"),
                    *Name,
                    *TypeString,
                    bHasValue ? TEXT(" = ") : TEXT(""),
                    bHasValue ? *NiagaraValueDisplay(ValueObj) : TEXT("")) + LINE_TERMINATOR;
            }
            else
            {
                if (bHasValue)
                {
                    Store.SetParameterData(NewBytes.GetData(), Variable, true);
                }
                else
                {
                    Store.AddParameter(Variable, true, true);
                }
                OutReport += FString::Printf(TEXT("Applied: %s user parameter %s (%s)%s%s"),
                    bAlreadyExists ? TEXT("updated") : TEXT("added"),
                    *Name,
                    *TypeString,
                    bHasValue ? TEXT(" = ") : TEXT(""),
                    bHasValue ? *NiagaraValueDisplay(ValueObj) : TEXT("")) + LINE_TERMINATOR;
            }
            ++Changed;
        }
        return Changed;
    }

    static int32 ApplyNiagaraEmitterActions(UNiagaraSystem* System, const TSharedPtr<FJsonObject>& Root, bool bDryRun, FString& OutReport, int32& OutUnsupported, int32& OutUnchanged)
    {
        TSharedPtr<FJsonObject> Actions = GetNiagaraImportActionsObject(Root);
        if (!System || !Actions.IsValid() || !Actions->HasTypedField<EJson::Array>(TEXT("emitters")))
        {
            return 0;
        }

        int32 Changed = 0;
        for (const TSharedPtr<FJsonValue>& Value : Actions->GetArrayField(TEXT("emitters")))
        {
            TSharedPtr<FJsonObject> Action = Value.IsValid() ? Value->AsObject() : nullptr;
            if (!Action.IsValid())
            {
                continue;
            }

            FString Op;
            FString NewName;
            Action->TryGetStringField(TEXT("op"), Op);
            Action->TryGetStringField(TEXT("name"), NewName);
            if (NewName.IsEmpty())
            {
                Action->TryGetStringField(TEXT("new_name"), NewName);
            }
            Op.ToLowerInline();
            if (Op.IsEmpty())
            {
                Op = TEXT("duplicate");
            }

            const bool bSetEmitterEnabled = (Op == TEXT("disable") || Op == TEXT("enable") || Op == TEXT("set_enabled") || Op == TEXT("set_enable"));
            if (bSetEmitterEnabled)
            {
                if (NewName.IsEmpty())
                {
                    Action->TryGetStringField(TEXT("target"), NewName);
                }
                if (NewName.IsEmpty())
                {
                    Action->TryGetStringField(TEXT("emitter"), NewName);
                }
                if (NewName.IsEmpty())
                {
                    ++OutUnsupported;
                    OutReport += TEXT("WARNING: emitter enable/disable action missing name/target/emitter.") LINE_TERMINATOR;
                    continue;
                }

                bool bEnabled = (Op == TEXT("enable"));
                if (Op == TEXT("set_enabled") || Op == TEXT("set_enable"))
                {
                    Action->TryGetBoolField(TEXT("enabled"), bEnabled);
                }

                int32 FoundIndex = INDEX_NONE;
                const TArray<FNiagaraEmitterHandle>& HandlesForSearch = System->GetEmitterHandles();
                for (int32 HandleIndex = 0; HandleIndex < HandlesForSearch.Num(); ++HandleIndex)
                {
                    const FNiagaraEmitterHandle& Handle = HandlesForSearch[HandleIndex];
                    if (Handle.GetName().ToString() == NewName || Handle.GetUniqueInstanceName() == NewName)
                    {
                        FoundIndex = HandleIndex;
                        break;
                    }
                }
                if (FoundIndex == INDEX_NONE)
                {
                    ++OutUnchanged;
                    OutReport += FString::Printf(TEXT("SKIP: emitter %s not found for enable/disable."), *NewName) + LINE_TERMINATOR;
                    continue;
                }

                if (bDryRun)
                {
                    OutReport += FString::Printf(TEXT("ACTION: set emitter %s enabled=%s"), *NewName, bEnabled ? TEXT("true") : TEXT("false")) + LINE_TERMINATOR;
                }
                else
                {
#if WITH_EDITORONLY_DATA
                    FNiagaraEmitterHandle& Handle = System->GetEmitterHandle(FoundIndex);
                    Handle.SetIsEnabled(bEnabled, *System, false);
                    if (UNiagaraEmitter* Emitter = Handle.GetInstance())
                    {
                        Emitter->Modify();
                        UNiagaraSystem::RequestCompileForEmitter(Emitter);
                    }
                    OutReport += FString::Printf(TEXT("Applied: set emitter %s enabled=%s"), *NewName, bEnabled ? TEXT("true") : TEXT("false")) + LINE_TERMINATOR;
#else
                    ++OutUnsupported;
                    OutReport += TEXT("WARNING: emitter enable/disable requires WITH_EDITORONLY_DATA.") LINE_TERMINATOR;
                    continue;
#endif
                }
                ++Changed;
                continue;
            }

            if (NewName.IsEmpty() || (Op != TEXT("duplicate") && Op != TEXT("clone_existing")))
            {
                ++OutUnsupported;
                OutReport += TEXT("WARNING: emitter action supports op=duplicate/clone_existing or op=disable/enable/set_enabled with name/new_name/target/emitter.") LINE_TERMINATOR;
                continue;
            }

            const FName NewEmitterName(*NewName);
            if (NiagaraEmitterHandleExistsByName(System, NewEmitterName))
            {
                ++OutUnchanged;
                OutReport += FString::Printf(TEXT("SKIP: emitter %s already exists."), *NewName) + LINE_TERMINATOR;
                continue;
            }

            int32 SourceIndex = 0;
            double SourceIndexNumber = 0.0;
            if (Action->TryGetNumberField(TEXT("duplicate_from_index"), SourceIndexNumber) || Action->TryGetNumberField(TEXT("source_index"), SourceIndexNumber))
            {
                SourceIndex = static_cast<int32>(SourceIndexNumber);
            }
            const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();
            if (!Handles.IsValidIndex(SourceIndex) || !Handles[SourceIndex].GetInstance())
            {
                ++OutUnsupported;
                OutReport += FString::Printf(TEXT("WARNING: cannot duplicate emitter %s; source index %d is invalid."), *NewName, SourceIndex) + LINE_TERMINATOR;
                continue;
            }

            if (bDryRun)
            {
                OutReport += FString::Printf(TEXT("ACTION: duplicate emitter index %d as %s"), SourceIndex, *NewName) + LINE_TERMINATOR;
            }
            else
            {
#if WITH_EDITORONLY_DATA
                System->DuplicateEmitterHandle(Handles[SourceIndex], NewEmitterName);
                bool bEnabled = true;
                Action->TryGetBoolField(TEXT("enabled"), bEnabled);
                const int32 NewIndex = System->GetEmitterHandles().Num() - 1;
                if (NewIndex >= 0)
                {
                    FNiagaraEmitterHandle& NewHandle = System->GetEmitterHandle(NewIndex);
                    NewHandle.SetIsEnabled(bEnabled, *System, false);
                    System->RefreshSystemParametersFromEmitter(NewHandle);
                    if (UNiagaraEmitter* NewEmitter = NewHandle.GetInstance())
                    {
                        NewEmitter->Modify();
                        UNiagaraSystem::RequestCompileForEmitter(NewEmitter);
                    }
                }
                OutReport += FString::Printf(TEXT("Applied: duplicated emitter index %d as %s"), SourceIndex, *NewName) + LINE_TERMINATOR;
#else
                ++OutUnsupported;
                OutReport += TEXT("WARNING: emitter duplication requires WITH_EDITORONLY_DATA.") LINE_TERMINATOR;
                continue;
#endif
            }
            ++Changed;
        }
        return Changed;
    }

    static void N2CCollectNiagaraModuleNodesForUsage(UEdGraph* Graph, ENiagaraScriptUsage Usage, const FString& ModuleName, TArray<UNiagaraNodeFunctionCall*>& OutModuleNodes);
    static UNiagaraScriptSource* N2CGetNiagaraSystemScriptSourceForUsage(UNiagaraSystem* System, ENiagaraScriptUsage Usage);
    static bool N2CRemoveNiagaraModuleNodeDirect(UNiagaraNodeFunctionCall* ModuleNode);

    static int32 ApplyNiagaraModuleActions(UNiagaraSystem* System, const TSharedPtr<FJsonObject>& Root, bool bDryRun, FString& OutReport, int32& OutUnsupported, int32& OutUnchanged)
    {
        TSharedPtr<FJsonObject> Actions = GetNiagaraImportActionsObject(Root);
        if (!System || !Actions.IsValid() || !Actions->HasTypedField<EJson::Array>(TEXT("modules")))
        {
            return 0;
        }

        int32 Changed = 0;
        for (const TSharedPtr<FJsonValue>& Value : Actions->GetArrayField(TEXT("modules")))
        {
            TSharedPtr<FJsonObject> Action = Value.IsValid() ? Value->AsObject() : nullptr;
            if (!Action.IsValid())
            {
                continue;
            }

            FString Op;
            Action->TryGetStringField(TEXT("op"), Op);
            Op.ToLowerInline();
            if (Op.IsEmpty())
            {
                Op = TEXT("add_if_missing");
            }

            FString ModulePath;
            FString ModuleName;
            FString Stage;
            Action->TryGetStringField(TEXT("module_path"), ModulePath);
            if (ModulePath.IsEmpty())
            {
                Action->TryGetStringField(TEXT("asset_path"), ModulePath);
            }
            Action->TryGetStringField(TEXT("module"), ModuleName);
            if (ModuleName.IsEmpty())
            {
                Action->TryGetStringField(TEXT("module_name"), ModuleName);
            }
            if (ModuleName.IsEmpty() && !ModulePath.IsEmpty())
            {
                int32 DotIndex = INDEX_NONE;
                if (ModulePath.FindLastChar(TEXT('.'), DotIndex) && DotIndex + 1 < ModulePath.Len())
                {
                    ModuleName = ModulePath.Mid(DotIndex + 1);
                }
                else
                {
                    int32 SlashIndex = INDEX_NONE;
                    if (ModulePath.FindLastChar(TEXT('/'), SlashIndex) && SlashIndex + 1 < ModulePath.Len())
                    {
                        ModuleName = ModulePath.Mid(SlashIndex + 1);
                    }
                }
            }
            Action->TryGetStringField(TEXT("stage"), Stage);

            const bool bRemoveModule = (Op == TEXT("remove") || Op == TEXT("remove_if_exists") || Op == TEXT("remove_all_if_exists") || Op == TEXT("delete") || Op == TEXT("delete_if_exists") || Op == TEXT("delete_all_if_exists"));
            const bool bReorderModules = (Op == TEXT("reorder") || Op == TEXT("reorder_stage") || Op == TEXT("set_order"));

            if ((!bRemoveModule && !bReorderModules && ModulePath.IsEmpty()) || (!bReorderModules && ModuleName.IsEmpty()) || Stage.IsEmpty())
            {
                ++OutUnsupported;
                OutReport += TEXT("WARNING: module action missing module_path/asset_path/module/order or stage.") LINE_TERMINATOR;
                continue;
            }

            ENiagaraScriptUsage Usage = ENiagaraScriptUsage::ParticleUpdateScript;
            bool bSystemStage = false;
            if (!ResolveNiagaraScriptUsageFromStage(Stage, Usage, bSystemStage))
            {
                ++OutUnsupported;
                OutReport += FString::Printf(TEXT("WARNING: unsupported Niagara module stage: %s"), *Stage) + LINE_TERMINATOR;
                continue;
            }

            if (bReorderModules)
            {
                const TArray<TSharedPtr<FJsonValue>>* OrderValues = nullptr;
                if (!Action->TryGetArrayField(TEXT("order"), OrderValues) || !OrderValues)
                {
                    ++OutUnsupported;
                    OutReport += TEXT("WARNING: reorder module action missing order array.") LINE_TERMINATOR;
                    continue;
                }

                TArray<FString> DesiredOrder;
                for (const TSharedPtr<FJsonValue>& OrderValue : *OrderValues)
                {
                    if (OrderValue.IsValid())
                    {
                        FString DesiredName = OrderValue->AsString();
                        DesiredName.TrimStartAndEndInline();
                        if (!DesiredName.IsEmpty())
                        {
                            DesiredOrder.Add(DesiredName);
                        }
                    }
                }
                if (DesiredOrder.Num() <= 0)
                {
                    ++OutUnsupported;
                    OutReport += TEXT("WARNING: reorder module action has empty order array.") LINE_TERMINATOR;
                    continue;
                }

                if (bSystemStage)
                {
                    UNiagaraScriptSource* Source = N2CGetNiagaraSystemScriptSourceForUsage(System, Usage);
                    if (!Source || !Source->NodeGraph)
                    {
                        ++OutUnsupported;
                        OutReport += FString::Printf(TEXT("WARNING: no source graph for system reorder stage %s"), *Stage) + LINE_TERMINATOR;
                        continue;
                    }
                    if (bDryRun)
                    {
                        OutReport += FString::Printf(TEXT("ACTION: reorder modules in %s"), *Stage) + LINE_TERMINATOR;
                        ++Changed;
                        continue;
                    }
                    FString Reason;
                    if (N2CEditorIntegration_Private::N2CReorderNiagaraModulesForUsage(Source->NodeGraph, Usage, DesiredOrder, Reason))
                    {
                        OutReport += FString::Printf(TEXT("Applied: reordered modules in %s"), *Stage) + LINE_TERMINATOR;
                        ++Changed;
                    }
                    else
                    {
                        ++OutUnsupported;
                        OutReport += FString::Printf(TEXT("WARNING: could not reorder modules in %s: %s"), *Stage, *Reason) + LINE_TERMINATOR;
                    }
                    continue;
                }

                TArray<UNiagaraEmitter*> TargetEmitters;
                FString TargetLabel;
                CollectTargetNiagaraEmitters(System, Action, TargetEmitters, TargetLabel);
                if (TargetEmitters.Num() <= 0)
                {
                    ++OutUnsupported;
                    OutReport += FString::Printf(TEXT("WARNING: no target emitter found for reorder modules in %s"), *Stage) + LINE_TERMINATOR;
                    continue;
                }

                for (UNiagaraEmitter* Emitter : TargetEmitters)
                {
                    if (!Emitter)
                    {
                        continue;
                    }
#if WITH_EDITORONLY_DATA
                    UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(Emitter->GraphSource);
                    if (!Source || !Source->NodeGraph)
                    {
                        ++OutUnsupported;
                        OutReport += FString::Printf(TEXT("WARNING: emitter %s has no graph source for reorder."), *Emitter->GetName()) + LINE_TERMINATOR;
                        continue;
                    }
                    if (bDryRun)
                    {
                        OutReport += FString::Printf(TEXT("ACTION: reorder modules in %s / %s"), *Emitter->GetUniqueEmitterName(), *Stage) + LINE_TERMINATOR;
                        ++Changed;
                        continue;
                    }
                    FString Reason;
                    if (N2CEditorIntegration_Private::N2CReorderNiagaraModulesForUsage(Source->NodeGraph, Usage, DesiredOrder, Reason))
                    {
                        OutReport += FString::Printf(TEXT("Applied: reordered modules in %s / %s"), *Emitter->GetUniqueEmitterName(), *Stage) + LINE_TERMINATOR;
                        ++Changed;
                    }
                    else
                    {
                        ++OutUnsupported;
                        OutReport += FString::Printf(TEXT("WARNING: could not reorder modules in %s / %s: %s"), *Emitter->GetUniqueEmitterName(), *Stage, *Reason) + LINE_TERMINATOR;
                    }
#else
                    ++OutUnsupported;
                    OutReport += TEXT("WARNING: module reorder requires WITH_EDITORONLY_DATA.") LINE_TERMINATOR;
#endif
                }
                continue;
            }

            if (bRemoveModule)
            {
                if (bSystemStage)
                {
                    UNiagaraScriptSource* Source = N2CGetNiagaraSystemScriptSourceForUsage(System, Usage);
                    if (!Source || !Source->NodeGraph)
                    {
                        ++OutUnsupported;
                        OutReport += FString::Printf(TEXT("WARNING: no source graph for system stage %s"), *Stage) + LINE_TERMINATOR;
                        continue;
                    }

                    TArray<UNiagaraNodeFunctionCall*> ModuleNodes;
                    N2CCollectNiagaraModuleNodesForUsage(Source->NodeGraph, Usage, ModuleName, ModuleNodes);
                    if (ModuleNodes.Num() <= 0)
                    {
                        ++OutUnchanged;
                        OutReport += FString::Printf(TEXT("SKIP: module %s not found in %s."), *ModuleName, *Stage) + LINE_TERMINATOR;
                        continue;
                    }

                    if (bDryRun)
                    {
                        OutReport += FString::Printf(TEXT("ACTION: remove module %s from %s"), *ModuleName, *Stage) + LINE_TERMINATOR;
                        ++Changed;
                        continue;
                    }

                    int32 RemovedCount = 0;
                    for (UNiagaraNodeFunctionCall* ModuleNode : ModuleNodes)
                    {
                        if (N2CRemoveNiagaraModuleNodeDirect(ModuleNode))
                        {
                            ++RemovedCount;
                        }
                    }
                    if (RemovedCount > 0)
                    {
                        OutReport += FString::Printf(TEXT("Applied: removed module %s from %s (%d node(s))"), *ModuleName, *Stage, RemovedCount) + LINE_TERMINATOR;
                        Changed += RemovedCount;
                    }
                    else
                    {
                        ++OutUnsupported;
                        OutReport += FString::Printf(TEXT("WARNING: could not remove module %s from %s"), *ModuleName, *Stage) + LINE_TERMINATOR;
                    }
                    continue;
                }

                TArray<UNiagaraEmitter*> TargetEmitters;
                FString TargetLabel;
                CollectTargetNiagaraEmitters(System, Action, TargetEmitters, TargetLabel);
                if (TargetEmitters.Num() <= 0)
                {
                    ++OutUnsupported;
                    OutReport += FString::Printf(TEXT("WARNING: no target emitter found for remove module %s"), *ModuleName) + LINE_TERMINATOR;
                    continue;
                }

                for (UNiagaraEmitter* Emitter : TargetEmitters)
                {
                    if (!Emitter)
                    {
                        continue;
                    }
#if WITH_EDITORONLY_DATA
                    UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(Emitter->GraphSource);
                    if (!Source || !Source->NodeGraph)
                    {
                        ++OutUnsupported;
                        OutReport += FString::Printf(TEXT("WARNING: emitter %s has no graph source."), *Emitter->GetName()) + LINE_TERMINATOR;
                        continue;
                    }

                    TArray<UNiagaraNodeFunctionCall*> ModuleNodes;
                    N2CCollectNiagaraModuleNodesForUsage(Source->NodeGraph, Usage, ModuleName, ModuleNodes);
                    if (ModuleNodes.Num() <= 0)
                    {
                        ++OutUnchanged;
                        OutReport += FString::Printf(TEXT("SKIP: module %s not found in %s / %s."), *ModuleName, *Emitter->GetUniqueEmitterName(), *Stage) + LINE_TERMINATOR;
                        continue;
                    }

                    if (bDryRun)
                    {
                        OutReport += FString::Printf(TEXT("ACTION: remove module %s from %s / %s"), *ModuleName, *Emitter->GetUniqueEmitterName(), *Stage) + LINE_TERMINATOR;
                        ++Changed;
                        continue;
                    }

                    int32 RemovedCount = 0;
                    for (UNiagaraNodeFunctionCall* ModuleNode : ModuleNodes)
                    {
                        if (N2CRemoveNiagaraModuleNodeDirect(ModuleNode))
                        {
                            ++RemovedCount;
                        }
                    }
                    if (RemovedCount > 0)
                    {
                        OutReport += FString::Printf(TEXT("Applied: removed module %s from emitter %s / %s (%d node(s))"), *ModuleName, *Emitter->GetUniqueEmitterName(), *Stage, RemovedCount) + LINE_TERMINATOR;
                        Changed += RemovedCount;
                    }
                    else
                    {
                        ++OutUnsupported;
                        OutReport += FString::Printf(TEXT("WARNING: could not remove module %s from emitter %s / %s"), *ModuleName, *Emitter->GetUniqueEmitterName(), *Stage) + LINE_TERMINATOR;
                    }
#else
                    ++OutUnsupported;
                    OutReport += TEXT("WARNING: module remove requires WITH_EDITORONLY_DATA.") LINE_TERMINATOR;
#endif
                }
                continue;
            }

            if (bSystemStage)
            {
                if (bDryRun)
                {
                    OutReport += FString::Printf(TEXT("ACTION: add module if missing %s to %s"), *ModulePath, *Stage) + LINE_TERMINATOR;
                    ++Changed;
                    continue;
                }

                UNiagaraScriptSourceBase* Source = GetNiagaraSourceForSystemStage(System, Usage);
                if (!Source)
                {
                    ++OutUnsupported;
                    OutReport += FString::Printf(TEXT("WARNING: no source graph for system stage %s"), *Stage) + LINE_TERMINATOR;
                    continue;
                }

                bool bFoundModule = false;
                const bool bAdded = Source->AddModuleIfMissing(ModulePath, Usage, bFoundModule);
                if (bAdded)
                {
                    OutReport += FString::Printf(TEXT("Applied: added module %s to %s"), *ModulePath, *Stage) + LINE_TERMINATOR;
                    ++Changed;
                }
                else if (bFoundModule)
                {
                    ++OutUnchanged;
                    OutReport += FString::Printf(TEXT("SKIP: module already exists or could not be inserted: %s / %s"), *ModulePath, *Stage) + LINE_TERMINATOR;
                }
                else
                {
                    ++OutUnsupported;
                    OutReport += FString::Printf(TEXT("WARNING: module asset not found: %s"), *ModulePath) + LINE_TERMINATOR;
                }
                continue;
            }

            TArray<UNiagaraEmitter*> TargetEmitters;
            FString TargetLabel;
            CollectTargetNiagaraEmitters(System, Action, TargetEmitters, TargetLabel);
            if (TargetEmitters.Num() <= 0)
            {
                ++OutUnsupported;
                OutReport += FString::Printf(TEXT("WARNING: no target emitter found for module %s"), *ModulePath) + LINE_TERMINATOR;
                continue;
            }

            for (UNiagaraEmitter* Emitter : TargetEmitters)
            {
                if (!Emitter)
                {
                    continue;
                }
                if (bDryRun)
                {
                    OutReport += FString::Printf(TEXT("ACTION: add module if missing %s to %s / %s"), *ModulePath, *TargetLabel, *Stage) + LINE_TERMINATOR;
                    ++Changed;
                    continue;
                }

#if WITH_EDITORONLY_DATA
                UNiagaraScriptSourceBase* Source = Emitter->GraphSource;
                if (!Source)
                {
                    ++OutUnsupported;
                    OutReport += FString::Printf(TEXT("WARNING: emitter %s has no graph source."), *Emitter->GetName()) + LINE_TERMINATOR;
                    continue;
                }

                bool bFoundModule = false;
                const bool bAdded = Source->AddModuleIfMissing(ModulePath, Usage, bFoundModule);
                if (bAdded)
                {
                    OutReport += FString::Printf(TEXT("Applied: added module %s to emitter %s / %s"), *ModulePath, *Emitter->GetUniqueEmitterName(), *Stage) + LINE_TERMINATOR;
                    ++Changed;
                }
                else if (bFoundModule)
                {
                    ++OutUnchanged;
                    OutReport += FString::Printf(TEXT("SKIP: module already exists or could not be inserted: %s / %s / %s"), *ModulePath, *Emitter->GetUniqueEmitterName(), *Stage) + LINE_TERMINATOR;
                }
                else
                {
                    ++OutUnsupported;
                    OutReport += FString::Printf(TEXT("WARNING: module asset not found: %s"), *ModulePath) + LINE_TERMINATOR;
                }
#else
                ++OutUnsupported;
                OutReport += TEXT("WARNING: module import requires WITH_EDITORONLY_DATA.") LINE_TERMINATOR;
#endif
            }
        }
        return Changed;
    }


    static bool GetNiagaraActionStringAny(const TSharedPtr<FJsonObject>& Action, const TCHAR* NameA, const TCHAR* NameB, FString& OutValue)
    {
        if (!Action.IsValid())
        {
            return false;
        }
        if (NameA && Action->TryGetStringField(NameA, OutValue) && !OutValue.IsEmpty())
        {
            return true;
        }
        if (NameB && Action->TryGetStringField(NameB, OutValue) && !OutValue.IsEmpty())
        {
            return true;
        }
        return false;
    }


    static bool N2CIsNiagaraNodeClassNamed(const UObject* Obj, const TCHAR* ShortClassName)
    {
        return Obj && Obj->GetClass() && Obj->GetClass()->GetName() == FString(ShortClassName);
    }

    static UClass* N2CFindNiagaraEditorNodeClass(const TCHAR* ShortClassName)
    {
        if (UClass* Found = FindObject<UClass>(ANY_PACKAGE, ShortClassName))
        {
            return Found;
        }
        const FString ScriptPath = FString::Printf(TEXT("/Script/NiagaraEditor.%s"), ShortClassName);
        return LoadObject<UClass>(nullptr, *ScriptPath);
    }

    static bool N2CIsNiagaraParameterMapPin(const UEdGraphPin* Pin)
    {
        if (!Pin)
        {
            return false;
        }

        const UEdGraphSchema_Niagara* NiagaraSchema = GetDefault<UEdGraphSchema_Niagara>();
        if (NiagaraSchema)
        {
            const FNiagaraTypeDefinition PinType = NiagaraSchema->PinToTypeDefinition(Pin);
            if (PinType == FNiagaraTypeDefinition::GetParameterMapDef())
            {
                return true;
            }
        }

        UScriptStruct* ParameterMapStruct = FNiagaraTypeDefinition::GetParameterMapStruct();
        if (Pin->PinType.PinSubCategoryObject == ParameterMapStruct)
        {
            return true;
        }

        const FString PinName = Pin->PinName.ToString();
        const FString SubCategoryName = Pin->PinType.PinSubCategoryObject.IsValid() ? Pin->PinType.PinSubCategoryObject->GetName() : FString();
        return PinName.Contains(TEXT("Map")) && SubCategoryName.Contains(TEXT("ParameterMap"));
    }

    static UEdGraphPin* N2CFindNiagaraParameterMapPin(UEdGraphNode* Node, EEdGraphPinDirection Direction)
    {
        if (!Node)
        {
            return nullptr;
        }

        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin && Pin->Direction == Direction && N2CIsNiagaraParameterMapPin(Pin))
            {
                return Pin;
            }
        }
        return nullptr;
    }

    static UEdGraphPin* N2CGetSingleLinkedPinSafe(UEdGraphPin* Pin)
    {
        if (!Pin || Pin->LinkedTo.Num() <= 0)
        {
            return nullptr;
        }
        return Pin->LinkedTo.IsValidIndex(0) ? Pin->LinkedTo[0] : nullptr;
    }

    static bool N2CNiagaraPinLooksUsable(const UEdGraphPin* Pin)
    {
        return Pin && !Pin->IsPendingKill() && Pin->GetOwningNode() && !Pin->GetOwningNode()->IsPendingKill();
    }

    static void N2CNiagaraMakeLink(UEdGraphPin* PinA, UEdGraphPin* PinB)
    {
        if (!N2CNiagaraPinLooksUsable(PinA) || !N2CNiagaraPinLooksUsable(PinB) || PinA == PinB)
        {
            return;
        }
        if (!PinA->LinkedTo.Contains(PinB))
        {
            PinA->MakeLinkTo(PinB);
        }

        // Do not call PinConnectionListChanged here. In UE4.27 Niagara nodes can assert while
        // low-level import is still building their dynamic pins. The graph is dirtied/refreshed
        // once after the staged edit is complete.
    }

    static void N2CNiagaraBreakAllPinLinks(UEdGraphPin* Pin)
    {
        if (Pin)
        {
            Pin->BreakAllPinLinks(true);
        }
    }

    static void N2CEnsureNiagaraParameterMapPins(UEdGraphNode* Node)
    {
        if (!Node)
        {
            return;
        }

        const FEdGraphPinType MapPinType = UEdGraphSchema_Niagara::TypeDefinitionToPinType(FNiagaraTypeDefinition::GetParameterMapDef());
        const FString ClassName = Node->GetClass() ? Node->GetClass()->GetName() : FString();

        if (ClassName == TEXT("NiagaraNodeParameterMapSet"))
        {
            if (!N2CFindNiagaraParameterMapPin(Node, EGPD_Input))
            {
                Node->CreatePin(EGPD_Input, MapPinType, FName(TEXT("Source")));
            }
            if (!N2CFindNiagaraParameterMapPin(Node, EGPD_Output))
            {
                Node->CreatePin(EGPD_Output, MapPinType, FName(TEXT("Dest")));
            }
        }
        else if (ClassName == TEXT("NiagaraNodeParameterMapGet"))
        {
            if (!N2CFindNiagaraParameterMapPin(Node, EGPD_Input))
            {
                Node->CreatePin(EGPD_Input, MapPinType, FName(TEXT("Source")));
            }
        }
        else if (ClassName == TEXT("NiagaraNodeCustomHlsl"))
        {
            if (!N2CFindNiagaraParameterMapPin(Node, EGPD_Input))
            {
                Node->CreatePin(EGPD_Input, MapPinType, FName(TEXT("Map")));
            }
        }
    }

    static void N2CNiagaraGraphImportTrace(const FString& Message)
    {
        UE_LOG(LogNodeToCode, Warning, TEXT("N2C Niagara graph import trace: %s"), *Message);
    }

#if N2C_WITH_NIAGARA_PRIVATE_GRAPH_API
    template <typename TNode>
    static UEdGraphNode* N2CNiagaraCreateTypedGraphNode(UEdGraph* Graph, int32 X, int32 Y, const TCHAR* Label)
    {
        if (!Graph)
        {
            return nullptr;
        }

        N2CNiagaraGraphImportTrace(FString::Printf(TEXT("Create typed node begin: %s"), Label));
        Graph->Modify();

        FGraphNodeCreator<TNode> NodeCreator(*Graph);
        TNode* Node = NodeCreator.CreateNode();
        if (!Node)
        {
            N2CNiagaraGraphImportTrace(FString::Printf(TEXT("Create typed node failed before finalize: %s"), Label));
            return nullptr;
        }

        Node->NodePosX = X;
        Node->NodePosY = Y;
        NodeCreator.Finalize();
        N2CEnsureNiagaraParameterMapPins(Node);
        Node->Modify();
        N2CNiagaraGraphImportTrace(FString::Printf(TEXT("Create typed node end: %s pins=%d"), Label, Node->Pins.Num()));
        return Node;
    }
#endif

    static UEdGraphNode* N2CNiagaraCreateGraphNode(UEdGraph* Graph, UClass* NodeClass, int32 X, int32 Y)
    {
        if (!Graph || !NodeClass)
        {
            return nullptr;
        }

        const FString ClassName = NodeClass->GetName();
        N2CNiagaraGraphImportTrace(FString::Printf(TEXT("Create node requested: %s"), *ClassName));

#if N2C_WITH_NIAGARA_PRIVATE_GRAPH_API
        // UE4.27 Niagara editor nodes have important initialization in FGraphNodeCreator::Finalize().
        // Manual NewObject + AddNode + AllocateDefaultPins can leave internal pin arrays empty and
        // assert inside Niagara UI/compiler. Use the same creation path as NiagaraStackGraphUtilities
        // when the private declarations are available.
        if (NodeClass->IsChildOf(UNiagaraNodeParameterMapSet::StaticClass()))
        {
            return N2CNiagaraCreateTypedGraphNode<UNiagaraNodeParameterMapSet>(Graph, X, Y, TEXT("NiagaraNodeParameterMapSet"));
        }
        if (NodeClass->IsChildOf(UNiagaraNodeParameterMapGet::StaticClass()))
        {
            return N2CNiagaraCreateTypedGraphNode<UNiagaraNodeParameterMapGet>(Graph, X, Y, TEXT("NiagaraNodeParameterMapGet"));
        }
        if (NodeClass->IsChildOf(UNiagaraNodeCustomHlsl::StaticClass()))
        {
            return N2CNiagaraCreateTypedGraphNode<UNiagaraNodeCustomHlsl>(Graph, X, Y, TEXT("NiagaraNodeCustomHlsl"));
        }
#endif

        // Reflection fallback: keep this only for unknown node classes. Do not rely on this path
        // for ParameterMapSet/Get in UE4.27, because it can create visually invalid Niagara nodes.
        Graph->Modify();
        UEdGraphNode* Node = NewObject<UEdGraphNode>(Graph, NodeClass, NAME_None, RF_Transactional);
        if (!Node)
        {
            return nullptr;
        }

        Node->CreateNewGuid();
        Node->NodePosX = X;
        Node->NodePosY = Y;
        Graph->AddNode(Node, false, false);
        Node->AllocateDefaultPins();
        N2CEnsureNiagaraParameterMapPins(Node);
        Node->Modify();
        N2CNiagaraGraphImportTrace(FString::Printf(TEXT("Create fallback node end: %s pins=%d"), *ClassName, Node->Pins.Num()));
        return Node;
    }

    static FString N2CGetNiagaraFunctionNodeName(UNiagaraNodeFunctionCall* Node)
    {
        if (!Node)
        {
            return FString();
        }
        FString Name = Node->GetFunctionName();
        if (!Name.IsEmpty())
        {
            return Name;
        }
        if (Node->FunctionScript)
        {
            return Node->FunctionScript->GetName();
        }
        return Node->GetName();
    }

    static bool N2CNiagaraFunctionNameMatches(UNiagaraNodeFunctionCall* Node, const FString& RequestedName)
    {
        if (!Node || RequestedName.IsEmpty())
        {
            return false;
        }
        const FString NodeName = N2CGetNiagaraFunctionNodeName(Node);
        if (NodeName.Equals(RequestedName, ESearchCase::IgnoreCase))
        {
            return true;
        }
        if (Node->FunctionScript && Node->FunctionScript->GetName().Equals(RequestedName, ESearchCase::IgnoreCase))
        {
            return true;
        }
        return NodeName.Contains(RequestedName) || RequestedName.Contains(NodeName);
    }

    static bool N2CIsNiagaraParticleSpawnUsage(ENiagaraScriptUsage Usage)
    {
        return Usage == ENiagaraScriptUsage::ParticleSpawnScript
            || Usage == ENiagaraScriptUsage::ParticleSpawnScriptInterpolated;
    }

    static bool N2CNiagaraScriptUsageMatches(ENiagaraScriptUsage Actual, ENiagaraScriptUsage Requested)
    {
        if (N2CIsNiagaraParticleSpawnUsage(Actual) && N2CIsNiagaraParticleSpawnUsage(Requested))
        {
            return true;
        }
        return Actual == Requested;
    }

    static void N2CGetOrderedNiagaraModuleNodesFromOutput(UEdGraphNode* OutputNode, TArray<UNiagaraNodeFunctionCall*>& OutModuleNodes)
    {
        OutModuleNodes.Empty();
        UEdGraphNode* PreviousNode = OutputNode;
        while (PreviousNode)
        {
            UEdGraphPin* PreviousInputPin = N2CFindNiagaraParameterMapPin(PreviousNode, EGPD_Input);
            if (PreviousInputPin && PreviousInputPin->LinkedTo.Num() == 1 && N2CGetSingleLinkedPinSafe(PreviousInputPin))
            {
                UEdGraphNode* CurrentNode = N2CGetSingleLinkedPinSafe(PreviousInputPin)->GetOwningNode();
                if (UNiagaraNodeFunctionCall* ModuleNode = Cast<UNiagaraNodeFunctionCall>(CurrentNode))
                {
                    OutModuleNodes.Insert(ModuleNode, 0);
                }
                PreviousNode = CurrentNode;
            }
            else
            {
                PreviousNode = nullptr;
            }
        }
    }

    static void N2CCollectNiagaraModuleNodesForUsage(UEdGraph* Graph, ENiagaraScriptUsage Usage, const FString& ModuleName, TArray<UNiagaraNodeFunctionCall*>& OutModuleNodes)
    {
        OutModuleNodes.Empty();
        if (!Graph)
        {
            return;
        }

        for (UEdGraphNode* Node : Graph->Nodes)
        {
            UNiagaraNodeOutput* OutputNode = Cast<UNiagaraNodeOutput>(Node);
            if (!OutputNode || !N2CNiagaraScriptUsageMatches(OutputNode->GetUsage(), Usage))
            {
                continue;
            }

            TArray<UNiagaraNodeFunctionCall*> OrderedModules;
            N2CGetOrderedNiagaraModuleNodesFromOutput(OutputNode, OrderedModules);
            for (UNiagaraNodeFunctionCall* ModuleNode : OrderedModules)
            {
                if (N2CNiagaraFunctionNameMatches(ModuleNode, ModuleName))
                {
                    OutModuleNodes.Add(ModuleNode);
                }
            }
        }
    }


    static bool N2CReorderNiagaraModulesForUsage(UEdGraph* Graph, ENiagaraScriptUsage Usage, const TArray<FString>& DesiredOrder, FString& OutReason)
    {
        if (!Graph)
        {
            OutReason = TEXT("missing Niagara graph for reorder");
            return false;
        }
        if (DesiredOrder.Num() <= 0)
        {
            OutReason = TEXT("empty desired module order");
            return false;
        }

        UNiagaraNodeOutput* OutputNode = nullptr;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            UNiagaraNodeOutput* CandidateOutput = Cast<UNiagaraNodeOutput>(Node);
            if (CandidateOutput && N2CNiagaraScriptUsageMatches(CandidateOutput->GetUsage(), Usage))
            {
                OutputNode = CandidateOutput;
                break;
            }
        }
        if (!OutputNode)
        {
            OutReason = TEXT("missing Niagara output node for reorder");
            return false;
        }

        TArray<UNiagaraNodeFunctionCall*> CurrentOrder;
        N2CGetOrderedNiagaraModuleNodesFromOutput(OutputNode, CurrentOrder);
        if (CurrentOrder.Num() <= 1)
        {
            return true;
        }

        TArray<UNiagaraNodeFunctionCall*> NewOrder;
        TSet<UNiagaraNodeFunctionCall*> Used;
        for (const FString& DesiredName : DesiredOrder)
        {
            for (UNiagaraNodeFunctionCall* ModuleNode : CurrentOrder)
            {
                if (ModuleNode && !Used.Contains(ModuleNode) && N2CNiagaraFunctionNameMatches(ModuleNode, DesiredName))
                {
                    NewOrder.Add(ModuleNode);
                    Used.Add(ModuleNode);
                    break;
                }
            }
        }
        for (UNiagaraNodeFunctionCall* ModuleNode : CurrentOrder)
        {
            if (ModuleNode && !Used.Contains(ModuleNode))
            {
                NewOrder.Add(ModuleNode);
            }
        }
        if (NewOrder.Num() != CurrentOrder.Num())
        {
            OutReason = TEXT("reorder internal mismatch");
            return false;
        }

        UEdGraphPin* OldFirstInputPin = N2CFindNiagaraParameterMapPin(CurrentOrder[0], EGPD_Input);
        UEdGraphPin* UpstreamMapPin = OldFirstInputPin ? N2CGetSingleLinkedPinSafe(OldFirstInputPin) : nullptr;
        UEdGraphPin* OutputInputPin = N2CFindNiagaraParameterMapPin(OutputNode, EGPD_Input);
        if (!UpstreamMapPin || !OutputInputPin)
        {
            OutReason = TEXT("could not resolve parameter map chain pins for reorder");
            return false;
        }

        Graph->Modify();
        OutputNode->Modify();
        OutputInputPin->BreakAllPinLinks(false);
        for (UNiagaraNodeFunctionCall* ModuleNode : CurrentOrder)
        {
            if (!ModuleNode)
            {
                continue;
            }
            ModuleNode->Modify();
            if (UEdGraphPin* InPin = N2CFindNiagaraParameterMapPin(ModuleNode, EGPD_Input))
            {
                InPin->BreakAllPinLinks(false);
            }
            if (UEdGraphPin* OutPin = N2CFindNiagaraParameterMapPin(ModuleNode, EGPD_Output))
            {
                OutPin->BreakAllPinLinks(false);
            }
        }

        UEdGraphPin* PreviousOutputPin = UpstreamMapPin;
        for (UNiagaraNodeFunctionCall* ModuleNode : NewOrder)
        {
            UEdGraphPin* InPin = N2CFindNiagaraParameterMapPin(ModuleNode, EGPD_Input);
            UEdGraphPin* OutPin = N2CFindNiagaraParameterMapPin(ModuleNode, EGPD_Output);
            if (!InPin || !OutPin || !PreviousOutputPin)
            {
                OutReason = FString::Printf(TEXT("could not resolve parameter map pins while reordering module %s"), *N2CGetNiagaraFunctionNodeName(ModuleNode));
                return false;
            }
            N2CNiagaraMakeLink(PreviousOutputPin, InPin);
            PreviousOutputPin = OutPin;
        }
        if (!PreviousOutputPin)
        {
            OutReason = TEXT("no final module output pin after reorder");
            return false;
        }
        N2CNiagaraMakeLink(PreviousOutputPin, OutputInputPin);
        Graph->NotifyGraphChanged();
        return true;
    }

    static bool N2CRemoveNiagaraModuleNodeDirect(UNiagaraNodeFunctionCall* ModuleNode)
    {
        if (!ModuleNode)
        {
            return false;
        }

        UEdGraph* Graph = ModuleNode->GetGraph();
        if (!Graph)
        {
            return false;
        }

        UEdGraphPin* ModuleInputMapPin = N2CFindNiagaraParameterMapPin(ModuleNode, EGPD_Input);
        UEdGraphPin* ModuleOutputMapPin = N2CFindNiagaraParameterMapPin(ModuleNode, EGPD_Output);

        UEdGraphPin* PreviousMapPin = nullptr;
        if (ModuleInputMapPin && ModuleInputMapPin->LinkedTo.Num() > 0)
        {
            PreviousMapPin = N2CGetSingleLinkedPinSafe(ModuleInputMapPin);
            if (!PreviousMapPin)
            {
                PreviousMapPin = ModuleInputMapPin->LinkedTo[0];
            }
        }

        TArray<UEdGraphPin*> NextMapPins;
        if (ModuleOutputMapPin)
        {
            for (UEdGraphPin* LinkedPin : ModuleOutputMapPin->LinkedTo)
            {
                if (LinkedPin)
                {
                    NextMapPins.Add(LinkedPin);
                }
            }
        }

        Graph->Modify();
        ModuleNode->Modify();

        ModuleNode->BreakAllNodeLinks();

        if (PreviousMapPin)
        {
            for (UEdGraphPin* NextMapPin : NextMapPins)
            {
                if (NextMapPin && NextMapPin != PreviousMapPin)
                {
                    PreviousMapPin->MakeLinkTo(NextMapPin);
                }
            }
        }

        Graph->RemoveNode(ModuleNode);
        Graph->NotifyGraphChanged();
        return true;
    }

    static UNiagaraScriptSource* N2CGetNiagaraSystemScriptSourceForUsage(UNiagaraSystem* System, ENiagaraScriptUsage Usage)
    {
        if (!System)
        {
            return nullptr;
        }
        if (Usage == ENiagaraScriptUsage::SystemSpawnScript && System->GetSystemSpawnScript())
        {
            return Cast<UNiagaraScriptSource>(System->GetSystemSpawnScript()->GetSource(FGuid()));
        }
        if (Usage == ENiagaraScriptUsage::SystemUpdateScript && System->GetSystemUpdateScript())
        {
            return Cast<UNiagaraScriptSource>(System->GetSystemUpdateScript()->GetSource(FGuid()));
        }
        return nullptr;
    }

    static bool N2CFindRapidIterationInputTypeInScript(UNiagaraScript* Script, const FString& ModuleName, const FString& InputName, FNiagaraTypeDefinition& OutType)
    {
        if (!Script)
        {
            return false;
        }

        TArray<FNiagaraVariable> Parameters;
        Script->RapidIterationParameters.GetParameters(Parameters);
        const FString WantedSuffix = FString::Printf(TEXT(".%s.%s"), *ModuleName, *InputName);
        for (const FNiagaraVariable& Variable : Parameters)
        {
            const FString VarName = Variable.GetName().ToString();
            if (VarName.EndsWith(WantedSuffix) || VarName.Contains(WantedSuffix))
            {
                OutType = Variable.GetType();
                return OutType.IsValid();
            }
        }
        return false;
    }

    static bool N2CFindNiagaraInputType(UNiagaraSystem* System, UNiagaraEmitter* Emitter, ENiagaraScriptUsage Usage, bool bSystemStage, const FString& ModuleName, const FString& InputName, const TSharedPtr<FJsonObject>& Action, FNiagaraTypeDefinition& OutType, FString& OutReason)
    {
        // v19: do not create arbitrary ParameterMapSet override pins just because JSON supplied a type.
        // UE/Niagara will keep such pins as "Invalid Input Override" if the module does not expose that
        // exact input. First verify the real RapidIteration parameter exists. JSON type is only a fallback
        // when the action explicitly opts into force_create_input/allow_missing_input for experimental assets.
        if (bSystemStage)
        {
            if (Usage == ENiagaraScriptUsage::SystemSpawnScript && N2CFindRapidIterationInputTypeInScript(System ? System->GetSystemSpawnScript() : nullptr, ModuleName, InputName, OutType))
            {
                return true;
            }
            if (Usage == ENiagaraScriptUsage::SystemUpdateScript && N2CFindRapidIterationInputTypeInScript(System ? System->GetSystemUpdateScript() : nullptr, ModuleName, InputName, OutType))
            {
                return true;
            }
        }
        else if (Emitter)
        {
#if WITH_EDITORONLY_DATA
            if (Usage == ENiagaraScriptUsage::EmitterSpawnScript && N2CFindRapidIterationInputTypeInScript(Emitter->EmitterSpawnScriptProps.Script, ModuleName, InputName, OutType))
            {
                return true;
            }
            if (Usage == ENiagaraScriptUsage::EmitterUpdateScript && N2CFindRapidIterationInputTypeInScript(Emitter->EmitterUpdateScriptProps.Script, ModuleName, InputName, OutType))
            {
                return true;
            }
#endif
            if (N2CIsNiagaraParticleSpawnUsage(Usage) && N2CFindRapidIterationInputTypeInScript(Emitter->SpawnScriptProps.Script, ModuleName, InputName, OutType))
            {
                return true;
            }
            if (Usage == ENiagaraScriptUsage::ParticleUpdateScript && N2CFindRapidIterationInputTypeInScript(Emitter->UpdateScriptProps.Script, ModuleName, InputName, OutType))
            {
                return true;
            }
        }

        bool bAllowMissingInput = false;
        if (Action.IsValid())
        {
            Action->TryGetBoolField(TEXT("allow_missing_input"), bAllowMissingInput);
            bool bForceCreateInput = false;
            if (Action->TryGetBoolField(TEXT("force_create_input"), bForceCreateInput) && bForceCreateInput)
            {
                bAllowMissingInput = true;
            }
        }

        FString TypeString;
        if (Action.IsValid() && (Action->TryGetStringField(TEXT("type"), TypeString) || Action->TryGetStringField(TEXT("input_type"), TypeString)))
        {
            const bool bKnownUnsafeAlias = ModuleName.Equals(TEXT("ScaleColor"), ESearchCase::IgnoreCase);
            if ((bAllowMissingInput || !bKnownUnsafeAlias) && TryNiagaraTypeFromString(TypeString, OutType))
            {
                return true;
            }
        }

        OutReason = FString::Printf(TEXT("Niagara input %s.%s is not exposed by the module; skipped to avoid Invalid Input Override"), *ModuleName, *InputName);
        return false;
    }

    static UEdGraphPin* N2CGetOrCreateNiagaraOverridePin(UNiagaraNodeFunctionCall* ModuleNode, const FString& InputAlias, const FNiagaraTypeDefinition& InputType, FString& OutReason)
    {
        if (!ModuleNode)
        {
            OutReason = TEXT("missing module node");
            return nullptr;
        }

        UEdGraphPin* ModuleInputPin = N2CFindNiagaraParameterMapPin(ModuleNode, EGPD_Input);
        if (!ModuleInputPin)
        {
            OutReason = FString::Printf(TEXT("module %s has no parameter map input pin"), *N2CGetNiagaraFunctionNodeName(ModuleNode));
            return nullptr;
        }
        if (!N2CGetSingleLinkedPinSafe(ModuleInputPin))
        {
            OutReason = FString::Printf(TEXT("module %s has no upstream parameter map link"), *N2CGetNiagaraFunctionNodeName(ModuleNode));
            return nullptr;
        }

        UEdGraphNode* OverrideNode = nullptr;
        UEdGraphPin* LinkedInputPin = N2CGetSingleLinkedPinSafe(ModuleInputPin);
        if (LinkedInputPin && N2CIsNiagaraNodeClassNamed(LinkedInputPin->GetOwningNode(), TEXT("NiagaraNodeParameterMapSet")))
        {
            OverrideNode = LinkedInputPin->GetOwningNode();
        }

        if (!OverrideNode)
        {
            UEdGraph* Graph = ModuleNode->GetGraph();
            UClass* MapSetClass = N2CFindNiagaraEditorNodeClass(TEXT("NiagaraNodeParameterMapSet"));
            N2CNiagaraGraphImportTrace(FString::Printf(TEXT("Override node missing; creating ParameterMapSet for module=%s input=%s"), *N2CGetNiagaraFunctionNodeName(ModuleNode), *InputAlias));
            OverrideNode = N2CNiagaraCreateGraphNode(Graph, MapSetClass, ModuleNode->NodePosX - 260, ModuleNode->NodePosY + 120);
            if (!OverrideNode)
            {
                OutReason = TEXT("failed to create NiagaraNodeParameterMapSet");
                return nullptr;
            }

            UEdGraphPin* OverrideInputPin = N2CFindNiagaraParameterMapPin(OverrideNode, EGPD_Input);
            UEdGraphPin* OverrideOutputPin = N2CFindNiagaraParameterMapPin(OverrideNode, EGPD_Output);
            UEdGraphPin* PreviousStackNodeOutputPin = N2CGetSingleLinkedPinSafe(ModuleInputPin);
            if (!OverrideInputPin || !OverrideOutputPin || !PreviousStackNodeOutputPin)
            {
                OutReason = TEXT("created override node but could not resolve parameter map pins");
                return nullptr;
            }

            TArray<UEdGraphPin*> PreviousLinkedPins = PreviousStackNodeOutputPin->LinkedTo;
            N2CNiagaraGraphImportTrace(FString::Printf(TEXT("Rewire stack begin: module=%s previous_links=%d"), *N2CGetNiagaraFunctionNodeName(ModuleNode), PreviousLinkedPins.Num()));
            N2CNiagaraBreakAllPinLinks(ModuleInputPin);
            N2CNiagaraMakeLink(ModuleInputPin, OverrideOutputPin);
            for (UEdGraphPin* PreviousLinkedPin : PreviousLinkedPins)
            {
                if (PreviousLinkedPin && PreviousLinkedPin != OverrideInputPin && PreviousLinkedPin != ModuleInputPin)
                {
                    N2CNiagaraMakeLink(PreviousLinkedPin, OverrideOutputPin);
                }
            }
            N2CNiagaraBreakAllPinLinks(PreviousStackNodeOutputPin);
            N2CNiagaraMakeLink(PreviousStackNodeOutputPin, OverrideInputPin);
            N2CNiagaraGraphImportTrace(FString::Printf(TEXT("Rewire stack end: module=%s"), *N2CGetNiagaraFunctionNodeName(ModuleNode)));
        }

        for (UEdGraphPin* Pin : OverrideNode->Pins)
        {
            if (Pin && Pin->Direction == EGPD_Input && Pin->PinName.ToString().Equals(InputAlias, ESearchCase::IgnoreCase))
            {
                return Pin;
            }
        }

        const UEdGraphSchema_Niagara* NiagaraSchema = GetDefault<UEdGraphSchema_Niagara>();
        if (!NiagaraSchema)
        {
            OutReason = TEXT("Niagara schema unavailable");
            return nullptr;
        }
        FEdGraphPinType PinType = UEdGraphSchema_Niagara::TypeDefinitionToPinType(InputType);
        int32 InsertIndex = OverrideNode->Pins.Num();
        for (int32 PinIndex = 0; PinIndex < OverrideNode->Pins.Num(); ++PinIndex)
        {
            UEdGraphPin* Pin = OverrideNode->Pins[PinIndex];
            if (Pin && Pin->Direction == EGPD_Input && Pin->PinName.ToString().Contains(TEXT("Add")))
            {
                InsertIndex = PinIndex;
                break;
            }
        }
        InsertIndex = FMath::Clamp(InsertIndex, 0, OverrideNode->Pins.Num());
        N2CNiagaraGraphImportTrace(FString::Printf(TEXT("Create override input pin begin: %s insert=%d existing_pins=%d"), *InputAlias, InsertIndex, OverrideNode->Pins.Num()));
        UEdGraphPin* NewPin = OverrideNode->CreatePin(EGPD_Input, PinType, FName(*InputAlias), InsertIndex);
        N2CNiagaraGraphImportTrace(FString::Printf(TEXT("Create override input pin end: %s result=%s pins=%d"), *InputAlias, NewPin ? TEXT("ok") : TEXT("null"), OverrideNode->Pins.Num()));
        if (!NewPin)
        {
            OutReason = FString::Printf(TEXT("failed to create override pin %s"), *InputAlias);
        }
        return NewPin;
    }

    static bool N2CApplyNiagaraUserParameterLink(UEdGraphPin* OverridePin, const FString& UserParameterName, const FNiagaraTypeDefinition& InputType, FString& OutReason)
    {
        if (!OverridePin)
        {
            OutReason = TEXT("missing override pin");
            return false;
        }
        if (UserParameterName.IsEmpty())
        {
            OutReason = TEXT("missing user parameter name");
            return false;
        }

        UEdGraph* Graph = OverridePin->GetOwningNode() ? OverridePin->GetOwningNode()->GetGraph() : nullptr;
        UClass* MapGetClass = N2CFindNiagaraEditorNodeClass(TEXT("NiagaraNodeParameterMapGet"));
        N2CNiagaraGraphImportTrace(FString::Printf(TEXT("Create ParameterMapGet for user param: %s"), *UserParameterName));
        UEdGraphNode* GetNode = N2CNiagaraCreateGraphNode(Graph, MapGetClass, OverridePin->GetOwningNode()->NodePosX - 260, OverridePin->GetOwningNode()->NodePosY + 80);
        if (!GetNode)
        {
            OutReason = TEXT("failed to create NiagaraNodeParameterMapGet");
            return false;
        }

        UEdGraphPin* GetInputPin = N2CFindNiagaraParameterMapPin(GetNode, EGPD_Input);
        UEdGraphPin* OverrideNodeInputPin = N2CFindNiagaraParameterMapPin(OverridePin->GetOwningNode(), EGPD_Input);
        UEdGraphPin* PreviousStackNodeOutputPin = N2CGetSingleLinkedPinSafe(OverrideNodeInputPin);
        if (!GetInputPin || !PreviousStackNodeOutputPin)
        {
            OutReason = TEXT("created ParameterMapGet but could not resolve parameter map pins");
            return false;
        }

        const FEdGraphPinType PinType = UEdGraphSchema_Niagara::TypeDefinitionToPinType(InputType);
        N2CNiagaraGraphImportTrace(FString::Printf(TEXT("Create ParameterMapGet output pin begin: %s"), *UserParameterName));
        UEdGraphPin* GetOutputPin = GetNode->CreatePin(EGPD_Output, PinType, FName(*UserParameterName));
        N2CNiagaraGraphImportTrace(FString::Printf(TEXT("Create ParameterMapGet output pin end: %s result=%s"), *UserParameterName, GetOutputPin ? TEXT("ok") : TEXT("null")));
        if (!GetOutputPin)
        {
            OutReason = FString::Printf(TEXT("failed to create ParameterMapGet output %s"), *UserParameterName);
            return false;
        }

        N2CNiagaraGraphImportTrace(FString::Printf(TEXT("Link user parameter begin: %s"), *UserParameterName));
        N2CNiagaraBreakAllPinLinks(OverridePin);
        N2CNiagaraMakeLink(GetInputPin, PreviousStackNodeOutputPin);
        N2CNiagaraMakeLink(GetOutputPin, OverridePin);
        N2CNiagaraGraphImportTrace(FString::Printf(TEXT("Link user parameter end: %s"), *UserParameterName));
        return true;
    }

    static void N2CSetUObjectEnumPropertyByName(UObject* Obj, const TCHAR* PropertyName, int64 Value)
    {
        if (!Obj)
        {
            return;
        }
        if (FByteProperty* ByteProp = FindFProperty<FByteProperty>(Obj->GetClass(), PropertyName))
        {
            ByteProp->SetPropertyValue_InContainer(Obj, static_cast<uint8>(Value));
            return;
        }
        if (FEnumProperty* EnumProp = FindFProperty<FEnumProperty>(Obj->GetClass(), PropertyName))
        {
            EnumProp->GetUnderlyingProperty()->SetIntPropertyValue(EnumProp->ContainerPtrToValuePtr<void>(Obj), Value);
        }
    }

    static void N2CSetUObjectStringPropertyByName(UObject* Obj, const TCHAR* PropertyName, const FString& Value)
    {
        if (!Obj)
        {
            return;
        }
        if (FStrProperty* StrProp = FindFProperty<FStrProperty>(Obj->GetClass(), PropertyName))
        {
            StrProp->SetPropertyValue_InContainer(Obj, Value);
        }
    }

    static bool N2CIsNiagaraDynamicAddPin(const UEdGraphPin* Pin)
    {
        return Pin && Pin->PinType.PinCategory == UEdGraphSchema_Niagara::PinCategoryMisc && Pin->PinType.PinSubCategory == FName(TEXT("DynamicAddPin"));
    }

    static UEdGraphPin* N2CFindNiagaraNamedDataPin(UEdGraphNode* Node, EEdGraphPinDirection Direction, const FString& PinName)
    {
        if (!Node)
        {
            return nullptr;
        }
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin && Pin->Direction == Direction && !N2CIsNiagaraDynamicAddPin(Pin) && Pin->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase))
            {
                return Pin;
            }
        }
        return nullptr;
    }

    static UEdGraphPin* N2CFindFirstNiagaraTypedOutputPin(UEdGraphNode* Node)
    {
        if (!Node)
        {
            return nullptr;
        }
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin && Pin->Direction == EGPD_Output && !N2CIsNiagaraParameterMapPin(Pin) && !N2CIsNiagaraDynamicAddPin(Pin))
            {
                return Pin;
            }
        }
        return nullptr;
    }

    struct FN2CNiagaraFormulaInputBinding
    {
        FString LocalName;
        FString UserParameter;
        FNiagaraTypeDefinition Type;
    };

    static FString N2CMakeSafeNiagaraFormulaLocalName(const FString& InName)
    {
        FString Result = InName;
        if (Result.StartsWith(TEXT("User.")))
        {
            Result = Result.RightChop(5);
        }
        if (Result.StartsWith(TEXT("N2C_")))
        {
            Result = Result.RightChop(4);
        }

        FString Clean;
        for (int32 CharIndex = 0; CharIndex < Result.Len(); ++CharIndex)
        {
            const TCHAR Ch = Result[CharIndex];
            const bool bAlphaNum = (Ch >= TEXT('A') && Ch <= TEXT('Z')) || (Ch >= TEXT('a') && Ch <= TEXT('z')) || (Ch >= TEXT('0') && Ch <= TEXT('9'));
            Clean.AppendChar((bAlphaNum || Ch == TEXT('_')) ? Ch : TEXT('_'));
        }
        if (Clean.IsEmpty())
        {
            Clean = TEXT("InputValue");
        }
        if (Clean[0] >= TEXT('0') && Clean[0] <= TEXT('9'))
        {
            Clean = TEXT("Input_") + Clean;
        }
        return Clean;
    }

    static bool N2CCollectNiagaraFormulaInputBindings(const TSharedPtr<FJsonObject>& Action, TArray<FN2CNiagaraFormulaInputBinding>& OutBindings, FString& OutExpression, FString& OutReason)
    {
        OutBindings.Empty();
        if (!Action.IsValid())
        {
            return true;
        }

        const TArray<TSharedPtr<FJsonValue>>* InputsArray = nullptr;
        if (!Action->TryGetArrayField(TEXT("formula_inputs"), InputsArray))
        {
            Action->TryGetArrayField(TEXT("custom_hlsl_inputs"), InputsArray);
        }
        if (!InputsArray)
        {
            Action->TryGetArrayField(TEXT("inputs"), InputsArray);
        }

        TSet<FString> UsedLocalNames;
        if (InputsArray)
        {
            for (const TSharedPtr<FJsonValue>& InputValue : *InputsArray)
            {
                TSharedPtr<FJsonObject> InputObj = InputValue.IsValid() ? InputValue->AsObject() : nullptr;
                if (!InputObj.IsValid())
                {
                    continue;
                }

                FString UserParameter;
                FString LocalName;
                FString TypeString;
                GetNiagaraActionStringAny(InputObj, TEXT("user_parameter"), TEXT("parameter"), UserParameter);
                GetNiagaraActionStringAny(InputObj, TEXT("name"), TEXT("local_name"), LocalName);
                InputObj->TryGetStringField(TEXT("type"), TypeString);
                if (UserParameter.IsEmpty())
                {
                    continue;
                }
                if (!UserParameter.StartsWith(TEXT("User.")))
                {
                    UserParameter = TEXT("User.") + UserParameter;
                }
                if (LocalName.IsEmpty())
                {
                    LocalName = N2CMakeSafeNiagaraFormulaLocalName(UserParameter);
                }
                LocalName = N2CMakeSafeNiagaraFormulaLocalName(LocalName);

                FString BaseLocalName = LocalName;
                int32 Suffix = 1;
                while (UsedLocalNames.Contains(LocalName))
                {
                    LocalName = FString::Printf(TEXT("%s_%d"), *BaseLocalName, Suffix++);
                }
                UsedLocalNames.Add(LocalName);

                FNiagaraTypeDefinition TypeDef = FNiagaraTypeDefinition::GetFloatDef();
                if (!TypeString.IsEmpty())
                {
                    FNiagaraTypeDefinition ParsedType;
                    if (!TryNiagaraTypeFromString(TypeString, ParsedType))
                    {
                        OutReason = FString::Printf(TEXT("unsupported custom_hlsl input type %s for %s"), *TypeString, *UserParameter);
                        return false;
                    }
                    TypeDef = ParsedType;
                }

                FN2CNiagaraFormulaInputBinding Binding;
                Binding.LocalName = LocalName;
                Binding.UserParameter = UserParameter;
                Binding.Type = TypeDef;
                OutBindings.Add(Binding);
            }
        }

        // UE4.27 Custom HLSL turns function input pins into HLSL parameters named In_<PinName>.
        // ProcessCustomHlsl normally rewrites local pin tokens to In_<PinName>, but imported nodes can
        // become stale after a manual edit/refresh if the editor rebuilds pins/signature in a slightly
        // different order. Writing the HLSL-side names explicitly makes the formula stable both before
        // and after the user edits the Custom HLSL text in Niagara.
        for (const FN2CNiagaraFormulaInputBinding& Binding : OutBindings)
        {
            const FString HlslInputName = FString(TEXT("In_")) + Binding.LocalName;
            OutExpression.ReplaceInline(*Binding.UserParameter, *HlslInputName, ESearchCase::CaseSensitive);
            if (!OutExpression.Contains(HlslInputName, ESearchCase::CaseSensitive))
            {
                OutExpression.ReplaceInline(*Binding.LocalName, *HlslInputName, ESearchCase::CaseSensitive);
            }
        }
        return true;
    }


    static void N2CSetNiagaraOverridePinLiteral(UEdGraphPin* Pin, const FString& LiteralValue)
    {
        if (!Pin)
        {
            return;
        }
        Pin->Modify();
        Pin->DefaultValue = LiteralValue;
        Pin->AutogeneratedDefaultValue = LiteralValue;
        Pin->DefaultTextValue = FText::FromString(LiteralValue);
    }

    static UNiagaraScript* N2CLoadNiagaraScriptAsset(const FString& InScriptPath)
    {
        if (InScriptPath.IsEmpty())
        {
            return nullptr;
        }

        FString ScriptPath = InScriptPath;
        ScriptPath.TrimStartAndEndInline();
        if (ScriptPath.StartsWith(TEXT("NiagaraScript'")) && ScriptPath.EndsWith(TEXT("'")))
        {
            ScriptPath = ScriptPath.Mid(14, ScriptPath.Len() - 15);
        }

        UNiagaraScript* Script = LoadObject<UNiagaraScript>(nullptr, *ScriptPath);
        if (!Script && !ScriptPath.StartsWith(TEXT("/")))
        {
            Script = LoadObject<UNiagaraScript>(nullptr, *FString::Printf(TEXT("/Niagara/%s.%s"), *ScriptPath, *FPaths::GetBaseFilename(ScriptPath)));
        }
        return Script;
    }

    static UNiagaraNodeFunctionCall* N2CCreateNiagaraDynamicInputFunctionNode(UEdGraphPin* TargetOverridePin, const FString& FunctionPath, const FNiagaraTypeDefinition& OutputType, FString& OutReason)
    {
        if (!TargetOverridePin || !TargetOverridePin->GetOwningNode())
        {
            OutReason = TEXT("missing target override pin for dynamic input tree");
            return nullptr;
        }

        UEdGraph* Graph = TargetOverridePin->GetOwningNode()->GetGraph();
        if (!Graph)
        {
            OutReason = TEXT("missing Niagara graph for dynamic input tree");
            return nullptr;
        }

        UNiagaraScript* DynamicInputScript = N2CLoadNiagaraScriptAsset(FunctionPath);
        if (!DynamicInputScript)
        {
            OutReason = FString::Printf(TEXT("failed to load dynamic input script: %s"), *FunctionPath);
            return nullptr;
        }

        Graph->Modify();

        // UNiagaraNodeFunctionCall is public/exported in UE4.27, unlike ParameterMapGet/Set.
        // Use the standard graph-node creator for FunctionCall only; keep ParameterMapGet/Set on the
        // reflection-safe path used elsewhere in this importer.
        FGraphNodeCreator<UNiagaraNodeFunctionCall> NodeCreator(*Graph);
        UNiagaraNodeFunctionCall* FunctionNode = NodeCreator.CreateNode();
        if (!FunctionNode)
        {
            OutReason = FString::Printf(TEXT("failed to create dynamic input function node for %s"), *FunctionPath);
            return nullptr;
        }

        FunctionNode->FunctionScript = DynamicInputScript;
        FunctionNode->NodePosX = TargetOverridePin->GetOwningNode()->NodePosX - 360;
        FunctionNode->NodePosY = TargetOverridePin->GetOwningNode()->NodePosY + 180;
        NodeCreator.Finalize();
        FunctionNode->Modify();

        UEdGraphPin* FunctionInputPin = N2CFindNiagaraParameterMapPin(FunctionNode, EGPD_Input);
        UEdGraphPin* FunctionOutputPin = N2CFindFirstNiagaraTypedOutputPin(FunctionNode);
        UEdGraphPin* OverrideNodeInputPin = N2CFindNiagaraParameterMapPin(TargetOverridePin->GetOwningNode(), EGPD_Input);
        UEdGraphPin* PreviousStackNodeOutputPin = N2CGetSingleLinkedPinSafe(OverrideNodeInputPin);

        if (!FunctionInputPin || !FunctionOutputPin || !PreviousStackNodeOutputPin)
        {
            OutReason = FString::Printf(TEXT("created dynamic input %s but could not resolve pins: map_in=%s out=%s prev=%s"),
                *FunctionPath,
                FunctionInputPin ? TEXT("ok") : TEXT("null"),
                FunctionOutputPin ? TEXT("ok") : TEXT("null"),
                PreviousStackNodeOutputPin ? TEXT("ok") : TEXT("null"));
            return nullptr;
        }

        N2CNiagaraBreakAllPinLinks(TargetOverridePin);
        N2CNiagaraMakeLink(FunctionInputPin, PreviousStackNodeOutputPin);
        N2CNiagaraMakeLink(FunctionOutputPin, TargetOverridePin);
        N2CNiagaraGraphImportTrace(FString::Printf(TEXT("Created dynamic input tree function %s output -> %s"), *N2CGetNiagaraFunctionNodeName(FunctionNode), *TargetOverridePin->PinName.ToString()));
        return FunctionNode;
    }

    static bool N2CGetJsonNumberOrStringLiteral(const TSharedPtr<FJsonObject>& Obj, const TCHAR* FieldName, FString& OutLiteral)
    {
        if (!Obj.IsValid() || !Obj->HasField(FieldName))
        {
            return false;
        }

        const TSharedPtr<FJsonValue> Value = Obj->TryGetField(FieldName);
        if (!Value.IsValid())
        {
            return false;
        }
        if (Value->Type == EJson::Number)
        {
            OutLiteral = FString::SanitizeFloat(Value->AsNumber());
            return true;
        }
        if (Value->Type == EJson::String)
        {
            OutLiteral = Value->AsString();
            return !OutLiteral.IsEmpty();
        }
        return false;
    }

    static FString N2CNormalizeNiagaraLinkedParameterName(const FString& InName)
    {
        FString Name = InName;
        Name.TrimStartAndEndInline();
        if (Name.IsEmpty())
        {
            return Name;
        }
        if (Name.Contains(TEXT(".")))
        {
            return Name;
        }
        return TEXT("User.") + Name;
    }

    static FString N2CNormalizeNiagaraDynamicInputChildName(const FString& FunctionPath, const FString& InChildInputName)
    {
        FString ChildInputName = InChildInputName;
        ChildInputName.TrimStartAndEndInline();

        // UE4.27 /Niagara/DynamicInputs/Angles/Sine exposes the input as
        // "Normalized Angle". Creating an override named "Sine.Angle" compiles the
        // graph but leaves Niagara with an "Invalid Input Override: Sine.Angle" warning
        // and the subtree is not actually used by the Sine node. Keep "Angle" as a JSON
        // convenience alias, but write the real exposed input name into the graph.
        if ((FunctionPath.Contains(TEXT("/DynamicInputs/Angles/Sine"), ESearchCase::IgnoreCase)
                || FunctionPath.EndsWith(TEXT("/Sine.Sine"), ESearchCase::IgnoreCase)
                || FunctionPath.Equals(TEXT("Sine"), ESearchCase::IgnoreCase))
            && ChildInputName.Equals(TEXT("Angle"), ESearchCase::IgnoreCase))
        {
            return TEXT("Normalized Angle");
        }

        return ChildInputName;
    }

    static FString N2CNormalizeNiagaraInputNameForCompare(const FString& InName)
    {
        FString Result;
        for (int32 Index = 0; Index < InName.Len(); ++Index)
        {
            const TCHAR Ch = InName[Index];
            const bool bAlphaNum = (Ch >= TEXT('A') && Ch <= TEXT('Z')) || (Ch >= TEXT('a') && Ch <= TEXT('z')) || (Ch >= TEXT('0') && Ch <= TEXT('9'));
            if (bAlphaNum)
            {
                Result.AppendChar(FChar::ToLower(Ch));
            }
        }
        return Result;
    }

    static bool N2CResolveNiagaraDynamicInputChildNameFromScript(UNiagaraNodeFunctionCall* FunctionNode, const FString& RequestedInputName, FString& OutInputName)
    {
        OutInputName = RequestedInputName;
        if (!FunctionNode || !FunctionNode->FunctionScript || RequestedInputName.IsEmpty())
        {
            return false;
        }

        const FString FunctionName = N2CGetNiagaraFunctionNodeName(FunctionNode);
        TArray<FNiagaraVariable> Parameters;
        FunctionNode->FunctionScript->RapidIterationParameters.GetParameters(Parameters);

        TArray<FString> CandidateInputs;
        for (const FNiagaraVariable& Variable : Parameters)
        {
            const FString VarName = Variable.GetName().ToString();
            FString Owner;
            FString Input;
            TArray<FString> Parts;
            VarName.ParseIntoArray(Parts, TEXT("."), true);
            if (Parts.Num() >= 2)
            {
                Owner = Parts[Parts.Num() - 2];
                Input = Parts[Parts.Num() - 1];
            }
            else
            {
                Input = VarName;
            }

            if (Input.IsEmpty())
            {
                continue;
            }
            if (!Owner.IsEmpty()
                && !Owner.Equals(FunctionName, ESearchCase::IgnoreCase)
                && !FunctionName.Contains(Owner, ESearchCase::IgnoreCase)
                && !Owner.Contains(FunctionName, ESearchCase::IgnoreCase))
            {
                continue;
            }
            CandidateInputs.AddUnique(Input);
        }

        for (const FString& Candidate : CandidateInputs)
        {
            if (Candidate.Equals(RequestedInputName, ESearchCase::IgnoreCase))
            {
                OutInputName = Candidate;
                return true;
            }
        }

        const FString RequestedNormalized = N2CNormalizeNiagaraInputNameForCompare(RequestedInputName);
        for (const FString& Candidate : CandidateInputs)
        {
            if (N2CNormalizeNiagaraInputNameForCompare(Candidate).Equals(RequestedNormalized, ESearchCase::IgnoreCase))
            {
                OutInputName = Candidate;
                return true;
            }
        }

        return false;
    }

    struct FN2CNiagaraDynamicTreeInputSpec
    {
        FString Name;
        FNiagaraTypeDefinition Type;

        FN2CNiagaraDynamicTreeInputSpec()
            : Type(FNiagaraTypeDefinition::GetFloatDef())
        {
        }

        FN2CNiagaraDynamicTreeInputSpec(const FString& InName, const FNiagaraTypeDefinition& InType)
            : Name(InName)
            , Type(InType)
        {
        }
    };

    static bool N2CGetNiagaraDynamicTreeOperatorSpec(const FString& InOperatorName, const FNiagaraTypeDefinition& ExpectedType, FString& OutNodeLabel, FString& OutExpression, FNiagaraTypeDefinition& OutOutputType, TArray<FN2CNiagaraDynamicTreeInputSpec>& OutInputs, FString& OutReason)
    {
        FString OperatorName = InOperatorName;
        OperatorName.TrimStartAndEndInline();
        OperatorName = OperatorName.ToLower();
        OperatorName.ReplaceInline(TEXT("-"), TEXT("_"));
        OperatorName.ReplaceInline(TEXT(" "), TEXT("_"));

        OutInputs.Empty();
        OutOutputType = ExpectedType.IsValid() ? ExpectedType : FNiagaraTypeDefinition::GetFloatDef();

        if (OperatorName == TEXT("normalized_age") || OperatorName == TEXT("particles_normalized_age") || OperatorName == TEXT("particle_normalized_age"))
        {
            OutNodeLabel = TEXT("N2C_NormalizedAge");
            OutExpression = TEXT("Particles.NormalizedAge");
            OutOutputType = FNiagaraTypeDefinition::GetFloatDef();
            return true;
        }
        if (OperatorName == TEXT("saturate") || OperatorName == TEXT("clamp01"))
        {
            OutNodeLabel = TEXT("N2C_Saturate");
            OutExpression = TEXT("saturate(In)");
            OutOutputType = FNiagaraTypeDefinition::GetFloatDef();
            OutInputs.Add(FN2CNiagaraDynamicTreeInputSpec(TEXT("In"), FNiagaraTypeDefinition::GetFloatDef()));
            return true;
        }
        if (OperatorName == TEXT("smoothstep") || OperatorName == TEXT("smooth_step"))
        {
            OutNodeLabel = TEXT("N2C_SmoothStep");
            OutExpression = TEXT("smoothstep(Edge0, Edge1, X)");
            OutOutputType = FNiagaraTypeDefinition::GetFloatDef();
            OutInputs.Add(FN2CNiagaraDynamicTreeInputSpec(TEXT("Edge0"), FNiagaraTypeDefinition::GetFloatDef()));
            OutInputs.Add(FN2CNiagaraDynamicTreeInputSpec(TEXT("Edge1"), FNiagaraTypeDefinition::GetFloatDef()));
            OutInputs.Add(FN2CNiagaraDynamicTreeInputSpec(TEXT("X"), FNiagaraTypeDefinition::GetFloatDef()));
            return true;
        }
        if (OperatorName == TEXT("one_minus") || OperatorName == TEXT("one_minus_float") || OperatorName == TEXT("invert") || OperatorName == TEXT("1_minus"))
        {
            OutNodeLabel = TEXT("N2C_OneMinus");
            OutExpression = TEXT("1.0 - X");
            OutOutputType = FNiagaraTypeDefinition::GetFloatDef();
            OutInputs.Add(FN2CNiagaraDynamicTreeInputSpec(TEXT("X"), FNiagaraTypeDefinition::GetFloatDef()));
            return true;
        }
        if (OperatorName == TEXT("multiply") || OperatorName == TEXT("mul") || OperatorName == TEXT("multiply_float"))
        {
            OutNodeLabel = TEXT("N2C_Multiply");
            OutExpression = TEXT("A * B");
            OutOutputType = FNiagaraTypeDefinition::GetFloatDef();
            OutInputs.Add(FN2CNiagaraDynamicTreeInputSpec(TEXT("A"), FNiagaraTypeDefinition::GetFloatDef()));
            OutInputs.Add(FN2CNiagaraDynamicTreeInputSpec(TEXT("B"), FNiagaraTypeDefinition::GetFloatDef()));
            return true;
        }
        if (OperatorName == TEXT("add") || OperatorName == TEXT("add_float"))
        {
            OutNodeLabel = TEXT("N2C_Add");
            OutExpression = TEXT("A + B");
            OutOutputType = FNiagaraTypeDefinition::GetFloatDef();
            OutInputs.Add(FN2CNiagaraDynamicTreeInputSpec(TEXT("A"), FNiagaraTypeDefinition::GetFloatDef()));
            OutInputs.Add(FN2CNiagaraDynamicTreeInputSpec(TEXT("B"), FNiagaraTypeDefinition::GetFloatDef()));
            return true;
        }
        if (OperatorName == TEXT("subtract") || OperatorName == TEXT("sub") || OperatorName == TEXT("subtract_float"))
        {
            OutNodeLabel = TEXT("N2C_Subtract");
            OutExpression = TEXT("A - B");
            OutOutputType = FNiagaraTypeDefinition::GetFloatDef();
            OutInputs.Add(FN2CNiagaraDynamicTreeInputSpec(TEXT("A"), FNiagaraTypeDefinition::GetFloatDef()));
            OutInputs.Add(FN2CNiagaraDynamicTreeInputSpec(TEXT("B"), FNiagaraTypeDefinition::GetFloatDef()));
            return true;
        }
        if (OperatorName == TEXT("lerp") || OperatorName == TEXT("lerp_float"))
        {
            OutNodeLabel = TEXT("N2C_LerpFloat");
            OutExpression = TEXT("lerp(A, B, Alpha)");
            OutOutputType = FNiagaraTypeDefinition::GetFloatDef();
            OutInputs.Add(FN2CNiagaraDynamicTreeInputSpec(TEXT("A"), FNiagaraTypeDefinition::GetFloatDef()));
            OutInputs.Add(FN2CNiagaraDynamicTreeInputSpec(TEXT("B"), FNiagaraTypeDefinition::GetFloatDef()));
            OutInputs.Add(FN2CNiagaraDynamicTreeInputSpec(TEXT("Alpha"), FNiagaraTypeDefinition::GetFloatDef()));
            return true;
        }

        OutReason = FString::Printf(TEXT("unsupported dynamic input tree operator: %s"), *InOperatorName);
        return false;
    }

    static UNiagaraNodeFunctionCall* N2CCreateNiagaraCustomHlslDynamicTreeNode(UEdGraphPin* TargetDataPin, const FString& NodeLabel, const FString& Expression, const FNiagaraTypeDefinition& OutputType, const TArray<FN2CNiagaraDynamicTreeInputSpec>& Inputs, int32 Depth, FString& OutReason)
    {
        if (!TargetDataPin || !TargetDataPin->GetOwningNode())
        {
            OutReason = TEXT("missing target pin for dynamic input operator");
            return nullptr;
        }

        UEdGraph* Graph = TargetDataPin->GetOwningNode()->GetGraph();
        if (!Graph)
        {
            OutReason = TEXT("missing graph for dynamic input operator");
            return nullptr;
        }

        UClass* CustomHlslClass = N2CFindNiagaraEditorNodeClass(TEXT("NiagaraNodeCustomHlsl"));
        if (!CustomHlslClass)
        {
            OutReason = TEXT("NiagaraNodeCustomHlsl class not found");
            return nullptr;
        }

        const int32 X = TargetDataPin->GetOwningNode()->NodePosX - 320 - Depth * 120;
        const int32 Y = TargetDataPin->GetOwningNode()->NodePosY + 90 + Depth * 140;
        UEdGraphNode* CustomNode = N2CNiagaraCreateGraphNode(Graph, CustomHlslClass, X, Y);
        if (!CustomNode)
        {
            OutReason = FString::Printf(TEXT("failed to create dynamic input operator node %s"), *NodeLabel);
            return nullptr;
        }

        N2CSetUObjectEnumPropertyByName(CustomNode, TEXT("ScriptUsage"), static_cast<int64>(ENiagaraScriptUsage::DynamicInput));
        N2CSetUObjectStringPropertyByName(CustomNode, TEXT("CustomHlsl"), Expression);

        UNiagaraNodeFunctionCall* FunctionCallNode = Cast<UNiagaraNodeFunctionCall>(CustomNode);
        if (!FunctionCallNode)
        {
            OutReason = FString::Printf(TEXT("dynamic input operator node %s is not a FunctionCall node"), *NodeLabel);
            return nullptr;
        }

        FunctionCallNode->Modify();
        FunctionCallNode->FunctionScript = nullptr;
        FunctionCallNode->Signature = FNiagaraFunctionSignature();
        FunctionCallNode->Signature.Name = *FString::Printf(TEXT("%s_%s"), *NodeLabel, *CustomNode->NodeGuid.ToString(EGuidFormats::Digits));
        FunctionCallNode->Signature.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetParameterMapDef(), TEXT("Map")));
        for (const FN2CNiagaraDynamicTreeInputSpec& Input : Inputs)
        {
            FunctionCallNode->Signature.Inputs.Add(FNiagaraVariable(Input.Type, FName(*Input.Name)));
        }
        FunctionCallNode->Signature.Outputs.Add(FNiagaraVariable(OutputType, TEXT("Result")));
        FunctionCallNode->Signature.bRequiresContext = true;
        FunctionCallNode->Signature.bSupportsCPU = true;
        FunctionCallNode->Signature.bSupportsGPU = true;

        for (UEdGraphPin* ExistingPin : CustomNode->Pins)
        {
            if (ExistingPin)
            {
                ExistingPin->BreakAllPinLinks(false);
            }
        }
        CustomNode->Pins.Empty();
        CustomNode->AllocateDefaultPins();

        UEdGraphPin* CustomInputPin = N2CFindNiagaraParameterMapPin(CustomNode, EGPD_Input);
        UEdGraphPin* CustomOutputPin = N2CFindFirstNiagaraTypedOutputPin(CustomNode);
        UEdGraphPin* OwningNodeInputPin = N2CFindNiagaraParameterMapPin(TargetDataPin->GetOwningNode(), EGPD_Input);
        UEdGraphPin* PreviousStackNodeOutputPin = N2CGetSingleLinkedPinSafe(OwningNodeInputPin);

        if (!CustomInputPin || !CustomOutputPin || !PreviousStackNodeOutputPin)
        {
            OutReason = FString::Printf(TEXT("created dynamic input operator %s but could not resolve pins: map_in=%s out=%s prev=%s"),
                *NodeLabel,
                CustomInputPin ? TEXT("ok") : TEXT("null"),
                CustomOutputPin ? TEXT("ok") : TEXT("null"),
                PreviousStackNodeOutputPin ? TEXT("ok") : TEXT("null"));
            return nullptr;
        }

        N2CNiagaraBreakAllPinLinks(TargetDataPin);
        N2CNiagaraMakeLink(CustomInputPin, PreviousStackNodeOutputPin);
        N2CNiagaraMakeLink(CustomOutputPin, TargetDataPin);
        N2CNiagaraGraphImportTrace(FString::Printf(TEXT("Created dynamic input operator node %s expr=%s"), *NodeLabel, *Expression));
        return FunctionCallNode;
    }

    static bool N2CApplyNiagaraDynamicInputTreeNode(UEdGraphPin* TargetOverridePin, const TSharedPtr<FJsonObject>& TreeNode, const FNiagaraTypeDefinition& ExpectedType, int32 Depth, FString& OutReason)
    {
        if (!TargetOverridePin)
        {
            OutReason = TEXT("dynamic input tree target pin is null");
            return false;
        }
        if (!TreeNode.IsValid())
        {
            OutReason = TEXT("dynamic input tree node is missing or invalid");
            return false;
        }
        if (Depth > 32)
        {
            OutReason = TEXT("dynamic input tree is too deep");
            return false;
        }

        FString Literal;
        if (N2CGetJsonNumberOrStringLiteral(TreeNode, TEXT("literal"), Literal) || N2CGetJsonNumberOrStringLiteral(TreeNode, TEXT("value"), Literal) || N2CGetJsonNumberOrStringLiteral(TreeNode, TEXT("constant"), Literal))
        {
            N2CNiagaraBreakAllPinLinks(TargetOverridePin);
            N2CSetNiagaraOverridePinLiteral(TargetOverridePin, Literal);
            return true;
        }

        FString LinkedParameter;
        if (GetNiagaraActionStringAny(TreeNode, TEXT("parameter"), TEXT("user_parameter"), LinkedParameter) || TreeNode->TryGetStringField(TEXT("linked_parameter"), LinkedParameter))
        {
            LinkedParameter = N2CNormalizeNiagaraLinkedParameterName(LinkedParameter);
            if (LinkedParameter.IsEmpty())
            {
                OutReason = TEXT("dynamic input tree linked parameter is empty");
                return false;
            }
            return N2CApplyNiagaraUserParameterLink(TargetOverridePin, LinkedParameter, ExpectedType, OutReason);
        }

        FString TypeString;
        FNiagaraTypeDefinition OutputType = ExpectedType;
        if (TreeNode->TryGetStringField(TEXT("type"), TypeString) || TreeNode->TryGetStringField(TEXT("output_type"), TypeString))
        {
            FNiagaraTypeDefinition ParsedType;
            if (!TryNiagaraTypeFromString(TypeString, ParsedType))
            {
                OutReason = FString::Printf(TEXT("unsupported dynamic input tree output type %s"), *TypeString);
                return false;
            }
            OutputType = ParsedType;
        }

        FString OperatorName;
        if (TreeNode->TryGetStringField(TEXT("operator"), OperatorName) || TreeNode->TryGetStringField(TEXT("tree_op"), OperatorName))
        {
            // v20 guard: dynamic_input_tree must recreate normal Niagara Dynamic Input function nodes.
            // Operator shortcuts previously fell back to Custom HLSL nodes and produced invalid/opaque
            // overrides in UE4.27. See Source/Documentation/N2C_Niagara_Import_Dynamic_Input_Tree_Format.md.
            OutReason = FString::Printf(TEXT("dynamic_input_tree operator node '%s' is not allowed; use a Niagara Dynamic Input function asset path in the 'function' field"), *OperatorName);
            return false;
        }

        FString FunctionPath;
        if (!GetNiagaraActionStringAny(TreeNode, TEXT("function"), TEXT("function_path"), FunctionPath) && !TreeNode->TryGetStringField(TEXT("asset_path"), FunctionPath))
        {
            OutReason = TEXT("dynamic input tree node must contain literal/value, parameter/user_parameter, or function/function_path");
            return false;
        }

        UNiagaraNodeFunctionCall* FunctionNode = N2CCreateNiagaraDynamicInputFunctionNode(TargetOverridePin, FunctionPath, OutputType, OutReason);
        if (!FunctionNode)
        {
            return false;
        }

        const TSharedPtr<FJsonObject>* InputsObject = nullptr;
        if (TreeNode->TryGetObjectField(TEXT("inputs"), InputsObject) && InputsObject && InputsObject->IsValid())
        {
            for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*InputsObject)->Values)
            {
                const FString RawChildInputName = Pair.Key;
                FString ChildInputName = N2CNormalizeNiagaraDynamicInputChildName(FunctionPath, RawChildInputName);

                // Generic safety pass for UE4.27 dynamic input aliases: resolve the requested child input
                // against the actual RapidIterationParameters exposed by the dynamic input script. This
                // avoids Sine.Angle-style invalid overrides for any other Niagara function whose UI label
                // or historical name differs from the real override name.
                FString ResolvedInputName;
                if (N2CResolveNiagaraDynamicInputChildNameFromScript(FunctionNode, ChildInputName, ResolvedInputName))
                {
                    ChildInputName = ResolvedInputName;
                }

                if (!RawChildInputName.Equals(ChildInputName, ESearchCase::CaseSensitive))
                {
                    N2CNiagaraGraphImportTrace(FString::Printf(TEXT("Dynamic input child alias normalized: function=%s %s -> %s"), *FunctionPath, *RawChildInputName, *ChildInputName));
                }
                TSharedPtr<FJsonObject> ChildNode = Pair.Value.IsValid() ? Pair.Value->AsObject() : nullptr;
                if (!ChildNode.IsValid())
                {
                    OutReason = FString::Printf(TEXT("dynamic input tree input %s is not an object"), *ChildInputName);
                    return false;
                }

                FNiagaraTypeDefinition ChildType = FNiagaraTypeDefinition::GetFloatDef();
                FString ChildTypeString;
                if (ChildNode->TryGetStringField(TEXT("type"), ChildTypeString) || ChildNode->TryGetStringField(TEXT("input_type"), ChildTypeString))
                {
                    FNiagaraTypeDefinition ParsedType;
                    if (!TryNiagaraTypeFromString(ChildTypeString, ParsedType))
                    {
                        OutReason = FString::Printf(TEXT("unsupported dynamic input tree child type %s for %s"), *ChildTypeString, *ChildInputName);
                        return false;
                    }
                    ChildType = ParsedType;
                }

                FString InputAlias = FString::Printf(TEXT("%s.%s"), *N2CGetNiagaraFunctionNodeName(FunctionNode), *ChildInputName);
                UEdGraphPin* ChildOverridePin = N2CGetOrCreateNiagaraOverridePin(FunctionNode, InputAlias, ChildType, OutReason);
                if (!ChildOverridePin)
                {
                    return false;
                }

                if (!N2CApplyNiagaraDynamicInputTreeNode(ChildOverridePin, ChildNode, ChildType, Depth + 1, OutReason))
                {
                    return false;
                }
            }
        }

        return true;
    }

    static bool N2CApplyNiagaraDynamicInputTree(UEdGraphPin* OverridePin, const FNiagaraTypeDefinition& InputType, const TSharedPtr<FJsonObject>& Action, FString& OutReason)
    {
        if (!Action.IsValid())
        {
            OutReason = TEXT("dynamic input tree action is invalid");
            return false;
        }

        const TSharedPtr<FJsonObject>* TreeObject = nullptr;
        if (!Action->TryGetObjectField(TEXT("tree"), TreeObject) && !Action->TryGetObjectField(TEXT("dynamic_input"), TreeObject) && !Action->TryGetObjectField(TEXT("expression_tree"), TreeObject))
        {
            OutReason = TEXT("dynamic input tree action is missing tree/dynamic_input/expression_tree object");
            return false;
        }
        return N2CApplyNiagaraDynamicInputTreeNode(OverridePin, TreeObject && TreeObject->IsValid() ? *TreeObject : nullptr, InputType, 0, OutReason);
    }

    static bool N2CApplyNiagaraCustomHlslInput(UEdGraphPin* OverridePin, const FString& InExpression, const FNiagaraTypeDefinition& InputType, const TSharedPtr<FJsonObject>& Action, FString& OutReason)
    {
        if (!OverridePin)
        {
            OutReason = TEXT("missing override pin");
            return false;
        }
        if (InExpression.IsEmpty())
        {
            OutReason = TEXT("missing custom_hlsl/expression");
            return false;
        }

        FString Expression = InExpression;
        TArray<FN2CNiagaraFormulaInputBinding> FormulaInputs;
        if (!N2CCollectNiagaraFormulaInputBindings(Action, FormulaInputs, Expression, OutReason))
        {
            return false;
        }

        UEdGraph* Graph = OverridePin->GetOwningNode() ? OverridePin->GetOwningNode()->GetGraph() : nullptr;
        UClass* CustomHlslClass = N2CFindNiagaraEditorNodeClass(TEXT("NiagaraNodeCustomHlsl"));
        N2CNiagaraGraphImportTrace(TEXT("Create CustomHlsl node begin"));
        UEdGraphNode* CustomNode = N2CNiagaraCreateGraphNode(Graph, CustomHlslClass, OverridePin->GetOwningNode()->NodePosX - 260, OverridePin->GetOwningNode()->NodePosY + 220);
        if (!CustomNode)
        {
            OutReason = TEXT("failed to create NiagaraNodeCustomHlsl");
            return false;
        }

        // InitAsCustomHlslDynamicInput(), RequestNewTypedPin(), and RebuildSignatureFromPins()
        // live in NiagaraEditor symbols which are not always safe to call from an external UE4.27 plugin.
        // Build the same final state manually: Signature first, then AllocateDefaultPins() so Niagara creates
        // the normal typed pins plus hidden dynamic Add pins. Without the Add pins, the translator reports
        // "Incorrect number of outputs" for Custom HLSL nodes.
        N2CSetUObjectEnumPropertyByName(CustomNode, TEXT("ScriptUsage"), static_cast<int64>(ENiagaraScriptUsage::DynamicInput));
        N2CSetUObjectStringPropertyByName(CustomNode, TEXT("CustomHlsl"), Expression);

        if (UNiagaraNodeFunctionCall* FunctionCallNode = Cast<UNiagaraNodeFunctionCall>(CustomNode))
        {
            FunctionCallNode->Modify();
            FunctionCallNode->FunctionScript = nullptr;
            FunctionCallNode->Signature = FNiagaraFunctionSignature();
            FunctionCallNode->Signature.Name = *FString::Printf(TEXT("N2C_CustomHlsl_%s"), *CustomNode->NodeGuid.ToString(EGuidFormats::Digits));
            FunctionCallNode->Signature.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetParameterMapDef(), TEXT("Map")));
            for (const FN2CNiagaraFormulaInputBinding& Binding : FormulaInputs)
            {
                FunctionCallNode->Signature.Inputs.Add(FNiagaraVariable(Binding.Type, FName(*Binding.LocalName)));
            }
            FunctionCallNode->Signature.Outputs.Add(FNiagaraVariable(InputType, TEXT("CustomHLSLOutput")));
            FunctionCallNode->Signature.bRequiresContext = true;
            FunctionCallNode->Signature.bSupportsCPU = true;
            FunctionCallNode->Signature.bSupportsGPU = true;
        }

        for (UEdGraphPin* ExistingPin : CustomNode->Pins)
        {
            if (ExistingPin)
            {
                ExistingPin->BreakAllPinLinks(false);
            }
        }
        CustomNode->Pins.Empty();
        CustomNode->AllocateDefaultPins();

        UEdGraphPin* CustomInputPin = N2CFindNiagaraParameterMapPin(CustomNode, EGPD_Input);
        UEdGraphPin* CustomOutputPin = N2CFindFirstNiagaraTypedOutputPin(CustomNode);
        N2CNiagaraGraphImportTrace(FString::Printf(TEXT("CustomHlsl pins rebuilt: map_in=%s typed_out=%s total=%d formula_inputs=%d expr=%s"), CustomInputPin ? TEXT("ok") : TEXT("null"), CustomOutputPin ? TEXT("ok") : TEXT("null"), CustomNode->Pins.Num(), FormulaInputs.Num(), *Expression));

        UEdGraphPin* OverrideNodeInputPin = N2CFindNiagaraParameterMapPin(OverridePin->GetOwningNode(), EGPD_Input);
        UEdGraphPin* PreviousStackNodeOutputPin = N2CGetSingleLinkedPinSafe(OverrideNodeInputPin);
        if (!CustomInputPin || !CustomOutputPin || !PreviousStackNodeOutputPin)
        {
            OutReason = TEXT("created CustomHlsl node but could not resolve pins");
            return false;
        }

        UEdGraphNode* GetNode = nullptr;
        UEdGraphPin* GetInputPin = nullptr;
        if (FormulaInputs.Num() > 0)
        {
            UClass* MapGetClass = N2CFindNiagaraEditorNodeClass(TEXT("NiagaraNodeParameterMapGet"));
            N2CNiagaraGraphImportTrace(FString::Printf(TEXT("Create ParameterMapGet for CustomHlsl inputs count=%d"), FormulaInputs.Num()));
            GetNode = N2CNiagaraCreateGraphNode(Graph, MapGetClass, OverridePin->GetOwningNode()->NodePosX - 520, OverridePin->GetOwningNode()->NodePosY + 220);
            if (!GetNode)
            {
                OutReason = TEXT("failed to create ParameterMapGet for CustomHlsl inputs");
                return false;
            }
            GetInputPin = N2CFindNiagaraParameterMapPin(GetNode, EGPD_Input);
            if (!GetInputPin)
            {
                OutReason = TEXT("created ParameterMapGet for CustomHlsl inputs but it has no parameter map input pin");
                return false;
            }
        }

        N2CNiagaraBreakAllPinLinks(OverridePin);
        N2CNiagaraMakeLink(CustomInputPin, PreviousStackNodeOutputPin);
        if (GetInputPin)
        {
            N2CNiagaraMakeLink(GetInputPin, PreviousStackNodeOutputPin);
        }

        for (const FN2CNiagaraFormulaInputBinding& Binding : FormulaInputs)
        {
            UEdGraphPin* CustomLocalInputPin = N2CFindNiagaraNamedDataPin(CustomNode, EGPD_Input, Binding.LocalName);
            if (!CustomLocalInputPin)
            {
                OutReason = FString::Printf(TEXT("CustomHlsl node missing local input pin %s"), *Binding.LocalName);
                return false;
            }
            const FEdGraphPinType BindingPinType = UEdGraphSchema_Niagara::TypeDefinitionToPinType(Binding.Type);
            UEdGraphPin* GetOutputPin = GetNode ? GetNode->CreatePin(EGPD_Output, BindingPinType, FName(*Binding.UserParameter)) : nullptr;
            if (!GetOutputPin)
            {
                OutReason = FString::Printf(TEXT("failed to create ParameterMapGet output for %s"), *Binding.UserParameter);
                return false;
            }
            N2CNiagaraGraphImportTrace(FString::Printf(TEXT("Link CustomHlsl input %s <= %s"), *Binding.LocalName, *Binding.UserParameter));
            N2CNiagaraMakeLink(GetOutputPin, CustomLocalInputPin);
        }

        N2CNiagaraMakeLink(CustomOutputPin, OverridePin);
        return true;
    }

    static void N2CRefreshNiagaraSourceGraph(UNiagaraScriptSource* Source)
    {
        if (!Source || !Source->NodeGraph)
        {
            return;
        }
        Source->NodeGraph->Modify();
        Source->NodeGraph->NotifyGraphChanged();
        Source->MarkPackageDirty();
    }

    static int32 ApplyNiagaraInputOverrideActions(UNiagaraSystem* System, const TSharedPtr<FJsonObject>& Root, bool bDryRun, FString& OutReport, int32& OutUnsupported, int32& OutUnchanged)
    {
        TSharedPtr<FJsonObject> Actions = GetNiagaraImportActionsObject(Root);
        if (!System || !Actions.IsValid() || !Actions->HasTypedField<EJson::Array>(TEXT("input_overrides")))
        {
            return 0;
        }

        int32 Changed = 0;
        int32 MaxApplyActions = 0;
        if (Actions->HasTypedField<EJson::Number>(TEXT("input_overrides_apply_limit")))
        {
            MaxApplyActions = FMath::Max(0, static_cast<int32>(Actions->GetNumberField(TEXT("input_overrides_apply_limit"))));
        }
        int32 AttemptedApplyActions = 0;
        int32 InputOverrideActionIndex = -1;
        for (const TSharedPtr<FJsonValue>& Value : Actions->GetArrayField(TEXT("input_overrides")))
        {
            ++InputOverrideActionIndex;
            TSharedPtr<FJsonObject> Action = Value.IsValid() ? Value->AsObject() : nullptr;
            if (!Action.IsValid())
            {
                continue;
            }

            FString Op;
            FString Stage;
            FString ModuleName;
            FString InputName;
            FString UserParameter;
            FString Expression;
            FString TargetLabel;
            Action->TryGetStringField(TEXT("op"), Op);
            GetNiagaraActionStringAny(Action, TEXT("stage"), nullptr, Stage);
            GetNiagaraActionStringAny(Action, TEXT("module"), TEXT("module_name"), ModuleName);
            GetNiagaraActionStringAny(Action, TEXT("input"), TEXT("input_name"), InputName);
            GetNiagaraActionStringAny(Action, TEXT("user_parameter"), TEXT("linked_parameter"), UserParameter);
            GetNiagaraActionStringAny(Action, TEXT("expression"), TEXT("custom_hlsl"), Expression);
            Action->TryGetStringField(TEXT("target"), TargetLabel);

            if (Op.IsEmpty())
            {
                Op = UserParameter.IsEmpty() ? TEXT("custom_hlsl") : TEXT("link_user_parameter");
            }
            if (!UserParameter.IsEmpty() && !UserParameter.StartsWith(TEXT("User.")))
            {
                UserParameter = TEXT("User.") + UserParameter;
            }

            ENiagaraScriptUsage Usage = ENiagaraScriptUsage::ParticleUpdateScript;
            bool bSystemStage = false;
            if (!ResolveNiagaraScriptUsageFromStage(Stage, Usage, bSystemStage))
            {
                ++OutUnsupported;
                OutReport += FString::Printf(TEXT("WARNING: unsupported input_override stage: %s"), *Stage) + LINE_TERMINATOR;
                continue;
            }
            if (ModuleName.IsEmpty() || InputName.IsEmpty())
            {
                ++OutUnsupported;
                OutReport += TEXT("WARNING: input_override missing module/input.") LINE_TERMINATOR;
                continue;
            }

            TArray<UNiagaraEmitter*> TargetEmitters;
            FString ResolvedTargetLabel;
            if (!bSystemStage)
            {
                CollectTargetNiagaraEmitters(System, Action, TargetEmitters, ResolvedTargetLabel);
                if (TargetEmitters.Num() <= 0)
                {
                    ++OutUnsupported;
                    OutReport += FString::Printf(TEXT("WARNING: input_override no target emitter for %s.%s"), *ModuleName, *InputName) + LINE_TERMINATOR;
                    continue;
                }
            }

            const FString InputAlias = FString::Printf(TEXT("%s.%s"), *ModuleName, *InputName);
            const int32 TargetCount = bSystemStage ? 1 : TargetEmitters.Num();
            for (int32 TargetIndex = 0; TargetIndex < TargetCount; ++TargetIndex)
            {
                UNiagaraEmitter* TargetEmitter = bSystemStage ? nullptr : TargetEmitters[TargetIndex];
                UNiagaraScriptSource* Source = nullptr;
                if (bSystemStage)
                {
                    Source = N2CGetNiagaraSystemScriptSourceForUsage(System, Usage);
                }
                else if (TargetEmitter)
                {
#if WITH_EDITORONLY_DATA
                    Source = Cast<UNiagaraScriptSource>(TargetEmitter->GraphSource);
#endif
                }
                if (!Source || !Source->NodeGraph)
                {
                    ++OutUnsupported;
                    OutReport += FString::Printf(TEXT("WARNING: input_override has no Niagara graph source for %s / %s"), *ModuleName, *Stage) + LINE_TERMINATOR;
                    continue;
                }

                FNiagaraTypeDefinition InputType;
                FString TypeReason;
                if (!N2CFindNiagaraInputType(System, TargetEmitter, Usage, bSystemStage, ModuleName, InputName, Action, InputType, TypeReason))
                {
                    ++OutUnsupported;
                    OutReport += FString::Printf(TEXT("WARNING: %s"), *TypeReason) + LINE_TERMINATOR;
                    continue;
                }

                TArray<UNiagaraNodeFunctionCall*> ModuleNodes;
                N2CCollectNiagaraModuleNodesForUsage(Source->NodeGraph, Usage, ModuleName, ModuleNodes);
                if (ModuleNodes.Num() <= 0)
                {
                    ++OutUnsupported;
                    OutReport += FString::Printf(TEXT("WARNING: input_override could not find module node %s in %s"), *ModuleName, *Stage) + LINE_TERMINATOR;
                    continue;
                }

                int32 ModuleInstanceIndex = 0;
                if (Action->HasTypedField<EJson::Number>(TEXT("module_instance_index")))
                {
                    ModuleInstanceIndex = FMath::Clamp(static_cast<int32>(Action->GetNumberField(TEXT("module_instance_index"))), 0, ModuleNodes.Num() - 1);
                }

                UNiagaraNodeFunctionCall* ModuleNode = ModuleNodes.IsValidIndex(ModuleInstanceIndex) ? ModuleNodes[ModuleInstanceIndex] : ModuleNodes[0];
                const FString TargetName = bSystemStage ? TEXT("system") : (TargetEmitter ? TargetEmitter->GetUniqueEmitterName() : TEXT("emitter"));

                if (bDryRun)
                {
                    OutReport += FString::Printf(TEXT("ACTION[%d]: low-level input override %s target=%s stage=%s module=%s input=%s user=%s expression=%s"), InputOverrideActionIndex, *Op, *TargetName, *Stage, *ModuleName, *InputName, *UserParameter, *Expression) + LINE_TERMINATOR;
                    ++Changed;
                    continue;
                }

                if (MaxApplyActions > 0 && AttemptedApplyActions >= MaxApplyActions)
                {
                    ++OutUnchanged;
                    OutReport += FString::Printf(TEXT("SKIP: input_override apply limit reached (%d). Skipped target=%s stage=%s module=%s input=%s"), MaxApplyActions, *TargetName, *Stage, *ModuleName, *InputName) + LINE_TERMINATOR;
                    continue;
                }

                ++AttemptedApplyActions;
                N2CNiagaraGraphImportTrace(FString::Printf(TEXT("Apply input_override begin action=%d attempt=%d/%d target=%s stage=%s module=%s input=%s op=%s"), InputOverrideActionIndex, AttemptedApplyActions, MaxApplyActions, *TargetName, *Stage, *ModuleName, *InputName, *Op));

                FString Reason;
                UEdGraphPin* OverridePin = N2CGetOrCreateNiagaraOverridePin(ModuleNode, InputAlias, InputType, Reason);
                if (!OverridePin)
                {
                    ++OutUnsupported;
                    OutReport += FString::Printf(TEXT("WARNING: could not create override pin for %s/%s: %s"), *TargetName, *InputAlias, *Reason) + LINE_TERMINATOR;
                    continue;
                }

                bool bApplied = false;
                if (Op.Equals(TEXT("link_user_parameter"), ESearchCase::IgnoreCase) || Op.Equals(TEXT("link"), ESearchCase::IgnoreCase) || !UserParameter.IsEmpty())
                {
                    bApplied = N2CApplyNiagaraUserParameterLink(OverridePin, UserParameter, InputType, Reason);
                }
                else if (Op.Equals(TEXT("dynamic_input_tree"), ESearchCase::IgnoreCase) || Op.Equals(TEXT("expression_tree"), ESearchCase::IgnoreCase) || Action->HasField(TEXT("tree")) || Action->HasField(TEXT("dynamic_input")) || Action->HasField(TEXT("expression_tree")))
                {
                    bApplied = N2CApplyNiagaraDynamicInputTree(OverridePin, InputType, Action, Reason);
                }
                else if (Op.Equals(TEXT("custom_hlsl"), ESearchCase::IgnoreCase) || Op.Equals(TEXT("formula"), ESearchCase::IgnoreCase) || !Expression.IsEmpty())
                {
                    bApplied = N2CApplyNiagaraCustomHlslInput(OverridePin, Expression, InputType, Action, Reason);
                }
                else
                {
                    Reason = FString::Printf(TEXT("unsupported input_override op: %s"), *Op);
                }

                if (bApplied)
                {
                    N2CNiagaraGraphImportTrace(FString::Printf(TEXT("Apply input_override graph links done action=%d target=%s module=%s input=%s"), InputOverrideActionIndex, *TargetName, *ModuleName, *InputName));
                    N2CRefreshNiagaraSourceGraph(Source);
                    ++Changed;
                    OutReport += FString::Printf(TEXT("Applied: low-level input override target=%s stage=%s module=%s input=%s"), *TargetName, *Stage, *ModuleName, *InputName) + LINE_TERMINATOR;
                }
                else
                {
                    ++OutUnsupported;
                    OutReport += FString::Printf(TEXT("WARNING: failed input_override target=%s module=%s input=%s: %s"), *TargetName, *ModuleName, *InputName, *Reason) + LINE_TERMINATOR;
                }
            }
        }
        return Changed;
    }


    static FString N2CStripTrailingDigits(FString In)
    {
        while (In.Len() > 0 && FChar::IsDigit(In[In.Len() - 1]))
        {
            In.LeftChopInline(1, false);
        }
        return In;
    }

    static bool N2CSplitNiagaraConstantName(const FString& Name, FString& OutScope, FString& OutModule, FString& OutInput)
    {
        OutScope.Empty();
        OutModule.Empty();
        OutInput.Empty();
        const FString Prefix = TEXT("Constants.");
        if (!Name.StartsWith(Prefix))
        {
            return false;
        }
        FString Rest = Name.Mid(Prefix.Len());
        int32 DotA = INDEX_NONE;
        if (!Rest.FindChar(TEXT('.'), DotA) || DotA <= 0)
        {
            return false;
        }
        OutScope = Rest.Left(DotA);
        Rest = Rest.Mid(DotA + 1);
        int32 DotB = INDEX_NONE;
        if (!Rest.FindChar(TEXT('.'), DotB) || DotB <= 0)
        {
            return false;
        }
        OutModule = Rest.Left(DotB);
        OutInput = Rest.Mid(DotB + 1);
        return !OutScope.IsEmpty() && !OutModule.IsEmpty() && !OutInput.IsEmpty();
    }

    static TSharedPtr<FJsonObject> N2CFindNiagaraImportValueForParameter(const FString& ParamName, const TMap<FString, TSharedPtr<FJsonObject>>& ImportValues, FString& OutMatchedName)
    {
        OutMatchedName.Empty();
        if (const TSharedPtr<FJsonObject>* ExactValue = ImportValues.Find(ParamName))
        {
            OutMatchedName = ParamName;
            return *ExactValue;
        }

        FString TargetScope;
        FString TargetModule;
        FString TargetInput;
        if (!N2CSplitNiagaraConstantName(ParamName, TargetScope, TargetModule, TargetInput))
        {
            return nullptr;
        }

        const FString TargetModuleBase = N2CStripTrailingDigits(TargetModule);
        int32 BestScore = INDEX_NONE;
        FString BestName;
        TSharedPtr<FJsonObject> BestValue;

        for (const TPair<FString, TSharedPtr<FJsonObject>>& Pair : ImportValues)
        {
            FString CandidateScope;
            FString CandidateModule;
            FString CandidateInput;
            if (!Pair.Value.IsValid() || !N2CSplitNiagaraConstantName(Pair.Key, CandidateScope, CandidateModule, CandidateInput))
            {
                continue;
            }
            if (!CandidateInput.Equals(TargetInput, ESearchCase::IgnoreCase))
            {
                continue;
            }

            int32 Score = 0;
            if (CandidateModule.Equals(TargetModule, ESearchCase::IgnoreCase))
            {
                Score += 80;
            }
            else if (N2CStripTrailingDigits(CandidateModule).Equals(TargetModuleBase, ESearchCase::IgnoreCase))
            {
                Score += 55;
            }
            else
            {
                continue;
            }

            if (CandidateScope.Equals(TargetScope, ESearchCase::IgnoreCase))
            {
                Score += 30;
            }
            else if (CandidateScope.Equals(TEXT("TEST"), ESearchCase::IgnoreCase))
            {
                Score += 20;
            }
            else
            {
                // Still allow cross-emitter JSON values when the module/input pair is unique.
                Score += 5;
            }

            if (Score > BestScore)
            {
                BestScore = Score;
                BestName = Pair.Key;
                BestValue = Pair.Value;
            }
        }

        if (BestValue.IsValid())
        {
            OutMatchedName = BestName;
        }
        return BestValue;
    }

    static int32 ApplyNiagaraParameterStoreFromJsonValues(FNiagaraParameterStore& Store, const FString& StoreLabel, const TMap<FString, TSharedPtr<FJsonObject>>& ImportValues, bool bDryRun, FString& OutReport, int32& OutMatched, int32& OutUnsupported, int32& OutUnchanged)
    {
        int32 Changed = 0;
        TArray<FNiagaraVariable> Parameters;
        Store.GetParameters(Parameters);
        Parameters.Sort([&Store](const FNiagaraVariable& A, const FNiagaraVariable& B)
        {
            return Store.IndexOf(A) < Store.IndexOf(B);
        });

        for (const FNiagaraVariable& Variable : Parameters)
        {
            const FString ParamName = Variable.GetName().ToString();
            FString MatchedImportName;
            TSharedPtr<FJsonObject> ImportValue = N2CFindNiagaraImportValueForParameter(ParamName, ImportValues, MatchedImportName);
            if (!ImportValue.IsValid())
            {
                continue;
            }

            ++OutMatched;
            TArray<uint8> NewBytes;
            FString Reason;
            if (!TryBuildNiagaraParameterBytes(Variable, ImportValue, NewBytes, Reason))
            {
                ++OutUnsupported;
                OutReport += FString::Printf(TEXT("WARNING: unsupported Niagara parameter %s/%s from %s: %s"), *StoreLabel, *ParamName, *MatchedImportName, *Reason) + LINE_TERMINATOR;
                continue;
            }

            if (NewBytes.Num() != Variable.GetSizeInBytes())
            {
                ++OutUnsupported;
                OutReport += FString::Printf(TEXT("WARNING: size mismatch for %s/%s: json bytes=%d target bytes=%d"), *StoreLabel, *ParamName, NewBytes.Num(), Variable.GetSizeInBytes()) + LINE_TERMINATOR;
                continue;
            }

            const uint8* CurrentData = Store.GetParameterData(Variable);
            const bool bSame = CurrentData && FMemory::Memcmp(CurrentData, NewBytes.GetData(), NewBytes.Num()) == 0;
            if (bSame)
            {
                ++OutUnchanged;
                continue;
            }

            const FString Display = NiagaraValueDisplay(ImportValue);
            if (bDryRun)
            {
                OutReport += FString::Printf(TEXT("CHANGE: %s / %s -> %s%s%s"), *StoreLabel, *ParamName, *Display, MatchedImportName == ParamName ? TEXT("") : TEXT(" [from "), MatchedImportName == ParamName ? TEXT("") : *MatchedImportName) + (MatchedImportName == ParamName ? FString() : FString(TEXT("]"))) + LINE_TERMINATOR;
            }
            else
            {
                Store.SetParameterData(NewBytes.GetData(), Variable, false);
                OutReport += FString::Printf(TEXT("Applied: %s / %s -> %s%s%s"), *StoreLabel, *ParamName, *Display, MatchedImportName == ParamName ? TEXT("") : TEXT(" [from "), MatchedImportName == ParamName ? TEXT("") : *MatchedImportName) + (MatchedImportName == ParamName ? FString() : FString(TEXT("]"))) + LINE_TERMINATOR;
            }
            ++Changed;
        }
        return Changed;
    }

    static int32 ApplyNiagaraScriptParameters(UNiagaraScript* Script, const FString& Label, const TMap<FString, TSharedPtr<FJsonObject>>& ImportValues, bool bDryRun, FString& OutReport, int32& OutMatched, int32& OutUnsupported, int32& OutUnchanged)
    {
        if (!Script)
        {
            return 0;
        }
        if (!bDryRun)
        {
            Script->Modify();
        }
        return ApplyNiagaraParameterStoreFromJsonValues(Script->RapidIterationParameters, Label, ImportValues, bDryRun, OutReport, OutMatched, OutUnsupported, OutUnchanged);
    }

    static bool ImportNiagaraN2CJsonToAsset(UObject* TargetAsset, const FString& ImportJson, bool bDryRun, FString& OutReport)
    {
        UNiagaraSystem* System = Cast<UNiagaraSystem>(TargetAsset);
        if (!System)
        {
            OutReport += TEXT("ERROR: target asset is not a NiagaraSystem. Current importer only supports NiagaraSystem assets.") LINE_TERMINATOR;
            return false;
        }

        TSharedPtr<FJsonObject> Root;
        if (!TryParseJsonRootObject(ImportJson, Root))
        {
            OutReport += TEXT("ERROR: failed to parse N2C Niagara JSON.") LINE_TERMINATOR;
            return false;
        }

        FString Schema;
        FString ExportKind;
        Root->TryGetStringField(TEXT("schema"), Schema);
        Root->TryGetStringField(TEXT("export_kind"), ExportKind);
        const bool bIsLegacyNiagaraAssetJson = (Schema == TEXT("N2C_AI_EXPORT_V2") && ExportKind == TEXT("Niagara"));
        const bool bIsProjectStyleSingleNiagaraAssetJson = (Schema == TEXT("N2C_NIAGARA_PROJECT_EXPORT_V1") && ExportKind == TEXT("NiagaraSystem") && Root->HasTypedField<EJson::Object>(TEXT("niagara_asset")));
        if (!bIsLegacyNiagaraAssetJson && !bIsProjectStyleSingleNiagaraAssetJson)
        {
            OutReport += FString::Printf(TEXT("ERROR: selected file is not an N2C Niagara asset JSON. schema=%s export_kind=%s"), *Schema, *ExportKind) + LINE_TERMINATOR;
            return false;
        }

        TMap<FString, TSharedPtr<FJsonObject>> ImportValues;
        CollectNiagaraImportValues(Root, ImportValues);
        const int32 ImportActionCount = CountNiagaraImportActions(Root);
        if (ImportValues.Num() <= 0 && ImportActionCount <= 0)
        {
            OutReport += TEXT("ERROR: N2C Niagara JSON contains no importable parameter values or import_actions.") LINE_TERMINATOR;
            return false;
        }

        FString SourceAssetPath;
        if (Root->HasTypedField<EJson::Object>(TEXT("niagara_asset")))
        {
            Root->GetObjectField(TEXT("niagara_asset"))->TryGetStringField(TEXT("asset_path"), SourceAssetPath);
        }

        OutReport += FString::Printf(TEXT("Target NiagaraSystem: %s"), *System->GetPathName()) + LINE_TERMINATOR;
        if (!SourceAssetPath.IsEmpty())
        {
            OutReport += FString::Printf(TEXT("Source NiagaraSystem: %s"), *SourceAssetPath) + LINE_TERMINATOR;
        }
        OutReport += FString::Printf(TEXT("Importable JSON values found: %d"), ImportValues.Num()) + LINE_TERMINATOR;
        OutReport += FString::Printf(TEXT("Import actions found: %d"), ImportActionCount) + LINE_TERMINATOR;

        int32 Matched = 0;
        int32 Unsupported = 0;
        int32 Unchanged = 0;
        int32 Changed = 0;

        TUniquePtr<FScopedTransaction> Transaction;
        if (!bDryRun)
        {
            FString BackupPath;
            if (!BackupAssetPackage(System, BackupPath, OutReport))
            {
                return false;
            }
            Transaction = MakeUnique<FScopedTransaction>(FText::FromString(TEXT("Import N2C Niagara Data")));
            System->Modify();
        }

        const auto ApplyAllCollectedNiagaraParameterValues = [&]() -> int32
        {
            int32 LocalChanged = 0;
            LocalChanged += ApplyNiagaraParameterStoreFromJsonValues(System->GetExposedParameters(), TEXT("System.ExposedParameters"), ImportValues, bDryRun, OutReport, Matched, Unsupported, Unchanged);
            LocalChanged += ApplyNiagaraScriptParameters(System->GetSystemSpawnScript(), TEXT("SystemSpawnScript"), ImportValues, bDryRun, OutReport, Matched, Unsupported, Unchanged);
            LocalChanged += ApplyNiagaraScriptParameters(System->GetSystemUpdateScript(), TEXT("SystemUpdateScript"), ImportValues, bDryRun, OutReport, Matched, Unsupported, Unchanged);

            const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();
            for (int32 EmitterIndex = 0; EmitterIndex < Handles.Num(); ++EmitterIndex)
            {
                const FNiagaraEmitterHandle& Handle = Handles[EmitterIndex];
                UNiagaraEmitter* Emitter = Handle.GetInstance();
                if (!Emitter)
                {
                    continue;
                }
                if (!bDryRun)
                {
                    Emitter->Modify();
                }

                const FString EmitterLabel = FString::Printf(TEXT("Emitter[%d:%s]"), EmitterIndex, *Handle.GetName().ToString());
#if WITH_EDITORONLY_DATA
                LocalChanged += ApplyNiagaraScriptParameters(Emitter->EmitterSpawnScriptProps.Script, EmitterLabel + TEXT(".EmitterSpawnScript"), ImportValues, bDryRun, OutReport, Matched, Unsupported, Unchanged);
                LocalChanged += ApplyNiagaraScriptParameters(Emitter->EmitterUpdateScriptProps.Script, EmitterLabel + TEXT(".EmitterUpdateScript"), ImportValues, bDryRun, OutReport, Matched, Unsupported, Unchanged);
#endif
                LocalChanged += ApplyNiagaraScriptParameters(Emitter->SpawnScriptProps.Script, EmitterLabel + TEXT(".ParticleSpawnScript"), ImportValues, bDryRun, OutReport, Matched, Unsupported, Unchanged);
                LocalChanged += ApplyNiagaraScriptParameters(Emitter->UpdateScriptProps.Script, EmitterLabel + TEXT(".ParticleUpdateScript"), ImportValues, bDryRun, OutReport, Matched, Unsupported, Unchanged);
                LocalChanged += ApplyNiagaraScriptParameters(Emitter->GetGPUComputeScript(), EmitterLabel + TEXT(".GPUComputeScript"), ImportValues, bDryRun, OutReport, Matched, Unsupported, Unchanged);
            }
            return LocalChanged;
        };

        int32 StructuralChanged = 0;
        StructuralChanged += ApplyNiagaraUserParameterActions(System, Root, bDryRun, OutReport, Unsupported, Unchanged);
        StructuralChanged += ApplyNiagaraEmitterActions(System, Root, bDryRun, OutReport, Unsupported, Unchanged);
        StructuralChanged += ApplyNiagaraModuleActions(System, Root, bDryRun, OutReport, Unsupported, Unchanged);
        StructuralChanged += ApplyNiagaraInputOverrideActions(System, Root, bDryRun, OutReport, Unsupported, Unchanged);
        Changed += StructuralChanged;

        // v24: AddModuleIfMissing/reorder can create RapidIteration parameters only after Niagara
        // refreshes the system/emitter scripts. Apply explicit parameter_values after that refresh
        // so newly inserted modules like SphereLocation/AddVelocityFromPoint do not keep UE defaults.
        if (!bDryRun && StructuralChanged > 0)
        {
            System->PostEditChange();
            System->MarkPackageDirty();
            System->RequestCompile(true);
            OutReport += TEXT("Applied: refreshed Niagara system after structural import before parameter_values.") LINE_TERMINATOR;
        }

        Changed += ApplyAllCollectedNiagaraParameterValues();

        // v24: one late pass catches parameters generated by compile/refresh after the first value pass.
        if (!bDryRun && StructuralChanged > 0)
        {
            const int32 LateChanged = ApplyAllCollectedNiagaraParameterValues();
            if (LateChanged > 0)
            {
                Changed += LateChanged;
                OutReport += FString::Printf(TEXT("Applied: late Niagara parameter_values pass changed %d item(s)."), LateChanged) + LINE_TERMINATOR;
            }
        }

        OutReport += FString::Printf(TEXT("Matched target parameters: %d"), Matched) + LINE_TERMINATOR;
        OutReport += FString::Printf(TEXT("Unsupported/skipped items: %d"), Unsupported) + LINE_TERMINATOR;
        OutReport += FString::Printf(TEXT("Already same/existing items: %d"), Unchanged) + LINE_TERMINATOR;
        if (bDryRun)
        {
            OutReport += FString::Printf(TEXT("Would change items: %d"), Changed) + LINE_TERMINATOR;
        }
        else
        {
            OutReport += FString::Printf(TEXT("Changed items: %d"), Changed) + LINE_TERMINATOR;
        }

        if (!bDryRun && Changed > 0)
        {
            System->PostEditChange();
            System->MarkPackageDirty();
            System->RequestCompile(true);
            SaveAssetPackageNoPrompt(System, OutReport);
        }

        if (Matched <= 0 && ImportActionCount <= 0)
        {
            OutReport += TEXT("ERROR: no matching target parameters were found. The JSON may be from a different Niagara graph/module stack.") LINE_TERMINATOR;
            return false;
        }

        if (Changed <= 0 && Unsupported > 0)
        {
            OutReport += TEXT("ERROR: Niagara import found actions/values but could not apply any of them.") LINE_TERMINATOR;
            return false;
        }

        return true;
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


    static bool StringArrayContainsIgnoreCase(const TArray<FString>& Values, const FString& Candidate)
    {
        for (const FString& Value : Values)
        {
            if (Value.Equals(Candidate, ESearchCase::IgnoreCase))
            {
                return true;
            }
        }
        return false;
    }

    static bool GetJsonBoolFieldAny(const TSharedPtr<FJsonObject>& Obj, const TCHAR* FieldName, bool DefaultValue)
    {
        bool Value = DefaultValue;
        if (Obj.IsValid())
        {
            Obj->TryGetBoolField(FieldName, Value);
        }
        return Value;
    }

    static FString JsonValueToImportString(const TSharedPtr<FJsonValue>& Value)
    {
        if (!Value.IsValid() || Value->IsNull())
        {
            return TEXT("");
        }

        switch (Value->Type)
        {
        case EJson::String:
            return Value->AsString();
        case EJson::Number:
            return FString::SanitizeFloat(Value->AsNumber());
        case EJson::Boolean:
            return Value->AsBool() ? TEXT("true") : TEXT("false");
        case EJson::Object:
            {
                TSharedPtr<FJsonObject> Obj = Value->AsObject();
                FString Raw;
                if (Obj.IsValid() && Obj->TryGetStringField(TEXT("raw"), Raw))
                {
                    return Raw;
                }
                FString Name;
                if (Obj.IsValid() && Obj->TryGetStringField(TEXT("name"), Name))
                {
                    return Name;
                }
                FString Path;
                if (Obj.IsValid() && Obj->TryGetStringField(TEXT("path"), Path))
                {
                    return Path;
                }
            }
            break;
        default:
            break;
        }
        return TEXT("");
    }

    static bool GetJsonStringFieldAny(const TSharedPtr<FJsonObject>& Obj, FString& OutValue, std::initializer_list<const TCHAR*> FieldNames)
    {
        OutValue.Empty();
        if (!Obj.IsValid())
        {
            return false;
        }

        for (const TCHAR* FieldName : FieldNames)
        {
            if (FieldName && Obj->TryGetStringField(FieldName, OutValue) && !OutValue.IsEmpty())
            {
                return true;
            }
        }
        return false;
    }

    static UObject* LoadObjectLooseForPin(const FString& PathOrName)
    {
        FString Clean = PathOrName;
        Clean.TrimStartAndEndInline();
        Clean.ReplaceInline(TEXT("const "), TEXT(""));
        while (Clean.RemoveFromEnd(TEXT("*")) || Clean.RemoveFromEnd(TEXT("&")))
        {
            Clean.TrimStartAndEndInline();
        }
        if (Clean.IsEmpty())
        {
            return nullptr;
        }

        if (UObject* Loaded = LoadObject<UObject>(nullptr, *Clean))
        {
            return Loaded;
        }

        // Common native struct paths exported as C++ names.
        TMap<FString, FString> NativeStructPaths;
        NativeStructPaths.Add(TEXT("FVector"), TEXT("/Script/CoreUObject.Vector"));
        NativeStructPaths.Add(TEXT("Vector"), TEXT("/Script/CoreUObject.Vector"));
        NativeStructPaths.Add(TEXT("FRotator"), TEXT("/Script/CoreUObject.Rotator"));
        NativeStructPaths.Add(TEXT("Rotator"), TEXT("/Script/CoreUObject.Rotator"));
        NativeStructPaths.Add(TEXT("FTransform"), TEXT("/Script/CoreUObject.Transform"));
        NativeStructPaths.Add(TEXT("Transform"), TEXT("/Script/CoreUObject.Transform"));
        NativeStructPaths.Add(TEXT("FLinearColor"), TEXT("/Script/CoreUObject.LinearColor"));
        NativeStructPaths.Add(TEXT("LinearColor"), TEXT("/Script/CoreUObject.LinearColor"));
        NativeStructPaths.Add(TEXT("FColor"), TEXT("/Script/CoreUObject.Color"));
        NativeStructPaths.Add(TEXT("Color"), TEXT("/Script/CoreUObject.Color"));
        NativeStructPaths.Add(TEXT("FGameplayTag"), TEXT("/Script/GameplayTags.GameplayTag"));
        NativeStructPaths.Add(TEXT("GameplayTag"), TEXT("/Script/GameplayTags.GameplayTag"));
        NativeStructPaths.Add(TEXT("UObject"), TEXT("/Script/CoreUObject.Object"));
        NativeStructPaths.Add(TEXT("Object"), TEXT("/Script/CoreUObject.Object"));
        NativeStructPaths.Add(TEXT("AActor"), TEXT("/Script/Engine.Actor"));
        NativeStructPaths.Add(TEXT("Actor"), TEXT("/Script/Engine.Actor"));
        NativeStructPaths.Add(TEXT("UActorComponent"), TEXT("/Script/Engine.ActorComponent"));
        NativeStructPaths.Add(TEXT("ActorComponent"), TEXT("/Script/Engine.ActorComponent"));

        if (const FString* NativePath = NativeStructPaths.Find(Clean))
        {
            if (UObject* Loaded = LoadObject<UObject>(nullptr, **NativePath))
            {
                return Loaded;
            }
        }

        FString ShortName = Clean;
        if (ShortName.StartsWith(TEXT("F")) && ShortName.Len() > 1)
        {
            ShortName = ShortName.RightChop(1);
        }

        if (UObject* Found = FindObject<UObject>(ANY_PACKAGE, *Clean))
        {
            return Found;
        }
        return FindObject<UObject>(ANY_PACKAGE, *ShortName);
    }

    static EPinContainerType ContainerTypeFromString(const FString& Container)
    {
        if (Container.Equals(TEXT("Array"), ESearchCase::IgnoreCase))
        {
            return EPinContainerType::Array;
        }
        if (Container.Equals(TEXT("Set"), ESearchCase::IgnoreCase))
        {
            return EPinContainerType::Set;
        }
        if (Container.Equals(TEXT("Map"), ESearchCase::IgnoreCase))
        {
            return EPinContainerType::Map;
        }
        return EPinContainerType::None;
    }

    static bool BuildPinTypeFromExplicitJson(const TSharedPtr<FJsonObject>& FieldObj, FEdGraphPinType& OutPinType)
    {
        const TSharedPtr<FJsonObject>* PinTypeObjPtr = nullptr;
        if (!FieldObj.IsValid() || !FieldObj->TryGetObjectField(TEXT("pin_type"), PinTypeObjPtr) || !PinTypeObjPtr || !PinTypeObjPtr->IsValid())
        {
            return false;
        }

        TSharedPtr<FJsonObject> PinTypeObj = *PinTypeObjPtr;
        FString Category;
        if (!PinTypeObj->TryGetStringField(TEXT("category"), Category) || Category.IsEmpty())
        {
            return false;
        }

        OutPinType = FEdGraphPinType();
        OutPinType.PinCategory = FName(*Category);

        FString SubCategory;
        if (PinTypeObj->TryGetStringField(TEXT("sub_category"), SubCategory) && !SubCategory.IsEmpty())
        {
            OutPinType.PinSubCategory = FName(*SubCategory);
        }

        FString Container;
        if (PinTypeObj->TryGetStringField(TEXT("container"), Container))
        {
            OutPinType.ContainerType = ContainerTypeFromString(Container);
        }

        FString SubCategoryObject;
        if (PinTypeObj->TryGetStringField(TEXT("sub_category_object"), SubCategoryObject) && !SubCategoryObject.IsEmpty())
        {
            OutPinType.PinSubCategoryObject = LoadObjectLooseForPin(SubCategoryObject);
        }
        return true;
    }

    static bool BuildPinTypeFromExportedStructField(const TSharedPtr<FJsonObject>& FieldObj, FEdGraphPinType& OutPinType, FString& OutWarning)
    {
        OutWarning.Empty();
        if (BuildPinTypeFromExplicitJson(FieldObj, OutPinType))
        {
            return true;
        }

        FString Type;
        FString CppType;
        FieldObj->TryGetStringField(TEXT("type"), Type);
        FieldObj->TryGetStringField(TEXT("cpp_type"), CppType);

        OutPinType = FEdGraphPinType();
        OutPinType.ContainerType = EPinContainerType::None;

        if (Type == TEXT("BoolProperty") || CppType == TEXT("bool"))
        {
            OutPinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
            return true;
        }
        if (Type == TEXT("FloatProperty") || CppType == TEXT("float") || CppType == TEXT("double"))
        {
            OutPinType.PinCategory = UEdGraphSchema_K2::PC_Float;
            return true;
        }
        if (Type == TEXT("IntProperty") || CppType == TEXT("int32") || CppType == TEXT("int"))
        {
            OutPinType.PinCategory = UEdGraphSchema_K2::PC_Int;
            return true;
        }
        if (Type == TEXT("Int64Property") || CppType == TEXT("int64"))
        {
            OutPinType.PinCategory = UEdGraphSchema_K2::PC_Int64;
            return true;
        }
        if (Type == TEXT("NameProperty") || CppType == TEXT("FName"))
        {
            OutPinType.PinCategory = UEdGraphSchema_K2::PC_Name;
            return true;
        }
        if (Type == TEXT("StrProperty") || CppType == TEXT("FString"))
        {
            OutPinType.PinCategory = UEdGraphSchema_K2::PC_String;
            return true;
        }
        if (Type == TEXT("TextProperty") || CppType == TEXT("FText"))
        {
            OutPinType.PinCategory = UEdGraphSchema_K2::PC_Text;
            return true;
        }
        if (Type == TEXT("ByteProperty") || Type == TEXT("EnumProperty") || CppType.Contains(TEXT("enum")) || CppType.Contains(TEXT("TEnumAsByte")))
        {
            OutPinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
            FString EnumPath;
            if (!GetJsonStringFieldAny(FieldObj, EnumPath, { TEXT("enum_path"), TEXT("sub_category_object"), TEXT("type_object") }))
            {
                const TSharedPtr<FJsonObject>* ValueObjPtr = nullptr;
                if (FieldObj->TryGetObjectField(TEXT("value"), ValueObjPtr) && ValueObjPtr && ValueObjPtr->IsValid())
                {
                    (*ValueObjPtr)->TryGetStringField(TEXT("enum_path"), EnumPath);
                }
            }
            if (!EnumPath.IsEmpty())
            {
                OutPinType.PinSubCategoryObject = LoadObjectLooseForPin(EnumPath);
            }
            return true;
        }
        if (Type == TEXT("StructProperty") || CppType.StartsWith(TEXT("F")))
        {
            UObject* StructObj = LoadObjectLooseForPin(CppType);
            if (!StructObj)
            {
                const TSharedPtr<FJsonObject>* ValueObjPtr = nullptr;
                if (FieldObj->TryGetObjectField(TEXT("value"), ValueObjPtr) && ValueObjPtr && ValueObjPtr->IsValid())
                {
                    FString StructPath;
                    if ((*ValueObjPtr)->TryGetStringField(TEXT("struct_path"), StructPath))
                    {
                        StructObj = LoadObjectLooseForPin(StructPath);
                    }
                }
            }
            if (StructObj && StructObj->IsA<UScriptStruct>())
            {
                OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
                OutPinType.PinSubCategoryObject = StructObj;
                return true;
            }
        }
        if (Type.Contains(TEXT("ObjectProperty")) || Type.Contains(TEXT("SoftObjectProperty")))
        {
            OutPinType.PinCategory = UEdGraphSchema_K2::PC_Object;
            FString ClassPath;
            if (GetJsonStringFieldAny(FieldObj, ClassPath, { TEXT("class_path"), TEXT("object_class_path"), TEXT("sub_category_object"), TEXT("type_object") }))
            {
                OutPinType.PinSubCategoryObject = LoadObjectLooseForPin(ClassPath);
            }
            if (!OutPinType.PinSubCategoryObject.IsValid() && !CppType.IsEmpty())
            {
                OutPinType.PinSubCategoryObject = LoadObjectLooseForPin(CppType);
            }
            return true;
        }
        if (Type.Contains(TEXT("ClassProperty")) || Type.Contains(TEXT("SoftClassProperty")))
        {
            OutPinType.PinCategory = UEdGraphSchema_K2::PC_Class;
            FString ClassPath;
            if (GetJsonStringFieldAny(FieldObj, ClassPath, { TEXT("class_path"), TEXT("meta_class_path"), TEXT("sub_category_object"), TEXT("type_object") }))
            {
                OutPinType.PinSubCategoryObject = LoadObjectLooseForPin(ClassPath);
            }
            if (!OutPinType.PinSubCategoryObject.IsValid() && !CppType.IsEmpty())
            {
                OutPinType.PinSubCategoryObject = LoadObjectLooseForPin(CppType);
            }
            return true;
        }

        OutWarning = FString::Printf(TEXT("unsupported/ambiguous struct field type: type='%s' cpp_type='%s'"), *Type, *CppType);
        return false;
    }

    static int32 GetVisibleUserDefinedEnumCount(const UEnum* Enum)
    {
        if (!Enum)
        {
            return 0;
        }

        int32 Count = Enum->NumEnums();
        while (Count > 0)
        {
            const int32 Index = Count - 1;
            const FString InternalName = Enum->GetNameStringByIndex(Index);
            const bool bHidden = Enum->HasMetaData(TEXT("Hidden"), Index);
            const bool bLooksLikeMax = InternalName.EndsWith(TEXT("_MAX")) || InternalName.Equals(TEXT("MAX"), ESearchCase::IgnoreCase);
            if (!bHidden && !bLooksLikeMax)
            {
                break;
            }
            --Count;
        }
        return Count;
    }

    struct FN2CEnumImportValue
    {
        FString DisplayName;
        FString InternalName;
        FString Tooltip;
    };

    static bool ExtractEnumImportValues(const TSharedPtr<FJsonObject>& Root, TArray<FN2CEnumImportValue>& OutValues, FString& OutReport)
    {
        OutValues.Empty();
        OutReport.Empty();
        if (!Root.IsValid())
        {
            OutReport += TEXT("ERROR: enum import JSON root is invalid.") LINE_TERMINATOR;
            return false;
        }

        FString Schema;
        FString ExportKind;
        Root->TryGetStringField(TEXT("schema"), Schema);
        Root->TryGetStringField(TEXT("export_kind"), ExportKind);

        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (!Root->TryGetArrayField(TEXT("values"), Values))
        {
            Root->TryGetArrayField(TEXT("enum_values"), Values);
        }

        if (!Values || Values->Num() == 0)
        {
            OutReport += TEXT("ERROR: enum import JSON has no values[].") LINE_TERMINATOR;
            return false;
        }

        for (const TSharedPtr<FJsonValue>& Value : *Values)
        {
            TSharedPtr<FJsonObject> Obj = Value.IsValid() ? Value->AsObject() : nullptr;
            if (!Obj.IsValid())
            {
                continue;
            }

            FN2CEnumImportValue Item;
            GetJsonStringFieldAny(Obj, Item.DisplayName, { TEXT("display_name"), TEXT("name"), TEXT("friendly_name") });
            GetJsonStringFieldAny(Obj, Item.InternalName, { TEXT("internal_name"), TEXT("cpp_name") });
            GetJsonStringFieldAny(Obj, Item.Tooltip, { TEXT("tooltip"), TEXT("tool_tip") });
            if (Item.DisplayName.IsEmpty())
            {
                Item.DisplayName = Item.InternalName;
            }
            if (!Item.DisplayName.IsEmpty())
            {
                OutValues.Add(Item);
            }
        }

        if (OutValues.Num() <= 0)
        {
            OutReport += TEXT("ERROR: enum import JSON contained no usable enum values.") LINE_TERMINATOR;
            return false;
        }

        if (!(Schema == TEXT("N2C_AI_EXPORT_V2") && ExportKind == TEXT("Enum")) && Schema != TEXT("N2C_ENUM_PATCH_V1"))
        {
            OutReport += FString::Printf(TEXT("WARNING: enum import schema/export_kind is not the canonical Enum import format: schema='%s', export_kind='%s'. Trying values[] anyway."), *Schema, *ExportKind) + LINE_TERMINATOR;
        }
        return true;
    }

    static bool ImportEnumN2CJsonToAsset(UObject* TargetAsset, const FString& ImportJson, bool bDryRun, FString& OutReport)
    {
        OutReport.Empty();
        UUserDefinedEnum* TargetEnum = Cast<UUserDefinedEnum>(TargetAsset);
        if (!TargetEnum)
        {
            OutReport += TEXT("ERROR: target asset is not a UUserDefinedEnum. Native C++ enums cannot be modified by N2C import.") LINE_TERMINATOR;
            return false;
        }

        TSharedPtr<FJsonObject> Root;
        if (!TryParseJsonRootObject(ImportJson, Root))
        {
            OutReport += TEXT("ERROR: enum import JSON parse failed.") LINE_TERMINATOR;
            return false;
        }

        TArray<FN2CEnumImportValue> Values;
        FString ParseReport;
        if (!ExtractEnumImportValues(Root, Values, ParseReport))
        {
            OutReport += ParseReport;
            return false;
        }
        OutReport += ParseReport;

        const int32 ExistingCount = GetVisibleUserDefinedEnumCount(TargetEnum);
        OutReport += FString::Printf(TEXT("%s Enum target: %s"), bDryRun ? TEXT("DRY RUN:") : TEXT("APPLY:"), *TargetEnum->GetPathName()) + LINE_TERMINATOR;
        OutReport += FString::Printf(TEXT("%s Enum values: existing=%d desired=%d"), bDryRun ? TEXT("DRY RUN:") : TEXT("CHANGE:"), ExistingCount, Values.Num()) + LINE_TERMINATOR;

        if (bDryRun)
        {
            for (int32 Index = 0; Index < Values.Num(); ++Index)
            {
                const FString CurrentName = Index < ExistingCount ? TargetEnum->GetDisplayNameTextByIndex(Index).ToString() : TEXT("<new>");
                OutReport += FString::Printf(TEXT("DRY RUN: [%d] '%s' -> '%s'"), Index, *CurrentName, *Values[Index].DisplayName) + LINE_TERMINATOR;
            }
            return true;
        }

        FString BackupPath;
        if (!BackupAssetPackage(TargetEnum, BackupPath, OutReport))
        {
            return false;
        }

        const FScopedTransaction Transaction(FText::FromString(TEXT("Import N2C Enum")));
        TargetEnum->Modify();

        while (GetVisibleUserDefinedEnumCount(TargetEnum) < Values.Num())
        {
            FEnumEditorUtils::AddNewEnumeratorForUserDefinedEnum(TargetEnum);
        }
        while (GetVisibleUserDefinedEnumCount(TargetEnum) > Values.Num())
        {
            FEnumEditorUtils::RemoveEnumeratorFromUserDefinedEnum(TargetEnum, GetVisibleUserDefinedEnumCount(TargetEnum) - 1);
        }

        for (int32 Index = 0; Index < Values.Num(); ++Index)
        {
            FEnumEditorUtils::SetEnumeratorDisplayName(TargetEnum, Index, FText::FromString(Values[Index].DisplayName));
            if (!Values[Index].Tooltip.IsEmpty())
            {
                TargetEnum->SetMetaData(TEXT("ToolTip"), *Values[Index].Tooltip, Index);
            }
            OutReport += FString::Printf(TEXT("CHANGE: enum value [%d] = %s"), Index, *Values[Index].DisplayName) + LINE_TERMINATOR;
        }

        TargetEnum->MarkPackageDirty();
        TargetEnum->PostEditChange();
        SaveAssetPackageNoPrompt(TargetEnum, OutReport);
        return true;
    }

    struct FN2CStructImportField
    {
        FString Name;
        FString DisplayName;
        FString Category;
        FString Tooltip;
        FString DefaultValue;
        FEdGraphPinType PinType;
        bool bHasUsableType = false;
    };

    static FStructVariableDescription* FindStructVarDescByName(UUserDefinedStruct* TargetStruct, const FString& DesiredName)
    {
        if (!TargetStruct || DesiredName.IsEmpty())
        {
            return nullptr;
        }

        TArray<FStructVariableDescription>& Descs = FStructureEditorUtils::GetVarDesc(TargetStruct);
        for (FStructVariableDescription& Desc : Descs)
        {
            if (Desc.FriendlyName.Equals(DesiredName, ESearchCase::IgnoreCase) || Desc.VarName.ToString().Equals(DesiredName, ESearchCase::IgnoreCase))
            {
                return &Desc;
            }
        }

        for (FStructVariableDescription& Desc : Descs)
        {
            if (Desc.VarName.ToString().StartsWith(DesiredName + TEXT("_"), ESearchCase::IgnoreCase))
            {
                return &Desc;
            }
        }
        return nullptr;
    }

    static bool SetStructVarCategoryCompat(UUserDefinedStruct* TargetStruct, const FGuid& VarGuid, const FString& Category)
    {
        // UE4.27 FStructVariableDescription does not expose a stable MetaData map in all builds.
        // Do not write UI category through Desc.Category either: that field is part of the pin type
        // and changing it can turn imported struct fields into int properties.
        // Keep this as a safe no-op until we add a version-specific category API path.
        return TargetStruct != nullptr && VarGuid.IsValid() && !Category.IsEmpty();
    }

    static FString StructPinTypeToDebugString(const FEdGraphPinType& PinType)
    {
        FString Result = PinType.PinCategory.ToString();
        if (!PinType.PinSubCategory.IsNone())
        {
            Result += TEXT("/") + PinType.PinSubCategory.ToString();
        }
        if (PinType.PinSubCategoryObject.IsValid())
        {
            Result += TEXT(" object=") + PinType.PinSubCategoryObject.Get()->GetPathName();
        }
        if (PinType.ContainerType != EPinContainerType::None)
        {
            Result += FString::Printf(TEXT(" container=%d"), static_cast<int32>(PinType.ContainerType));
        }
        return Result;
    }

    static bool ApplyStructVarPinTypeCompat(UUserDefinedStruct* TargetStruct, const FGuid& VarGuid, const FEdGraphPinType& PinType, FString& OutWarning)
    {
        OutWarning.Empty();
        if (!TargetStruct || !VarGuid.IsValid())
        {
            OutWarning = TEXT("invalid struct or variable guid");
            return false;
        }

        // UE4.27 FStructureEditorUtils::ChangeVariableType can silently fall back to int for
        // UserDefinedStruct fields in some editor paths. Update the stored descriptor type
        // fields directly, then compile the struct below.
        TArray<FStructVariableDescription>& Descs = FStructureEditorUtils::GetVarDesc(TargetStruct);
        for (FStructVariableDescription& Desc : Descs)
        {
            if (Desc.VarGuid == VarGuid)
            {
                Desc.Category = PinType.PinCategory;
                Desc.SubCategory = PinType.PinSubCategory;
                Desc.SubCategoryObject = PinType.PinSubCategoryObject.Get();
                Desc.ContainerType = PinType.ContainerType;
                Desc.PinValueType = PinType.PinValueType;
                return true;
            }
        }

        OutWarning = FString::Printf(TEXT("struct variable guid not found: %s"), *VarGuid.ToString());
        return false;
    }

    static FString ExtractDefaultValueFromField(const TSharedPtr<FJsonObject>& FieldObj)
    {
        FString DefaultValue;
        if (!FieldObj.IsValid())
        {
            return DefaultValue;
        }
        if (FieldObj->TryGetStringField(TEXT("default_value"), DefaultValue) || FieldObj->TryGetStringField(TEXT("raw"), DefaultValue))
        {
            return DefaultValue;
        }

        const TSharedPtr<FJsonValue> ValuePtr = FieldObj->TryGetField(TEXT("value"));
        if (ValuePtr.IsValid())
        {
            return JsonValueToImportString(ValuePtr);
        }
        return DefaultValue;
    }

    static bool ExtractStructImportFields(const TSharedPtr<FJsonObject>& Root, TArray<FN2CStructImportField>& OutFields, bool& bOutReplaceFields, FString& OutReport)
    {
        OutFields.Empty();
        bOutReplaceFields = false;
        OutReport.Empty();
        if (!Root.IsValid())
        {
            OutReport += TEXT("ERROR: struct import JSON root is invalid.") LINE_TERMINATOR;
            return false;
        }

        FString Schema;
        FString ExportKind;
        Root->TryGetStringField(TEXT("schema"), Schema);
        Root->TryGetStringField(TEXT("export_kind"), ExportKind);
        bOutReplaceFields = GetJsonBoolFieldAny(Root, TEXT("replace_fields"), false);

        const TArray<TSharedPtr<FJsonValue>>* Fields = nullptr;
        if (!Root->TryGetArrayField(TEXT("fields"), Fields))
        {
            Root->TryGetArrayField(TEXT("struct_fields"), Fields);
        }
        if (!Fields || Fields->Num() == 0)
        {
            OutReport += TEXT("ERROR: struct import JSON has no fields[].") LINE_TERMINATOR;
            return false;
        }

        for (const TSharedPtr<FJsonValue>& FieldValue : *Fields)
        {
            TSharedPtr<FJsonObject> FieldObj = FieldValue.IsValid() ? FieldValue->AsObject() : nullptr;
            if (!FieldObj.IsValid())
            {
                continue;
            }

            FN2CStructImportField Field;
            GetJsonStringFieldAny(FieldObj, Field.Name, { TEXT("name"), TEXT("var_name") });
            GetJsonStringFieldAny(FieldObj, Field.DisplayName, { TEXT("display_name"), TEXT("friendly_name"), TEXT("name") });
            GetJsonStringFieldAny(FieldObj, Field.Category, { TEXT("category") });
            GetJsonStringFieldAny(FieldObj, Field.Tooltip, { TEXT("tooltip"), TEXT("tool_tip") });
            Field.DefaultValue = ExtractDefaultValueFromField(FieldObj);

            if (Field.DisplayName.IsEmpty())
            {
                Field.DisplayName = Field.Name;
            }
            if (Field.DisplayName.IsEmpty())
            {
                continue;
            }

            FString TypeWarning;
            Field.bHasUsableType = BuildPinTypeFromExportedStructField(FieldObj, Field.PinType, TypeWarning);
            if (!TypeWarning.IsEmpty())
            {
                OutReport += FString::Printf(TEXT("WARNING: field '%s': %s"), *Field.DisplayName, *TypeWarning) + LINE_TERMINATOR;
            }
            OutFields.Add(Field);
        }

        if (OutFields.Num() <= 0)
        {
            OutReport += TEXT("ERROR: struct import JSON contained no usable fields.") LINE_TERMINATOR;
            return false;
        }

        if (!(Schema == TEXT("N2C_AI_EXPORT_V2") && ExportKind == TEXT("Struct")) && Schema != TEXT("N2C_STRUCT_PATCH_V1"))
        {
            OutReport += FString::Printf(TEXT("WARNING: struct import schema/export_kind is not the canonical Struct import format: schema='%s', export_kind='%s'. Trying fields[] anyway."), *Schema, *ExportKind) + LINE_TERMINATOR;
        }
        return true;
    }

    static bool ImportStructN2CJsonToAsset(UObject* TargetAsset, const FString& ImportJson, bool bDryRun, FString& OutReport)
    {
        OutReport.Empty();
        UUserDefinedStruct* TargetStruct = Cast<UUserDefinedStruct>(TargetAsset);
        if (!TargetStruct)
        {
            OutReport += TEXT("ERROR: target asset is not a UUserDefinedStruct. Native C++ structs cannot be modified by N2C import.") LINE_TERMINATOR;
            return false;
        }

        TSharedPtr<FJsonObject> Root;
        if (!TryParseJsonRootObject(ImportJson, Root))
        {
            OutReport += TEXT("ERROR: struct import JSON parse failed.") LINE_TERMINATOR;
            return false;
        }

        TArray<FN2CStructImportField> Fields;
        bool bReplaceFields = false;
        FString ParseReport;
        if (!ExtractStructImportFields(Root, Fields, bReplaceFields, ParseReport))
        {
            OutReport += ParseReport;
            return false;
        }
        OutReport += ParseReport;

        OutReport += FString::Printf(TEXT("%s Struct target: %s"), bDryRun ? TEXT("DRY RUN:") : TEXT("APPLY:"), *TargetStruct->GetPathName()) + LINE_TERMINATOR;
        OutReport += FString::Printf(TEXT("%s Struct fields: existing=%d desired=%d replace_fields=%s"), bDryRun ? TEXT("DRY RUN:") : TEXT("CHANGE:"), FStructureEditorUtils::GetVarDesc(TargetStruct).Num(), Fields.Num(), bReplaceFields ? TEXT("true") : TEXT("false")) + LINE_TERMINATOR;

        bool bAllTypesOk = true;
        for (const FN2CStructImportField& Field : Fields)
        {
            const bool bExists = FindStructVarDescByName(TargetStruct, Field.DisplayName) != nullptr;
            OutReport += FString::Printf(TEXT("%s field %s: %s type=%s"), bDryRun ? TEXT("DRY RUN:") : TEXT("CHANGE:"), bExists ? TEXT("update") : TEXT("add"), *Field.DisplayName, *StructPinTypeToDebugString(Field.PinType)) + LINE_TERMINATOR;
            if (!Field.bHasUsableType)
            {
                bAllTypesOk = false;
                OutReport += FString::Printf(TEXT("ERROR: field '%s' has no importable pin type."), *Field.DisplayName) + LINE_TERMINATOR;
            }
        }

        if (!bAllTypesOk)
        {
            return false;
        }
        if (bDryRun)
        {
            return true;
        }

        FString BackupPath;
        if (!BackupAssetPackage(TargetStruct, BackupPath, OutReport))
        {
            return false;
        }

        const FScopedTransaction Transaction(FText::FromString(TEXT("Import N2C Struct")));
        TargetStruct->Modify();

        if (bReplaceFields)
        {
            TArray<FString> DesiredNames;
            for (const FN2CStructImportField& Field : Fields)
            {
                DesiredNames.Add(Field.DisplayName);
            }

            TArray<FGuid> RemoveGuids;
            for (const FStructVariableDescription& Desc : FStructureEditorUtils::GetVarDesc(TargetStruct))
            {
                if (!StringArrayContainsIgnoreCase(DesiredNames, Desc.FriendlyName))
                {
                    RemoveGuids.Add(Desc.VarGuid);
                }
            }
            for (const FGuid& Guid : RemoveGuids)
            {
                FStructureEditorUtils::RemoveVariable(TargetStruct, Guid);
                OutReport += FString::Printf(TEXT("CHANGE: removed struct field guid %s"), *Guid.ToString()) + LINE_TERMINATOR;
            }
        }

        for (const FN2CStructImportField& Field : Fields)
        {
            FStructVariableDescription* Desc = FindStructVarDescByName(TargetStruct, Field.DisplayName);
            if (!Desc)
            {
                const int32 BeforeCount = FStructureEditorUtils::GetVarDesc(TargetStruct).Num();
                if (!FStructureEditorUtils::AddVariable(TargetStruct, Field.PinType))
                {
                    OutReport += FString::Printf(TEXT("ERROR: failed to add struct field '%s'."), *Field.DisplayName) + LINE_TERMINATOR;
                    return false;
                }

                TArray<FStructVariableDescription>& Descs = FStructureEditorUtils::GetVarDesc(TargetStruct);
                if (Descs.Num() <= BeforeCount)
                {
                    OutReport += FString::Printf(TEXT("ERROR: AddVariable reported success but no new field appeared for '%s'."), *Field.DisplayName) + LINE_TERMINATOR;
                    return false;
                }
                Desc = &Descs.Last();
            }

            const FGuid VarGuid = Desc->VarGuid;

            FString TypeApplyWarning;
            if (!ApplyStructVarPinTypeCompat(TargetStruct, VarGuid, Field.PinType, TypeApplyWarning))
            {
                OutReport += FString::Printf(TEXT("ERROR: failed to set type for struct field '%s': %s"), *Field.DisplayName, *TypeApplyWarning) + LINE_TERMINATOR;
                return false;
            }

            FStructureEditorUtils::RenameVariable(TargetStruct, VarGuid, Field.DisplayName);
            if (!Field.Category.IsEmpty())
            {
                if (!SetStructVarCategoryCompat(TargetStruct, VarGuid, Field.Category))
                {
                    OutReport += FString::Printf(TEXT("WARNING: failed to set category for struct field '%s'."), *Field.DisplayName) + LINE_TERMINATOR;
                }
            }
            if (!Field.Tooltip.IsEmpty())
            {
                FStructureEditorUtils::ChangeVariableTooltip(TargetStruct, VarGuid, Field.Tooltip);
            }
            if (!Field.DefaultValue.IsEmpty())
            {
                FStructureEditorUtils::ChangeVariableDefaultValue(TargetStruct, VarGuid, Field.DefaultValue);
            }
            OutReport += FString::Printf(TEXT("CHANGE: struct field imported: %s type=%s"), *Field.DisplayName, *StructPinTypeToDebugString(Field.PinType)) + LINE_TERMINATOR;
        }

        FStructureEditorUtils::CompileStructure(TargetStruct);
        TargetStruct->MarkPackageDirty();
        TargetStruct->PostEditChange();
        SaveAssetPackageNoPrompt(TargetStruct, OutReport);
        return true;
    }

    static bool JsonRootLooksLikeKind(const FString& Json, const FString& ExpectedExportKind, const FString& ExpectedPatchSchema)
    {
        TSharedPtr<FJsonObject> Root;
        if (!TryParseJsonRootObject(Json, Root) || !Root.IsValid())
        {
            return false;
        }
        FString Schema;
        FString ExportKind;
        Root->TryGetStringField(TEXT("schema"), Schema);
        Root->TryGetStringField(TEXT("export_kind"), ExportKind);
        return (Schema == TEXT("N2C_AI_EXPORT_V2") && ExportKind == ExpectedExportKind) || Schema == ExpectedPatchSchema;
    }

    static bool LoadTypedN2CJsonFromZip(const FString& ZipPath, const FString& ManifestKind, const FString& ExportKind, const FString& PatchSchema, FString& OutJson, FString& OutJsonEntry, FString& OutError)
    {
        OutJson.Empty();
        OutJsonEntry.Empty();
        OutError.Empty();

        TMap<FString, TArray<uint8>> Entries;
        if (!ExtractStoredZipEntries(ZipPath, Entries, OutError))
        {
            return false;
        }

        TArray<FString> CandidateEntries;
        if (const TArray<uint8>* ManifestBytes = Entries.Find(TEXT("N2C_PROJECT_MANIFEST.json")))
        {
            FString ManifestJson;
            FFileHelper::BufferToString(ManifestJson, ManifestBytes->GetData(), ManifestBytes->Num());
            TSharedPtr<FJsonObject> Manifest;
            if (TryParseJsonRootObject(ManifestJson, Manifest))
            {
                const TArray<TSharedPtr<FJsonValue>>* Assets = nullptr;
                if (Manifest->TryGetArrayField(TEXT("assets"), Assets))
                {
                    for (const TSharedPtr<FJsonValue>& AssetValue : *Assets)
                    {
                        TSharedPtr<FJsonObject> AssetObj = AssetValue.IsValid() ? AssetValue->AsObject() : nullptr;
                        FString Kind;
                        FString JsonPath;
                        if (AssetObj.IsValid() && AssetObj->TryGetStringField(TEXT("kind"), Kind) && Kind.Equals(ManifestKind, ESearchCase::IgnoreCase) && AssetObj->TryGetStringField(TEXT("json"), JsonPath) && Entries.Contains(JsonPath))
                        {
                            CandidateEntries.Add(JsonPath);
                        }
                    }
                }
            }
        }

        TArray<FString> AllEntries;
        Entries.GetKeys(AllEntries);
        AllEntries.Sort();
        for (const FString& EntryName : AllEntries)
        {
            const FString Lower = EntryName.ToLower();
            if (EntryName.EndsWith(TEXT(".json")) && EntryName != TEXT("N2C_PROJECT_MANIFEST.json"))
            {
                const bool bKindFolderMatch = Lower.StartsWith(ManifestKind + TEXT("s/")) || Lower.Contains(FString(TEXT("/")) + ManifestKind + TEXT("s/"));
                if (bKindFolderMatch || CandidateEntries.Num() == 0)
                {
                    CandidateEntries.AddUnique(EntryName);
                }
            }
        }

        for (const FString& EntryName : CandidateEntries)
        {
            const TArray<uint8>* JsonBytes = Entries.Find(EntryName);
            if (!JsonBytes)
            {
                continue;
            }

            FString CandidateJson;
            FFileHelper::BufferToString(CandidateJson, JsonBytes->GetData(), JsonBytes->Num());
            if (JsonRootLooksLikeKind(CandidateJson, ExportKind, PatchSchema))
            {
                OutJson = CandidateJson;
                OutJsonEntry = EntryName;
                return true;
            }
        }

        OutError = FString::Printf(TEXT("ZIP archive contains no %s JSON. Expected N2C_AI_EXPORT_V2 export_kind=%s or %s."), *ExportKind, *ExportKind, *PatchSchema);
        return false;
    }

    static bool LoadTypedN2CImportJsonFromFile(const FString& ImportPath, const FString& ManifestKind, const FString& ExportKind, const FString& PatchSchema, FString& OutJson, FString& OutSourceLabel, FString& OutError)
    {
        OutJson.Empty();
        OutSourceLabel.Empty();
        OutError.Empty();

        const FString Extension = FPaths::GetExtension(ImportPath).ToLower();
        if (Extension == TEXT("zip"))
        {
            FString EntryName;
            if (!LoadTypedN2CJsonFromZip(ImportPath, ManifestKind, ExportKind, PatchSchema, OutJson, EntryName, OutError))
            {
                return false;
            }
            OutSourceLabel = FString::Printf(TEXT("%s :: %s"), *ImportPath, *EntryName);
            return true;
        }

        if (!FFileHelper::LoadFileToString(OutJson, *ImportPath))
        {
            OutError = FString::Printf(TEXT("Could not read JSON file: %s"), *ImportPath);
            return false;
        }
        if (!JsonRootLooksLikeKind(OutJson, ExportKind, PatchSchema))
        {
            OutError = FString::Printf(TEXT("JSON file is not an %s import JSON. Expected N2C_AI_EXPORT_V2 export_kind=%s or %s."), *ExportKind, *ExportKind, *PatchSchema);
            return false;
        }
        OutSourceLabel = ImportPath;
        return true;
    }

}

FN2CEditorIntegration& FN2CEditorIntegration::Get()
{
    static FN2CEditorIntegration Instance;
    return Instance;
}

#if WITH_DEV_AUTOMATION_TESTS
bool FN2CEditorIntegration::QueueBackupRestoreForAutomation(
    UObject* Asset,
    const FString& BackupPath,
    FString& OutManifestPath,
    FString& OutReport)
{
    OutManifestPath.Empty();
    return N2CEditorIntegration_Private::QueueBackupRestoreToAsset(
        Asset,
        BackupPath,
        false,
        &OutManifestPath,
        OutReport);
}

FString FN2CEditorIntegration::GetPendingRestoreStatusForAutomation() const
{
    return N2CEditorIntegration_Private::BuildPendingRestoreStatusReport();
}

FString FN2CEditorIntegration::FormatDiagnosticForAutomation(const FString& DiagnosticLine) const
{
    return N2CEditorIntegration_Private::HumanizeN2CDiagnostic(DiagnosticLine);
}
#endif

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

    const FString SaveDir = FPaths::ProjectSavedDir() / TEXT("NodeToCode/ProjectExports");
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
        FExecuteAction::CreateRaw(this, &FN2CEditorIntegration::ExecuteBack2DeadExportProject),
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

    // Main export button: opens the project export picker.
    Builder.AddToolBarButton(
        FN2CToolbarCommand::Get().ExportAllCommand,
        NAME_None,
        FN2CToolbarCommand::CommandLabel_ExportAll,
        FN2CToolbarCommand::CommandTooltip_ExportAll,
        FSlateIcon(FName("NodeToCodeStyle"), FName("NodeToCode.ExportButton"), FName("NodeToCode.ExportButton.Small"))
    );

    // Adjacent dropdown: secondary project export actions only.
    // Do not put folder export here; the dropdown is intentionally kept for the file list export.
    Builder.AddComboButton(
        FUIAction(),
        FOnGetContent::CreateRaw(this, &FN2CEditorIntegration::MakeExportAllDropdownMenu),
        FText::FromString(TEXT("Export Options")),
        FText::FromString(TEXT("Export helper project metadata such as the project file list.")),
        FSlateIcon(FName("NodeToCodeStyle"), FName("NodeToCode.ExportButton"), FName("NodeToCode.ExportButton.Small")),
        true
    );

}

TSharedRef<SWidget> FN2CEditorIntegration::MakeExportAllDropdownMenu()
{
    FMenuBuilder MenuBuilder(true, LevelEditorCommandList);

    MenuBuilder.BeginSection(TEXT("N2CProjectExportOptions"), FText::FromString(TEXT("NodeToCode Export Options")));

    MenuBuilder.AddMenuEntry(
        FText::FromString(TEXT("Export project file list")),
        FText::FromString(TEXT("Export one AI-friendly JSON with UE asset metadata, content files and import source files.")),
        FSlateIcon(FName("NodeToCodeStyle"), FName("NodeToCode.ExportButton"), FName("NodeToCode.ExportButton.Small")),
        FUIAction(FExecuteAction::CreateRaw(this, &FN2CEditorIntegration::ExecuteBack2DeadExportProjectFileList))
    );

    MenuBuilder.EndSection();

    MenuBuilder.BeginSection(TEXT("N2CRestoreStatus"), FText::FromString(TEXT("NodeToCode Restore / Safety")));
    MenuBuilder.AddMenuEntry(
        FText::FromString(TEXT("Pending restore status...")),
        FText::FromString(TEXT("Show queued, applied and failed N2C deferred backup restores.")),
        FSlateIcon(FName("NodeToCodeStyle"), FName("NodeToCode.ImportButton"), FName("NodeToCode.ImportButton.Small")),
        FUIAction(FExecuteAction::CreateRaw(this, &FN2CEditorIntegration::ExecuteShowPendingRestoreStatus))
    );
    MenuBuilder.AddMenuEntry(
        FText::FromString(TEXT("Cancel all pending restores...")),
        FText::FromString(TEXT("Safely cancel queued restores before restarting UE. Pending files are moved to PendingRestoreCancelled, not destroyed.")),
        FSlateIcon(FName("NodeToCodeStyle"), FName("NodeToCode.ImportButton"), FName("NodeToCode.ImportButton.Small")),
        FUIAction(FExecuteAction::CreateRaw(this, &FN2CEditorIntegration::ExecuteCancelPendingRestore))
    );
    MenuBuilder.EndSection();
    return MenuBuilder.MakeWidget();
}

TSharedRef<SWidget> FN2CEditorIntegration::MakeRestoreBackupDropdownMenu(TWeakObjectPtr<UObject> Asset)
{
    using namespace N2CEditorIntegration_Private;
    FMenuBuilder MenuBuilder(true, nullptr);

    UObject* TargetAsset = Asset.Get();
    const FString AssetName = TargetAsset ? TargetAsset->GetName() : TEXT("Invalid asset");

    MenuBuilder.BeginSection(TEXT("N2CRestoreBackup"), FText::FromString(FString::Printf(TEXT("N2C Backups: %s"), *AssetName)));

    MenuBuilder.AddMenuEntry(
        FText::FromString(TEXT("Previous backup")),
        FText::FromString(TEXT("Restore the latest N2C backup found in Saved/NodeToCode/Backups for this asset.")),
        FSlateIcon(FName("NodeToCodeStyle"), FName("NodeToCode.ImportButton"), FName("NodeToCode.ImportButton.Small")),
        FUIAction(FExecuteAction::CreateRaw(this, &FN2CEditorIntegration::ExecuteRestoreLatestBackupForAsset, Asset))
    );

    MenuBuilder.AddMenuEntry(
        FText::FromString(TEXT("Choose backup...")),
        FText::FromString(TEXT("Choose a .uasset backup file and restore it over this asset package.")),
        FSlateIcon(FName("NodeToCodeStyle"), FName("NodeToCode.ImportButton"), FName("NodeToCode.ImportButton.Small")),
        FUIAction(FExecuteAction::CreateRaw(this, &FN2CEditorIntegration::ExecuteRestoreChosenBackupForAsset, Asset))
    );

    MenuBuilder.AddMenuSeparator();

    MenuBuilder.AddMenuEntry(
        FText::FromString(TEXT("Pending restore status...")),
        FText::FromString(TEXT("Show queued/applied N2C restore state for this editor session.")),
        FSlateIcon(FName("NodeToCodeStyle"), FName("NodeToCode.ImportButton"), FName("NodeToCode.ImportButton.Small")),
        FUIAction(FExecuteAction::CreateRaw(this, &FN2CEditorIntegration::ExecuteShowPendingRestoreStatus))
    );

    MenuBuilder.AddMenuEntry(
        FText::FromString(TEXT("Cancel pending restore for this asset...")),
        FText::FromString(TEXT("Cancel this asset's queued deferred restore before restarting UE.")),
        FSlateIcon(FName("NodeToCodeStyle"), FName("NodeToCode.ImportButton"), FName("NodeToCode.ImportButton.Small")),
        FUIAction(FExecuteAction::CreateRaw(this, &FN2CEditorIntegration::ExecuteCancelPendingRestoreForAsset, Asset))
    );

    MenuBuilder.EndSection();
    return MenuBuilder.MakeWidget();
}

void FN2CEditorIntegration::ExecuteRestoreLatestBackupForAsset(TWeakObjectPtr<UObject> Asset)
{
    using namespace N2CEditorIntegration_Private;
    UObject* TargetAsset = Asset.Get();
    if (!TargetAsset)
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(TEXT("NOT OK") LINE_TERMINATOR LINE_TERMINATOR TEXT("Target asset is no longer valid.")));
        ShowN2CNotification(TEXT("N2C restore failed: invalid asset"), SNotificationItem::CS_Fail);
        return;
    }

    TArray<FString> Backups;
    FindBackupFilesForAsset(TargetAsset, Backups);
    if (Backups.Num() <= 0)
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(FString::Printf(TEXT("NOT OK") LINE_TERMINATOR LINE_TERMINATOR TEXT("No N2C backups found for asset: %s") LINE_TERMINATOR TEXT("Folder: %s"), *TargetAsset->GetPathName(), *(FPaths::ProjectSavedDir() / TEXT("NodeToCode/Backups")))));
        ShowN2CNotification(TEXT("N2C restore failed: no backup found"), SNotificationItem::CS_Fail);
        return;
    }

    FString Report;
    const bool bOk = RestoreBackupFileToAsset(TargetAsset, Backups[0], Report);
    FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(FString(bOk ? TEXT("OK") : TEXT("NOT OK")) + FString(LINE_TERMINATOR LINE_TERMINATOR) + Report));
    ShowN2CNotification(bOk ? TEXT("N2C backup restored") : TEXT("N2C restore failed"), bOk ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
}

void FN2CEditorIntegration::ExecuteRestoreChosenBackupForAsset(TWeakObjectPtr<UObject> Asset)
{
    using namespace N2CEditorIntegration_Private;
    UObject* TargetAsset = Asset.Get();
    if (!TargetAsset)
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(TEXT("NOT OK") LINE_TERMINATOR LINE_TERMINATOR TEXT("Target asset is no longer valid.")));
        ShowN2CNotification(TEXT("N2C restore failed: invalid asset"), SNotificationItem::CS_Fail);
        return;
    }

    FString BackupPath;
    if (!PickBackupFileForAsset(TargetAsset, BackupPath))
    {
        return;
    }

    FString Report;
    const bool bOk = RestoreBackupFileToAsset(TargetAsset, BackupPath, Report);
    FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(FString(bOk ? TEXT("OK") : TEXT("NOT OK")) + FString(LINE_TERMINATOR LINE_TERMINATOR) + Report));
    ShowN2CNotification(bOk ? TEXT("N2C backup restored") : TEXT("N2C restore failed"), bOk ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
}

void FN2CEditorIntegration::ExecuteRestoreLatestBackupForSelectedAssets(TArray<FAssetData> SelectedAssets)
{
    using namespace N2CEditorIntegration_Private;
    int32 Restored = 0;
    int32 Failed = 0;
    FString Report;

    for (const FAssetData& AssetData : SelectedAssets)
    {
        UObject* Asset = AssetData.GetAsset();
        if (!Asset)
        {
            ++Failed;
            Report += FString::Printf(TEXT("ERROR: failed to load asset: %s"), *AssetData.ObjectPath.ToString()) + LINE_TERMINATOR;
            continue;
        }

        TArray<FString> Backups;
        FindBackupFilesForAsset(Asset, Backups);
        if (Backups.Num() <= 0)
        {
            ++Failed;
            Report += FString::Printf(TEXT("ERROR: no backup found for %s"), *Asset->GetPathName()) + LINE_TERMINATOR;
            continue;
        }

        FString AssetReport;
        if (RestoreBackupFileToAsset(Asset, Backups[0], AssetReport))
        {
            ++Restored;
        }
        else
        {
            ++Failed;
        }
        Report += AssetReport + LINE_TERMINATOR;
    }

    FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(FString::Printf(TEXT("N2C restore latest backups finished") LINE_TERMINATOR LINE_TERMINATOR TEXT("Restored: %d") LINE_TERMINATOR TEXT("Failed/skipped: %d") LINE_TERMINATOR LINE_TERMINATOR TEXT("%s"), Restored, Failed, *Report)));
    ShowN2CNotification(FString::Printf(TEXT("N2C restore: %d restored, %d failed"), Restored, Failed), Failed == 0 ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
}

void FN2CEditorIntegration::ExecuteRestoreChosenBackupForSelectedAsset(TArray<FAssetData> SelectedAssets)
{
    if (SelectedAssets.Num() != 1)
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(TEXT("NOT OK") LINE_TERMINATOR LINE_TERMINATOR TEXT("Choose backup supports exactly one selected asset.")));
        return;
    }

    UObject* Asset = SelectedAssets[0].GetAsset();
    ExecuteRestoreChosenBackupForAsset(TWeakObjectPtr<UObject>(Asset));
}

void FN2CEditorIntegration::ExecuteShowPendingRestoreStatus()
{
    using namespace N2CEditorIntegration_Private;
    const FString Report = BuildPendingRestoreStatusReport();
    FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Report));
    ShowN2CNotification(TEXT("N2C pending restore status shown"), SNotificationItem::CS_None);
}

void FN2CEditorIntegration::ExecuteCancelPendingRestore()
{
    using namespace N2CEditorIntegration_Private;
    const EAppReturnType::Type Confirm = FMessageDialog::Open(
        EAppMsgType::YesNo,
        FText::FromString(TEXT("Cancel all queued N2C pending restores?\n\nPending restore files will be moved to Saved/NodeToCode/Backups/PendingRestoreCancelled. Rollback files are kept."))
    );
    if (Confirm != EAppReturnType::Yes)
    {
        ShowN2CNotification(TEXT("N2C cancel pending restore cancelled by user"), SNotificationItem::CS_None);
        return;
    }

    FString Report;
    const bool bOk = CancelPendingRestoreManifests(nullptr, Report);
    FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(FString(bOk ? TEXT("OK") : TEXT("NOT OK")) + FString(LINE_TERMINATOR LINE_TERMINATOR) + Report));
    ShowN2CNotification(bOk ? TEXT("N2C pending restores cancelled") : TEXT("N2C pending restore cancel finished with errors"), bOk ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
}

void FN2CEditorIntegration::ExecuteCancelPendingRestoreForAsset(TWeakObjectPtr<UObject> Asset)
{
    using namespace N2CEditorIntegration_Private;
    UObject* TargetAsset = Asset.Get();
    if (!TargetAsset)
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(TEXT("NOT OK") LINE_TERMINATOR LINE_TERMINATOR TEXT("Target asset is no longer valid.")));
        return;
    }

    const EAppReturnType::Type Confirm = FMessageDialog::Open(
        EAppMsgType::YesNo,
        FText::FromString(FString::Printf(TEXT("Cancel queued N2C pending restore for this asset?\n\n%s\n\nPending files will be moved to PendingRestoreCancelled. Rollback files are kept."), *TargetAsset->GetPathName()))
    );
    if (Confirm != EAppReturnType::Yes)
    {
        ShowN2CNotification(TEXT("N2C cancel pending restore cancelled by user"), SNotificationItem::CS_None);
        return;
    }

    FString Report;
    const bool bOk = CancelPendingRestoreManifests(TargetAsset, Report);
    FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(FString(bOk ? TEXT("OK") : TEXT("NOT OK")) + FString(LINE_TERMINATOR LINE_TERMINATOR) + Report));
    ShowN2CNotification(bOk ? TEXT("N2C pending restore cancelled") : TEXT("N2C pending restore cancel failed"), bOk ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
}

void FN2CEditorIntegration::Initialize()
{
    // Apply any queued disk-level backup restore before users reopen the asset.
    // Live-unloading Blueprint packages can trigger UE4.27 pin-link ensures, so restore is deferred to startup.
    N2CEditorIntegration_Private::ProcessPendingBackupRestores(false);
    N2CEditorIntegration_Private::SchedulePendingRestoreStartupSummary();
    // Deferred rollback files are restored only by a fresh Editor startup.
    // UE4.27 can still hold the target package/file during OnPreExit, so attempting
    // the copy in the failing process races package shutdown and produces a false
    // "could not delete target file" failure. The manifest intentionally remains
    // queued until the next process starts before the asset is loaded.

    // Register commands
    FN2CToolbarCommand::Register();

    // Register main editor toolbar buttons for project-level patch import / project export picker.
    RegisterLevelEditorToolbar();

    // Register Content Browser right-click export for selected supported assets.
    RegisterContentBrowserAssetContextMenu();

    // Register Content Browser right-click export for selected folders.
    RegisterContentBrowserFolderContextMenu();

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
    // Clear editor command lists
    EditorCommandLists.Empty();
    AssetEditorCommandLists.Empty();
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

    // Unsubscribe from Content Browser folder/path context menu extension.
    if (ContentBrowserFolderExtenderDelegateHandle.IsValid() && FModuleManager::Get().IsModuleLoaded(TEXT("ContentBrowser")))
    {
        FContentBrowserModule& ContentBrowserModule = FModuleManager::GetModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
        TArray<FContentBrowserMenuExtender_SelectedPaths>& Extenders = ContentBrowserModule.GetAllPathViewContextMenuExtenders();
        Extenders.RemoveAll([this](const FContentBrowserMenuExtender_SelectedPaths& Delegate)
        {
            return Delegate.GetHandle() == ContentBrowserFolderExtenderDelegateHandle;
        });
        ContentBrowserFolderExtenderDelegateHandle.Reset();
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

    FString PendingRestoreManifest;
    if (N2CEditorIntegration_Private::IsAssetQueuedForPendingRestore(Asset, PendingRestoreManifest))
    {
        // Pending restore lock: do not register toolbars or touch the toolkit.
        // Closing is deferred because immediate close/modal dialog inside OnAssetOpenedInEditor
        // can crash UE4.27 editor callbacks with SharedPointer.h IsValid() asserts.
        N2CEditorIntegration_Private::ScheduleClosePendingRestoreAssetEditor(Asset, PendingRestoreManifest);
        return;
    }

    // Niagara/Enum/Struct editors are not Blueprint editors, but they still derive from FAssetEditorToolkit.
    // Add N2C buttons directly to their editor toolbar when a supported asset is opened.
    if (N2CEditorIntegration_Private::IsNiagaraObjectAsset(Asset))
    {
        RegisterToolbarForNiagaraEditor(Asset, EditorInstance);
        return;
    }

    if (N2CEditorIntegration_Private::IsEnumObjectAsset(Asset))
    {
        RegisterToolbarForEnumEditor(Asset, EditorInstance);
        return;
    }

    if (N2CEditorIntegration_Private::IsStructObjectAsset(Asset))
    {
        RegisterToolbarForStructEditor(Asset, EditorInstance);
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
        FToolBarExtensionDelegate::CreateLambda([this, CommandList, WeakEditor](FToolBarBuilder& Builder)
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


            Builder.AddComboButton(
                FUIAction(),
                FOnGetContent::CreateLambda([this, WeakEditor]()
                {
                    UObject* Asset = nullptr;
                    TSharedPtr<FBlueprintEditor> Editor = WeakEditor.Pin();
                    if (Editor.IsValid())
                    {
                        Asset = Editor->GetBlueprintObj();
                    }
                    return MakeRestoreBackupDropdownMenu(TWeakObjectPtr<UObject>(Asset));
                }),
                FText::FromString(TEXT("Previous Backup")),
                FText::FromString(TEXT("Restore the latest N2C backup or choose a backup for this Blueprint.")),
                FSlateIcon(FName("NodeToCodeStyle"), FName("NodeToCode.ImportButton"), FName("NodeToCode.ImportButton.Small")),
                true
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

void FN2CEditorIntegration::RegisterToolbarForNiagaraEditor(UObject* NiagaraAsset, IAssetEditorInstance* EditorInstance)
{
    using namespace N2CEditorIntegration_Private;

    if (!NiagaraAsset || !EditorInstance || !IsNiagaraObjectAsset(NiagaraAsset))
    {
        return;
    }

    FAssetEditorToolkit* AssetEditorToolkit = static_cast<FAssetEditorToolkit*>(EditorInstance);
    if (!AssetEditorToolkit)
    {
        FN2CLogger::Get().LogWarning(TEXT("N2C Niagara toolbar registration skipped: editor instance is not an asset editor toolkit."));
        return;
    }

    const FString AssetName = NiagaraAsset->GetName();
    TWeakObjectPtr<UObject> WeakNiagaraAsset(NiagaraAsset);

    TSharedPtr<FUICommandList> CommandList = MakeShareable(new FUICommandList);

    CommandList->MapAction(
        FN2CToolbarCommand::Get().ExportCommand,
        FExecuteAction::CreateRaw(this, &FN2CEditorIntegration::ExecuteBack2DeadExportNiagaraEditor, WeakNiagaraAsset),
        FCanExecuteAction::CreateLambda([WeakNiagaraAsset]()
        {
            return WeakNiagaraAsset.IsValid();
        })
    );

    CommandList->MapAction(
        FN2CToolbarCommand::Get().ImportCommand,
        FExecuteAction::CreateRaw(this, &FN2CEditorIntegration::ExecuteBack2DeadImportNiagaraEditor, WeakNiagaraAsset),
        FCanExecuteAction::CreateLambda([WeakNiagaraAsset]()
        {
            return WeakNiagaraAsset.IsValid();
        })
    );

    AssetEditorCommandLists.Add(CommandList);

    TSharedPtr<FExtender> ToolbarExtender = MakeShareable(new FExtender);
    ToolbarExtender->AddToolBarExtension(
        "Asset",
        EExtensionHook::After,
        CommandList,
        FToolBarExtensionDelegate::CreateLambda([this, CommandList, WeakNiagaraAsset](FToolBarBuilder& Builder)
        {
            Builder.AddSeparator();

            Builder.AddToolBarButton(
                FN2CToolbarCommand::Get().ExportCommand,
                NAME_None,
                FText::FromString(TEXT("Export N2C Niagara")),
                FText::FromString(TEXT("Export the currently edited Niagara asset into an AI-friendly N2C ZIP archive.")),
                FSlateIcon(FName("NodeToCodeStyle"), FName("NodeToCode.ExportButton"), FName("NodeToCode.ExportButton.Small"))
            );

            Builder.AddToolBarButton(
                FN2CToolbarCommand::Get().ImportCommand,
                NAME_None,
                FText::FromString(TEXT("Import N2C Niagara")),
                FText::FromString(TEXT("Import supported N2C Niagara parameter values from an N2C ZIP/JSON into the target NiagaraSystem. Graph recreation is not implemented yet.")),
                FSlateIcon(FName("NodeToCodeStyle"), FName("NodeToCode.ImportButton"), FName("NodeToCode.ImportButton.Small"))
            );


            Builder.AddComboButton(
                FUIAction(),
                FOnGetContent::CreateLambda([this, WeakNiagaraAsset]()
                {
                    return MakeRestoreBackupDropdownMenu(WeakNiagaraAsset);
                }),
                FText::FromString(TEXT("Previous Backup")),
                FText::FromString(TEXT("Restore the latest N2C backup or choose a backup for this Niagara asset.")),
                FSlateIcon(FName("NodeToCodeStyle"), FName("NodeToCode.ImportButton"), FName("NodeToCode.ImportButton.Small")),
                true
            );
        })
    );

    AssetEditorToolkit->AddToolbarExtender(ToolbarExtender);
    AssetEditorToolkit->RegenerateMenusAndToolbars();

    FN2CLogger::Get().Log(
        FString::Printf(TEXT("Registered N2C toolbar buttons for Niagara editor: %s"), *AssetName),
        EN2CLogSeverity::Info
    );
}


void FN2CEditorIntegration::RegisterToolbarForEnumEditor(UObject* EnumAsset, IAssetEditorInstance* EditorInstance)
{
    using namespace N2CEditorIntegration_Private;

    if (!EnumAsset || !EditorInstance || !IsEnumObjectAsset(EnumAsset))
    {
        return;
    }

    FAssetEditorToolkit* AssetEditorToolkit = static_cast<FAssetEditorToolkit*>(EditorInstance);
    if (!AssetEditorToolkit)
    {
        FN2CLogger::Get().LogWarning(TEXT("N2C Enum toolbar registration skipped: editor instance is not an asset editor toolkit."));
        return;
    }

    const FString AssetName = EnumAsset->GetName();
    TWeakObjectPtr<UObject> WeakEnumAsset(EnumAsset);

    TSharedPtr<FUICommandList> CommandList = MakeShareable(new FUICommandList);
    CommandList->MapAction(
        FN2CToolbarCommand::Get().ExportCommand,
        FExecuteAction::CreateLambda([this, WeakEnumAsset]()
        {
            UObject* Asset = WeakEnumAsset.Get();
            if (!Asset || !N2CEditorIntegration_Private::IsEnumObjectAsset(Asset))
            {
                FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(TEXT("NOT OK") LINE_TERMINATOR LINE_TERMINATOR TEXT("The Enum editor asset is no longer valid.")));
                ShowN2CNotification(TEXT("Enum editor N2C export failed: invalid asset"), SNotificationItem::CS_Fail);
                return;
            }

            TArray<FAssetData> Assets;
            Assets.Add(FAssetData(Asset));
            ExecuteBack2DeadExportSelectedEnums(Assets);
        }),
        FCanExecuteAction::CreateLambda([WeakEnumAsset]()
        {
            return WeakEnumAsset.IsValid();
        })
    );

    CommandList->MapAction(
        FN2CToolbarCommand::Get().ImportCommand,
        FExecuteAction::CreateRaw(this, &FN2CEditorIntegration::ExecuteBack2DeadImportEnumEditor, WeakEnumAsset),
        FCanExecuteAction::CreateLambda([WeakEnumAsset]()
        {
            return WeakEnumAsset.IsValid();
        })
    );

    AssetEditorCommandLists.Add(CommandList);

    TSharedPtr<FExtender> ToolbarExtender = MakeShareable(new FExtender);
    ToolbarExtender->AddToolBarExtension(
        "Asset",
        EExtensionHook::After,
        CommandList,
        FToolBarExtensionDelegate::CreateLambda([this, CommandList, WeakEnumAsset](FToolBarBuilder& Builder)
        {
            Builder.AddSeparator();
            Builder.AddToolBarButton(
                FN2CToolbarCommand::Get().ExportCommand,
                NAME_None,
                FText::FromString(TEXT("Export N2C Enum")),
                FText::FromString(TEXT("Export the currently edited Enum asset into an AI-friendly N2C ZIP archive.")),
                FSlateIcon(FName("NodeToCodeStyle"), FName("NodeToCode.ExportButton"), FName("NodeToCode.ExportButton.Small"))
            );
            Builder.AddToolBarButton(
                FN2CToolbarCommand::Get().ImportCommand,
                NAME_None,
                FText::FromString(TEXT("Import N2C Enum")),
                FText::FromString(TEXT("Import N2C enum values from an N2C ZIP/JSON into the currently edited UserDefinedEnum.")),
                FSlateIcon(FName("NodeToCodeStyle"), FName("NodeToCode.ImportButton"), FName("NodeToCode.ImportButton.Small"))
            );


            Builder.AddComboButton(
                FUIAction(),
                FOnGetContent::CreateLambda([this, WeakEnumAsset]()
                {
                    return MakeRestoreBackupDropdownMenu(WeakEnumAsset);
                }),
                FText::FromString(TEXT("Previous Backup")),
                FText::FromString(TEXT("Restore the latest N2C backup or choose a backup for this Enum asset.")),
                FSlateIcon(FName("NodeToCodeStyle"), FName("NodeToCode.ImportButton"), FName("NodeToCode.ImportButton.Small")),
                true
            );
        })
    );

    AssetEditorToolkit->AddToolbarExtender(ToolbarExtender);
    AssetEditorToolkit->RegenerateMenusAndToolbars();

    FN2CLogger::Get().Log(
        FString::Printf(TEXT("Registered N2C toolbar button for Enum editor: %s"), *AssetName),
        EN2CLogSeverity::Info
    );
}

void FN2CEditorIntegration::RegisterToolbarForStructEditor(UObject* StructAsset, IAssetEditorInstance* EditorInstance)
{
    using namespace N2CEditorIntegration_Private;

    if (!StructAsset || !EditorInstance || !IsStructObjectAsset(StructAsset))
    {
        return;
    }

    FAssetEditorToolkit* AssetEditorToolkit = static_cast<FAssetEditorToolkit*>(EditorInstance);
    if (!AssetEditorToolkit)
    {
        FN2CLogger::Get().LogWarning(TEXT("N2C Struct toolbar registration skipped: editor instance is not an asset editor toolkit."));
        return;
    }

    const FString AssetName = StructAsset->GetName();
    TWeakObjectPtr<UObject> WeakStructAsset(StructAsset);

    TSharedPtr<FUICommandList> CommandList = MakeShareable(new FUICommandList);
    CommandList->MapAction(
        FN2CToolbarCommand::Get().ExportCommand,
        FExecuteAction::CreateLambda([this, WeakStructAsset]()
        {
            UObject* Asset = WeakStructAsset.Get();
            if (!Asset || !N2CEditorIntegration_Private::IsStructObjectAsset(Asset))
            {
                FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(TEXT("NOT OK") LINE_TERMINATOR LINE_TERMINATOR TEXT("The Struct editor asset is no longer valid.")));
                ShowN2CNotification(TEXT("Struct editor N2C export failed: invalid asset"), SNotificationItem::CS_Fail);
                return;
            }

            TArray<FAssetData> Assets;
            Assets.Add(FAssetData(Asset));
            ExecuteBack2DeadExportSelectedStructs(Assets);
        }),
        FCanExecuteAction::CreateLambda([WeakStructAsset]()
        {
            return WeakStructAsset.IsValid();
        })
    );

    CommandList->MapAction(
        FN2CToolbarCommand::Get().ImportCommand,
        FExecuteAction::CreateRaw(this, &FN2CEditorIntegration::ExecuteBack2DeadImportStructEditor, WeakStructAsset),
        FCanExecuteAction::CreateLambda([WeakStructAsset]()
        {
            return WeakStructAsset.IsValid();
        })
    );

    AssetEditorCommandLists.Add(CommandList);

    TSharedPtr<FExtender> ToolbarExtender = MakeShareable(new FExtender);
    ToolbarExtender->AddToolBarExtension(
        "Asset",
        EExtensionHook::After,
        CommandList,
        FToolBarExtensionDelegate::CreateLambda([this, CommandList, WeakStructAsset](FToolBarBuilder& Builder)
        {
            Builder.AddSeparator();
            Builder.AddToolBarButton(
                FN2CToolbarCommand::Get().ExportCommand,
                NAME_None,
                FText::FromString(TEXT("Export N2C Struct")),
                FText::FromString(TEXT("Export the currently edited Struct asset into an AI-friendly N2C ZIP archive.")),
                FSlateIcon(FName("NodeToCodeStyle"), FName("NodeToCode.ExportButton"), FName("NodeToCode.ExportButton.Small"))
            );
            Builder.AddToolBarButton(
                FN2CToolbarCommand::Get().ImportCommand,
                NAME_None,
                FText::FromString(TEXT("Import N2C Struct")),
                FText::FromString(TEXT("Import N2C struct fields from an N2C ZIP/JSON into the currently edited UserDefinedStruct.")),
                FSlateIcon(FName("NodeToCodeStyle"), FName("NodeToCode.ImportButton"), FName("NodeToCode.ImportButton.Small"))
            );


            Builder.AddComboButton(
                FUIAction(),
                FOnGetContent::CreateLambda([this, WeakStructAsset]()
                {
                    return MakeRestoreBackupDropdownMenu(WeakStructAsset);
                }),
                FText::FromString(TEXT("Previous Backup")),
                FText::FromString(TEXT("Restore the latest N2C backup or choose a backup for this Struct asset.")),
                FSlateIcon(FName("NodeToCodeStyle"), FName("NodeToCode.ImportButton"), FName("NodeToCode.ImportButton.Small")),
                true
            );
        })
    );

    AssetEditorToolkit->AddToolbarExtender(ToolbarExtender);
    AssetEditorToolkit->RegenerateMenusAndToolbars();

    FN2CLogger::Get().Log(
        FString::Printf(TEXT("Registered N2C toolbar button for Struct editor: %s"), *AssetName),
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
    const FString ExportDir = FPaths::ProjectSavedDir() / TEXT("NodeToCode/ProjectExports") / FString::Printf(TEXT("N2C_%s_%s"), *SafeBlueprintName, *Timestamp);
    const FString MainPath = ExportDir / FString::Printf(TEXT("N2C_%s_%s.json"), *SafeBlueprintName, *Timestamp);

    if (!FN2CAIExport::SaveJsonToFile(JsonOutput, MainPath, Error))
    {
        FN2CLogger::Get().LogError(FString::Printf(TEXT("Back2Dead export save failed: %s"), *Error));
        ShowN2CNotification(FString::Printf(TEXT("NodeToCode export save failed: %s"), *Error), SNotificationItem::CS_Fail);
        return;
    }

    FString Report;
    FString CoverageJson;
    FString CoveragePath;
    if (!SaveBlueprintCoverageSidecar(BlueprintObj, JsonOutput, MainPath, CoverageJson, CoveragePath, Report))
    {
        FN2CLogger::Get().LogError(FString::Printf(TEXT("Back2Dead coverage export failed: %s"), *Report));
        ShowN2CNotification(TEXT("NodeToCode export failed: coverage sidecar was not saved"), SNotificationItem::CS_Fail);
        return;
    }
    SaveFunctionSplitFiles(JsonOutput, ExportDir / TEXT("functions"), Report);

    const FString ZipPath = ExportDir + TEXT(".zip");
    ZipDirectoryForSharing(ExportDir, ZipPath, Report);
    DeleteExportWorkDirAfterZip(ExportDir, ZipPath, Report);

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

    bool bDeveloperOverride = false;
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

    if (!bPatchLooksValid && !PatchJson.IsEmpty())
    {
        FN2CPreflightResult OverridePreflight;
        FString OverrideReport;
        const bool bOnlyVerificationGap = FN2CPatchImporter::PreflightPatch(BlueprintObj, PatchJson, true, OverridePreflight, OverrideReport) &&
            OverridePreflight.bHasVerificationGaps && !OverridePreflight.bHasRuntimeBlockers;
        if (bOnlyVerificationGap)
        {
            const EAppReturnType::Type OverrideChoice = FMessageDialog::Open(
                EAppMsgType::YesNo,
                FText::FromString(TEXT("Strict Apply is blocked only because this patch has supported-but-unverified variants. Developer override is temporary for this operation and cannot permit unsupported nodes, runtime semantic loss, missing metadata, unresolved metadata, parse/schema errors, or unknown actions. Continue with developer override?")));
            if (OverrideChoice == EAppReturnType::Yes)
            {
                bDeveloperOverride = true;
                bPatchLooksValid = FN2CPatchImporter::DryRunPatch(BlueprintObj, PatchJson, DryRunReport, true);
            }
        }
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
        : FN2CPatchImporter::ApplyPatchToBlueprint(BlueprintObj, PatchJson, ApplyReport, bDeveloperOverride);

    if (bApplied)
    {
        QueueBlueprintAssistAutoFormatIfEnabled(AutoFormatTargets, ApplyReport);
    }

    FN2CLogger::Get().Log(FString::Printf(TEXT("Back2Dead import apply report:\n%s"), *ApplyReport), bApplied ? EN2CLogSeverity::Info : EN2CLogSeverity::Error);

    FMessageDialog::Open(
        EAppMsgType::Ok,
        FText::FromString(MakePatchApplyDialogText(bApplied, ApplyDialogName, ApplyReport, DryRunReport))
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
        FText::FromString(MakePatchApplyDialogText(bApplied, ApplyDialogName, ApplyReport, DryRunReport))
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

void FN2CEditorIntegration::RegisterContentBrowserFolderContextMenu()
{
    if (ContentBrowserFolderExtenderDelegateHandle.IsValid())
    {
        return;
    }

    FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
    TArray<FContentBrowserMenuExtender_SelectedPaths>& Extenders = ContentBrowserModule.GetAllPathViewContextMenuExtenders();
    Extenders.Add(FContentBrowserMenuExtender_SelectedPaths::CreateRaw(this, &FN2CEditorIntegration::OnExtendContentBrowserFolderSelectionMenu));
    ContentBrowserFolderExtenderDelegateHandle = Extenders.Last().GetHandle();

    FN2CLogger::Get().Log(TEXT("N2C Content Browser folder context menu registered"), EN2CLogSeverity::Info);
}

TSharedRef<FExtender> FN2CEditorIntegration::OnExtendContentBrowserFolderSelectionMenu(const TArray<FString>& SelectedPaths)
{
    TSharedRef<FExtender> Extender = MakeShareable(new FExtender);

    if (SelectedPaths.Num() <= 0)
    {
        return Extender;
    }

    Extender->AddMenuExtension(
        TEXT("PathContextBulkOperations"),
        EExtensionHook::After,
        nullptr,
        FMenuExtensionDelegate::CreateRaw(this, &FN2CEditorIntegration::AddContentBrowserFolderN2CMenuEntry, SelectedPaths)
    );

    return Extender;
}

void FN2CEditorIntegration::AddContentBrowserFolderN2CMenuEntry(FMenuBuilder& MenuBuilder, TArray<FString> SelectedPaths)
{
    MenuBuilder.BeginSection(TEXT("N2CFolderExport"), FText::FromString(TEXT("NodeToCode")));

    MenuBuilder.AddMenuEntry(
        FText::FromString(TEXT("Export N2C...")),
        FText::FromString(TEXT("Choose Blueprint, Niagara System, Enum and/or Struct under the selected folder(s) and export one N2C ZIP archive.")),
        FSlateIcon(FName("NodeToCodeStyle"), FName("NodeToCode.ExportButton"), FName("NodeToCode.ExportButton.Small")),
        FUIAction(FExecuteAction::CreateRaw(this, &FN2CEditorIntegration::ExecuteBack2DeadExportSelectedFoldersWithPicker, SelectedPaths))
    );

    MenuBuilder.EndSection();
}


TSharedRef<FExtender> FN2CEditorIntegration::OnExtendContentBrowserAssetSelectionMenu(const TArray<FAssetData>& SelectedAssets)
{
    using namespace N2CEditorIntegration_Private;

    TSharedRef<FExtender> Extender = MakeShareable(new FExtender);

    bool bHasBlueprint = false;
    bool bHasNiagara = false;
    bool bHasEnum = false;
    bool bHasStruct = false;
    for (const FAssetData& AssetData : SelectedAssets)
    {
        bHasBlueprint |= IsBlueprintAssetData(AssetData);
        bHasNiagara |= IsNiagaraAssetData(AssetData);
        bHasEnum |= IsEnumAssetData(AssetData);
        bHasStruct |= IsStructAssetData(AssetData);

        if (bHasBlueprint && bHasNiagara && bHasEnum && bHasStruct)
        {
            break;
        }
    }

    // Always add the N2C section for non-empty asset selections. The export action itself
    // still validates supported asset kinds, but this prevents the entry from disappearing
    // when UE4.27 reports UserDefinedEnum/UserDefinedStruct/Niagara class names inconsistently.
    if (SelectedAssets.Num() <= 0)
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
    using namespace N2CEditorIntegration_Private;

    bool bHasBlueprint = false;
    bool bHasNiagara = false;
    bool bHasEnum = false;
    bool bHasStruct = false;
    for (const FAssetData& AssetData : SelectedAssets)
    {
        bHasBlueprint |= IsBlueprintAssetData(AssetData);
        bHasNiagara |= IsNiagaraAssetData(AssetData);
        bHasEnum |= IsEnumAssetData(AssetData);
        bHasStruct |= IsStructAssetData(AssetData);
    }

    MenuBuilder.BeginSection(TEXT("N2CExport"), FText::FromString(TEXT("NodeToCode")));

    MenuBuilder.AddMenuEntry(
        FText::FromString(TEXT("Export N2C...")),
        FText::FromString(TEXT("Choose Blueprint, Niagara System, Enum and/or Struct from the selected assets and export one N2C ZIP archive.")),
        FSlateIcon(FName("NodeToCodeStyle"), FName("NodeToCode.ExportButton"), FName("NodeToCode.ExportButton.Small")),
        FUIAction(FExecuteAction::CreateRaw(this, &FN2CEditorIntegration::ExecuteBack2DeadExportSelectedAssetsWithPicker, SelectedAssets))
    );


    MenuBuilder.AddMenuSeparator();

    MenuBuilder.AddMenuEntry(
        FText::FromString(TEXT("Restore previous N2C backup")),
        FText::FromString(TEXT("Restore the latest backup from Saved/NodeToCode/Backups for the selected asset(s).")),
        FSlateIcon(FName("NodeToCodeStyle"), FName("NodeToCode.ImportButton"), FName("NodeToCode.ImportButton.Small")),
        FUIAction(FExecuteAction::CreateRaw(this, &FN2CEditorIntegration::ExecuteRestoreLatestBackupForSelectedAssets, SelectedAssets))
    );

    if (SelectedAssets.Num() == 1)
    {
        MenuBuilder.AddMenuEntry(
            FText::FromString(TEXT("Choose N2C backup...")),
            FText::FromString(TEXT("Choose one .uasset backup file and restore it over the selected asset package.")),
            FSlateIcon(FName("NodeToCodeStyle"), FName("NodeToCode.ImportButton"), FName("NodeToCode.ImportButton.Small")),
            FUIAction(FExecuteAction::CreateRaw(this, &FN2CEditorIntegration::ExecuteRestoreChosenBackupForSelectedAsset, SelectedAssets))
        );
    }

    MenuBuilder.AddMenuEntry(
        FText::FromString(TEXT("Pending restore status...")),
        FText::FromString(TEXT("Show queued/applied N2C restore state.")),
        FSlateIcon(FName("NodeToCodeStyle"), FName("NodeToCode.ImportButton"), FName("NodeToCode.ImportButton.Small")),
        FUIAction(FExecuteAction::CreateRaw(this, &FN2CEditorIntegration::ExecuteShowPendingRestoreStatus))
    );

    MenuBuilder.AddMenuEntry(
        FText::FromString(TEXT("Cancel pending restores...")),
        FText::FromString(TEXT("Cancel queued N2C deferred restores before restarting UE.")),
        FSlateIcon(FName("NodeToCodeStyle"), FName("NodeToCode.ImportButton"), FName("NodeToCode.ImportButton.Small")),
        FUIAction(FExecuteAction::CreateRaw(this, &FN2CEditorIntegration::ExecuteCancelPendingRestore))
    );

    MenuBuilder.AddMenuSeparator();

    if (bHasNiagara)
    {
        MenuBuilder.AddMenuEntry(
            FText::FromString(TEXT("Import N2C Niagara")),
            FText::FromString(TEXT("Import supported N2C Niagara parameter values from an N2C ZIP/JSON into the target NiagaraSystem. Graph recreation is not implemented yet.")),
            FSlateIcon(FName("NodeToCodeStyle"), FName("NodeToCode.ImportButton"), FName("NodeToCode.ImportButton.Small")),
            FUIAction(FExecuteAction::CreateRaw(this, &FN2CEditorIntegration::ExecuteBack2DeadImportSelectedNiagara, SelectedAssets))
        );
    }

    if (bHasEnum)
    {
        MenuBuilder.AddMenuEntry(
            FText::FromString(TEXT("Import N2C Enum")),
            FText::FromString(TEXT("Import N2C enum values from an N2C ZIP/JSON into the selected UserDefinedEnum asset(s).")),
            FSlateIcon(FName("NodeToCodeStyle"), FName("NodeToCode.ImportButton"), FName("NodeToCode.ImportButton.Small")),
            FUIAction(FExecuteAction::CreateRaw(this, &FN2CEditorIntegration::ExecuteBack2DeadImportSelectedEnums, SelectedAssets))
        );
    }

    if (bHasStruct)
    {
        MenuBuilder.AddMenuEntry(
            FText::FromString(TEXT("Import N2C Struct")),
            FText::FromString(TEXT("Import N2C struct fields from an N2C ZIP/JSON into the selected UserDefinedStruct asset(s).")),
            FSlateIcon(FName("NodeToCodeStyle"), FName("NodeToCode.ImportButton"), FName("NodeToCode.ImportButton.Small")),
            FUIAction(FExecuteAction::CreateRaw(this, &FN2CEditorIntegration::ExecuteBack2DeadImportSelectedStructs, SelectedAssets))
        );
    }

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
    TArray<FString> CoverageSidecars;
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

        FString CoverageJson;
        FString CoveragePath;
        if (!SaveBlueprintCoverageSidecar(Blueprint, BlueprintJson, MainPath, CoverageJson, CoveragePath, OutReport))
        {
            ++SkippedCount;
            continue;
        }
        CoverageSidecars.Add(CoverageJson);

        FString FunctionReport;
        SaveFunctionSplitFiles(BlueprintJson, BlueprintDir / TEXT("functions"), FunctionReport);

        TSharedPtr<FJsonObject> ManifestAsset = MakeShared<FJsonObject>();
        ManifestAsset->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
        ManifestAsset->SetStringField(TEXT("blueprint_path"), Blueprint->GetPathName());
        ManifestAsset->SetStringField(TEXT("asset_object_path"), AssetData.ObjectPath.ToString());
        ManifestAsset->SetStringField(TEXT("json"), MainRelativePath);
        ManifestAsset->SetStringField(TEXT("coverage"), MainRelativePath.LeftChop(5) + TEXT(".coverage.json"));
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
    TSharedPtr<FJsonObject> CoverageSummary;
    FN2CCoverageClassifier::BuildAggregateCoverage(CoverageSidecars, CoverageSummary);
    Manifest->SetObjectField(TEXT("coverage_summary"), CoverageSummary);
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

    if (FPaths::FileExists(OutZipPath))
    {
        DeleteExportWorkDirAfterZip(ExportRootDir, OutZipPath, OutReport);
    }

    if (!FPaths::FileExists(OutZipPath))
    {
        OutReport += FString::Printf(TEXT("ERROR: ZIP file was not created: %s"), *OutZipPath) + LINE_TERMINATOR;
        return false;
    }

    OutReport += FString::Printf(TEXT("Project N2C export complete: %d Blueprint(s), %d skipped asset(s)."), ExportedCount, SkippedCount) + LINE_TERMINATOR;
    return true;
}

bool FN2CEditorIntegration::ExportNiagaraAssetsToSingleArchive(const TArray<FAssetData>& NiagaraAssets, const FString& ExportLabel, FString& OutZipPath, FString& OutReport)
{
    using namespace N2CEditorIntegration_Private;

    OutZipPath.Empty();
    OutReport.Empty();

    if (NiagaraAssets.Num() <= 0)
    {
        OutReport += TEXT("ERROR: no Niagara assets were provided for export.") LINE_TERMINATOR;
        return false;
    }

    const FString Timestamp = MakeExportTimestamp();
    const FString SafeLabel = SanitizeForFileName(ExportLabel.IsEmpty() ? TEXT("Niagara") : ExportLabel);
    const FString ExportRootDir = FPaths::ProjectSavedDir() / TEXT("NodeToCode/ProjectExports") / FString::Printf(TEXT("N2C_%s_%s"), *SafeLabel, *Timestamp);
    const FString NiagaraDir = ExportRootDir / TEXT("niagara");

    IFileManager::Get().DeleteDirectory(*ExportRootDir, false, true);
    IFileManager::Get().MakeDirectory(*NiagaraDir, true);

    TArray<TSharedPtr<FJsonValue>> ManifestAssets;
    TArray<FString> CoverageSidecars;
    TSet<FName> SeenPackages;
    int32 ExportedCount = 0;
    int32 SkippedCount = 0;

    FScopedSlowTask SlowTask(NiagaraAssets.Num(), FText::FromString(FString::Printf(TEXT("Exporting %d Niagara asset(s) to N2C..."), NiagaraAssets.Num())));
    SlowTask.MakeDialog(true);

    for (const FAssetData& AssetData : NiagaraAssets)
    {
        SlowTask.EnterProgressFrame(1.0f, FText::FromName(AssetData.AssetName));
        if (SlowTask.ShouldCancel())
        {
            OutReport += TEXT("WARNING: Niagara export cancelled by user.") LINE_TERMINATOR;
            break;
        }

        if (SeenPackages.Contains(AssetData.PackageName))
        {
            continue;
        }
        SeenPackages.Add(AssetData.PackageName);

        if (!IsNiagaraAssetData(AssetData))
        {
            ++SkippedCount;
            OutReport += FString::Printf(TEXT("WARNING: skipped non-Niagara asset: %s"), *AssetData.ObjectPath.ToString()) + LINE_TERMINATOR;
            continue;
        }

        UObject* LoadedAsset = AssetData.GetAsset();
        if (!LoadedAsset)
        {
            ++SkippedCount;
            OutReport += FString::Printf(TEXT("WARNING: skipped unloaded Niagara asset: %s"), *AssetData.ObjectPath.ToString()) + LINE_TERMINATOR;
            continue;
        }

        FString NiagaraJson;
        FString Error;
        if (!FN2CAIExport::BuildNiagaraAssetAIJson(LoadedAsset, NiagaraJson, Error))
        {
            ++SkippedCount;
            OutReport += FString::Printf(TEXT("ERROR: failed to export Niagara asset %s: %s"), *LoadedAsset->GetPathName(), *Error) + LINE_TERMINATOR;
            continue;
        }

        const FString SafeNiagaraPath = SanitizeForFileName(LoadedAsset->GetPathName()).Replace(TEXT("."), TEXT("_"));
        const FString SafeAssetName = SanitizeForFileName(LoadedAsset->GetName());
        const FString MainRelativePath = FString::Printf(TEXT("niagara/%s/N2C_%s_%s.json"), *SafeNiagaraPath, *SafeAssetName, *Timestamp);
        const FString MainPath = ExportRootDir / MainRelativePath;

        FString SaveError;
        if (!SaveTextFile(MainPath, NiagaraJson, SaveError))
        {
            ++SkippedCount;
            OutReport += SaveError + LINE_TERMINATOR;
            continue;
        }

        TSharedPtr<FJsonObject> ManifestAsset = MakeShared<FJsonObject>();
        ManifestAsset->SetStringField(TEXT("asset_name"), LoadedAsset->GetName());
        ManifestAsset->SetStringField(TEXT("asset_path"), LoadedAsset->GetPathName());
        ManifestAsset->SetStringField(TEXT("asset_class"), LoadedAsset->GetClass() ? LoadedAsset->GetClass()->GetName() : TEXT(""));
        ManifestAsset->SetStringField(TEXT("asset_object_path"), AssetData.ObjectPath.ToString());
        ManifestAsset->SetStringField(TEXT("json"), MainRelativePath);
        ManifestAsset->SetStringField(TEXT("coverage"), MainRelativePath.LeftChop(5) + TEXT(".coverage.json"));
        ManifestAssets.Add(MakeShared<FJsonValueObject>(ManifestAsset));

        ++ExportedCount;
        OutReport += FString::Printf(TEXT("Exported Niagara asset: %s"), *LoadedAsset->GetPathName()) + LINE_TERMINATOR;
    }

    TSharedPtr<FJsonObject> Manifest = MakeShared<FJsonObject>();
    Manifest->SetStringField(TEXT("schema"), TEXT("N2C_NIAGARA_PROJECT_EXPORT_V1"));
    Manifest->SetStringField(TEXT("export_kind"), TEXT("NiagaraProjectArchive"));
    Manifest->SetStringField(TEXT("export_label"), ExportLabel);
    Manifest->SetStringField(TEXT("engine_target"), TEXT("UE4.27"));
    Manifest->SetStringField(TEXT("timestamp"), Timestamp);
    Manifest->SetNumberField(TEXT("source_asset_count"), NiagaraAssets.Num());
    Manifest->SetNumberField(TEXT("exported_niagara_count"), ExportedCount);
    Manifest->SetNumberField(TEXT("skipped_asset_count"), SkippedCount);
    Manifest->SetArrayField(TEXT("assets"), ManifestAssets);
    Manifest->SetStringField(TEXT("import_status"), TEXT("parameter_import_v1_supported_no_graph_rebuild"));

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
        OutReport += TEXT("ERROR: export finished with zero exported Niagara assets.") LINE_TERMINATOR;
        return false;
    }

    OutZipPath = ExportRootDir + TEXT(".zip");
    ZipDirectoryForSharing(ExportRootDir, OutZipPath, OutReport);

    if (FPaths::FileExists(OutZipPath))
    {
        DeleteExportWorkDirAfterZip(ExportRootDir, OutZipPath, OutReport);
    }

    if (!FPaths::FileExists(OutZipPath))
    {
        OutReport += FString::Printf(TEXT("ERROR: ZIP file was not created: %s"), *OutZipPath) + LINE_TERMINATOR;
        return false;
    }

    OutReport += FString::Printf(TEXT("Niagara N2C export complete: %d Niagara asset(s), %d skipped asset(s)."), ExportedCount, SkippedCount) + LINE_TERMINATOR;
    return true;
}


bool FN2CEditorIntegration::ExportMixedAssetsToSingleArchive(const TArray<FAssetData>& BlueprintAssets, const TArray<FAssetData>& NiagaraAssets, const TArray<FAssetData>& EnumAssets, const TArray<FAssetData>& StructAssets, const FString& ExportLabel, FString& OutZipPath, FString& OutReport)
{
    using namespace N2CEditorIntegration_Private;

    OutZipPath.Empty();
    OutReport.Empty();

    const int32 SourceAssetCount = BlueprintAssets.Num() + NiagaraAssets.Num() + EnumAssets.Num() + StructAssets.Num();
    if (SourceAssetCount <= 0)
    {
        OutReport += TEXT("ERROR: no supported assets were provided for export.") LINE_TERMINATOR;
        return false;
    }

    const FString Timestamp = MakeExportTimestamp();
    const FString SafeLabel = SanitizeForFileName(ExportLabel.IsEmpty() ? TEXT("Project") : ExportLabel);
    const FString ExportRootDir = FPaths::ProjectSavedDir() / TEXT("NodeToCode/ProjectExports") / FString::Printf(TEXT("N2C_%s_%s"), *SafeLabel, *Timestamp);
    const FString BlueprintsDir = ExportRootDir / TEXT("blueprints");
    const FString NiagaraDir = ExportRootDir / TEXT("niagara");
    const FString EnumsDir = ExportRootDir / TEXT("enums");
    const FString StructsDir = ExportRootDir / TEXT("structs");

    IFileManager::Get().DeleteDirectory(*ExportRootDir, false, true);
    IFileManager::Get().MakeDirectory(*BlueprintsDir, true);
    IFileManager::Get().MakeDirectory(*NiagaraDir, true);
    IFileManager::Get().MakeDirectory(*EnumsDir, true);
    IFileManager::Get().MakeDirectory(*StructsDir, true);

    TArray<TSharedPtr<FJsonValue>> ManifestAssets;
    TArray<FString> CoverageSidecars;
    TSet<FName> SeenPackages;
    int32 ExportedBlueprintCount = 0;
    int32 ExportedNiagaraCount = 0;
    int32 ExportedEnumCount = 0;
    int32 ExportedStructCount = 0;
    int32 SkippedCount = 0;

    FScopedSlowTask SlowTask(SourceAssetCount, FText::FromString(FString::Printf(TEXT("Exporting %d N2C asset(s)..."), SourceAssetCount)));
    SlowTask.MakeDialog(true);

    auto AddManifestAsset = [&ManifestAssets](const FString& Kind, UObject* LoadedAsset, const FAssetData& AssetData, const FString& JsonRelativePath)
    {
        TSharedPtr<FJsonObject> ManifestAsset = MakeShared<FJsonObject>();
        ManifestAsset->SetStringField(TEXT("kind"), Kind);
        ManifestAsset->SetStringField(TEXT("asset_name"), LoadedAsset ? LoadedAsset->GetName() : AssetData.AssetName.ToString());
        ManifestAsset->SetStringField(TEXT("asset_path"), LoadedAsset ? LoadedAsset->GetPathName() : AssetData.ObjectPath.ToString());
        ManifestAsset->SetStringField(TEXT("asset_class"), LoadedAsset && LoadedAsset->GetClass() ? LoadedAsset->GetClass()->GetName() : AssetData.AssetClass.ToString());
        ManifestAsset->SetStringField(TEXT("asset_object_path"), AssetData.ObjectPath.ToString());
        ManifestAsset->SetStringField(TEXT("json"), JsonRelativePath);
        ManifestAssets.Add(MakeShared<FJsonValueObject>(ManifestAsset));
    };

    auto ShouldSkipSeenPackage = [&SeenPackages](const FAssetData& AssetData) -> bool
    {
        if (SeenPackages.Contains(AssetData.PackageName))
        {
            return true;
        }
        SeenPackages.Add(AssetData.PackageName);
        return false;
    };

    for (const FAssetData& AssetData : BlueprintAssets)
    {
        SlowTask.EnterProgressFrame(1.0f, FText::FromName(AssetData.AssetName));
        if (SlowTask.ShouldCancel())
        {
            OutReport += TEXT("WARNING: export cancelled by user.") LINE_TERMINATOR;
            break;
        }
        if (ShouldSkipSeenPackage(AssetData))
        {
            continue;
        }
        UObject* LoadedAsset = AssetData.GetAsset();
        UBlueprint* Blueprint = Cast<UBlueprint>(LoadedAsset);
        if (!Blueprint)
        {
            ++SkippedCount;
            OutReport += FString::Printf(TEXT("WARNING: skipped non-Blueprint asset: %s"), *AssetData.ObjectPath.ToString()) + LINE_TERMINATOR;
            continue;
        }

        FString Json;
        FString Error;
        if (!FN2CAIExport::BuildBlueprintAIJson(Blueprint, Json, Error))
        {
            ++SkippedCount;
            OutReport += FString::Printf(TEXT("ERROR: failed to export Blueprint %s: %s"), *Blueprint->GetPathName(), *Error) + LINE_TERMINATOR;
            continue;
        }

        const FString SafeAssetPath = SanitizeForFileName(Blueprint->GetPathName()).Replace(TEXT("."), TEXT("_"));
        const FString SafeAssetName = SanitizeForFileName(Blueprint->GetName());
        const FString MainRelativePath = FString::Printf(TEXT("blueprints/%s/N2C_%s_%s.json"), *SafeAssetPath, *SafeAssetName, *Timestamp);
        const FString MainPath = ExportRootDir / MainRelativePath;

        FString SaveError;
        if (!SaveTextFile(MainPath, Json, SaveError))
        {
            ++SkippedCount;
            OutReport += SaveError + LINE_TERMINATOR;
            continue;
        }

        FString CoverageJson;
        FString CoveragePath;
        if (!SaveBlueprintCoverageSidecar(Blueprint, Json, MainPath, CoverageJson, CoveragePath, OutReport))
        {
            ++SkippedCount;
            continue;
        }
        CoverageSidecars.Add(CoverageJson);

        FString FunctionReport;
        SaveFunctionSplitFiles(Json, BlueprintsDir / SafeAssetPath / TEXT("functions"), FunctionReport);

        AddManifestAsset(TEXT("blueprint"), Blueprint, AssetData, MainRelativePath);
        ++ExportedBlueprintCount;
        OutReport += FString::Printf(TEXT("Exported Blueprint: %s"), *Blueprint->GetPathName()) + LINE_TERMINATOR;
    }

    for (const FAssetData& AssetData : NiagaraAssets)
    {
        SlowTask.EnterProgressFrame(1.0f, FText::FromName(AssetData.AssetName));
        if (SlowTask.ShouldCancel())
        {
            OutReport += TEXT("WARNING: export cancelled by user.") LINE_TERMINATOR;
            break;
        }
        if (ShouldSkipSeenPackage(AssetData))
        {
            continue;
        }
        if (!IsNiagaraAssetData(AssetData))
        {
            ++SkippedCount;
            OutReport += FString::Printf(TEXT("WARNING: skipped non-NiagaraSystem asset: %s"), *AssetData.ObjectPath.ToString()) + LINE_TERMINATOR;
            continue;
        }

        UObject* LoadedAsset = AssetData.GetAsset();
        if (!LoadedAsset)
        {
            ++SkippedCount;
            OutReport += FString::Printf(TEXT("WARNING: skipped unloaded NiagaraSystem asset: %s"), *AssetData.ObjectPath.ToString()) + LINE_TERMINATOR;
            continue;
        }

        FString Json;
        FString Error;
        if (!FN2CAIExport::BuildNiagaraAssetAIJson(LoadedAsset, Json, Error))
        {
            ++SkippedCount;
            OutReport += FString::Printf(TEXT("ERROR: failed to export NiagaraSystem %s: %s"), *LoadedAsset->GetPathName(), *Error) + LINE_TERMINATOR;
            continue;
        }

        const FString SafeAssetPath = SanitizeForFileName(LoadedAsset->GetPathName()).Replace(TEXT("."), TEXT("_"));
        const FString SafeAssetName = SanitizeForFileName(LoadedAsset->GetName());
        const FString MainRelativePath = FString::Printf(TEXT("niagara/%s/N2C_%s_%s.json"), *SafeAssetPath, *SafeAssetName, *Timestamp);
        const FString MainPath = ExportRootDir / MainRelativePath;

        FString SaveError;
        if (!SaveTextFile(MainPath, Json, SaveError))
        {
            ++SkippedCount;
            OutReport += SaveError + LINE_TERMINATOR;
            continue;
        }

        AddManifestAsset(TEXT("niagara_system"), LoadedAsset, AssetData, MainRelativePath);
        ++ExportedNiagaraCount;
        OutReport += FString::Printf(TEXT("Exported NiagaraSystem: %s"), *LoadedAsset->GetPathName()) + LINE_TERMINATOR;
    }

    for (const FAssetData& AssetData : EnumAssets)
    {
        SlowTask.EnterProgressFrame(1.0f, FText::FromName(AssetData.AssetName));
        if (SlowTask.ShouldCancel())
        {
            OutReport += TEXT("WARNING: export cancelled by user.") LINE_TERMINATOR;
            break;
        }
        if (ShouldSkipSeenPackage(AssetData))
        {
            continue;
        }
        if (!IsEnumAssetData(AssetData))
        {
            ++SkippedCount;
            OutReport += FString::Printf(TEXT("WARNING: skipped non-Enum asset: %s"), *AssetData.ObjectPath.ToString()) + LINE_TERMINATOR;
            continue;
        }

        UObject* LoadedAsset = AssetData.GetAsset();
        if (!LoadedAsset)
        {
            ++SkippedCount;
            OutReport += FString::Printf(TEXT("WARNING: skipped unloaded Enum asset: %s"), *AssetData.ObjectPath.ToString()) + LINE_TERMINATOR;
            continue;
        }

        FString Json;
        FString Error;
        if (!FN2CAIExport::BuildEnumAssetAIJson(LoadedAsset, Json, Error))
        {
            ++SkippedCount;
            OutReport += FString::Printf(TEXT("ERROR: failed to export Enum %s: %s"), *LoadedAsset->GetPathName(), *Error) + LINE_TERMINATOR;
            continue;
        }

        const FString SafeAssetPath = SanitizeForFileName(LoadedAsset->GetPathName()).Replace(TEXT("."), TEXT("_"));
        const FString SafeAssetName = SanitizeForFileName(LoadedAsset->GetName());
        const FString MainRelativePath = FString::Printf(TEXT("enums/%s/N2C_%s_%s.json"), *SafeAssetPath, *SafeAssetName, *Timestamp);
        const FString MainPath = ExportRootDir / MainRelativePath;

        FString SaveError;
        if (!SaveTextFile(MainPath, Json, SaveError))
        {
            ++SkippedCount;
            OutReport += SaveError + LINE_TERMINATOR;
            continue;
        }

        AddManifestAsset(TEXT("enum"), LoadedAsset, AssetData, MainRelativePath);
        ++ExportedEnumCount;
        OutReport += FString::Printf(TEXT("Exported Enum: %s"), *LoadedAsset->GetPathName()) + LINE_TERMINATOR;
    }

    for (const FAssetData& AssetData : StructAssets)
    {
        SlowTask.EnterProgressFrame(1.0f, FText::FromName(AssetData.AssetName));
        if (SlowTask.ShouldCancel())
        {
            OutReport += TEXT("WARNING: export cancelled by user.") LINE_TERMINATOR;
            break;
        }
        if (ShouldSkipSeenPackage(AssetData))
        {
            continue;
        }
        if (!IsStructAssetData(AssetData))
        {
            ++SkippedCount;
            OutReport += FString::Printf(TEXT("WARNING: skipped non-Struct asset: %s"), *AssetData.ObjectPath.ToString()) + LINE_TERMINATOR;
            continue;
        }

        UObject* LoadedAsset = AssetData.GetAsset();
        if (!LoadedAsset)
        {
            ++SkippedCount;
            OutReport += FString::Printf(TEXT("WARNING: skipped unloaded Struct asset: %s"), *AssetData.ObjectPath.ToString()) + LINE_TERMINATOR;
            continue;
        }

        FString Json;
        FString Error;
        if (!FN2CAIExport::BuildStructAssetAIJson(LoadedAsset, Json, Error))
        {
            ++SkippedCount;
            OutReport += FString::Printf(TEXT("ERROR: failed to export Struct %s: %s"), *LoadedAsset->GetPathName(), *Error) + LINE_TERMINATOR;
            continue;
        }

        const FString SafeAssetPath = SanitizeForFileName(LoadedAsset->GetPathName()).Replace(TEXT("."), TEXT("_"));
        const FString SafeAssetName = SanitizeForFileName(LoadedAsset->GetName());
        const FString MainRelativePath = FString::Printf(TEXT("structs/%s/N2C_%s_%s.json"), *SafeAssetPath, *SafeAssetName, *Timestamp);
        const FString MainPath = ExportRootDir / MainRelativePath;

        FString SaveError;
        if (!SaveTextFile(MainPath, Json, SaveError))
        {
            ++SkippedCount;
            OutReport += SaveError + LINE_TERMINATOR;
            continue;
        }

        AddManifestAsset(TEXT("struct"), LoadedAsset, AssetData, MainRelativePath);
        ++ExportedStructCount;
        OutReport += FString::Printf(TEXT("Exported Struct: %s"), *LoadedAsset->GetPathName()) + LINE_TERMINATOR;
    }

    const int32 TotalExportedCount = ExportedBlueprintCount + ExportedNiagaraCount + ExportedEnumCount + ExportedStructCount;

    TSharedPtr<FJsonObject> Manifest = MakeShared<FJsonObject>();
    Manifest->SetStringField(TEXT("schema"), TEXT("N2C_PROJECT_EXPORT_V2"));
    Manifest->SetStringField(TEXT("export_kind"), TEXT("MixedProjectArchive"));
    Manifest->SetStringField(TEXT("export_label"), ExportLabel);
    Manifest->SetStringField(TEXT("engine_target"), TEXT("UE4.27"));
    Manifest->SetStringField(TEXT("timestamp"), Timestamp);
    Manifest->SetNumberField(TEXT("source_asset_count"), SourceAssetCount);
    Manifest->SetNumberField(TEXT("exported_total_count"), TotalExportedCount);
    Manifest->SetNumberField(TEXT("exported_blueprint_count"), ExportedBlueprintCount);
    Manifest->SetNumberField(TEXT("exported_niagara_system_count"), ExportedNiagaraCount);
    Manifest->SetNumberField(TEXT("exported_enum_count"), ExportedEnumCount);
    Manifest->SetNumberField(TEXT("exported_struct_count"), ExportedStructCount);
    Manifest->SetNumberField(TEXT("skipped_asset_count"), SkippedCount);
    TSharedPtr<FJsonObject> CoverageSummary;
    FN2CCoverageClassifier::BuildAggregateCoverage(CoverageSidecars, CoverageSummary);
    Manifest->SetObjectField(TEXT("coverage_summary"), CoverageSummary);
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

    if (TotalExportedCount <= 0)
    {
        OutReport += TEXT("ERROR: export finished with zero exported supported assets.") LINE_TERMINATOR;
        return false;
    }

    OutZipPath = ExportRootDir + TEXT(".zip");
    ZipDirectoryForSharing(ExportRootDir, OutZipPath, OutReport);

    if (FPaths::FileExists(OutZipPath))
    {
        DeleteExportWorkDirAfterZip(ExportRootDir, OutZipPath, OutReport);
    }

    if (!FPaths::FileExists(OutZipPath))
    {
        OutReport += FString::Printf(TEXT("ERROR: ZIP file was not created: %s"), *OutZipPath) + LINE_TERMINATOR;
        return false;
    }

    OutReport += FString::Printf(TEXT("Mixed N2C export complete: %d total asset(s): %d Blueprint, %d NiagaraSystem, %d Enum, %d Struct; %d skipped."),
        TotalExportedCount, ExportedBlueprintCount, ExportedNiagaraCount, ExportedEnumCount, ExportedStructCount, SkippedCount) + LINE_TERMINATOR;
    return true;
}

bool FN2CEditorIntegration::ExportEnumAssetsToSingleArchive(const TArray<FAssetData>& EnumAssets, const FString& ExportLabel, FString& OutZipPath, FString& OutReport)
{
    return ExportMixedAssetsToSingleArchive(TArray<FAssetData>(), TArray<FAssetData>(), EnumAssets, TArray<FAssetData>(), ExportLabel, OutZipPath, OutReport);
}

bool FN2CEditorIntegration::ExportStructAssetsToSingleArchive(const TArray<FAssetData>& StructAssets, const FString& ExportLabel, FString& OutZipPath, FString& OutReport)
{
    return ExportMixedAssetsToSingleArchive(TArray<FAssetData>(), TArray<FAssetData>(), TArray<FAssetData>(), StructAssets, ExportLabel, OutZipPath, OutReport);
}


void FN2CEditorIntegration::ExecuteBack2DeadExportSelectedFoldersWithPicker(TArray<FString> SelectedPaths)
{
    using namespace N2CEditorIntegration_Private;

    if (SelectedPaths.Num() <= 0)
    {
        ShowN2CNotification(TEXT("Folder N2C export cancelled: no folders selected"), SNotificationItem::CS_None);
        return;
    }

    TArray<FAssetData> FolderAssets;
    GetAssetsUnderPaths(SelectedPaths, FolderAssets);

    const FN2CExportKindSelection AvailableKinds = MakeExportAvailabilityFromAssets(FolderAssets);
    FN2CExportKindSelection Selection = AvailableKinds;

    if (!Selection.HasAnySelected())
    {
        FString PathList;
        for (const FString& Path : SelectedPaths)
        {
            PathList += FString::Printf(TEXT("- %s"), *Path) + LINE_TERMINATOR;
        }

        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(
            FString(TEXT("NOT OK")) + LINE_TERMINATOR + LINE_TERMINATOR
            + TEXT("Selected folder(s) do not contain supported N2C export types.") + LINE_TERMINATOR + LINE_TERMINATOR
            + TEXT("Folders:") + LINE_TERMINATOR + PathList));
        ShowN2CNotification(TEXT("Folder N2C export failed: no supported assets"), SNotificationItem::CS_Fail);
        return;
    }

    if (!PickExportKindsForN2C(
        TEXT("Export N2C Selected Folders"),
        TEXT("Choose what to export recursively from the selected Content Browser folder(s)."),
        AvailableKinds,
        Selection))
    {
        ShowN2CNotification(TEXT("Folder N2C export cancelled"), SNotificationItem::CS_None);
        return;
    }

    TArray<FAssetData> BlueprintAssets;
    TArray<FAssetData> NiagaraAssets;
    TArray<FAssetData> EnumAssets;
    TArray<FAssetData> StructAssets;
    FilterAssetsForN2CExportKinds(FolderAssets, Selection, BlueprintAssets, NiagaraAssets, EnumAssets, StructAssets);

    FString ZipPath;
    FString Report;
    const bool bOk = ExportMixedAssetsToSingleArchive(BlueprintAssets, NiagaraAssets, EnumAssets, StructAssets, TEXT("SelectedFolders"), ZipPath, Report);

    FN2CLogger::Get().Log(FString::Printf(TEXT("Selected folder mixed N2C export report:\n%s"), *Report), bOk ? EN2CLogSeverity::Info : EN2CLogSeverity::Error);

    if (bOk)
    {
        const FString Message = FString(TEXT("OK")) + LINE_TERMINATOR + LINE_TERMINATOR
            + TEXT("Selected folder export finished.") + LINE_TERMINATOR + LINE_TERMINATOR
            + FString::Printf(TEXT("Folders: %d"), SelectedPaths.Num()) + LINE_TERMINATOR
            + FString::Printf(TEXT("Blueprints: %d"), BlueprintAssets.Num()) + LINE_TERMINATOR
            + FString::Printf(TEXT("Niagara Systems: %d"), NiagaraAssets.Num()) + LINE_TERMINATOR
            + FString::Printf(TEXT("Enums: %d"), EnumAssets.Num()) + LINE_TERMINATOR
            + FString::Printf(TEXT("Structs: %d"), StructAssets.Num()) + LINE_TERMINATOR
            + FString::Printf(TEXT("ZIP: %s"), *ZipPath);
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Message));
        ShowN2CNotification(FString::Printf(TEXT("Folder N2C ZIP ready: %s"), *ZipPath), SNotificationItem::CS_Success);
    }
    else
    {
        const FString Message = FString(TEXT("NOT OK")) + LINE_TERMINATOR + LINE_TERMINATOR
            + TEXT("Selected folder export failed.") + LINE_TERMINATOR + LINE_TERMINATOR
            + TEXT("Problems:") + LINE_TERMINATOR
            + Report;
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Message));
        ShowN2CNotification(TEXT("Folder N2C export failed"), SNotificationItem::CS_Fail);
    }
}


void FN2CEditorIntegration::ExecuteBack2DeadExportSelectedAssetsWithPicker(TArray<FAssetData> SelectedAssets)
{
    using namespace N2CEditorIntegration_Private;

    const FN2CExportKindSelection AvailableKinds = MakeExportAvailabilityFromAssets(SelectedAssets);
    FN2CExportKindSelection Selection = AvailableKinds;

    if (!Selection.HasAnySelected())
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(TEXT("NOT OK") LINE_TERMINATOR LINE_TERMINATOR TEXT("Selected assets do not contain supported N2C export types.")));
        ShowN2CNotification(TEXT("Selected N2C export failed: no supported assets"), SNotificationItem::CS_Fail);
        return;
    }

    if (!PickExportKindsForN2C(
        TEXT("Export N2C Selected Assets"),
        TEXT("Choose what to export from the selected Content Browser assets."),
        AvailableKinds,
        Selection))
    {
        ShowN2CNotification(TEXT("Selected N2C export cancelled"), SNotificationItem::CS_None);
        return;
    }

    TArray<FAssetData> BlueprintAssets;
    TArray<FAssetData> NiagaraAssets;
    TArray<FAssetData> EnumAssets;
    TArray<FAssetData> StructAssets;
    FilterAssetsForN2CExportKinds(SelectedAssets, Selection, BlueprintAssets, NiagaraAssets, EnumAssets, StructAssets);

    FString ZipPath;
    FString Report;
    const bool bOk = ExportMixedAssetsToSingleArchive(BlueprintAssets, NiagaraAssets, EnumAssets, StructAssets, TEXT("SelectedAssets"), ZipPath, Report);

    FN2CLogger::Get().Log(FString::Printf(TEXT("Selected mixed N2C export report:\n%s"), *Report), bOk ? EN2CLogSeverity::Info : EN2CLogSeverity::Error);

    if (bOk)
    {
        const FString Message = FString(TEXT("OK")) + LINE_TERMINATOR + LINE_TERMINATOR
            + TEXT("Selected asset export finished.") + LINE_TERMINATOR + LINE_TERMINATOR
            + FString::Printf(TEXT("Blueprints: %d"), BlueprintAssets.Num()) + LINE_TERMINATOR
            + FString::Printf(TEXT("Niagara Systems: %d"), NiagaraAssets.Num()) + LINE_TERMINATOR
            + FString::Printf(TEXT("Enums: %d"), EnumAssets.Num()) + LINE_TERMINATOR
            + FString::Printf(TEXT("Structs: %d"), StructAssets.Num()) + LINE_TERMINATOR
            + FString::Printf(TEXT("ZIP: %s"), *ZipPath);
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Message));
        ShowN2CNotification(FString::Printf(TEXT("Selected N2C ZIP ready: %s"), *ZipPath), SNotificationItem::CS_Success);
    }
    else
    {
        const FString Message = FString(TEXT("NOT OK")) + LINE_TERMINATOR + LINE_TERMINATOR
            + TEXT("Selected asset export failed.") + LINE_TERMINATOR + LINE_TERMINATOR
            + TEXT("Problems:") + LINE_TERMINATOR
            + Report;
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Message));
        ShowN2CNotification(TEXT("Selected N2C export failed"), SNotificationItem::CS_Fail);
    }
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

void FN2CEditorIntegration::ExecuteBack2DeadExportSelectedNiagara(TArray<FAssetData> SelectedAssets)
{
    using namespace N2CEditorIntegration_Private;

    FString ZipPath;
    FString Report;
    const bool bOk = ExportNiagaraAssetsToSingleArchive(SelectedAssets, TEXT("SelectedNiagara"), ZipPath, Report);

    FN2CLogger::Get().Log(FString::Printf(TEXT("Selected Niagara N2C export report:\n%s"), *Report), bOk ? EN2CLogSeverity::Info : EN2CLogSeverity::Error);

    if (bOk)
    {
        const FString Message = FString(TEXT("OK")) + LINE_TERMINATOR + LINE_TERMINATOR
            + TEXT("Selected Niagara export finished.") + LINE_TERMINATOR + LINE_TERMINATOR
            + FString::Printf(TEXT("Niagara assets: %d"), CountNiagaraAssets(SelectedAssets)) + LINE_TERMINATOR
            + FString::Printf(TEXT("ZIP: %s"), *ZipPath);
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Message));
        ShowN2CNotification(FString::Printf(TEXT("Selected Niagara N2C ZIP ready: %s"), *ZipPath), SNotificationItem::CS_Success);
    }
    else
    {
        const FString Message = FString(TEXT("NOT OK")) + LINE_TERMINATOR + LINE_TERMINATOR
            + TEXT("Selected Niagara export failed.") + LINE_TERMINATOR + LINE_TERMINATOR
            + TEXT("Problems:") + LINE_TERMINATOR
            + Report;
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Message));
        ShowN2CNotification(TEXT("Selected Niagara N2C export failed"), SNotificationItem::CS_Fail);
    }
}

void FN2CEditorIntegration::ExecuteBack2DeadExportSelectedEnums(TArray<FAssetData> SelectedAssets)
{
    using namespace N2CEditorIntegration_Private;

    FString ZipPath;
    FString Report;
    const bool bOk = ExportEnumAssetsToSingleArchive(SelectedAssets, TEXT("SelectedEnums"), ZipPath, Report);

    FN2CLogger::Get().Log(FString::Printf(TEXT("Selected Enum N2C export report:\n%s"), *Report), bOk ? EN2CLogSeverity::Info : EN2CLogSeverity::Error);

    if (bOk)
    {
        const FString Message = FString(TEXT("OK")) + LINE_TERMINATOR + LINE_TERMINATOR
            + TEXT("Selected Enum export finished.") + LINE_TERMINATOR + LINE_TERMINATOR
            + FString::Printf(TEXT("Enums: %d"), CountEnumAssets(SelectedAssets)) + LINE_TERMINATOR
            + FString::Printf(TEXT("ZIP: %s"), *ZipPath);
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Message));
        ShowN2CNotification(FString::Printf(TEXT("Selected Enum N2C ZIP ready: %s"), *ZipPath), SNotificationItem::CS_Success);
    }
    else
    {
        const FString Message = FString(TEXT("NOT OK")) + LINE_TERMINATOR + LINE_TERMINATOR
            + TEXT("Selected Enum export failed.") + LINE_TERMINATOR + LINE_TERMINATOR
            + TEXT("Problems:") + LINE_TERMINATOR
            + Report;
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Message));
        ShowN2CNotification(TEXT("Selected Enum N2C export failed"), SNotificationItem::CS_Fail);
    }
}

void FN2CEditorIntegration::ExecuteBack2DeadExportSelectedStructs(TArray<FAssetData> SelectedAssets)
{
    using namespace N2CEditorIntegration_Private;

    FString ZipPath;
    FString Report;
    const bool bOk = ExportStructAssetsToSingleArchive(SelectedAssets, TEXT("SelectedStructs"), ZipPath, Report);

    FN2CLogger::Get().Log(FString::Printf(TEXT("Selected Struct N2C export report:\n%s"), *Report), bOk ? EN2CLogSeverity::Info : EN2CLogSeverity::Error);

    if (bOk)
    {
        const FString Message = FString(TEXT("OK")) + LINE_TERMINATOR + LINE_TERMINATOR
            + TEXT("Selected Struct export finished.") + LINE_TERMINATOR + LINE_TERMINATOR
            + FString::Printf(TEXT("Structs: %d"), CountStructAssets(SelectedAssets)) + LINE_TERMINATOR
            + FString::Printf(TEXT("ZIP: %s"), *ZipPath);
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Message));
        ShowN2CNotification(FString::Printf(TEXT("Selected Struct N2C ZIP ready: %s"), *ZipPath), SNotificationItem::CS_Success);
    }
    else
    {
        const FString Message = FString(TEXT("NOT OK")) + LINE_TERMINATOR + LINE_TERMINATOR
            + TEXT("Selected Struct export failed.") + LINE_TERMINATOR + LINE_TERMINATOR
            + TEXT("Problems:") + LINE_TERMINATOR
            + Report;
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Message));
        ShowN2CNotification(TEXT("Selected Struct N2C export failed"), SNotificationItem::CS_Fail);
    }
}



void FN2CEditorIntegration::ExecuteBack2DeadImportSelectedEnums(TArray<FAssetData> SelectedAssets)
{
    using namespace N2CEditorIntegration_Private;

    TArray<UObject*> TargetAssets;
    for (const FAssetData& AssetData : SelectedAssets)
    {
        if (!IsEnumAssetData(AssetData))
        {
            continue;
        }

        UObject* Asset = AssetData.GetAsset();
        if (Asset && Asset->IsA<UUserDefinedEnum>())
        {
            TargetAssets.Add(Asset);
        }
    }

    if (TargetAssets.Num() <= 0)
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(TEXT("NOT OK") LINE_TERMINATOR LINE_TERMINATOR TEXT("Select at least one UserDefinedEnum asset before importing N2C Enum data. Native C++ enums cannot be modified.")));
        ShowN2CNotification(TEXT("Enum import failed: no UserDefinedEnum selected"), SNotificationItem::CS_Fail);
        return;
    }

    FString ImportPath;
    if (!PickNiagaraImportFile(ImportPath))
    {
        ShowN2CNotification(TEXT("Enum N2C import cancelled: no file selected"), SNotificationItem::CS_None);
        return;
    }

    FString ImportJson;
    FString SourceLabel;
    FString LoadError;
    if (!LoadTypedN2CImportJsonFromFile(ImportPath, TEXT("enum"), TEXT("Enum"), TEXT("N2C_ENUM_PATCH_V1"), ImportJson, SourceLabel, LoadError))
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(FString(TEXT("NOT OK")) + LINE_TERMINATOR + LINE_TERMINATOR + LoadError));
        ShowN2CNotification(TEXT("Enum N2C import failed: could not load JSON"), SNotificationItem::CS_Fail);
        return;
    }

    FString DryRunReport;
    bool bDryRunOk = true;
    for (UObject* TargetAsset : TargetAssets)
    {
        FString AssetReport;
        const bool bAssetOk = ImportEnumN2CJsonToAsset(TargetAsset, ImportJson, true, AssetReport);
        DryRunReport += AssetReport + LINE_TERMINATOR;
        bDryRunOk = bDryRunOk && bAssetOk;
    }

    FN2CLogger::Get().Log(FString::Printf(TEXT("Enum selected import dry-run report:%s%s"), LINE_TERMINATOR, *DryRunReport), bDryRunOk ? EN2CLogSeverity::Info : EN2CLogSeverity::Error);

    if (!bDryRunOk)
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(FString(TEXT("NOT OK")) + LINE_TERMINATOR + LINE_TERMINATOR
            + TEXT("Enum import dry-run failed.") + LINE_TERMINATOR + LINE_TERMINATOR
            + TEXT("Source: ") + SourceLabel + LINE_TERMINATOR + LINE_TERMINATOR
            + TEXT("Problems/details:") + LINE_TERMINATOR + DryRunReport));
        ShowN2CNotification(TEXT("Enum N2C import dry-run failed"), SNotificationItem::CS_Fail);
        return;
    }

    const FString ConfirmMessage = FString(TEXT("OK")) + LINE_TERMINATOR + LINE_TERMINATOR
        + TEXT("Apply N2C Enum import now?") + LINE_TERMINATOR + LINE_TERMINATOR
        + FString::Printf(TEXT("Target UserDefinedEnum assets: %d"), TargetAssets.Num()) + LINE_TERMINATOR
        + TEXT("Source: ") + SourceLabel + LINE_TERMINATOR + LINE_TERMINATOR
        + TEXT("Dry run:") + LINE_TERMINATOR + DryRunReport;

    const EAppReturnType::Type Confirm = FMessageDialog::Open(EAppMsgType::YesNo, FText::FromString(ConfirmMessage));
    if (Confirm != EAppReturnType::Yes)
    {
        ShowN2CNotification(TEXT("Enum N2C import cancelled by user"), SNotificationItem::CS_None);
        return;
    }

    FString ApplyReport;
    bool bApplyOk = true;
    for (UObject* TargetAsset : TargetAssets)
    {
        FString AssetReport;
        const bool bAssetOk = ImportEnumN2CJsonToAsset(TargetAsset, ImportJson, false, AssetReport);
        ApplyReport += AssetReport + LINE_TERMINATOR;
        bApplyOk = bApplyOk && bAssetOk;
    }

    FN2CLogger::Get().Log(FString::Printf(TEXT("Enum selected import apply report:%s%s"), LINE_TERMINATOR, *ApplyReport), bApplyOk ? EN2CLogSeverity::Info : EN2CLogSeverity::Error);

    FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(FString(bApplyOk ? TEXT("OK") : TEXT("NOT OK")) + LINE_TERMINATOR + LINE_TERMINATOR
        + TEXT("Enum N2C import finished.") + LINE_TERMINATOR + LINE_TERMINATOR
        + TEXT("Source: ") + SourceLabel + LINE_TERMINATOR + LINE_TERMINATOR
        + ApplyReport));

    ShowN2CNotification(bApplyOk ? TEXT("Enum N2C import finished") : TEXT("Enum N2C import finished with errors"), bApplyOk ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
}

void FN2CEditorIntegration::ExecuteBack2DeadImportSelectedStructs(TArray<FAssetData> SelectedAssets)
{
    using namespace N2CEditorIntegration_Private;

    TArray<UObject*> TargetAssets;
    for (const FAssetData& AssetData : SelectedAssets)
    {
        if (!IsStructAssetData(AssetData))
        {
            continue;
        }

        UObject* Asset = AssetData.GetAsset();
        if (Asset && Asset->IsA<UUserDefinedStruct>())
        {
            TargetAssets.Add(Asset);
        }
    }

    if (TargetAssets.Num() <= 0)
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(TEXT("NOT OK") LINE_TERMINATOR LINE_TERMINATOR TEXT("Select at least one UserDefinedStruct asset before importing N2C Struct data. Native C++ structs cannot be modified.")));
        ShowN2CNotification(TEXT("Struct import failed: no UserDefinedStruct selected"), SNotificationItem::CS_Fail);
        return;
    }

    FString ImportPath;
    if (!PickNiagaraImportFile(ImportPath))
    {
        ShowN2CNotification(TEXT("Struct N2C import cancelled: no file selected"), SNotificationItem::CS_None);
        return;
    }

    FString ImportJson;
    FString SourceLabel;
    FString LoadError;
    if (!LoadTypedN2CImportJsonFromFile(ImportPath, TEXT("struct"), TEXT("Struct"), TEXT("N2C_STRUCT_PATCH_V1"), ImportJson, SourceLabel, LoadError))
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(FString(TEXT("NOT OK")) + LINE_TERMINATOR + LINE_TERMINATOR + LoadError));
        ShowN2CNotification(TEXT("Struct N2C import failed: could not load JSON"), SNotificationItem::CS_Fail);
        return;
    }

    FString DryRunReport;
    bool bDryRunOk = true;
    for (UObject* TargetAsset : TargetAssets)
    {
        FString AssetReport;
        const bool bAssetOk = ImportStructN2CJsonToAsset(TargetAsset, ImportJson, true, AssetReport);
        DryRunReport += AssetReport + LINE_TERMINATOR;
        bDryRunOk = bDryRunOk && bAssetOk;
    }

    FN2CLogger::Get().Log(FString::Printf(TEXT("Struct selected import dry-run report:%s%s"), LINE_TERMINATOR, *DryRunReport), bDryRunOk ? EN2CLogSeverity::Info : EN2CLogSeverity::Error);

    if (!bDryRunOk)
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(FString(TEXT("NOT OK")) + LINE_TERMINATOR + LINE_TERMINATOR
            + TEXT("Struct import dry-run failed.") + LINE_TERMINATOR + LINE_TERMINATOR
            + TEXT("Source: ") + SourceLabel + LINE_TERMINATOR + LINE_TERMINATOR
            + TEXT("Problems/details:") + LINE_TERMINATOR + DryRunReport));
        ShowN2CNotification(TEXT("Struct N2C import dry-run failed"), SNotificationItem::CS_Fail);
        return;
    }

    const FString ConfirmMessage = FString(TEXT("OK")) + LINE_TERMINATOR + LINE_TERMINATOR
        + TEXT("Apply N2C Struct import now?") + LINE_TERMINATOR + LINE_TERMINATOR
        + FString::Printf(TEXT("Target UserDefinedStruct assets: %d"), TargetAssets.Num()) + LINE_TERMINATOR
        + TEXT("Source: ") + SourceLabel + LINE_TERMINATOR + LINE_TERMINATOR
        + TEXT("Dry run:") + LINE_TERMINATOR + DryRunReport;

    const EAppReturnType::Type Confirm = FMessageDialog::Open(EAppMsgType::YesNo, FText::FromString(ConfirmMessage));
    if (Confirm != EAppReturnType::Yes)
    {
        ShowN2CNotification(TEXT("Struct N2C import cancelled by user"), SNotificationItem::CS_None);
        return;
    }

    FString ApplyReport;
    bool bApplyOk = true;
    for (UObject* TargetAsset : TargetAssets)
    {
        FString AssetReport;
        const bool bAssetOk = ImportStructN2CJsonToAsset(TargetAsset, ImportJson, false, AssetReport);
        ApplyReport += AssetReport + LINE_TERMINATOR;
        bApplyOk = bApplyOk && bAssetOk;
    }

    FN2CLogger::Get().Log(FString::Printf(TEXT("Struct selected import apply report:%s%s"), LINE_TERMINATOR, *ApplyReport), bApplyOk ? EN2CLogSeverity::Info : EN2CLogSeverity::Error);

    FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(FString(bApplyOk ? TEXT("OK") : TEXT("NOT OK")) + LINE_TERMINATOR + LINE_TERMINATOR
        + TEXT("Struct N2C import finished.") + LINE_TERMINATOR + LINE_TERMINATOR
        + TEXT("Source: ") + SourceLabel + LINE_TERMINATOR + LINE_TERMINATOR
        + ApplyReport));

    ShowN2CNotification(bApplyOk ? TEXT("Struct N2C import finished") : TEXT("Struct N2C import finished with errors"), bApplyOk ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
}

void FN2CEditorIntegration::ExecuteBack2DeadImportSelectedNiagara(TArray<FAssetData> SelectedAssets)
{
    using namespace N2CEditorIntegration_Private;

    TArray<UObject*> TargetAssets;
    for (const FAssetData& AssetData : SelectedAssets)
    {
        if (!IsNiagaraAssetData(AssetData))
        {
            continue;
        }

        UObject* Asset = AssetData.GetAsset();
        if (Asset && Asset->IsA<UNiagaraSystem>())
        {
            TargetAssets.Add(Asset);
        }
    }

    if (TargetAssets.Num() <= 0)
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(TEXT("NOT OK") LINE_TERMINATOR LINE_TERMINATOR TEXT("Select at least one NiagaraSystem asset before importing N2C Niagara data.")));
        ShowN2CNotification(TEXT("Niagara import failed: no NiagaraSystem selected"), SNotificationItem::CS_Fail);
        return;
    }

    FString ImportPath;
    if (!PickNiagaraImportFile(ImportPath))
    {
        ShowN2CNotification(TEXT("Niagara N2C import cancelled: no file selected"), SNotificationItem::CS_None);
        return;
    }

    FString ImportJson;
    FString SourceLabel;
    FString LoadError;
    if (!LoadNiagaraImportJsonFromFile(ImportPath, ImportJson, SourceLabel, LoadError))
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(FString(TEXT("NOT OK")) + LINE_TERMINATOR + LINE_TERMINATOR + LoadError));
        ShowN2CNotification(TEXT("Niagara N2C import failed: could not load JSON"), SNotificationItem::CS_Fail);
        return;
    }

    FString DryRunReport;
    bool bDryRunOk = true;
    for (UObject* TargetAsset : TargetAssets)
    {
        FString AssetReport;
        const bool bAssetOk = ImportNiagaraN2CJsonToAsset(TargetAsset, ImportJson, true, AssetReport);
        DryRunReport += AssetReport + LINE_TERMINATOR;
        bDryRunOk = bDryRunOk && bAssetOk;
    }

    FN2CLogger::Get().Log(FString::Printf(TEXT("Niagara selected import dry-run report:%s%s"), LINE_TERMINATOR, *DryRunReport), bDryRunOk ? EN2CLogSeverity::Info : EN2CLogSeverity::Error);

    if (!bDryRunOk)
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(FString(TEXT("NOT OK")) + LINE_TERMINATOR + LINE_TERMINATOR
            + TEXT("Niagara import dry-run failed.") + LINE_TERMINATOR + LINE_TERMINATOR
            + TEXT("Source: ") + SourceLabel + LINE_TERMINATOR + LINE_TERMINATOR
            + TEXT("Problems/details:") + LINE_TERMINATOR + DryRunReport));
        ShowN2CNotification(TEXT("Niagara N2C import dry-run failed"), SNotificationItem::CS_Fail);
        return;
    }

    const FString ConfirmMessage = FString(TEXT("OK")) + LINE_TERMINATOR + LINE_TERMINATOR
        + TEXT("Apply N2C Niagara parameter import now?") + LINE_TERMINATOR + LINE_TERMINATOR
        + FString::Printf(TEXT("Target NiagaraSystem assets: %d"), TargetAssets.Num()) + LINE_TERMINATOR
        + TEXT("Source: ") + SourceLabel + LINE_TERMINATOR + LINE_TERMINATOR
        + TEXT("Dry run:") + LINE_TERMINATOR + DryRunReport;

    const EAppReturnType::Type Confirm = FMessageDialog::Open(EAppMsgType::YesNo, FText::FromString(ConfirmMessage));
    if (Confirm != EAppReturnType::Yes)
    {
        ShowN2CNotification(TEXT("Niagara N2C import cancelled by user"), SNotificationItem::CS_None);
        return;
    }

    FString ApplyReport;
    bool bApplyOk = true;
    for (UObject* TargetAsset : TargetAssets)
    {
        FString AssetReport;
        const bool bAssetOk = ImportNiagaraN2CJsonToAsset(TargetAsset, ImportJson, false, AssetReport);
        ApplyReport += AssetReport + LINE_TERMINATOR;
        bApplyOk = bApplyOk && bAssetOk;
    }

    FN2CLogger::Get().Log(FString::Printf(TEXT("Niagara selected import apply report:%s%s"), LINE_TERMINATOR, *ApplyReport), bApplyOk ? EN2CLogSeverity::Info : EN2CLogSeverity::Error);

    FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(FString(bApplyOk ? TEXT("OK") : TEXT("NOT OK")) + LINE_TERMINATOR + LINE_TERMINATOR
        + TEXT("Niagara N2C import finished.") + LINE_TERMINATOR + LINE_TERMINATOR
        + TEXT("Source: ") + SourceLabel + LINE_TERMINATOR + LINE_TERMINATOR
        + ApplyReport));

    ShowN2CNotification(bApplyOk ? TEXT("Niagara N2C import finished") : TEXT("Niagara N2C import finished with errors"), bApplyOk ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
}

void FN2CEditorIntegration::ExecuteBack2DeadExportNiagaraEditor(TWeakObjectPtr<UObject> NiagaraAsset)
{
    using namespace N2CEditorIntegration_Private;

    UObject* Asset = NiagaraAsset.Get();
    if (!Asset || !IsNiagaraObjectAsset(Asset))
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(TEXT("NOT OK") LINE_TERMINATOR LINE_TERMINATOR TEXT("The Niagara editor asset is no longer valid.")));
        ShowN2CNotification(TEXT("Niagara editor N2C export failed: invalid asset"), SNotificationItem::CS_Fail);
        return;
    }

    TArray<FAssetData> Assets;
    Assets.Add(FAssetData(Asset));

    FString ZipPath;
    FString Report;
    const bool bOk = ExportNiagaraAssetsToSingleArchive(Assets, FString::Printf(TEXT("NiagaraEditor_%s"), *Asset->GetName()), ZipPath, Report);

    FN2CLogger::Get().Log(FString::Printf(TEXT("Niagara Editor N2C export report:\n%s"), *Report), bOk ? EN2CLogSeverity::Info : EN2CLogSeverity::Error);

    if (bOk)
    {
        const FString Message = FString(TEXT("OK")) + LINE_TERMINATOR + LINE_TERMINATOR
            + TEXT("Niagara editor export finished.") + LINE_TERMINATOR + LINE_TERMINATOR
            + FString::Printf(TEXT("Asset: %s"), *Asset->GetPathName()) + LINE_TERMINATOR
            + FString::Printf(TEXT("ZIP: %s"), *ZipPath);
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Message));
        ShowN2CNotification(FString::Printf(TEXT("Niagara N2C ZIP ready: %s"), *ZipPath), SNotificationItem::CS_Success);
    }
    else
    {
        const FString Message = FString(TEXT("NOT OK")) + LINE_TERMINATOR + LINE_TERMINATOR
            + TEXT("Niagara editor export failed.") + LINE_TERMINATOR + LINE_TERMINATOR
            + TEXT("Problems:") + LINE_TERMINATOR
            + Report;
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Message));
        ShowN2CNotification(TEXT("Niagara editor N2C export failed"), SNotificationItem::CS_Fail);
    }
}

void FN2CEditorIntegration::ExecuteBack2DeadImportNiagaraEditor(TWeakObjectPtr<UObject> NiagaraAsset)
{
    using namespace N2CEditorIntegration_Private;

    UObject* Asset = NiagaraAsset.Get();
    if (!Asset || !Asset->IsA<UNiagaraSystem>())
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(TEXT("NOT OK") LINE_TERMINATOR LINE_TERMINATOR TEXT("The Niagara editor asset is invalid or is not a NiagaraSystem.")));
        ShowN2CNotification(TEXT("Niagara editor import failed: invalid NiagaraSystem"), SNotificationItem::CS_Fail);
        return;
    }

    FString ImportPath;
    if (!PickNiagaraImportFile(ImportPath))
    {
        ShowN2CNotification(TEXT("Niagara N2C import cancelled: no file selected"), SNotificationItem::CS_None);
        return;
    }

    FString ImportJson;
    FString SourceLabel;
    FString LoadError;
    if (!LoadNiagaraImportJsonFromFile(ImportPath, ImportJson, SourceLabel, LoadError))
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(FString(TEXT("NOT OK")) + LINE_TERMINATOR + LINE_TERMINATOR + LoadError));
        ShowN2CNotification(TEXT("Niagara N2C import failed: could not load JSON"), SNotificationItem::CS_Fail);
        return;
    }

    FString DryRunReport;
    const bool bDryRunOk = ImportNiagaraN2CJsonToAsset(Asset, ImportJson, true, DryRunReport);
    FN2CLogger::Get().Log(FString::Printf(TEXT("Niagara editor import dry-run report:%s%s"), LINE_TERMINATOR, *DryRunReport), bDryRunOk ? EN2CLogSeverity::Info : EN2CLogSeverity::Error);

    if (!bDryRunOk)
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(FString(TEXT("NOT OK")) + LINE_TERMINATOR + LINE_TERMINATOR
            + TEXT("Niagara import dry-run failed.") + LINE_TERMINATOR + LINE_TERMINATOR
            + TEXT("Source: ") + SourceLabel + LINE_TERMINATOR + LINE_TERMINATOR
            + TEXT("Problems/details:") + LINE_TERMINATOR + DryRunReport));
        ShowN2CNotification(TEXT("Niagara N2C import dry-run failed"), SNotificationItem::CS_Fail);
        return;
    }

    const FString ConfirmMessage = FString(TEXT("OK")) + LINE_TERMINATOR + LINE_TERMINATOR
        + TEXT("Apply N2C Niagara parameter import to the currently opened NiagaraSystem?") + LINE_TERMINATOR + LINE_TERMINATOR
        + FString::Printf(TEXT("Target: %s"), *Asset->GetPathName()) + LINE_TERMINATOR
        + TEXT("Source: ") + SourceLabel + LINE_TERMINATOR + LINE_TERMINATOR
        + TEXT("Dry run:") + LINE_TERMINATOR + DryRunReport;

    const EAppReturnType::Type Confirm = FMessageDialog::Open(EAppMsgType::YesNo, FText::FromString(ConfirmMessage));
    if (Confirm != EAppReturnType::Yes)
    {
        ShowN2CNotification(TEXT("Niagara N2C import cancelled by user"), SNotificationItem::CS_None);
        return;
    }

    FString ApplyReport;
    const bool bApplyOk = ImportNiagaraN2CJsonToAsset(Asset, ImportJson, false, ApplyReport);

    FN2CLogger::Get().Log(FString::Printf(TEXT("Niagara editor import apply report:%s%s"), LINE_TERMINATOR, *ApplyReport), bApplyOk ? EN2CLogSeverity::Info : EN2CLogSeverity::Error);

    FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(FString(bApplyOk ? TEXT("OK") : TEXT("NOT OK")) + LINE_TERMINATOR + LINE_TERMINATOR
        + TEXT("Niagara N2C import finished.") + LINE_TERMINATOR + LINE_TERMINATOR
        + TEXT("Source: ") + SourceLabel + LINE_TERMINATOR + LINE_TERMINATOR
        + ApplyReport));

    ShowN2CNotification(bApplyOk ? TEXT("Niagara N2C import finished") : TEXT("Niagara N2C import finished with errors"), bApplyOk ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
}


void FN2CEditorIntegration::ExecuteBack2DeadImportEnumEditor(TWeakObjectPtr<UObject> EnumAsset)
{
    using namespace N2CEditorIntegration_Private;

    UObject* Asset = EnumAsset.Get();
    if (!Asset || !Asset->IsA<UUserDefinedEnum>())
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(TEXT("NOT OK") LINE_TERMINATOR LINE_TERMINATOR TEXT("The Enum editor asset is invalid or is not a UserDefinedEnum.")));
        ShowN2CNotification(TEXT("Enum editor import failed: invalid UserDefinedEnum"), SNotificationItem::CS_Fail);
        return;
    }

    TArray<FAssetData> Assets;
    Assets.Add(FAssetData(Asset));
    ExecuteBack2DeadImportSelectedEnums(Assets);
}

void FN2CEditorIntegration::ExecuteBack2DeadImportStructEditor(TWeakObjectPtr<UObject> StructAsset)
{
    using namespace N2CEditorIntegration_Private;

    UObject* Asset = StructAsset.Get();
    if (!Asset || !Asset->IsA<UUserDefinedStruct>())
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(TEXT("NOT OK") LINE_TERMINATOR LINE_TERMINATOR TEXT("The Struct editor asset is invalid or is not a UserDefinedStruct.")));
        ShowN2CNotification(TEXT("Struct editor import failed: invalid UserDefinedStruct"), SNotificationItem::CS_Fail);
        return;
    }

    TArray<FAssetData> Assets;
    Assets.Add(FAssetData(Asset));
    ExecuteBack2DeadImportSelectedStructs(Assets);
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

void FN2CEditorIntegration::ExecuteBack2DeadExportNiagaraFromSelectedFolders(TArray<FString> SelectedPaths)
{
    using namespace N2CEditorIntegration_Private;

    if (SelectedPaths.Num() <= 0)
    {
        ShowN2CNotification(TEXT("Folder Niagara N2C export cancelled: no folders selected"), SNotificationItem::CS_None);
        return;
    }

    TArray<FAssetData> NiagaraAssets;
    GetNiagaraAssetsUnderPaths(SelectedPaths, NiagaraAssets);

    if (NiagaraAssets.Num() <= 0)
    {
        FString PathList;
        for (const FString& Path : SelectedPaths)
        {
            PathList += TEXT("- ") + Path + LINE_TERMINATOR;
        }
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(FString(TEXT("NOT OK")) + LINE_TERMINATOR + LINE_TERMINATOR
            + TEXT("No NiagaraSystem assets found in selected folder(s).") + LINE_TERMINATOR + LINE_TERMINATOR
            + TEXT("Folders:") + LINE_TERMINATOR + PathList));
        ShowN2CNotification(TEXT("Folder Niagara N2C export failed: no NiagaraSystem found"), SNotificationItem::CS_Fail);
        return;
    }

    FString Label = TEXT("FolderNiagara");
    if (SelectedPaths.Num() == 1)
    {
        Label = FString::Printf(TEXT("FolderNiagara_%s"), *SanitizeForFileName(SelectedPaths[0]));
    }

    FString ZipPath;
    FString Report;
    const bool bOk = ExportNiagaraAssetsToSingleArchive(NiagaraAssets, Label, ZipPath, Report);

    FN2CLogger::Get().Log(FString::Printf(TEXT("Folder Niagara N2C export report:\n%s"), *Report), bOk ? EN2CLogSeverity::Info : EN2CLogSeverity::Error);

    if (bOk)
    {
        FString PathList;
        for (const FString& Path : SelectedPaths)
        {
            PathList += TEXT("- ") + Path + LINE_TERMINATOR;
        }

        const FString Message = FString(TEXT("OK")) + LINE_TERMINATOR + LINE_TERMINATOR
            + TEXT("Folder Niagara export finished.") + LINE_TERMINATOR + LINE_TERMINATOR
            + FString::Printf(TEXT("Folders: %d"), SelectedPaths.Num()) + LINE_TERMINATOR
            + FString::Printf(TEXT("NiagaraSystem assets: %d"), NiagaraAssets.Num()) + LINE_TERMINATOR
            + FString::Printf(TEXT("ZIP: %s"), *ZipPath) + LINE_TERMINATOR + LINE_TERMINATOR
            + TEXT("Folders:") + LINE_TERMINATOR + PathList;

        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Message));
        ShowN2CNotification(FString::Printf(TEXT("Folder Niagara N2C ZIP ready: %s"), *ZipPath), SNotificationItem::CS_Success);
    }
    else
    {
        const FString Message = FString(TEXT("NOT OK")) + LINE_TERMINATOR + LINE_TERMINATOR
            + TEXT("Folder Niagara export failed.") + LINE_TERMINATOR + LINE_TERMINATOR
            + TEXT("Problems:") + LINE_TERMINATOR
            + Report;
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Message));
        ShowN2CNotification(TEXT("Folder Niagara N2C export failed"), SNotificationItem::CS_Fail);
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
    DeleteExportWorkDirAfterZip(ExportRootDir, ZipPath, Report);

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

bool FN2CEditorIntegration::ExportAllProjectAssetsForAutomation(FString& OutZipPath, FString& OutReport)
{
    using namespace N2CEditorIntegration_Private;

    TArray<FAssetData> AllAssets;
    GetAllProjectAssets(AllAssets);

    const FN2CExportKindSelection Selection = MakeExportAvailabilityFromAssets(AllAssets);
    if (!Selection.HasAnySelected())
    {
        OutZipPath.Empty();
        OutReport = TEXT("No supported N2C assets found under /Game.");
        return false;
    }

    TArray<FAssetData> BlueprintAssets;
    TArray<FAssetData> NiagaraAssets;
    TArray<FAssetData> EnumAssets;
    TArray<FAssetData> StructAssets;
    FilterAssetsForN2CExportKinds(AllAssets, Selection, BlueprintAssets, NiagaraAssets, EnumAssets, StructAssets);
    return ExportMixedAssetsToSingleArchive(BlueprintAssets, NiagaraAssets, EnumAssets, StructAssets, TEXT("Project"), OutZipPath, OutReport);
}

void FN2CEditorIntegration::ExecuteBack2DeadExportProject()
{
    using namespace N2CEditorIntegration_Private;

    TArray<FAssetData> AllAssets;
    GetAllProjectAssets(AllAssets);

    const FN2CExportKindSelection AvailableKinds = MakeExportAvailabilityFromAssets(AllAssets);
    FN2CExportKindSelection Selection = AvailableKinds;

    if (!Selection.HasAnySelected())
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(TEXT("NOT OK") LINE_TERMINATOR LINE_TERMINATOR TEXT("No supported N2C assets found under /Game.")));
        ShowN2CNotification(TEXT("Export Project failed: no supported assets found"), SNotificationItem::CS_Fail);
        return;
    }

    if (!PickExportKindsForN2C(
        TEXT("Export Project"),
        TEXT("Choose what to export from the whole /Game project. The export will create one N2C ZIP archive."),
        AvailableKinds,
        Selection))
    {
        ShowN2CNotification(TEXT("Export Project cancelled"), SNotificationItem::CS_None);
        return;
    }

    TArray<FAssetData> BlueprintAssets;
    TArray<FAssetData> NiagaraAssets;
    TArray<FAssetData> EnumAssets;
    TArray<FAssetData> StructAssets;
    FilterAssetsForN2CExportKinds(AllAssets, Selection, BlueprintAssets, NiagaraAssets, EnumAssets, StructAssets);

    FString ZipPath;
    FString Report;
    const bool bOk = ExportMixedAssetsToSingleArchive(BlueprintAssets, NiagaraAssets, EnumAssets, StructAssets, TEXT("Project"), ZipPath, Report);

    FN2CLogger::Get().Log(FString::Printf(TEXT("Export Project N2C report:\n%s"), *Report), bOk ? EN2CLogSeverity::Info : EN2CLogSeverity::Error);

    if (bOk)
    {
        const FString Message = FString(TEXT("OK")) + LINE_TERMINATOR + LINE_TERMINATOR
            + TEXT("Export Project finished.") + LINE_TERMINATOR + LINE_TERMINATOR
            + FString::Printf(TEXT("Blueprints: %d"), BlueprintAssets.Num()) + LINE_TERMINATOR
            + FString::Printf(TEXT("Niagara Systems: %d"), NiagaraAssets.Num()) + LINE_TERMINATOR
            + FString::Printf(TEXT("Enums: %d"), EnumAssets.Num()) + LINE_TERMINATOR
            + FString::Printf(TEXT("Structs: %d"), StructAssets.Num()) + LINE_TERMINATOR
            + FString::Printf(TEXT("ZIP: %s"), *ZipPath);

        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Message));
        ShowN2CNotification(FString::Printf(TEXT("Export Project N2C ZIP ready: %s"), *ZipPath), SNotificationItem::CS_Success);
    }
    else
    {
        const FString Message = FString(TEXT("NOT OK")) + LINE_TERMINATOR + LINE_TERMINATOR
            + TEXT("Export Project failed.") + LINE_TERMINATOR + LINE_TERMINATOR
            + TEXT("Problems:") + LINE_TERMINATOR
            + Report;
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Message));
        ShowN2CNotification(TEXT("Export Project N2C failed"), SNotificationItem::CS_Fail);
    }
}

void FN2CEditorIntegration::ExecuteBack2DeadExportAllBlueprints()
{
    ExecuteBack2DeadExportProject();
}

void FN2CEditorIntegration::ExecuteCollectNodesForEditor(TWeakPtr<FBlueprintEditor> InEditor)
{
    // Back2Dead fork: direct LLM translation/code generation has been removed.
    // Keep this legacy action safe by redirecting it to the local raw JSON copy/export path.
    ExecuteCopyJsonForEditor(InEditor);
}







