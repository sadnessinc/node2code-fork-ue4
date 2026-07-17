// Copyright (c) 2025 Nick McClure (Protospatial). All Rights Reserved.
// Back2Dead custom extension: safe Blueprint patch importer.

#include "Core/N2CPatchImporter.h"
#include "Core/N2CMacroReference.h"
#include "Core/N2CCoverage.h"
#include "Core/N2CStructuralSnapshot.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphNodeUtils.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/DataTable.h"
#include "Engine/TimelineTemplate.h"
#include "Engine/SimpleConstructionScript.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "GameFramework/Actor.h"
#include "Curves/CurveFloat.h"
#include "Curves/CurveVector.h"
#include "Curves/CurveLinearColor.h"
#include "Engine/SCS_Node.h"
#include "HAL/FileManager.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallFunctionOnMember.h"
#include "K2Node_CallParentFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_EditablePinBase.h"
#include "K2Node_GetDataTableRow.h"
#include "K2Node_SetFieldsInStruct.h"
#include "K2Node_AddComponent.h"
#include "K2Node_Timeline.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_Composite.h"
#include "K2Node_Tunnel.h"
#include "K2Node_BaseMCDelegate.h"
#include "K2Node_AddDelegate.h"
#include "K2Node_AssignDelegate.h"
#include "K2Node_CallDelegate.h"
#include "K2Node_ClearDelegate.h"
#include "K2Node_RemoveDelegate.h"
#include "K2Node_CreateDelegate.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_GetArrayItem.h"
#include "K2Node_Knot.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Self.h"
#include "K2Node_SwitchEnum.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Blueprint/UserWidget.h"
#include "AnimGraphNode_LocalRefPose.h"
#include "AnimGraphNode_Root.h"
#include "Animation/AnimBlueprint.h"
#include "AnimationGraphSchema.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/FileHelper.h"
#include "Misc/App.h"
#include "FileHelpers.h"
#include "Editor.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/OutputDeviceNull.h"
#include "Misc/DefaultValueHelper.h"
#include "Misc/SecureHash.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonReader.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Utils/N2CLogger.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"
#include "UObject/Package.h"
#include "UObject/StructOnScope.h"
#include "WidgetBlueprint.h"

namespace N2CPatchImporter_Private
{
    static FString GetStringFieldSafe(const TSharedPtr<FJsonObject>& Obj, const FString& FieldName, const FString& DefaultValue = TEXT(""))
    {
        if (!Obj.IsValid())
        {
            return DefaultValue;
        }
        FString Value;
        return Obj->TryGetStringField(FieldName, Value) ? Value : DefaultValue;
    }

    static bool GetBoolFieldSafe(const TSharedPtr<FJsonObject>& Obj, const FString& FieldName, bool bDefaultValue = false)
    {
        if (!Obj.IsValid())
        {
            return bDefaultValue;
        }
        bool bValue = bDefaultValue;
        return Obj->TryGetBoolField(FieldName, bValue) ? bValue : bDefaultValue;
    }

    static bool TryGetBoolFieldSafe(const TSharedPtr<FJsonObject>& Obj, const FString& FieldName, bool& OutValue)
    {
        if (!Obj.IsValid())
        {
            return false;
        }
        return Obj->TryGetBoolField(FieldName, OutValue);
    }

    static const TArray<TSharedPtr<FJsonValue>>* GetArrayFieldSafe(const TSharedPtr<FJsonObject>& Obj, const FString& FieldName)
    {
        if (!Obj.IsValid())
        {
            return nullptr;
        }
        const TArray<TSharedPtr<FJsonValue>>* ArrayPtr = nullptr;
        Obj->TryGetArrayField(FieldName, ArrayPtr);
        return ArrayPtr;
    }

    static void GetStringArrayField(const TSharedPtr<FJsonObject>& Obj, const FString& FieldName, TArray<FString>& OutValues)
    {
        const TArray<TSharedPtr<FJsonValue>>* Values = GetArrayFieldSafe(Obj, FieldName);
        if (!Values)
        {
            return;
        }
        for (const TSharedPtr<FJsonValue>& Value : *Values)
        {
            FString Str;
            if (Value.IsValid() && Value->TryGetString(Str) && !Str.IsEmpty())
            {
                OutValues.Add(Str);
            }
        }
    }

    static FString SanitizeForFileName(FString In)
    {
        In.ReplaceInline(TEXT("/"), TEXT("_"));
        In.ReplaceInline(TEXT("\\"), TEXT("_"));
        In.ReplaceInline(TEXT(":"), TEXT("_"));
        In.ReplaceInline(TEXT(" "), TEXT("_"));
        return In;
    }

    static void AppendLine(FString& Report, const FString& Line)
    {
        Report += Line;
        Report += LINE_TERMINATOR;
    }

    static bool ParseRoot(const FString& PatchJson, TSharedPtr<FJsonObject>& OutRoot, FString& OutReport)
    {
        TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(PatchJson);
        if (!FJsonSerializer::Deserialize(Reader, OutRoot) || !OutRoot.IsValid())
        {
            AppendLine(OutReport, TEXT("ERROR: patch JSON parse failed."));
            return false;
        }

        const FString Schema = GetStringFieldSafe(OutRoot, TEXT("schema"));
        if (Schema != TEXT("N2C_PATCH_V1"))
        {
            AppendLine(OutReport, Schema == TEXT("N2C_AI_EXPORT_V2") ? TEXT("ERROR: N2C_AI_EXPORT_V2 is an export format, not an apply patch. Apply accepts N2C_PATCH_V1; the normalizer is P3.") : FString::Printf(TEXT("ERROR: unsupported schema '%s'. Expected N2C_PATCH_V1."), *Schema));
            return false;
        }

        if (!GetArrayFieldSafe(OutRoot, TEXT("actions")))
        {
            AppendLine(OutReport, TEXT("ERROR: patch does not contain actions[]."));
            return false;
        }

        return true;
    }

    static void PruneBlueprintPatchBackups(const FString& BackupDir, const FString& SafeAssetName, int32 MaxBackupsToKeep, FString& Report)
    {
        if (BackupDir.IsEmpty() || SafeAssetName.IsEmpty() || MaxBackupsToKeep <= 0)
        {
            return;
        }

        TArray<FString> BackupFiles;
        IFileManager::Get().FindFilesRecursive(BackupFiles, *BackupDir, TEXT("*.uasset"), true, false);
        for (int32 Index = BackupFiles.Num() - 1; Index >= 0; --Index)
        {
            const FString Normalized = BackupFiles[Index].Replace(TEXT("\\"), TEXT("/"));
            const FString Base = FPaths::GetBaseFilename(BackupFiles[Index]);
            if (!Base.StartsWith(TEXT("N2C_") + SafeAssetName + TEXT("_")) ||
                Normalized.Contains(TEXT("/RestoreRollback/")) ||
                Normalized.Contains(TEXT("/PendingRestore/")) ||
                Normalized.Contains(TEXT("/PendingRestoreApplied/")) ||
                Normalized.Contains(TEXT("/PendingRestoreCancelled/")))
            {
                BackupFiles.RemoveAt(Index);
            }
        }

        BackupFiles.Sort([](const FString& A, const FString& B)
        {
            return IFileManager::Get().GetTimeStamp(*A) > IFileManager::Get().GetTimeStamp(*B);
        });

        int32 DeletedCount = 0;
        for (int32 Index = MaxBackupsToKeep; Index < BackupFiles.Num(); ++Index)
        {
            if (IFileManager::Get().Delete(*BackupFiles[Index], false, true, true))
            {
                ++DeletedCount;
            }
            else
            {
                AppendLine(Report, FString::Printf(TEXT("WARNING: backup retention could not delete old patch backup: %s"), *BackupFiles[Index]));
            }
        }
        if (DeletedCount > 0)
        {
            AppendLine(Report, FString::Printf(TEXT("Backup retention: kept last %d Blueprint patch backup(s), deleted %d old backup(s)."), MaxBackupsToKeep, DeletedCount));
        }
    }

    static bool BackupBlueprintAsset(UBlueprint* Blueprint, FString& OutBackupPath, FString& OutReport)
    {
        OutBackupPath.Empty();
        if (!Blueprint || !Blueprint->GetOutermost())
        {
            AppendLine(OutReport, TEXT("ERROR: cannot backup invalid Blueprint package."));
            return false;
        }

        FString SourceFilename;
        const FString PackageName = Blueprint->GetOutermost()->GetName();
        if (!FPackageName::DoesPackageExist(PackageName, nullptr, &SourceFilename))
        {
            AppendLine(OutReport, TEXT("ERROR: Blueprint package file was not found. Save the Blueprint first, then retry."));
            return false;
        }

        const FString BackupDir = FPaths::ProjectSavedDir() / TEXT("NodeToCode/Backups");
        IFileManager::Get().MakeDirectory(*BackupDir, true);

        const FString SafeAssetName = SanitizeForFileName(Blueprint->GetName());
        const FDateTime Now = FDateTime::Now();
        const FString Timestamp = FString::Printf(
            TEXT("%04d%02d%02d_%02d%02d%02d"),
            Now.GetYear(), Now.GetMonth(), Now.GetDay(),
            Now.GetHour(), Now.GetMinute(), Now.GetSecond());
        // Keep Blueprint patch backups aligned with normal N2C export filenames:
        // N2C_<AssetName>_<YYYYMMDD_HHMMSS>.uasset
        OutBackupPath = BackupDir / FString::Printf(TEXT("N2C_%s_%s.uasset"), *SafeAssetName, *Timestamp);

        const int32 CopyResult = IFileManager::Get().Copy(*OutBackupPath, *SourceFilename, true, true);
        if (CopyResult == COPY_OK)
        {
            AppendLine(OutReport, FString::Printf(TEXT("Backup created: %s"), *OutBackupPath));
            PruneBlueprintPatchBackups(BackupDir, SafeAssetName, 10, OutReport);
            return true;
        }

        AppendLine(OutReport, FString::Printf(TEXT("ERROR: failed to create backup from %s to %s (copy result %d)"), *SourceFilename, *OutBackupPath, CopyResult));
        return false;
    }

    static UEdGraph* FindFunctionGraph(UBlueprint* Blueprint, const FString& FunctionName)
    {
        if (!Blueprint)
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

    static UEdGraph* FindMacroGraph(UBlueprint* Blueprint, const FString& MacroName)
    {
        if (!Blueprint)
        {
            return nullptr;
        }
        for (UEdGraph* Graph : Blueprint->MacroGraphs)
        {
            if (Graph && Graph->GetName() == MacroName)
            {
                return Graph;
            }
        }
        return nullptr;
    }

    static UClass* ResolveBlueprintInterfaceClass(const FString& InterfacePath)
    {
        if (InterfacePath.IsEmpty())
        {
            return nullptr;
        }
        if (UClass* DirectClass = LoadObject<UClass>(nullptr, *InterfacePath))
        {
            if (DirectClass->HasAnyClassFlags(CLASS_Interface))
            {
                return DirectClass;
            }
        }
        if (UClass* ExistingClass = FindObject<UClass>(ANY_PACKAGE, *InterfacePath))
        {
            if (ExistingClass->HasAnyClassFlags(CLASS_Interface))
            {
                return ExistingClass;
            }
        }

        FString BlueprintPath = InterfacePath;
        int32 DotIndex = INDEX_NONE;
        if (BlueprintPath.FindLastChar(TEXT('.'), DotIndex))
        {
            FString ObjectName = BlueprintPath.Mid(DotIndex + 1);
            if (ObjectName.EndsWith(TEXT("_C")))
            {
                ObjectName.LeftChopInline(2);
                BlueprintPath = BlueprintPath.Left(DotIndex + 1) + ObjectName;
            }
        }
        UBlueprint* InterfaceBlueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
        if (!InterfaceBlueprint)
        {
            InterfaceBlueprint = FindObject<UBlueprint>(ANY_PACKAGE, *BlueprintPath);
        }
        if (InterfaceBlueprint)
        {
            return InterfaceBlueprint->GeneratedClass;
        }
        return nullptr;
    }


    static bool IsEventGraphPatchAction(const FString& ActionType)
    {
        return ActionType == TEXT("patch_event_graph") ||
               ActionType == TEXT("add_event_graph_nodes") ||
               ActionType == TEXT("patch_graph") ||
               ActionType == TEXT("add_nodes_to_graph");
    }

    static bool IsWidgetGraphPatchAction(const FString& ActionType)
    {
        return ActionType == TEXT("patch_widget_graph");
    }

    static bool IsAnimationGraphPatchAction(const FString& ActionType)
    {
        return ActionType == TEXT("patch_animation_graph");
    }

    static bool IsSupportedActionType(const FString& ActionType)
    {
        return ActionType == TEXT("add_or_replace_function") ||
               ActionType == TEXT("replace_function_body") ||
               ActionType == TEXT("add_member_variables") ||
               ActionType == TEXT("add_variables") ||
               ActionType == TEXT("rename_function") ||
               ActionType == TEXT("rename_variable") ||
               ActionType == TEXT("delete_function") ||
               ActionType == TEXT("delete_functions") ||
               ActionType == TEXT("delete_variable") ||
               ActionType == TEXT("delete_variables") ||
               ActionType == TEXT("delete_member_variable") ||
               ActionType == TEXT("delete_member_variables") ||
               ActionType == TEXT("delete_event_dispatcher") ||
               ActionType == TEXT("delete_event_dispatchers") ||
               ActionType == TEXT("delete_dispatcher") ||
               ActionType == TEXT("delete_dispatchers") ||
               ActionType == TEXT("delete_macro") ||
               ActionType == TEXT("delete_macros") ||
               ActionType == TEXT("delete_event_graph_nodes") ||
               ActionType == TEXT("delete_graph_nodes") ||
               ActionType == TEXT("rename_custom_event") ||
               ActionType == TEXT("add_macro") ||
               ActionType == TEXT("add_event_dispatcher") ||
               ActionType == TEXT("add_event_dispatchers") ||
               ActionType == TEXT("implement_interface") ||
               ActionType == TEXT("import_scs_hierarchy") ||
               ActionType == TEXT("add_scs_components") ||
               ActionType == TEXT("update_scs_hierarchy") ||
               ActionType == TEXT("set_function_category") ||
               ActionType == TEXT("move_function_to_category") ||
               ActionType == TEXT("create_collapsed_graph") ||
               ActionType == TEXT("replace_collapsed_graph") ||
               IsEventGraphPatchAction(ActionType) ||
               IsWidgetGraphPatchAction(ActionType) ||
               IsAnimationGraphPatchAction(ActionType);
    }

    static bool IsSupportedNodeType(const FString& Type)
    {
        const FString Lower = Type.ToLower();
        return Lower == TEXT("entry") ||
               Lower == TEXT("k2node_tunnel") || Lower == TEXT("tunnel") ||
               Lower == TEXT("animgraphnode_localrefpose") ||
               Lower == TEXT("local_ref_pose") ||
               Lower == TEXT("localrefpose") ||
               Lower == TEXT("functionentry") ||
               Lower.Contains(TEXT("functionentry")) ||
               Lower == TEXT("return") ||
               Lower == TEXT("functionresult") ||
               Lower.Contains(TEXT("functionresult")) ||
               Lower == TEXT("k2node_knot") ||
               Lower == TEXT("knot") ||
               Lower == TEXT("reroute") ||
               Lower.Contains(TEXT("reroute")) ||
               Lower == TEXT("k2node_self") ||
               Lower == TEXT("self") ||
               Lower == TEXT("k2node_getarrayitem") ||
               Lower == TEXT("getarrayitem") ||
               Lower == TEXT("get_array_item") ||
               Lower == TEXT("k2node_callparentfunction") ||
               Lower == TEXT("callparentfunction") ||
               Lower == TEXT("call_parent_function") ||
               Lower == TEXT("k2node_callfunctiononmember") ||
               Lower == TEXT("callfunctiononmember") ||
               Lower == TEXT("call_function_on_member") ||
               Lower == TEXT("k2node_event") ||
               Lower == TEXT("event") ||
               Lower == TEXT("builtin_event") ||
               Lower == TEXT("branch") ||
               Lower.Contains(TEXT("ifthenelse")) ||
               Lower == TEXT("binaryoperator") ||
               Lower == TEXT("binary_operator") ||
               Lower == TEXT("commutativeassociativebinaryoperator") ||
               Lower.Contains(TEXT("commutativeassociativebinaryoperator")) ||
               Lower == TEXT("sequence") ||
               Lower.Contains(TEXT("executionsequence")) ||
               Lower == TEXT("select") ||
               Lower.Contains(TEXT("k2node_select")) ||
               Lower == TEXT("switchenum") ||
               Lower == TEXT("switch_enum") ||
               Lower.Contains(TEXT("switchenum")) ||
               Lower == TEXT("callfunction") ||
               Lower == TEXT("call_function") ||
               Lower.Contains(TEXT("callfunction")) ||
               Lower == TEXT("callarrayfunction") ||
               Lower == TEXT("call_array_function") ||
               Lower.Contains(TEXT("callarrayfunction")) ||
               Lower == TEXT("breakstruct") ||
               Lower == TEXT("break_struct") ||
               Lower.Contains(TEXT("breakstruct")) ||
               Lower == TEXT("makestruct") ||
               Lower == TEXT("make_struct") ||
               Lower.Contains(TEXT("makestruct")) ||
               Lower == TEXT("setfieldsinstruct") ||
               Lower == TEXT("setmembersinstruct") ||
               Lower == TEXT("set_fields_in_struct") ||
               Lower == TEXT("set_members_in_struct") ||
               Lower.Contains(TEXT("setfieldsinstruct")) ||
               Lower.Contains(TEXT("setmembersinstruct")) ||
               Lower == TEXT("makearray") ||
               Lower == TEXT("make_array") ||
               Lower.Contains(TEXT("makearray")) ||
               Lower == TEXT("arrayget") ||
               Lower == TEXT("array_get") ||
               Lower == TEXT("arrayadd") ||
               Lower == TEXT("array_add") ||
               Lower == TEXT("arrayaddunique") ||
               Lower == TEXT("array_add_unique") ||
               Lower == TEXT("arrayclear") ||
               Lower == TEXT("array_clear") ||
               Lower == TEXT("arraycontains") ||
               Lower == TEXT("array_contains") ||
               Lower == TEXT("arrayfind") ||
               Lower == TEXT("array_find") ||
               Lower == TEXT("arrayset") ||
               Lower == TEXT("array_set") ||
               Lower == TEXT("arrayremove") ||
               Lower == TEXT("array_remove") ||
               Lower == TEXT("arrayremoveitem") ||
               Lower == TEXT("array_remove_item") ||
               Lower == TEXT("arraylength") ||
               Lower == TEXT("array_length") ||
               Lower == TEXT("foreachloop") ||
               Lower == TEXT("for_each_loop") ||
               Lower == TEXT("forloop") ||
               Lower == TEXT("for_loop") ||
               Lower == TEXT("forloopwithbreak") ||
               Lower == TEXT("for_loop_with_break") ||
               Lower == TEXT("whileloop") ||
               Lower == TEXT("while_loop") ||
               Lower == TEXT("doonce") ||
               Lower == TEXT("do_once") ||
               Lower == TEXT("gate") ||
               Lower == TEXT("flipflop") ||
               Lower == TEXT("flip_flop") ||
               Lower == TEXT("foreachloopwithbreak") ||
               Lower == TEXT("for_each_loop_with_break") ||
               Lower == TEXT("macroinstance") ||
               Lower == TEXT("macro_instance") ||
               Lower.Contains(TEXT("macroinstance")) ||
               Lower == TEXT("isvalid") ||
               Lower == TEXT("is_valid") ||
               Lower == TEXT("incrementint") ||
               Lower == TEXT("increment_int") ||
               Lower == TEXT("customevent") ||
               Lower == TEXT("custom_event") ||
               Lower.Contains(TEXT("customevent")) ||
               Lower == TEXT("spawnactorfromclass") ||
               Lower == TEXT("spawn_actor_from_class") ||
               Lower.Contains(TEXT("spawnactorfromclass")) ||
               Lower == TEXT("addcomponent") ||
               Lower == TEXT("add_component") ||
               Lower.Contains(TEXT("addcomponent")) ||
               Lower == TEXT("timeline") ||
               Lower == TEXT("timeline_node") ||
               Lower.Contains(TEXT("timeline")) ||
               Lower == TEXT("delay") ||
               Lower == TEXT("latent_delay") ||
               Lower.Contains(TEXT("delay")) ||
               Lower == TEXT("dynamiccast") ||
               Lower == TEXT("castto") ||
               Lower == TEXT("cast_to") ||
               Lower.Contains(TEXT("dynamiccast")) ||
               Lower == TEXT("interfacecall") ||
               Lower == TEXT("interface_call") ||
               Lower == TEXT("getdatatablerow") ||
               Lower == TEXT("get_data_table_row") ||
               Lower.Contains(TEXT("getdatatablerow")) ||
               Lower == TEXT("enumequality") ||
               Lower == TEXT("enum_equality") ||
               Lower.Contains(TEXT("enumequality")) ||
               Lower == TEXT("enuminequality") ||
               Lower == TEXT("enum_inequality") ||
               Lower.Contains(TEXT("enuminequality")) ||
               Lower == TEXT("componentboundevent") ||
               Lower == TEXT("component_bound_event") ||
               Lower.Contains(TEXT("componentboundevent")) ||
               Lower == TEXT("assigndelegate") ||
               Lower == TEXT("assign_delegate") ||
               Lower.Contains(TEXT("assigndelegate")) ||
               Lower == TEXT("removedelegate") ||
               Lower == TEXT("remove_delegate") ||
               Lower == TEXT("unbinddelegate") ||
               Lower == TEXT("unbind_delegate") ||
               Lower.Contains(TEXT("removedelegate")) ||
               Lower == TEXT("adddelegate") ||
               Lower == TEXT("binddelegate") ||
               Lower == TEXT("bind_delegate") ||
               Lower.Contains(TEXT("adddelegate")) ||
               Lower == TEXT("calldelegate") ||
               Lower == TEXT("call_delegate") ||
               Lower.Contains(TEXT("calldelegate")) ||
               Lower == TEXT("cleardelegate") ||
               Lower == TEXT("clear_delegate") ||
               Lower.Contains(TEXT("cleardelegate")) ||
               Lower == TEXT("createdelegate") ||
               Lower == TEXT("create_delegate") ||
               Lower.Contains(TEXT("createdelegate")) ||
               Lower == TEXT("variableget") ||
               Lower == TEXT("variable_get") ||
               Lower.Contains(TEXT("variableget")) ||
               Lower == TEXT("variableset") ||
               Lower == TEXT("variable_set") ||
               Lower.Contains(TEXT("variableset")) ||
               Lower.Contains(TEXT("getenumeratornameasstring")) ||
               Lower.Contains(TEXT("callmaterialparametercollection")) ||
               Lower.Contains(TEXT("createwidget")) ||
               Lower.Contains(TEXT("inputaction")) ||
               Lower.Contains(TEXT("inputaxis")) ||
               Lower.Contains(TEXT("inputkey")) ||
               Lower.Contains(TEXT("inputtouch")) ||
               Lower.Contains(TEXT("foreachelementinenum")) ||
               Lower.Contains(TEXT("message")) ||
               Lower.Contains(TEXT("easefunction")) ||
               Lower.Contains(TEXT("castbytetoenum")) ||
               Lower.Contains(TEXT("enumliteral"));
               
    }

    static bool IsGuardedSafeSkippedNodeType(const FString& Type)
    {
        // P0-P2 constructors now validate their required context before mutation.
        // Keep this hook for future high-risk UE4.27 nodes, but do not silently count
        // the currently supported P0/P1/P2 nodes as successful safe-skips.
        return false;
    }


    static FString GetDeclaredPinCategoryToken(const TSharedPtr<FJsonObject>& Obj)
    {
        if (!Obj.IsValid())
        {
            return TEXT("");
        }

        FString Token = GetStringFieldSafe(Obj, TEXT("type"), GetStringFieldSafe(Obj, TEXT("category")));
        if (Token.IsEmpty())
        {
            const TSharedPtr<FJsonObject>* PinTypeObj = nullptr;
            if (Obj->TryGetObjectField(TEXT("pin_type"), PinTypeObj) && PinTypeObj && PinTypeObj->IsValid())
            {
                Token = GetStringFieldSafe(*PinTypeObj, TEXT("type"), GetStringFieldSafe(*PinTypeObj, TEXT("category")));
            }
        }
        Token.TrimStartAndEndInline();
        return Token.ToLower();
    }

    static bool IsExplicitEnumBackedByteDeclaration(const TSharedPtr<FJsonObject>& Obj)
    {
        if (!Obj.IsValid())
        {
            return false;
        }

        if (GetDeclaredPinCategoryToken(Obj) == TEXT("enum"))
        {
            return true;
        }

        // Exported enum-backed byte declarations carry a durable enum identity,
        // while raw uint8/Byte declarations intentionally have no subtype object.
        static const TCHAR* EnumIdentityFields[] = {
            TEXT("enum_path"), TEXT("enum_type"), TEXT("enum_name"),
            TEXT("sub_category_object"), TEXT("subcategory_object"),
            TEXT("pin_sub_category_object"), TEXT("type_object")
        };
        for (const TCHAR* Field : EnumIdentityFields)
        {
            if (!GetStringFieldSafe(Obj, Field).IsEmpty())
            {
                return true;
            }
        }

        const TSharedPtr<FJsonObject>* PinTypeObj = nullptr;
        return Obj->TryGetObjectField(TEXT("pin_type"), PinTypeObj) && PinTypeObj && PinTypeObj->IsValid()
            ? IsExplicitEnumBackedByteDeclaration(*PinTypeObj)
            : false;
    }

    static bool IsPinTypeSafeForBlueprintMemberImport(
        const FEdGraphPinType& PinType,
        bool bExplicitEnumBackedByte,
        FString& OutReason)
    {
        OutReason.Empty();
        if (PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard || PinType.PinCategory.IsNone())
        {
            OutReason = TEXT("wildcard/unknown pin type");
            return false;
        }

        // UE4.27 represents both raw uint8 and enum-backed byte variables with
        // PC_Byte. Only the enum-backed form requires PinSubCategoryObject=UEnum.
        // Rejecting every PC_Byte without a subtype incorrectly blocked raw Byte.
        if (PinType.PinCategory == UEdGraphSchema_K2::PC_Byte)
        {
            if (bExplicitEnumBackedByte && !Cast<UEnum>(PinType.PinSubCategoryObject.Get()))
            {
                OutReason = TEXT("enum-backed byte type has no resolved UEnum subtype object");
                return false;
            }
            return true;
        }

        // UE4.27 serializes Set and Map member defaults through the same public
        // FBlueprintEditorUtils::AddMemberVariable path used for arrays. The
        // caller still resolves both map key/value types before reaching here.
        if ((PinType.PinCategory == UEdGraphSchema_K2::PC_Object ||
             PinType.PinCategory == UEdGraphSchema_K2::PC_Class ||
             PinType.PinCategory == UEdGraphSchema_K2::PC_SoftObject ||
             PinType.PinCategory == UEdGraphSchema_K2::PC_SoftClass ||
             PinType.PinCategory == UEdGraphSchema_K2::PC_Struct) &&
            !PinType.PinSubCategoryObject.IsValid())
        {
            OutReason = TEXT("object/class/struct type has no resolved subtype object");
            return false;
        }

        return true;
    }

    static bool NodeHasLoosePin(UEdGraphNode* Node, const FString& Name, EEdGraphPinDirection Direction)
    {
        if (!Node || Name.IsEmpty())
        {
            return false;
        }
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin || Pin->Direction != Direction)
            {
                continue;
            }
            if (Pin->PinName.ToString() == Name)
            {
                return true;
            }
        }
        return false;
    }

    static void BreakAllLinksOnNode(UEdGraphNode* Node)
    {
        if (!Node)
        {
            return;
        }
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin)
            {
                Pin->BreakAllPinLinks();
            }
        }
    }

    static FString NormalizeLoosePinName(FString Value);
    static FEdGraphPinType MakePinTypeFromJson(const TSharedPtr<FJsonObject>& Obj);
    static bool ApplyNodePinDefaults(UEdGraphNode* Node, const TSharedPtr<FJsonObject>& NodeObj, FString& Report);
    static UEdGraphNode* CreatePatchNode(UEdGraph* Graph, UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& NodeObj, int32 Index, FString& Report);
    static bool ConnectEdges(UEdGraph* Graph, const TMap<FString, UEdGraphNode*>& NodesById, const TSharedPtr<FJsonObject>& ActionObj, FString& Report);
    static FString GetActionFunctionName(const TSharedPtr<FJsonObject>& ActionObj);
    static UK2Node_FunctionEntry* FindOrCreateEntryNode(UEdGraph* Graph, const FString& FunctionName);
    static UK2Node_FunctionResult* FindOrCreateResultNode(UEdGraph* Graph, const FString& FunctionName);

    static void CollectDesiredSignaturePins(const TSharedPtr<FJsonObject>& ActionObj, const FString& FieldName, TMap<FName, FEdGraphPinType>& OutPins)
    {
        const TArray<TSharedPtr<FJsonValue>>* Values = GetArrayFieldSafe(ActionObj, FieldName);
        if (!Values)
        {
            return;
        }

        for (const TSharedPtr<FJsonValue>& Value : *Values)
        {
            const TSharedPtr<FJsonObject> PinObj = Value.IsValid() ? Value->AsObject() : nullptr;
            const FString PinName = GetStringFieldSafe(PinObj, TEXT("name"));
            if (!PinName.IsEmpty())
            {
                OutPins.Add(FName(*PinName), MakePinTypeFromJson(PinObj));
            }
        }
    }

    static int32 SynchronizeEditableSignaturePins(
        UK2Node_EditablePinBase* Node,
        const TMap<FName, FEdGraphPinType>& DesiredPins,
        EEdGraphPinDirection Direction,
        const FString& FunctionName,
        const TCHAR* SignatureSide,
        FString& Report)
    {
        if (!Node)
        {
            return 0;
        }

        TArray<FName> PinsToRemove;
        for (const TSharedPtr<FUserPinInfo>& UserPin : Node->UserDefinedPins)
        {
            if (!UserPin.IsValid() || UserPin->DesiredPinDirection != Direction)
            {
                continue;
            }

            const FEdGraphPinType* DesiredType = DesiredPins.Find(UserPin->PinName);
            if (!DesiredType || !(UserPin->PinType == *DesiredType))
            {
                PinsToRemove.Add(UserPin->PinName);
            }
        }

        for (const FName& PinName : PinsToRemove)
        {
            Node->RemoveUserDefinedPinByName(PinName);
            AppendLine(Report, FString::Printf(TEXT("CHANGE: removed stale %s signature pin: %s.%s"), SignatureSide, *FunctionName, *PinName.ToString()));
        }

        return PinsToRemove.Num();
    }

    static int32 CleanupFunctionSignaturePins(UEdGraph* Graph, const TSharedPtr<FJsonObject>& ActionObj, const FString& FunctionName, FString& Report)
    {
        if (!Graph || !ActionObj.IsValid())
        {
            return 0;
        }

        TMap<FName, FEdGraphPinType> DesiredInputs;
        TMap<FName, FEdGraphPinType> DesiredOutputs;
        CollectDesiredSignaturePins(ActionObj, TEXT("inputs"), DesiredInputs);
        CollectDesiredSignaturePins(ActionObj, TEXT("outputs"), DesiredOutputs);

        UK2Node_FunctionEntry* Entry = FindOrCreateEntryNode(Graph, FunctionName);
        UK2Node_FunctionResult* Result = FindOrCreateResultNode(Graph, FunctionName);
        if (!Entry || !Result)
        {
            AppendLine(Report, FString::Printf(TEXT("WARNING: signature cleanup could not resolve Entry/Return for function '%s'."), *FunctionName));
            return 0;
        }

        Entry->Modify();
        Result->Modify();
        int32 Removed = 0;
        Removed += SynchronizeEditableSignaturePins(Entry, DesiredInputs, EGPD_Output, FunctionName, TEXT("input"), Report);
        Removed += SynchronizeEditableSignaturePins(Result, DesiredOutputs, EGPD_Input, FunctionName, TEXT("output"), Report);
        return Removed;
    }

    static void CollectExistingSignaturePins(UK2Node_EditablePinBase* Node, EEdGraphPinDirection Direction, TMap<FName, FEdGraphPinType>& OutPins)
    {
        if (!Node)
        {
            return;
        }
        for (const TSharedPtr<FUserPinInfo>& UserPin : Node->UserDefinedPins)
        {
            if (UserPin.IsValid() && UserPin->DesiredPinDirection == Direction)
            {
                OutPins.Add(UserPin->PinName, UserPin->PinType);
            }
        }
    }

    static int32 CountStaleSignaturePins(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& ActionObj, FString& Report)
    {
        if (!Blueprint || !ActionObj.IsValid())
        {
            return 0;
        }

        const FString FunctionName = GetActionFunctionName(ActionObj);
        UEdGraph* Graph = FindFunctionGraph(Blueprint, FunctionName);
        if (!Graph)
        {
            return 0;
        }

        UK2Node_FunctionEntry* Entry = nullptr;
        UK2Node_FunctionResult* Result = nullptr;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            Entry = Entry ? Entry : Cast<UK2Node_FunctionEntry>(Node);
            Result = Result ? Result : Cast<UK2Node_FunctionResult>(Node);
        }

        TMap<FName, FEdGraphPinType> DesiredInputs;
        TMap<FName, FEdGraphPinType> DesiredOutputs;
        TMap<FName, FEdGraphPinType> ExistingInputs;
        TMap<FName, FEdGraphPinType> ExistingOutputs;
        CollectDesiredSignaturePins(ActionObj, TEXT("inputs"), DesiredInputs);
        CollectDesiredSignaturePins(ActionObj, TEXT("outputs"), DesiredOutputs);
        CollectExistingSignaturePins(Entry, EGPD_Output, ExistingInputs);
        CollectExistingSignaturePins(Result, EGPD_Input, ExistingOutputs);

        int32 StaleCount = 0;
        auto CountStale = [&StaleCount](const TMap<FName, FEdGraphPinType>& Existing, const TMap<FName, FEdGraphPinType>& Desired)
        {
            for (const TPair<FName, FEdGraphPinType>& Pair : Existing)
            {
                const FEdGraphPinType* DesiredType = Desired.Find(Pair.Key);
                if (!DesiredType || !(Pair.Value == *DesiredType))
                {
                    ++StaleCount;
                }
            }
        };
        CountStale(ExistingInputs, DesiredInputs);
        CountStale(ExistingOutputs, DesiredOutputs);

        if (StaleCount > 0)
        {
            AppendLine(Report, FString::Printf(TEXT("VALIDATION: function '%s' has %d stale signature pin(s); replace_body will remove them before graph mutation."), *FunctionName, StaleCount));
        }
        return StaleCount;
    }

    static void ClearFunctionBodyPreservingSignature(UBlueprint* Blueprint, UEdGraph* Graph, FString& Report)
    {
        if (!Blueprint || !Graph)
        {
            return;
        }

        TArray<UEdGraphNode*> NodesToRemove;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (!Node)
            {
                continue;
            }

            if (Node->IsA<UK2Node_FunctionEntry>() || Node->IsA<UK2Node_FunctionResult>())
            {
                Node->Modify();
                BreakAllLinksOnNode(Node);
                continue;
            }

            NodesToRemove.Add(Node);
        }

        for (UEdGraphNode* Node : NodesToRemove)
        {
            if (Node)
            {
                Node->Modify();
                FBlueprintEditorUtils::RemoveNode(Blueprint, Node, true);
            }
        }

        AppendLine(Report, FString::Printf(TEXT("CHANGE: function body: %s"), *Graph->GetName()));
    }

    static bool IsFunctionPatchAction(const FString& ActionType)
    {
        return ActionType == TEXT("add_or_replace_function") || ActionType == TEXT("replace_function_body");
    }

    static bool IsMemberVariablesAction(const FString& ActionType)
    {
        return ActionType == TEXT("add_member_variables") || ActionType == TEXT("add_variables");
    }

    static FString GetActionFunctionName(const TSharedPtr<FJsonObject>& ActionObj)
    {
        return GetStringFieldSafe(ActionObj, TEXT("function_name"), GetStringFieldSafe(ActionObj, TEXT("name")));
    }

    static const TArray<TSharedPtr<FJsonValue>>* GetMemberVariablesArray(const TSharedPtr<FJsonObject>& Obj)
    {
        const TArray<TSharedPtr<FJsonValue>>* Variables = GetArrayFieldSafe(Obj, TEXT("member_variables"));
        if (!Variables)
        {
            Variables = GetArrayFieldSafe(Obj, TEXT("variables"));
        }
        return Variables;
    }

    static bool IsSafeBlueprintIdentifier(const FString& Name)
    {
        if (Name.IsEmpty() || Name == TEXT("None"))
        {
            return false;
        }

        const FString BadChars = TEXT("/\\:.\"'` ,;()[]{}+-=*?!@#$%^&|<>\t\r\n");
        for (int32 Index = 0; Index < BadChars.Len(); ++Index)
        {
            const FString BadCharString = BadChars.Mid(Index, 1);
            if (Name.Contains(BadCharString))
            {
                return false;
            }
        }
        return true;
    }

    static bool ValidateMemberVariablesShape(const TSharedPtr<FJsonObject>& Obj, const FString& Context, FString& Report)
    {
        const TArray<TSharedPtr<FJsonValue>>* Variables = GetMemberVariablesArray(Obj);
        if (!Variables || Variables->Num() == 0)
        {
            AppendLine(Report, FString::Printf(TEXT("ERROR: %s requires variables[] or member_variables[]."), *Context));
            return false;
        }

        bool bOk = true;
        TSet<FString> Names;
        for (const TSharedPtr<FJsonValue>& Value : *Variables)
        {
            TSharedPtr<FJsonObject> VarObj = Value.IsValid() ? Value->AsObject() : nullptr;
            const FString Name = GetStringFieldSafe(VarObj, TEXT("name"));
            if (!IsSafeBlueprintIdentifier(Name))
            {
                AppendLine(Report, FString::Printf(TEXT("ERROR: %s contains invalid member variable name '%s'."), *Context, *Name));
                bOk = false;
                continue;
            }
            if (Names.Contains(Name))
            {
                AppendLine(Report, FString::Printf(TEXT("ERROR: %s contains duplicate member variable '%s'."), *Context, *Name));
                bOk = false;
            }
            Names.Add(Name);

            const FEdGraphPinType PinType = MakePinTypeFromJson(VarObj);
            if (PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard || PinType.PinCategory.IsNone())
            {
                AppendLine(Report, FString::Printf(
                    TEXT("ERROR: %s contains unsupported member variable type for '%s': wildcard/unknown pin type."),
                    *Context,
                    *Name));
                bOk = false;
            }
        }
        return bOk;
    }

    static bool BlueprintHasMemberVariable(UBlueprint* Blueprint, const FString& VariableName);
    static bool ValidateHighRiskNodeContext(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& NodeObj, bool bFunctionGraph, FString& Report);
    static bool ValidateFunctionPatchSafety(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& ActionObj, FString& Report);
    static UEdGraph* FindTargetGraphForPatch(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& ActionObj);

    static FString BoundaryGraphKind(UBlueprint* Blueprint, UEdGraph* Graph)
    {
        if (!Blueprint || !Graph) return TEXT("unknown");
        if (Blueprint->UbergraphPages.Contains(Graph)) return Graph->GetFName() == UEdGraphSchema_K2::FN_UserConstructionScript ? TEXT("construction_script") : TEXT("event_graph");
        if (Blueprint->FunctionGraphs.Contains(Graph)) return TEXT("function");
        if (Blueprint->MacroGraphs.Contains(Graph)) return TEXT("macro");
        if (Blueprint->DelegateSignatureGraphs.Contains(Graph)) return TEXT("delegate_signature");
        if (Graph->GetOuter() && Graph->GetOuter()->IsA<UK2Node_Composite>()) return TEXT("collapsed_graph");
        return TEXT("extra_graph");
    }

    static FString BoundaryGraphIdentity(UBlueprint* Blueprint, UEdGraph* Graph, const FString& LogicalBlueprintPath = FString())
    {
        const FString RuntimeBlueprintPath = Blueprint ? Blueprint->GetPathName() : FString();
        const FString IdentityBlueprintPath = LogicalBlueprintPath.IsEmpty() ? RuntimeBlueprintPath : LogicalBlueprintPath;
        FString GraphPath = Graph ? Graph->GetPathName() : FString();
        if (!LogicalBlueprintPath.IsEmpty() && !RuntimeBlueprintPath.IsEmpty() && GraphPath.StartsWith(RuntimeBlueprintPath))
        {
            GraphPath = LogicalBlueprintPath + GraphPath.Mid(RuntimeBlueprintPath.Len());
        }
        return FString::Printf(TEXT("%s|%s|%s|%s"), *IdentityBlueprintPath, *BoundaryGraphKind(Blueprint, Graph), Graph ? *Graph->GetName() : TEXT(""), *GraphPath);
    }

    // UK2Node_Composite owns its BoundGraph. UE therefore inserts the transiently
    // generated object name (for example K2Node_Composite_0) into Graph->GetPathName().
    // That numeric object suffix is not a stable patch identity: it may change after
    // recreation or when another Composite node is inserted first. JSON may use either
    // the exact exported path or the canonical path with that generated segment removed.
    static FString CanonicalizeCompositeBoundGraphPath(const FString& GraphPath)
    {
        FString Canonical = GraphPath;
        const FString Marker = TEXT(".K2Node_Composite_");
        while (true)
        {
            const int32 MarkerIndex = Canonical.Find(Marker, ESearchCase::CaseSensitive, ESearchDir::FromStart);
            if (MarkerIndex == INDEX_NONE) break;
            const int32 SegmentEnd = Canonical.Find(TEXT("."), ESearchCase::CaseSensitive, ESearchDir::FromStart, MarkerIndex + Marker.Len());
            if (SegmentEnd == INDEX_NONE) break;
            Canonical.RemoveAt(MarkerIndex, SegmentEnd - MarkerIndex, false);
        }
        return Canonical;
    }

    static FString CanonicalizeCompositeBoundGraphIdentity(const FString& Identity)
    {
        TArray<FString> Parts;
        Identity.ParseIntoArray(Parts, TEXT("|"), false);
        if (Parts.Num() != 4 || Parts[1] != TEXT("collapsed_graph")) return Identity;
        Parts[3] = CanonicalizeCompositeBoundGraphPath(Parts[3]);
        return FString::Join(Parts, TEXT("|"));
    }

    static bool CompositeBoundGraphIdentityMatches(UBlueprint* Blueprint, UEdGraph* BoundGraph, const FString& DeclaredIdentity, const FString& LogicalBlueprintPath = FString())
    {
        if (!Blueprint || !BoundGraph || DeclaredIdentity.IsEmpty()) return false;
        const FString ActualIdentity = BoundaryGraphIdentity(Blueprint, BoundGraph, LogicalBlueprintPath);
        return ActualIdentity == DeclaredIdentity ||
            CanonicalizeCompositeBoundGraphIdentity(ActualIdentity) == CanonicalizeCompositeBoundGraphIdentity(DeclaredIdentity);
    }

    static bool RejectBoundary(FString& Report, const TCHAR* Code, const FString& Detail)
    {
        AppendLine(Report, FString::Printf(TEXT("ERROR: code=%s; %s"), Code, *Detail));
        return false;
    }

    static bool ValidateGraphBoundaryAction(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& ActionObj, FString& Report, const FString& LogicalBlueprintPath = FString())
    {
        const FString Type = GetStringFieldSafe(ActionObj, TEXT("type"));
        const bool bMacro = Type == TEXT("add_macro");
        const bool bCollapsed = Type == TEXT("create_collapsed_graph") || Type == TEXT("replace_collapsed_graph");
        bool bHasBoundaryNodes = false; const TArray<TSharedPtr<FJsonValue>>* BoundaryNodes = GetArrayFieldSafe(ActionObj, TEXT("nodes"));
        if (BoundaryNodes) for (const TSharedPtr<FJsonValue>& Value : *BoundaryNodes) { const TSharedPtr<FJsonObject> Node = Value.IsValid() ? Value->AsObject() : nullptr; const FString NodeType = GetStringFieldSafe(Node, TEXT("type"), GetStringFieldSafe(Node, TEXT("class"))).ToLower(); bHasBoundaryNodes |= NodeType.Contains(TEXT("tunnel")) || NodeType.Contains(TEXT("composite")); }
        if (!bMacro && !bCollapsed && !bHasBoundaryNodes) return true;
        const FString Owner = GetStringFieldSafe(ActionObj, TEXT("owner_blueprint_path"));
        const FString IdentityBlueprintPath = LogicalBlueprintPath.IsEmpty() ? (Blueprint ? Blueprint->GetPathName() : FString()) : LogicalBlueprintPath;
        if (Owner.IsEmpty() || GetStringFieldSafe(ActionObj, TEXT("owning_graph_identity")).IsEmpty()) return RejectBoundary(Report, TEXT("graph_owner_identity_missing"), TEXT("owner_blueprint_path and owning_graph_identity are required before graph mutation"));
        if (!Blueprint || Owner != IdentityBlueprintPath) return RejectBoundary(Report, TEXT("graph_owner_mismatch"), FString::Printf(TEXT("declared owner '%s' does not match '%s'"), *Owner, *IdentityBlueprintPath));

        UEdGraph* Graph = bMacro ? FindMacroGraph(Blueprint, GetStringFieldSafe(ActionObj, TEXT("macro_name"), GetStringFieldSafe(ActionObj, TEXT("name")))) : FindTargetGraphForPatch(Blueprint, ActionObj);
        const FString DeclaredKind = GetStringFieldSafe(ActionObj, TEXT("owning_graph_kind"));
        const FString ExpectedKind = bMacro ? TEXT("macro") : BoundaryGraphKind(Blueprint, Graph);
        if (DeclaredKind != ExpectedKind) return RejectBoundary(Report, TEXT("graph_kind_mismatch"), FString::Printf(TEXT("declared kind '%s' expected '%s'"), *DeclaredKind, *ExpectedKind));
        if (Graph && GetStringFieldSafe(ActionObj, TEXT("owning_graph_identity")) != BoundaryGraphIdentity(Blueprint, Graph, LogicalBlueprintPath)) return RejectBoundary(Report, TEXT("graph_owner_mismatch"), TEXT("owning_graph_identity does not resolve to the selected graph"));
        if (bMacro && !Graph) { const FString Name = GetStringFieldSafe(ActionObj, TEXT("macro_name"), GetStringFieldSafe(ActionObj, TEXT("name"))); const FString ExpectedIdentity = FString::Printf(TEXT("%s|macro|%s|%s:%s"), *IdentityBlueprintPath, *Name, *IdentityBlueprintPath, *Name); if (GetStringFieldSafe(ActionObj, TEXT("owning_graph_identity")) != ExpectedIdentity) return RejectBoundary(Report, TEXT("graph_owner_mismatch"), TEXT("new macro owning_graph_identity is not the deterministic UE4.27 graph identity")); }

        const TArray<TSharedPtr<FJsonValue>>* Nodes = GetArrayFieldSafe(ActionObj, TEXT("nodes"));
        int32 EntryCount = 0, ExitCount = 0; TSet<FString> EntryPins, ExitPins;
        if (Nodes) for (const TSharedPtr<FJsonValue>& Value : *Nodes)
        {
            const TSharedPtr<FJsonObject> Node = Value.IsValid() ? Value->AsObject() : nullptr;
            const FString NodeType = GetStringFieldSafe(Node, TEXT("type"), GetStringFieldSafe(Node, TEXT("class"))).ToLower();
            if (!NodeType.Contains(TEXT("tunnel")) || NodeType.Contains(TEXT("composite"))) continue;
            const FString Role = GetStringFieldSafe(Node, TEXT("tunnel_role"));
            if (Role.IsEmpty()) return RejectBoundary(Report, TEXT("tunnel_role_missing"), TEXT("each tunnel record requires entry or exit role"));
            const FString ExpectedBoundaryIdentity = bCollapsed ? GetStringFieldSafe(ActionObj, TEXT("bound_graph_identity")) : GetStringFieldSafe(ActionObj, TEXT("owning_graph_identity"));
            const FString ExpectedBoundaryKind = bCollapsed ? TEXT("collapsed_graph") : ExpectedKind;
            if (GetStringFieldSafe(Node, TEXT("owning_graph_identity")) != ExpectedBoundaryIdentity) return RejectBoundary(Report, TEXT("graph_owner_mismatch"), TEXT("tunnel owning_graph_identity does not match its graph action"));
            if (GetStringFieldSafe(Node, TEXT("owning_graph_kind")) != ExpectedBoundaryKind) return RejectBoundary(Report, TEXT("graph_kind_mismatch"), TEXT("tunnel owning_graph_kind does not match its graph action"));
            const bool bInputs = GetBoolFieldSafe(Node, TEXT("can_have_inputs")); const bool bOutputs = GetBoolFieldSafe(Node, TEXT("can_have_outputs"));
            if ((Role == TEXT("entry") && (bInputs || !bOutputs)) || (Role == TEXT("exit") && (!bInputs || bOutputs))) return RejectBoundary(Report, TEXT("tunnel_signature_mismatch"), FString::Printf(TEXT("role/flags conflict for '%s'"), *Role));
            if (Role == TEXT("entry")) ++EntryCount; else if (Role == TEXT("exit")) ++ExitCount; else return RejectBoundary(Report, TEXT("graph_boundary_variant_unsupported"), FString::Printf(TEXT("unsupported tunnel role '%s'"), *Role));
            const TArray<TSharedPtr<FJsonValue>>* Signature = GetArrayFieldSafe(Node, TEXT("user_defined_pin_signature"));
            if (!Signature) return RejectBoundary(Report, TEXT("tunnel_signature_mismatch"), TEXT("ordered user_defined_pin_signature is required"));
            for (const TSharedPtr<FJsonValue>& PinValue : *Signature) { const TSharedPtr<FJsonObject> Pin = PinValue.IsValid() ? PinValue->AsObject() : nullptr; const FString PinName = GetStringFieldSafe(Pin, TEXT("name")); if (PinName.IsEmpty()) return RejectBoundary(Report, TEXT("tunnel_signature_mismatch"), TEXT("boundary pin name is empty")); (Role == TEXT("entry") ? EntryPins : ExitPins).Add(PinName); }
        }
        if (EntryCount > 1 || ExitCount > 1) return RejectBoundary(Report, TEXT("duplicate_tunnel_role"), TEXT("a graph may bind only one entry and one exit tunnel"));
        if (EntryCount != 1 || ExitCount != 1) return RejectBoundary(Report, TEXT("tunnel_role_missing"), TEXT("one distinct entry and one distinct exit tunnel are required"));

        if (bCollapsed)
        {
            const FString BoundIdentity = GetStringFieldSafe(ActionObj, TEXT("bound_graph_identity"));
            if (BoundIdentity.IsEmpty()) return RejectBoundary(Report, TEXT("composite_bound_graph_identity_missing"), TEXT("bound_graph_identity is required"));
            UK2Node_Composite* Existing = nullptr;
            int32 ExistingMatchCount = 0;
            if (Graph) for (UEdGraphNode* Node : Graph->Nodes)
            {
                UK2Node_Composite* Candidate = Cast<UK2Node_Composite>(Node);
                if (Candidate && Candidate->BoundGraph && Candidate->BoundGraph->GetName() == GetStringFieldSafe(ActionObj, TEXT("collapsed_graph_name"), GetStringFieldSafe(ActionObj, TEXT("name"))))
                {
                    Existing = Candidate;
                    ++ExistingMatchCount;
                }
            }
            if (ExistingMatchCount > 1) return RejectBoundary(Report, TEXT("composite_bound_graph_ambiguous"), TEXT("multiple Composite nodes own a BoundGraph with the requested collapsed_graph_name"));
            if (Existing && !CompositeBoundGraphIdentityMatches(Blueprint, Existing->BoundGraph, BoundIdentity, LogicalBlueprintPath)) return RejectBoundary(Report, TEXT("composite_bound_graph_mismatch"), TEXT("bound_graph_identity does not match the existing Composite BoundGraph after canonicalizing the generated K2Node_Composite_<n> object segment"));
            const TArray<TSharedPtr<FJsonValue>>* Mapping = GetArrayFieldSafe(ActionObj, TEXT("outer_to_inner_pin_mapping"));
            if (!Mapping) return RejectBoundary(Report, TEXT("composite_pin_mapping_mismatch"), TEXT("outer_to_inner_pin_mapping is required"));
            TSet<FString> OuterPins;
            for (const TSharedPtr<FJsonValue>& MapValue : *Mapping) { const TSharedPtr<FJsonObject> Map = MapValue.IsValid() ? MapValue->AsObject() : nullptr; const FString OuterPin = GetStringFieldSafe(Map, TEXT("outer_pin")); const FString InnerPin = GetStringFieldSafe(Map, TEXT("inner_pin")); const FString Role = GetStringFieldSafe(Map, TEXT("inner_tunnel_role"), GetStringFieldSafe(Map, TEXT("inner_role"))); if (OuterPin.IsEmpty() || InnerPin.IsEmpty() || OuterPins.Contains(OuterPin) || (Role == TEXT("entry") && !EntryPins.Contains(InnerPin)) || (Role == TEXT("exit") && !ExitPins.Contains(InnerPin)) || (Role != TEXT("entry") && Role != TEXT("exit"))) return RejectBoundary(Report, TEXT("composite_pin_mapping_mismatch"), FString::Printf(TEXT("invalid mapping '%s' -> '%s:%s'"), *OuterPin, *Role, *InnerPin)); OuterPins.Add(OuterPin); }
        }
        return true;
    }

    static bool IsDataTableResultPinAlias(const FString& PinName)
    {
        const FString Normalized = NormalizeLoosePinName(PinName);
        return Normalized == TEXT("returnvalue") || Normalized == TEXT("outrow") ||
            Normalized == TEXT("result") || Normalized == TEXT("return");
    }

    static bool ValidateLinkedDataTableOutputAuthority(
        const TSharedPtr<FJsonObject>& ActionObj,
        FString& Report)
    {
        const TArray<TSharedPtr<FJsonValue>>* Nodes = GetArrayFieldSafe(ActionObj, TEXT("nodes"));
        if (!Nodes) return true;

        TSet<FString> LinkedGetRowIds;
        for (const TSharedPtr<FJsonValue>& Value : *Nodes)
        {
            const TSharedPtr<FJsonObject> NodeObj = Value.IsValid() ? Value->AsObject() : nullptr;
            const FString Type = GetStringFieldSafe(NodeObj, TEXT("type"), GetStringFieldSafe(NodeObj, TEXT("class"))).ToLower();
            if (Type.Contains(TEXT("getdatatablerow")) &&
                GetBoolFieldSafe(NodeObj, TEXT("data_table_pin_linked"), false))
            {
                const FString NodeId = GetStringFieldSafe(NodeObj, TEXT("id"));
                if (!NodeId.IsEmpty()) LinkedGetRowIds.Add(NodeId);
            }
        }
        if (LinkedGetRowIds.Num() == 0) return true;

        TSet<FString> TypedOutputIds;
        auto ScanEdges = [&](const FString& FieldName)
        {
            const TArray<TSharedPtr<FJsonValue>>* Edges = GetArrayFieldSafe(ActionObj, FieldName);
            if (!Edges) return;
            for (const TSharedPtr<FJsonValue>& Value : *Edges)
            {
                const TSharedPtr<FJsonObject> EdgeObj = Value.IsValid() ? Value->AsObject() : nullptr;
                const FString FromId = GetStringFieldSafe(EdgeObj, TEXT("from_node_id"), GetStringFieldSafe(EdgeObj, TEXT("from")));
                const FString FromPin = GetStringFieldSafe(EdgeObj, TEXT("from_pin"));
                if (LinkedGetRowIds.Contains(FromId) && IsDataTableResultPinAlias(FromPin))
                {
                    TypedOutputIds.Add(FromId);
                }
            }
        };
        ScanEdges(TEXT("data_edges"));
        ScanEdges(TEXT("edges"));

        bool bOk = true;
        for (const FString& NodeId : LinkedGetRowIds)
        {
            if (!TypedOutputIds.Contains(NodeId))
            {
                bOk = false;
                AppendLine(Report, FString::Printf(
                    TEXT("N2C_PREFLIGHT_GUARD|code=datatable_linked_output_untyped|node=%s"),
                    *NodeId));
                AppendLine(Report, FString::Printf(
                    TEXT("ERROR: linked GetDataTableRow '%s' requires an outgoing ReturnValue/Out Row data edge to a typed row-struct pin. UE4.27 cannot persist linked row type without that authority. Rejected before mutation."),
                    *NodeId));
            }
        }
        return bOk;
    }

    static bool ValidateActionShape(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& ActionObj, FString& Report, const FString& LogicalBlueprintPath = FString())
    {
        if (!ActionObj.IsValid())
        {
            AppendLine(Report, TEXT("ERROR: invalid action object."));
            return false;
        }

        const FString ActionType = GetStringFieldSafe(ActionObj, TEXT("type"));
        if (!IsSupportedActionType(ActionType))
        {
            AppendLine(Report, FString::Printf(TEXT("ERROR: unsupported action type '%s'."), *ActionType));
            return false;
        }

        if (IsMemberVariablesAction(ActionType))
        {
            return ValidateMemberVariablesShape(ActionObj, TEXT("add_member_variables action"), Report);
        }

        if (ActionType == TEXT("add_event_dispatcher") || ActionType == TEXT("add_event_dispatchers"))
        {
            const TArray<TSharedPtr<FJsonValue>>* Dispatchers = GetArrayFieldSafe(ActionObj, TEXT("dispatchers"));
            const FString SingleName = GetStringFieldSafe(ActionObj, TEXT("name"));
            if ((!Dispatchers || Dispatchers->Num() == 0) && SingleName.IsEmpty())
            {
                AppendLine(Report, FString::Printf(TEXT("ERROR: %s requires name or dispatchers[]."), *ActionType));
                return false;
            }
            return true;
        }

        if (ActionType == TEXT("implement_interface"))
        {
            const FString InterfacePath = GetStringFieldSafe(ActionObj, TEXT("interface_class_path"), GetStringFieldSafe(ActionObj, TEXT("interface_path")));
            UClass* InterfaceClass = ResolveBlueprintInterfaceClass(InterfacePath);
            if (!InterfaceClass || !InterfaceClass->HasAnyClassFlags(CLASS_Interface))
            {
                AppendLine(Report, FString::Printf(TEXT("ERROR: implement_interface requires a resolvable Blueprint interface class path. Got '%s'."), *InterfacePath));
                return false;
            }
            return true;
        }

        if (ActionType == TEXT("import_scs_hierarchy") || ActionType == TEXT("add_scs_components") || ActionType == TEXT("update_scs_hierarchy"))
        {
            const TArray<TSharedPtr<FJsonValue>>* Components = GetArrayFieldSafe(ActionObj, TEXT("components"));
            if (!Components || Components->Num() == 0)
            {
                AppendLine(Report, FString::Printf(TEXT("ERROR: %s requires components[]."), *ActionType));
                return false;
            }
            return true;
        }

        if (ActionType == TEXT("add_macro"))
        {
            const FString MacroName = GetStringFieldSafe(ActionObj, TEXT("macro_name"), GetStringFieldSafe(ActionObj, TEXT("name")));
            if (!IsSafeBlueprintIdentifier(MacroName))
            {
                AppendLine(Report, FString::Printf(TEXT("ERROR: add_macro requires safe macro_name/name. Got '%s'."), *MacroName));
                return false;
            }
            return ValidateGraphBoundaryAction(Blueprint, ActionObj, Report, LogicalBlueprintPath);
        }

        if (ActionType == TEXT("create_collapsed_graph") || ActionType == TEXT("replace_collapsed_graph"))
        {
            const FString CollapsedName = GetStringFieldSafe(ActionObj, TEXT("collapsed_graph_name"), GetStringFieldSafe(ActionObj, TEXT("name")));
            if (!IsSafeBlueprintIdentifier(CollapsedName))
            {
                AppendLine(Report, FString::Printf(TEXT("ERROR: %s requires safe collapsed_graph_name/name. Got '%s'."), *ActionType, *CollapsedName));
                return false;
            }
            if (!FindTargetGraphForPatch(Blueprint, ActionObj))
            {
                AppendLine(Report, FString::Printf(TEXT("ERROR: %s target parent graph was not found."), *ActionType));
                return false;
            }
            return ValidateGraphBoundaryAction(Blueprint, ActionObj, Report, LogicalBlueprintPath);
        }

        if (ActionType == TEXT("rename_variable"))
        {
            const FString OldName = GetStringFieldSafe(ActionObj, TEXT("old_variable_name"), GetStringFieldSafe(ActionObj, TEXT("from")));
            const FString NewName = GetStringFieldSafe(ActionObj, TEXT("new_variable_name"), GetStringFieldSafe(ActionObj, TEXT("to")));
            if (!IsSafeBlueprintIdentifier(OldName) || !IsSafeBlueprintIdentifier(NewName))
            {
                AppendLine(Report, FString::Printf(TEXT("ERROR: rename_variable requires safe old_variable_name/from and new_variable_name/to. Old='%s' New='%s'."), *OldName, *NewName));
                return false;
            }
            if (!BlueprintHasMemberVariable(Blueprint, OldName) && !BlueprintHasMemberVariable(Blueprint, NewName))
            {
                AppendLine(Report, FString::Printf(TEXT("ERROR: rename_variable source variable not found: %s."), *OldName));
                return false;
            }
            return true;
        }

        if (ActionType == TEXT("delete_event_graph_nodes") || ActionType == TEXT("delete_graph_nodes"))
        {
            const TArray<TSharedPtr<FJsonValue>>* Names = GetArrayFieldSafe(ActionObj, TEXT("names"));
            if (!Names || Names->Num() == 0)
            {
                AppendLine(Report, FString::Printf(TEXT("ERROR: %s requires names[]."), *ActionType));
                return false;
            }
            return true;
        }

        if (ActionType == TEXT("rename_custom_event"))
        {
            const FString OldName = GetStringFieldSafe(ActionObj, TEXT("old_name"), GetStringFieldSafe(ActionObj, TEXT("from")));
            const FString NewName = GetStringFieldSafe(ActionObj, TEXT("new_name"), GetStringFieldSafe(ActionObj, TEXT("to")));
            if (OldName.IsEmpty() || !IsSafeBlueprintIdentifier(NewName))
            {
                AppendLine(Report, FString::Printf(TEXT("ERROR: rename_custom_event requires old_name/from and safe new_name/to. Old='%s' New='%s'."), *OldName, *NewName));
                return false;
            }
            return true;
        }

        if (IsEventGraphPatchAction(ActionType) || IsWidgetGraphPatchAction(ActionType) || IsAnimationGraphPatchAction(ActionType))
        {
            const TArray<TSharedPtr<FJsonValue>>* Nodes = GetArrayFieldSafe(ActionObj, TEXT("nodes"));
            if (!Nodes || Nodes->Num() == 0)
            {
                AppendLine(Report, FString::Printf(TEXT("ERROR: %s requires nodes[]."), *ActionType));
                return false;
            }

            if (!ValidateGraphBoundaryAction(Blueprint, ActionObj, Report, LogicalBlueprintPath)) return false;
            TSet<FString> NodeIds;
            bool bOk = true;
            for (const TSharedPtr<FJsonValue>& Value : *Nodes)
            {
                TSharedPtr<FJsonObject> NodeObj = Value.IsValid() ? Value->AsObject() : nullptr;
                const FString NodeId = GetStringFieldSafe(NodeObj, TEXT("id"));
                FString Type = GetStringFieldSafe(NodeObj, TEXT("type"));
                if (Type.IsEmpty())
                {
                    Type = GetStringFieldSafe(NodeObj, TEXT("class"));
                }
                if (NodeId.IsEmpty())
                {
                    AppendLine(Report, TEXT("ERROR: event graph patch contains node without id."));
                    bOk = false;
                }
                else if (NodeIds.Contains(NodeId))
                {
                    AppendLine(Report, FString::Printf(TEXT("ERROR: event graph patch contains duplicate node id '%s'."), *NodeId));
                    bOk = false;
                }
                else
                {
                    NodeIds.Add(NodeId);
                }
                if (Type.IsEmpty() || !IsSupportedNodeType(Type))
                {
                    AppendLine(Report, FString::Printf(TEXT("ERROR: event graph patch uses unsupported node type '%s'."), *Type));
                    bOk = false;
                }
                else
                {
                    bOk &= ValidateHighRiskNodeContext(Blueprint, NodeObj, false, Report);
                }
            }
            bOk &= ValidateLinkedDataTableOutputAuthority(ActionObj, Report);
            return bOk;
        }

        if (ActionType == TEXT("rename_function"))
        {
            const FString OldName = GetStringFieldSafe(ActionObj, TEXT("old_function_name"), GetStringFieldSafe(ActionObj, TEXT("from")));
            const FString NewName = GetStringFieldSafe(ActionObj, TEXT("new_function_name"), GetStringFieldSafe(ActionObj, TEXT("to")));
            if (!IsSafeBlueprintIdentifier(OldName) || !IsSafeBlueprintIdentifier(NewName))
            {
                AppendLine(Report, FString::Printf(TEXT("ERROR: rename_function requires safe old_function_name/from and new_function_name/to. Old='%s' New='%s'."), *OldName, *NewName));
                return false;
            }
            if (!FindFunctionGraph(Blueprint, OldName))
            {
                if (FindFunctionGraph(Blueprint, NewName))
                {
                    return true;
                }
                AppendLine(Report, FString::Printf(TEXT("ERROR: rename_function source graph not found: %s."), *OldName));
                return false;
            }
            if (FindFunctionGraph(Blueprint, NewName))
            {
                AppendLine(Report, FString::Printf(TEXT("ERROR: rename_function destination already exists: %s."), *NewName));
                return false;
            }
            return true;
        }

        if (ActionType == TEXT("delete_function") || ActionType == TEXT("delete_functions"))
        {
            TArray<FString> Names;
            GetStringArrayField(ActionObj, TEXT("function_names"), Names);
            GetStringArrayField(ActionObj, TEXT("names"), Names);
            const FString SingleName = GetStringFieldSafe(ActionObj, TEXT("function_name"), GetStringFieldSafe(ActionObj, TEXT("name")));
            if (!SingleName.IsEmpty())
            {
                Names.Add(SingleName);
            }
            if (Names.Num() == 0)
            {
                AppendLine(Report, TEXT("ERROR: delete_function requires function_name/name/function_names[]."));
                return false;
            }
            for (const FString& Name : Names)
            {
                if (!IsSafeBlueprintIdentifier(Name))
                {
                    AppendLine(Report, FString::Printf(TEXT("ERROR: delete_function contains unsafe function name: %s."), *Name));
                    return false;
                }
            }
            return true;
        }

        if (ActionType == TEXT("delete_variable") || ActionType == TEXT("delete_variables") ||
            ActionType == TEXT("delete_member_variable") || ActionType == TEXT("delete_member_variables"))
        {
            TArray<FString> Names;
            GetStringArrayField(ActionObj, TEXT("variable_names"), Names);
            GetStringArrayField(ActionObj, TEXT("names"), Names);
            const FString SingleName = GetStringFieldSafe(ActionObj, TEXT("variable_name"), GetStringFieldSafe(ActionObj, TEXT("name")));
            if (!SingleName.IsEmpty())
            {
                Names.Add(SingleName);
            }
            if (Names.Num() == 0)
            {
                AppendLine(Report, TEXT("ERROR: delete_variable requires variable_name/name/variable_names[]."));
                return false;
            }
            for (const FString& Name : Names)
            {
                if (!IsSafeBlueprintIdentifier(Name))
                {
                    AppendLine(Report, FString::Printf(TEXT("ERROR: delete_variable contains unsafe variable name: %s."), *Name));
                    return false;
                }
            }
            return true;
        }

        if (ActionType == TEXT("delete_event_dispatcher") || ActionType == TEXT("delete_event_dispatchers") ||
            ActionType == TEXT("delete_dispatcher") || ActionType == TEXT("delete_dispatchers"))
        {
            TArray<FString> Names;
            GetStringArrayField(ActionObj, TEXT("dispatcher_names"), Names);
            GetStringArrayField(ActionObj, TEXT("names"), Names);
            const FString SingleName = GetStringFieldSafe(ActionObj, TEXT("dispatcher_name"), GetStringFieldSafe(ActionObj, TEXT("name")));
            if (!SingleName.IsEmpty())
            {
                Names.Add(SingleName);
            }
            if (Names.Num() == 0)
            {
                AppendLine(Report, TEXT("ERROR: delete_event_dispatcher requires dispatcher_name/name/dispatcher_names[]."));
                return false;
            }
            for (const FString& Name : Names)
            {
                if (!IsSafeBlueprintIdentifier(Name))
                {
                    AppendLine(Report, FString::Printf(TEXT("ERROR: delete_event_dispatcher contains unsafe dispatcher name: %s."), *Name));
                    return false;
                }
            }
            return true;
        }

        if (ActionType == TEXT("delete_macro") || ActionType == TEXT("delete_macros"))
        {
            TArray<FString> Names;
            GetStringArrayField(ActionObj, TEXT("macro_names"), Names);
            GetStringArrayField(ActionObj, TEXT("names"), Names);
            const FString SingleName = GetStringFieldSafe(ActionObj, TEXT("macro_name"), GetStringFieldSafe(ActionObj, TEXT("name")));
            if (!SingleName.IsEmpty())
            {
                Names.Add(SingleName);
            }
            if (Names.Num() == 0)
            {
                AppendLine(Report, TEXT("ERROR: delete_macro requires macro_name/name/macro_names[]."));
                return false;
            }
            for (const FString& Name : Names)
            {
                if (!IsSafeBlueprintIdentifier(Name))
                {
                    AppendLine(Report, FString::Printf(TEXT("ERROR: delete_macro contains unsafe macro name: %s."), *Name));
                    return false;
                }
            }
            return true;
        }

        if (ActionType == TEXT("set_function_category") || ActionType == TEXT("move_function_to_category"))
        {
            const FString FunctionName = GetActionFunctionName(ActionObj);
            const FString Category = GetStringFieldSafe(ActionObj, TEXT("category"), GetStringFieldSafe(ActionObj, TEXT("folder")));
            if (!IsSafeBlueprintIdentifier(FunctionName))
            {
                AppendLine(Report, FString::Printf(TEXT("ERROR: %s requires function_name."), *ActionType));
                return false;
            }
            if (!FindFunctionGraph(Blueprint, FunctionName))
            {
                AppendLine(Report, FString::Printf(TEXT("ERROR: %s target function graph not found: %s."), *ActionType, *FunctionName));
                return false;
            }
            if (Category.IsEmpty())
            {
                AppendLine(Report, FString::Printf(TEXT("ERROR: %s requires category/folder."), *ActionType));
                return false;
            }
            return true;
        }

        const FString FunctionName = GetActionFunctionName(ActionObj);
        if (!IsSafeBlueprintIdentifier(FunctionName))
        {
            AppendLine(Report, TEXT("ERROR: action is missing a safe function_name."));
            return false;
        }

        if (ActionType == TEXT("replace_function_body") && !FindFunctionGraph(Blueprint, FunctionName))
        {
            AppendLine(Report, FString::Printf(TEXT("ERROR: replace_function_body requires an existing function graph: %s."), *FunctionName));
            return false;
        }

        const TArray<TSharedPtr<FJsonValue>>* Nodes = GetArrayFieldSafe(ActionObj, TEXT("nodes"));
        if (!Nodes || Nodes->Num() == 0)
        {
            AppendLine(Report, FString::Printf(TEXT("ERROR: function '%s' has no nodes[]."), *FunctionName));
            return false;
        }

        bool bHasEntry = false;
        bool bHasReturn = false;
        TSet<FString> NodeIds;
        bool bOk = true;

        for (const TSharedPtr<FJsonValue>& Value : *Nodes)
        {
            TSharedPtr<FJsonObject> NodeObj = Value.IsValid() ? Value->AsObject() : nullptr;
            if (!NodeObj.IsValid())
            {
                AppendLine(Report, FString::Printf(TEXT("ERROR: function '%s' contains an invalid node object."), *FunctionName));
                bOk = false;
                continue;
            }

            const FString NodeId = GetStringFieldSafe(NodeObj, TEXT("id"));
            if (NodeId.IsEmpty())
            {
                AppendLine(Report, FString::Printf(TEXT("ERROR: function '%s' contains node without id."), *FunctionName));
                bOk = false;
            }
            else if (NodeIds.Contains(NodeId))
            {
                AppendLine(Report, FString::Printf(TEXT("ERROR: function '%s' contains duplicate node id '%s'."), *FunctionName, *NodeId));
                bOk = false;
            }
            else
            {
                NodeIds.Add(NodeId);
            }

            FString Type = GetStringFieldSafe(NodeObj, TEXT("type"));
            if (Type.IsEmpty())
            {
                Type = GetStringFieldSafe(NodeObj, TEXT("class"));
            }

            if (Type.IsEmpty() || !IsSupportedNodeType(Type))
            {
                AppendLine(Report, FString::Printf(TEXT("ERROR: function '%s' uses unsupported node type '%s'."), *FunctionName, *Type));
                bOk = false;
            }
            else
            {
                bOk &= ValidateHighRiskNodeContext(Blueprint, NodeObj, true, Report);
            }

            const FString Lower = Type.ToLower();
            bHasEntry |= Lower == TEXT("entry") || Lower.Contains(TEXT("functionentry"));
            bHasReturn |= Lower == TEXT("return") || Lower.Contains(TEXT("functionresult"));
        }

        if (!bHasEntry)
        {
            AppendLine(Report, FString::Printf(TEXT("ERROR: function '%s' patch must contain FunctionEntry/Entry node."), *FunctionName));
            bOk = false;
        }
        if (!bHasReturn)
        {
            AppendLine(Report, FString::Printf(TEXT("ERROR: function '%s' patch must contain FunctionResult/Return node."), *FunctionName));
            bOk = false;
        }

        auto ValidateEdges = [&](const FString& FieldName)
        {
            const TArray<TSharedPtr<FJsonValue>>* Edges = GetArrayFieldSafe(ActionObj, FieldName);
            if (!Edges)
            {
                return;
            }
            for (const TSharedPtr<FJsonValue>& Value : *Edges)
            {
                TSharedPtr<FJsonObject> EdgeObj = Value.IsValid() ? Value->AsObject() : nullptr;
                if (!EdgeObj.IsValid())
                {
                    AppendLine(Report, FString::Printf(TEXT("ERROR: function '%s' contains invalid edge object in %s."), *FunctionName, *FieldName));
                    bOk = false;
                    continue;
                }
                const FString FromId = GetStringFieldSafe(EdgeObj, TEXT("from_node_id"), GetStringFieldSafe(EdgeObj, TEXT("from")));
                const FString ToId = GetStringFieldSafe(EdgeObj, TEXT("to_node_id"), GetStringFieldSafe(EdgeObj, TEXT("to")));
                if (!NodeIds.Contains(FromId) || !NodeIds.Contains(ToId))
                {
                    AppendLine(Report, FString::Printf(TEXT("ERROR: function '%s' edge references unknown node id: %s -> %s."), *FunctionName, *FromId, *ToId));
                    bOk = false;
                }
            }
        };

        ValidateEdges(TEXT("exec_edges"));
        ValidateEdges(TEXT("data_edges"));
        ValidateEdges(TEXT("edges"));
        bOk &= ValidateLinkedDataTableOutputAuthority(ActionObj, Report);

        if (bOk)
        {
            bOk &= ValidateFunctionPatchSafety(Blueprint, ActionObj, Report);
        }
        return bOk;
    }

    static FString GetTypeObjectPathFromJson(const TSharedPtr<FJsonObject>& Obj)
    {
        if (!Obj.IsValid())
        {
            return TEXT("");
        }

        static const TCHAR* DirectFields[] =
        {
            TEXT("sub_category_object"),
            TEXT("struct_path"),
            TEXT("enum_path"),
            TEXT("type_path"),
            TEXT("asset_path"),
            TEXT("object_path")
        };

        for (const TCHAR* FieldName : DirectFields)
        {
            const FString Value = GetStringFieldSafe(Obj, FieldName);
            if (!Value.IsEmpty())
            {
                return Value;
            }
        }

        const TSharedPtr<FJsonObject>* PinTypeObj = nullptr;
        if (Obj->TryGetObjectField(TEXT("pin_type"), PinTypeObj) && PinTypeObj && PinTypeObj->IsValid())
        {
            const FString NestedValue = GetTypeObjectPathFromJson(*PinTypeObj);
            if (!NestedValue.IsEmpty())
            {
                return NestedValue;
            }
        }

        return TEXT("");
    }

    static FString NormalizeTypeObjectPathForUE427(FString ObjectPath)
    {
        ObjectPath.TrimStartAndEndInline();

        // Common generated/test shorthand mistake: UObject lives in CoreUObject,
        // not Engine. UE silently leaves the pin type less compatible if this is
        // not normalized, and standard macros such as IsValid can reject links.
        if (ObjectPath == TEXT("/Script/Engine.Object") ||
            ObjectPath == TEXT("/Script/Engine.UObject") ||
            ObjectPath == TEXT("/Script/CoreUObject.UObject"))
        {
            return TEXT("/Script/CoreUObject.Object");
        }

        return ObjectPath;
    }

    static UObject* ResolveTypeObjectFromJson(const TSharedPtr<FJsonObject>& Obj)
    {
        const FString ObjectPath = NormalizeTypeObjectPathForUE427(GetTypeObjectPathFromJson(Obj));
        if (ObjectPath.IsEmpty())
        {
            return nullptr;
        }

        UObject* TypeObject = FindObject<UObject>(ANY_PACKAGE, *ObjectPath);
        if (!TypeObject)
        {
            TypeObject = LoadObject<UObject>(nullptr, *ObjectPath);
        }
        return TypeObject;
    }


    static bool HasNativeMakeBreakMetadata(UScriptStruct* StructType)
    {
        if (!StructType)
        {
            return false;
        }

        // UE4 native structs such as FVector/FTransform/FRotator often expose
        // Blueprint make/break through dedicated UKismetMathLibrary functions.
        // Creating generic K2Node_MakeStruct/K2Node_BreakStruct for them can compile
        // as "not a BlueprintType" or "use specialized break function".
        return StructType->HasMetaData(TEXT("HasNativeMake")) ||
               StructType->HasMetaData(TEXT("HasNativeBreak"));
    }

    static bool ShouldGuardGenericStructMakeBreak(UScriptStruct* StructType)
    {
        if (!StructType)
        {
            return true;
        }

        if (HasNativeMakeBreakMetadata(StructType))
        {
            return true;
        }

        const FString StructPath = StructType->GetPathName();
        return StructPath.Contains(TEXT("/Script/CoreUObject.Vector")) ||
               StructPath.Contains(TEXT("/Script/CoreUObject.Vector2D")) ||
               StructPath.Contains(TEXT("/Script/CoreUObject.Vector4")) ||
               StructPath.Contains(TEXT("/Script/CoreUObject.Rotator")) ||
               StructPath.Contains(TEXT("/Script/CoreUObject.Transform")) ||
               StructPath.Contains(TEXT("/Script/CoreUObject.Quat")) ||
               StructPath.Contains(TEXT("/Script/CoreUObject.LinearColor"));
    }

    static bool IsExecLikePinName(const FString& PinName)
    {
        const FString Lower = PinName.ToLower().Replace(TEXT("_"), TEXT(""));
        return Lower.IsEmpty() ||
               Lower == TEXT("then") || Lower == TEXT("execute") || Lower == TEXT("exec") ||
               Lower == TEXT("completed") || Lower == TEXT("complete") ||
               Lower == TEXT("loopbody") || Lower == TEXT("body") ||
               Lower == TEXT("true") || Lower == TEXT("false") ||
               Lower == TEXT("rowfound") || Lower == TEXT("rownotfound") ||
               Lower == TEXT("update") || Lower == TEXT("finished") ||
               Lower == TEXT("notvalid") || Lower == TEXT("isvalid");
    }

    static bool ValidateFunctionPatchSafety(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& ActionObj, FString& Report)
    {
        if (!ActionObj.IsValid())
        {
            return false;
        }

        const FString FunctionName = GetActionFunctionName(ActionObj);
        const FString ActionType = GetStringFieldSafe(ActionObj, TEXT("type"));
        bool bOk = true;

        auto ValidateDeclarationArray = [&](const TCHAR* FieldName, bool bLocal)
        {
            const TArray<TSharedPtr<FJsonValue>>* Declarations = GetArrayFieldSafe(ActionObj, FieldName);
            if (!Declarations) return;
            for (const TSharedPtr<FJsonValue>& Value : *Declarations)
            {
                const TSharedPtr<FJsonObject> Declaration = Value.IsValid() ? Value->AsObject() : nullptr;
                if (!Declaration.IsValid()) continue;
                const FEdGraphPinType PinType = MakePinTypeFromJson(Declaration);
                const bool bHasDefault = Declaration->HasField(TEXT("default")) || Declaration->HasField(TEXT("default_value"));
                const bool bUnsupportedFlags = PinType.bIsReference || PinType.bIsConst ||
                    GetBoolFieldSafe(Declaration, TEXT("reference"), false) ||
                    GetBoolFieldSafe(Declaration, TEXT("ref"), false) ||
                    GetBoolFieldSafe(Declaration, TEXT("const"), false) ||
                    GetBoolFieldSafe(Declaration, TEXT("return_by_reference"), false) ||
                    GetBoolFieldSafe(Declaration, TEXT("out"), false);
                if (bUnsupportedFlags)
                {
                    AppendLine(Report, FString::Printf(TEXT("N2C_PREFLIGHT_GUARD|code=unsupported_parameter_flags|field=%s|name=%s"), FieldName, *GetStringFieldSafe(Declaration, TEXT("name"))));
                    bOk = false;
                }
                if (bHasDefault)
                {
                    const bool bReferenceDefault = PinType.ContainerType != EPinContainerType::None ||
                        PinType.PinCategory == UEdGraphSchema_K2::PC_Object ||
                        PinType.PinCategory == UEdGraphSchema_K2::PC_Class ||
                        PinType.PinCategory == UEdGraphSchema_K2::PC_SoftObject ||
                        PinType.PinCategory == UEdGraphSchema_K2::PC_SoftClass;
                    const TCHAR* Code = bReferenceDefault ? TEXT("unsupported_reference_default") :
                        (bLocal ? TEXT("unsupported_local_default") : TEXT("unsupported_parameter_default"));
                    AppendLine(Report, FString::Printf(TEXT("N2C_PREFLIGHT_GUARD|code=%s|field=%s|name=%s"), Code, FieldName, *GetStringFieldSafe(Declaration, TEXT("name"))));
                    bOk = false;
                }
            }
        };
        ValidateDeclarationArray(TEXT("inputs"), false);
        ValidateDeclarationArray(TEXT("outputs"), false);
        ValidateDeclarationArray(TEXT("parameters"), false);
        ValidateDeclarationArray(TEXT("local_variables"), true);
        ValidateDeclarationArray(TEXT("locals"), true);

        const TArray<TSharedPtr<FJsonValue>>* Nodes = GetArrayFieldSafe(ActionObj, TEXT("nodes"));
        TSet<FString> EntryIds;
        TSet<FString> ReturnIds;
        if (Nodes)
        {
            for (const TSharedPtr<FJsonValue>& Value : *Nodes)
            {
                const TSharedPtr<FJsonObject> NodeObj = Value.IsValid() ? Value->AsObject() : nullptr;
                if (!NodeObj.IsValid())
                {
                    continue;
                }

                const FString NodeId = GetStringFieldSafe(NodeObj, TEXT("id"));
                FString Type = GetStringFieldSafe(NodeObj, TEXT("type"), GetStringFieldSafe(NodeObj, TEXT("class")));
                const FString Lower = Type.ToLower();
                if (Lower == TEXT("entry") || Lower.Contains(TEXT("functionentry")))
                {
                    EntryIds.Add(NodeId);
                }
                if (Lower == TEXT("return") || Lower.Contains(TEXT("functionresult")))
                {
                    ReturnIds.Add(NodeId);
                }

                const bool bGenericMake = Lower == TEXT("makestruct") || Lower == TEXT("make_struct") || Lower.Contains(TEXT("k2node_makestruct"));
                const bool bGenericBreak = Lower == TEXT("breakstruct") || Lower == TEXT("break_struct") || Lower.Contains(TEXT("k2node_breakstruct"));
                if (bGenericMake || bGenericBreak)
                {
                    UScriptStruct* StructType = Cast<UScriptStruct>(ResolveTypeObjectFromJson(NodeObj));
                    if (!StructType)
                    {
                        AppendLine(Report, FString::Printf(TEXT("ERROR: function '%s' contains generic %s with unresolved struct type; rejected before graph mutation."), *FunctionName, bGenericMake ? TEXT("MakeStruct") : TEXT("BreakStruct")));
                        bOk = false;
                    }
                    else if (ShouldGuardGenericStructMakeBreak(StructType))
                    {
                        AppendLine(Report, FString::Printf(TEXT("ERROR: function '%s' contains unsupported generic %s for native struct '%s'; use the specialized Kismet make/break function. Rejected before graph mutation."), *FunctionName, bGenericMake ? TEXT("MakeStruct") : TEXT("BreakStruct"), *StructType->GetPathName()));
                        bOk = false;
                    }
                }
            }
        }

        if (!GetBoolFieldSafe(ActionObj, TEXT("allow_unconnected_internal_exec"), false))
        {
            TMultiMap<FString, FString> ExecAdjacency;
            auto ReadExecEdges = [&](const FString& FieldName, bool bAssumeExec)
            {
                const TArray<TSharedPtr<FJsonValue>>* Edges = GetArrayFieldSafe(ActionObj, FieldName);
                if (!Edges)
                {
                    return;
                }
                for (const TSharedPtr<FJsonValue>& EdgeValue : *Edges)
                {
                    const TSharedPtr<FJsonObject> EdgeObj = EdgeValue.IsValid() ? EdgeValue->AsObject() : nullptr;
                    if (!EdgeObj.IsValid())
                    {
                        continue;
                    }
                    const FString FromId = GetStringFieldSafe(EdgeObj, TEXT("from_node_id"), GetStringFieldSafe(EdgeObj, TEXT("from")));
                    const FString ToId = GetStringFieldSafe(EdgeObj, TEXT("to_node_id"), GetStringFieldSafe(EdgeObj, TEXT("to")));
                    const FString FromPin = GetStringFieldSafe(EdgeObj, TEXT("from_pin"), TEXT("Then"));
                    const FString ToPin = GetStringFieldSafe(EdgeObj, TEXT("to_pin"), TEXT("Execute"));
                    if (!FromId.IsEmpty() && !ToId.IsEmpty() && (bAssumeExec || (IsExecLikePinName(FromPin) && IsExecLikePinName(ToPin))))
                    {
                        ExecAdjacency.Add(FromId, ToId);
                    }
                }
            };
            ReadExecEdges(TEXT("exec_edges"), true);
            ReadExecEdges(TEXT("edges"), false);

            TArray<FString> Pending = EntryIds.Array();
            TSet<FString> Visited;
            bool bReachedReturn = false;
            while (Pending.Num() > 0 && !bReachedReturn)
            {
                const FString Current = Pending.Pop(false);
                if (Visited.Contains(Current))
                {
                    continue;
                }
                Visited.Add(Current);
                if (ReturnIds.Contains(Current))
                {
                    bReachedReturn = true;
                    break;
                }
                TArray<FString> Next;
                ExecAdjacency.MultiFind(Current, Next);
                Pending.Append(Next);
            }

            if (!bReachedReturn)
            {
                AppendLine(Report, FString::Printf(TEXT("ERROR: function '%s' has no required internal exec path from FunctionEntry.then to FunctionResult.execute. Rejected before apply."), *FunctionName));
                bOk = false;
            }
        }

        if (ActionType == TEXT("replace_function_body") || GetBoolFieldSafe(ActionObj, TEXT("replace_body"), false))
        {
            CountStaleSignaturePins(Blueprint, ActionObj, Report);
        }

        return bOk;
    }

    static FEdGraphPinType MakePinTypeFromJson(const TSharedPtr<FJsonObject>& Obj)
    {
        FEdGraphPinType PinType;
        PinType.PinCategory = UEdGraphSchema_K2::PC_Wildcard;

        FString Type = GetStringFieldSafe(Obj, TEXT("type"));
        if (Type.IsEmpty())
        {
            Type = GetStringFieldSafe(Obj, TEXT("category"));
        }
        if (Type.IsEmpty())
        {
            if (Obj.IsValid())
            {
                const TSharedPtr<FJsonObject>* PinTypeObj = nullptr;
                if (Obj->TryGetObjectField(TEXT("pin_type"), PinTypeObj) && PinTypeObj)
                {
                    Type = GetStringFieldSafe(*PinTypeObj, TEXT("category"));
                }
            }
        }
        Type.TrimStartAndEndInline();

        const FString Lower = Type.ToLower();
        if (Lower == TEXT("bool") || Lower == TEXT("boolean"))
        {
            PinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
        }
        else if (Lower == TEXT("name"))
        {
            PinType.PinCategory = UEdGraphSchema_K2::PC_Name;
        }
        else if (Lower == TEXT("string"))
        {
            PinType.PinCategory = UEdGraphSchema_K2::PC_String;
        }
        else if (Lower == TEXT("text"))
        {
            PinType.PinCategory = UEdGraphSchema_K2::PC_Text;
        }
        else if (Lower == TEXT("float") || Lower == TEXT("real") || Lower == TEXT("double"))
        {
            PinType.PinCategory = UEdGraphSchema_K2::PC_Float;
        }
        else if (Lower == TEXT("int") || Lower == TEXT("integer"))
        {
            PinType.PinCategory = UEdGraphSchema_K2::PC_Int;
        }
        else if (Lower == TEXT("byte") || Lower == TEXT("enum"))
        {
            PinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
        }
        else if (Lower == TEXT("object"))
        {
            PinType.PinCategory = UEdGraphSchema_K2::PC_Object;
        }
        else if (Lower == TEXT("class"))
        {
            PinType.PinCategory = UEdGraphSchema_K2::PC_Class;
        }
        else if (Lower == TEXT("struct"))
        {
            PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
        }
        else if (Lower == TEXT("delegate"))
        {
            PinType.PinCategory = UEdGraphSchema_K2::PC_Delegate;
        }
        else if (Lower == TEXT("multicastdelegate") || Lower == TEXT("mcdelegate") || Lower == TEXT("event_dispatcher"))
        {
            PinType.PinCategory = UEdGraphSchema_K2::PC_MCDelegate;
        }
        else if (Lower == TEXT("softobject") || Lower == TEXT("soft_object"))
        {
            PinType.PinCategory = UEdGraphSchema_K2::PC_SoftObject;
        }
        else if (Lower == TEXT("softclass") || Lower == TEXT("soft_class"))
        {
            PinType.PinCategory = UEdGraphSchema_K2::PC_SoftClass;
        }
        else if (Lower == TEXT("exec"))
        {
            PinType.PinCategory = UEdGraphSchema_K2::PC_Exec;
        }

        if (Obj.IsValid())
        {
            if (UObject* TypeObject = ResolveTypeObjectFromJson(Obj))
            {
                PinType.PinSubCategoryObject = TypeObject;
            }

            FString Container = GetStringFieldSafe(Obj, TEXT("container"));
            if (Container.IsEmpty())
            {
                Container = GetStringFieldSafe(Obj, TEXT("container_type"));
            }
            const FString ContainerLower = Container.ToLower();
            if (ContainerLower == TEXT("array"))
            {
                PinType.ContainerType = EPinContainerType::Array;
            }
            else if (ContainerLower == TEXT("set"))
            {
                PinType.ContainerType = EPinContainerType::Set;
            }
            else if (ContainerLower == TEXT("map"))
            {
                PinType.ContainerType = EPinContainerType::Map;
                const TSharedPtr<FJsonObject>* ValueTypeObj = nullptr;
                if (Obj->TryGetObjectField(TEXT("value_type"), ValueTypeObj) && ValueTypeObj && ValueTypeObj->IsValid())
                {
                    const FEdGraphPinType ValuePinType = MakePinTypeFromJson(*ValueTypeObj);
                    PinType.PinValueType.TerminalCategory = ValuePinType.PinCategory;
                    PinType.PinValueType.TerminalSubCategory = ValuePinType.PinSubCategory;
                    PinType.PinValueType.TerminalSubCategoryObject = ValuePinType.PinSubCategoryObject;
                }
            }
        }

        return PinType;
    }

    static UK2Node_FunctionEntry* FindOrCreateEntryNode(UEdGraph* Graph, const FString& FunctionName)
    {
        if (!Graph)
        {
            return nullptr;
        }
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
            {
                return Entry;
            }
        }

        FGraphNodeCreator<UK2Node_FunctionEntry> Creator(*Graph);
        UK2Node_FunctionEntry* Entry = Creator.CreateNode();
        Entry->FunctionReference.SetSelfMember(FName(*FunctionName));
        Entry->NodePosX = 0;
        Entry->NodePosY = 0;
        Creator.Finalize();
        return Entry;
    }

    static UK2Node_FunctionResult* FindOrCreateResultNode(UEdGraph* Graph, const FString& FunctionName)
    {
        if (!Graph)
        {
            return nullptr;
        }
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (UK2Node_FunctionResult* Result = Cast<UK2Node_FunctionResult>(Node))
            {
                return Result;
            }
        }

        FGraphNodeCreator<UK2Node_FunctionResult> Creator(*Graph);
        UK2Node_FunctionResult* Result = Creator.CreateNode();
        Result->FunctionReference.SetSelfMember(FName(*FunctionName));
        Result->NodePosX = 900;
        Result->NodePosY = 0;
        Creator.Finalize();
        return Result;
    }

    static UK2Node_FunctionResult* CreateAdditionalResultNode(UEdGraph* Graph, const FString& FunctionName, int32 PosX, int32 PosY)
    {
        if (!Graph)
        {
            return nullptr;
        }

        FGraphNodeCreator<UK2Node_FunctionResult> Creator(*Graph);
        UK2Node_FunctionResult* Result = Creator.CreateNode();
        Result->FunctionReference.SetSelfMember(FName(*FunctionName));
        Result->NodePosX = PosX;
        Result->NodePosY = PosY;
        Creator.Finalize();
        UK2Node_FunctionResult* SignatureSource = nullptr;
        for (UEdGraphNode* GraphNode : Graph->Nodes)
        {
            UK2Node_FunctionResult* Existing = Cast<UK2Node_FunctionResult>(GraphNode);
            if (Existing && Existing != Result) { SignatureSource = Existing; break; }
        }
        if (SignatureSource)
        {
            for (const TSharedPtr<FUserPinInfo>& UserPin : SignatureSource->UserDefinedPins)
            {
                if (UserPin.IsValid() && UserPin->DesiredPinDirection == EGPD_Input && !NodeHasLoosePin(Result, UserPin->PinName.ToString(), EGPD_Input))
                {
                    Result->CreateUserDefinedPin(UserPin->PinName, UserPin->PinType, EGPD_Input);
                }
            }
        }
        Result->ReconstructNode();
        return Result;
    }

    static void AddSignaturePins(UEdGraph* Graph, const TSharedPtr<FJsonObject>& ActionObj, const FString& FunctionName, FString& Report)
    {
        UK2Node_FunctionEntry* Entry = FindOrCreateEntryNode(Graph, FunctionName);
        UK2Node_FunctionResult* Result = FindOrCreateResultNode(Graph, FunctionName);

        if (!Entry || !Result)
        {
            AppendLine(Report, FString::Printf(TEXT("WARNING: function '%s' signature nodes could not be created."), *FunctionName));
            return;
        }

        Entry->Modify();
        Result->Modify();

        const TArray<TSharedPtr<FJsonValue>>* Inputs = GetArrayFieldSafe(ActionObj, TEXT("inputs"));
        if (Inputs)
        {
            for (const TSharedPtr<FJsonValue>& Value : *Inputs)
            {
                TSharedPtr<FJsonObject> PinObj = Value.IsValid() ? Value->AsObject() : nullptr;
                const FString Name = GetStringFieldSafe(PinObj, TEXT("name"));
                if (Name.IsEmpty())
                {
                    continue;
                }
                if (NodeHasLoosePin(Entry, Name, EGPD_Output))
                {
                    AppendLine(Report, FString::Printf(TEXT("Signature input already exists, skipped duplicate: %s.%s"), *FunctionName, *Name));
                    continue;
                }
                Entry->CreateUserDefinedPin(FName(*Name), MakePinTypeFromJson(PinObj), EGPD_Output);
                AppendLine(Report, FString::Printf(TEXT("Added signature input: %s.%s"), *FunctionName, *Name));
            }
        }

        const TArray<TSharedPtr<FJsonValue>>* Outputs = GetArrayFieldSafe(ActionObj, TEXT("outputs"));
        if (Outputs)
        {
            for (const TSharedPtr<FJsonValue>& Value : *Outputs)
            {
                TSharedPtr<FJsonObject> PinObj = Value.IsValid() ? Value->AsObject() : nullptr;
                const FString Name = GetStringFieldSafe(PinObj, TEXT("name"));
                if (Name.IsEmpty())
                {
                    continue;
                }
                if (NodeHasLoosePin(Result, Name, EGPD_Input))
                {
                    AppendLine(Report, FString::Printf(TEXT("Signature output already exists, skipped duplicate: %s.%s"), *FunctionName, *Name));
                    continue;
                }
                Result->CreateUserDefinedPin(FName(*Name), MakePinTypeFromJson(PinObj), EGPD_Input);
                AppendLine(Report, FString::Printf(TEXT("Added signature output: %s.%s"), *FunctionName, *Name));
            }
        }

        Entry->ReconstructNode();
        Result->ReconstructNode();
    }

    static bool FunctionGraphHasLocalVariable(const UBlueprint* Blueprint, const UEdGraph* Graph, const FString& VariableName)
    {
        if (!Blueprint || !Graph || VariableName.IsEmpty())
        {
            return false;
        }

        UK2Node_FunctionEntry* FunctionEntry = nullptr;
        return FBlueprintEditorUtils::FindLocalVariable(
            Blueprint,
            Graph,
            FName(*VariableName),
            &FunctionEntry
        ) != nullptr;
    }

    static void AddLocalVariables(UBlueprint* Blueprint, UEdGraph* Graph, const TSharedPtr<FJsonObject>& ActionObj, const FString& FunctionName, FString& Report)
    {
        if (!Blueprint || !Graph || !ActionObj.IsValid())
        {
            return;
        }

        const TArray<TSharedPtr<FJsonValue>>* Locals = GetArrayFieldSafe(ActionObj, TEXT("local_variables"));
        if (!Locals)
        {
            Locals = GetArrayFieldSafe(ActionObj, TEXT("locals"));
        }

        if (!Locals)
        {
            return;
        }

        for (const TSharedPtr<FJsonValue>& Value : *Locals)
        {
            TSharedPtr<FJsonObject> VarObj = Value.IsValid() ? Value->AsObject() : nullptr;
            const FString Name = GetStringFieldSafe(VarObj, TEXT("name"));
            if (Name.IsEmpty())
            {
                AppendLine(Report, FString::Printf(TEXT("WARNING: function '%s' contains a local variable without name; skipped."), *FunctionName));
                continue;
            }

            if (FunctionGraphHasLocalVariable(Blueprint, Graph, Name))
            {
                AppendLine(Report, FString::Printf(TEXT("Local variable already exists, skipped duplicate: %s.%s"), *FunctionName, *Name));
                continue;
            }

            if (!FBlueprintEditorUtils::DoesSupportLocalVariables(Graph))
            {
                AppendLine(Report, FString::Printf(TEXT("WARNING: graph does not support local variables, skipped: %s.%s"), *FunctionName, *Name));
                continue;
            }

            const bool bAddedLocalVariable = FBlueprintEditorUtils::AddLocalVariable(Blueprint, Graph, FName(*Name), MakePinTypeFromJson(VarObj), FString());
            if (bAddedLocalVariable)
            {
                AppendLine(Report, FString::Printf(TEXT("Added local variable: %s.%s"), *FunctionName, *Name));
            }
            else
            {
                AppendLine(Report, FString::Printf(TEXT("WARNING: failed to add local variable: %s.%s"), *FunctionName, *Name));
            }
        }
    }


    static FString JsonValueToPinDefaultString(const TSharedPtr<FJsonValue>& Value);

    static bool BlueprintHasMemberVariable(UBlueprint* Blueprint, const FString& VariableName)
    {
        return Blueprint && !VariableName.IsEmpty() && FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, FName(*VariableName)) != INDEX_NONE;
    }

    static FString GetVariableDefaultValue(const TSharedPtr<FJsonObject>& VarObj)
    {
        if (!VarObj.IsValid())
        {
            return TEXT("");
        }

        FString DefaultString;
        if (VarObj->TryGetStringField(TEXT("default_value"), DefaultString))
        {
            return DefaultString;
        }

        const TSharedPtr<FJsonValue>* Value = VarObj->Values.Find(TEXT("default"));
        if (Value && Value->IsValid())
        {
            return JsonValueToPinDefaultString(*Value);
        }
        return TEXT("");
    }

    static bool IsLooseNumericString(const FString& Value)
    {
        if (Value.IsEmpty())
        {
            return false;
        }

        for (int32 Index = 0; Index < Value.Len(); ++Index)
        {
            const TCHAR Ch = Value[Index];
            if (!FChar::IsDigit(Ch) && Ch != TEXT('.') && Ch != TEXT('-') && Ch != TEXT('+'))
            {
                return false;
            }
        }
        return true;
    }

    static FString NormalizeMemberVariableDefaultValue(const FEdGraphPinType& PinType, FString DefaultValue)
    {
        DefaultValue.TrimStartAndEndInline();
        if (DefaultValue.IsEmpty())
        {
            return DefaultValue;
        }

        if (PinType.PinCategory == UEdGraphSchema_K2::PC_Int || PinType.PinCategory == UEdGraphSchema_K2::PC_Byte)
        {
            if (IsLooseNumericString(DefaultValue))
            {
                return FString::FromInt(FMath::RoundToInt(FCString::Atod(*DefaultValue)));
            }
        }
        else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean)
        {
            const FString Lower = DefaultValue.ToLower();
            if (Lower == TEXT("1") || Lower == TEXT("true") || Lower == TEXT("yes"))
            {
                return TEXT("true");
            }
            if (Lower == TEXT("0") || Lower == TEXT("false") || Lower == TEXT("no"))
            {
                return TEXT("false");
            }
        }

        return DefaultValue;
    }

    static FString MakeK2FloatTriple(float A, float B, float C)
    {
        return FString::Printf(
            TEXT("%s,%s,%s"),
            *FString::SanitizeFloat(A),
            *FString::SanitizeFloat(B),
            *FString::SanitizeFloat(C));
    }

    static bool TryFormatK2CustomStructDefault(UScriptStruct* StructType, const void* StructMemory, FString& OutDefault)
    {
        if (!StructType || !StructMemory)
        {
            return false;
        }

        if (StructType == TBaseStructure<FVector>::Get())
        {
            const FVector& Vector = *static_cast<const FVector*>(StructMemory);
            OutDefault = MakeK2FloatTriple(Vector.X, Vector.Y, Vector.Z);
            return FDefaultValueHelper::IsStringValidVector(OutDefault);
        }

        if (StructType == TBaseStructure<FRotator>::Get())
        {
            const FRotator& Rotator = *static_cast<const FRotator*>(StructMemory);
            // Blueprint pins/defaults use FDefaultValueHelper's CSV contract, not
            // UScriptStruct::ExportText's named "(Pitch=...,Yaw=...,Roll=...)" form.
            OutDefault = MakeK2FloatTriple(Rotator.Pitch, Rotator.Yaw, Rotator.Roll);
            return FDefaultValueHelper::IsStringValidRotator(OutDefault);
        }

        return false;
    }

    static bool TryCanonicalizeK2CustomStructDefault(
        UScriptStruct* StructType,
        const FString& InputDefault,
        UObject* Owner,
        FString& OutDefault)
    {
        if (!StructType || InputDefault.IsEmpty())
        {
            return false;
        }

        if (StructType == TBaseStructure<FVector>::Get())
        {
            FVector ParsedVector = FVector::ZeroVector;
            if (FDefaultValueHelper::ParseVector(InputDefault, ParsedVector))
            {
                OutDefault = MakeK2FloatTriple(ParsedVector.X, ParsedVector.Y, ParsedVector.Z);
                return true;
            }
        }
        else if (StructType == TBaseStructure<FRotator>::Get())
        {
            FRotator ParsedRotator = FRotator::ZeroRotator;
            if (FDefaultValueHelper::ParseRotator(InputDefault, ParsedRotator))
            {
                OutDefault = MakeK2FloatTriple(ParsedRotator.Pitch, ParsedRotator.Yaw, ParsedRotator.Roll);
                return true;
            }
        }
        else
        {
            return false;
        }

        FStructOnScope StructData(StructType);
        FOutputDeviceNull NullOutput;
        const TCHAR* ParseEnd = StructType->ImportText(
            *InputDefault,
            StructData.GetStructMemory(),
            Owner,
            PPF_SerializedAsImportText,
            &NullOutput,
            TEXT("N2CDefault"));
        if (!ParseEnd)
        {
            return false;
        }
        while (*ParseEnd && FChar::IsWhitespace(*ParseEnd))
        {
            ++ParseEnd;
        }
        return *ParseEnd == TEXT('\0') && TryFormatK2CustomStructDefault(StructType, StructData.GetStructMemory(), OutDefault);
    }

    static bool CanonicalizeStructMemberVariableDefaultValue(
        UBlueprint* Blueprint,
        const FString& VariableName,
        const FEdGraphPinType& PinType,
        bool bHasDefaultValue,
        FString& InOutDefaultValue,
        FString& Report)
    {
        if (!bHasDefaultValue || InOutDefaultValue.IsEmpty() ||
            PinType.ContainerType != EPinContainerType::None ||
            PinType.PinCategory != UEdGraphSchema_K2::PC_Struct)
        {
            return true;
        }

        UScriptStruct* StructType = Cast<UScriptStruct>(PinType.PinSubCategoryObject.Get());
        if (!StructType)
        {
            AppendLine(Report, FString::Printf(
                TEXT("N2C_PREFLIGHT_GUARD|code=member_default_struct_type_missing|variable=%s"),
                *VariableName));
            AppendLine(Report, FString::Printf(
                TEXT("ERROR: struct member variable '%s' has a default but no resolvable UScriptStruct type."),
                *VariableName));
            return false;
        }

        FStructOnScope StructData(StructType);
        FOutputDeviceNull NullOutput;
        const TCHAR* ParseEnd = StructType->ImportText(
            *InOutDefaultValue,
            StructData.GetStructMemory(),
            Blueprint,
            PPF_SerializedAsImportText,
            &NullOutput,
            VariableName);
        if (!ParseEnd)
        {
            AppendLine(Report, FString::Printf(
                TEXT("N2C_PREFLIGHT_GUARD|code=member_default_import_text_invalid|variable=%s|struct=%s"),
                *VariableName,
                *StructType->GetPathName()));
            AppendLine(Report, FString::Printf(
                TEXT("ERROR: member variable '%s' default '%s' is not valid UE4.27 ImportText for %s. Rejected before mutation."),
                *VariableName,
                *InOutDefaultValue,
                *StructType->GetPathName()));
            return false;
        }

        while (*ParseEnd && FChar::IsWhitespace(*ParseEnd))
        {
            ++ParseEnd;
        }
        if (*ParseEnd != TEXT('\0'))
        {
            AppendLine(Report, FString::Printf(
                TEXT("N2C_PREFLIGHT_GUARD|code=member_default_import_text_trailing_data|variable=%s|struct=%s"),
                *VariableName,
                *StructType->GetPathName()));
            AppendLine(Report, FString::Printf(
                TEXT("ERROR: member variable '%s' default contains trailing data after valid %s ImportText. Rejected before mutation."),
                *VariableName,
                *StructType->GetPathName()));
            return false;
        }

        FString CanonicalDefault;
        if (!TryFormatK2CustomStructDefault(StructType, StructData.GetStructMemory(), CanonicalDefault))
        {
            StructType->ExportText(
                CanonicalDefault,
                StructData.GetStructMemory(),
                StructData.GetStructMemory(),
                Blueprint,
                PPF_SerializedAsImportText,
                nullptr);
        }
        if (CanonicalDefault.IsEmpty())
        {
            AppendLine(Report, FString::Printf(
                TEXT("N2C_PREFLIGHT_GUARD|code=member_default_export_text_failed|variable=%s|struct=%s"),
                *VariableName,
                *StructType->GetPathName()));
            AppendLine(Report, FString::Printf(
                TEXT("ERROR: member variable '%s' default could not be canonicalized for %s. Rejected before mutation."),
                *VariableName,
                *StructType->GetPathName()));
            return false;
        }

        if (CanonicalDefault != InOutDefaultValue)
        {
            AppendLine(Report, FString::Printf(
                TEXT("INFO: canonicalized struct member default: %s = %s"),
                *VariableName,
                *CanonicalDefault));
            InOutDefaultValue = CanonicalDefault;
        }
        return true;
    }

    static FBPVariableDescription* FindMemberVariableDescription(UBlueprint* Blueprint, const FString& Name)
    {
        if (!Blueprint)
        {
            return nullptr;
        }
        for (FBPVariableDescription& Var : Blueprint->NewVariables)
        {
            if (Var.VarName.ToString() == Name)
            {
                return &Var;
            }
        }
        return nullptr;
    }

    static bool ApplyMemberVariableDefaultValue(UBlueprint* Blueprint, const FString& Name, const FString& DefaultValue, bool bHasDefaultValue, bool bDryRun, FString& Report)
    {
        if (!bHasDefaultValue)
        {
            return true;
        }

        if (bDryRun)
        {
            AppendLine(Report, FString::Printf(TEXT("DRY RUN CHANGE: variable default: %s = %s"), *Name, *DefaultValue));
            return true;
        }

        FBPVariableDescription* VarDesc = FindMemberVariableDescription(Blueprint, Name);
        if (!VarDesc)
        {
            AppendLine(Report, FString::Printf(TEXT("ERROR: variable default skipped because variable was not found: %s"), *Name));
            return false;
        }

        if (VarDesc->DefaultValue == DefaultValue)
        {
            AppendLine(Report, FString::Printf(TEXT("Variable default already matches, skipped: %s"), *Name));
            return true;
        }

        if (VarDesc->VarType.PinCategory == UEdGraphSchema_K2::PC_Text && Blueprint->GeneratedClass)
        {
            UObject* CDO = Blueprint->GeneratedClass->GetDefaultObject(false);
            FTextProperty* TextProperty = FindFProperty<FTextProperty>(Blueprint->GeneratedClass, FName(*Name));
            if (CDO && TextProperty)
            {
                const FText& CurrentText = TextProperty->GetPropertyValue_InContainer(CDO);
                const FString* CurrentSource = FTextInspector::GetSourceString(CurrentText);
                if ((CurrentSource && *CurrentSource == DefaultValue) || CurrentText.ToString() == DefaultValue)
                {
                    AppendLine(Report, FString::Printf(TEXT("Variable text default already matches, skipped: %s"), *Name));
                    return true;
                }
            }
        }

        // FBPVariableDescription is the UE4.27 editor-side input used before structural
        // compilation. After compile, the generated-class CDO is authoritative and the
        // descriptor string may be cleared; persistence tests must read the CDO/export.
        VarDesc->DefaultValue = DefaultValue;
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
        AppendLine(Report, FString::Printf(TEXT("CHANGE: variable default: %s = %s"), *Name, *DefaultValue));
        return true;
    }

    static bool TryGetBoolOption(const TSharedPtr<FJsonObject>& Obj, const FString& FieldName, bool& OutValue)
    {
        if (TryGetBoolFieldSafe(Obj, FieldName, OutValue))
        {
            return true;
        }

        const TSharedPtr<FJsonObject>* FlagsObj = nullptr;
        if (Obj.IsValid() && Obj->TryGetObjectField(TEXT("flags"), FlagsObj) && FlagsObj && FlagsObj->IsValid())
        {
            return (*FlagsObj)->TryGetBoolField(FieldName, OutValue);
        }
        return false;
    }

    static void SetOrRemoveVariableMetaData(FBPVariableDescription& Var, const FName& Key, bool bEnabled)
    {
        for (int32 Index = Var.MetaDataArray.Num() - 1; Index >= 0; --Index)
        {
            if (Var.MetaDataArray[Index].DataKey == Key)
            {
                Var.MetaDataArray.RemoveAt(Index);
            }
        }

        if (bEnabled)
        {
            FBPVariableMetaDataEntry Entry;
            Entry.DataKey = Key;
            Entry.DataValue = TEXT("true");
            Var.MetaDataArray.Add(Entry);
        }
    }

    static bool ApplyMemberVariableOptions(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& VarObj, const FString& Name, bool bDryRun, FString& Report)
    {
        bool bHasAnyOption = false;
        bool bValue = false;
        bHasAnyOption |= TryGetBoolOption(VarObj, TEXT("instance_editable"), bValue);
        bHasAnyOption |= TryGetBoolOption(VarObj, TEXT("editable"), bValue);
        bHasAnyOption |= TryGetBoolOption(VarObj, TEXT("blueprint_read_only"), bValue);
        bHasAnyOption |= TryGetBoolOption(VarObj, TEXT("save_game"), bValue);
        bHasAnyOption |= TryGetBoolOption(VarObj, TEXT("transient"), bValue);
        bHasAnyOption |= TryGetBoolOption(VarObj, TEXT("expose_on_spawn"), bValue);
        bHasAnyOption |= TryGetBoolOption(VarObj, TEXT("private"), bValue);
        if (!bHasAnyOption)
        {
            return true;
        }

        if (bDryRun)
        {
            AppendLine(Report, FString::Printf(TEXT("DRY RUN CHANGE: variable options: %s"), *Name));
            return true;
        }

        FBPVariableDescription* VarDesc = FindMemberVariableDescription(Blueprint, Name);
        if (!VarDesc)
        {
            AppendLine(Report, FString::Printf(TEXT("WARNING: variable options skipped because variable was not found: %s"), *Name));
            return false;
        }

        if (TryGetBoolOption(VarObj, TEXT("instance_editable"), bValue) || TryGetBoolOption(VarObj, TEXT("editable"), bValue))
        {
            if (bValue) { VarDesc->PropertyFlags |= CPF_Edit; }
            else { VarDesc->PropertyFlags &= ~CPF_Edit; }
        }
        if (TryGetBoolOption(VarObj, TEXT("blueprint_read_only"), bValue))
        {
            if (bValue) { VarDesc->PropertyFlags |= CPF_BlueprintReadOnly; }
            else { VarDesc->PropertyFlags &= ~CPF_BlueprintReadOnly; }
        }
        if (TryGetBoolOption(VarObj, TEXT("save_game"), bValue))
        {
            if (bValue) { VarDesc->PropertyFlags |= CPF_SaveGame; }
            else { VarDesc->PropertyFlags &= ~CPF_SaveGame; }
        }
        if (TryGetBoolOption(VarObj, TEXT("transient"), bValue))
        {
            if (bValue) { VarDesc->PropertyFlags |= CPF_Transient; }
            else { VarDesc->PropertyFlags &= ~CPF_Transient; }
        }
        if (TryGetBoolOption(VarObj, TEXT("expose_on_spawn"), bValue))
        {
            SetOrRemoveVariableMetaData(*VarDesc, TEXT("ExposeOnSpawn"), bValue);
        }
        if (TryGetBoolOption(VarObj, TEXT("private"), bValue))
        {
            SetOrRemoveVariableMetaData(*VarDesc, TEXT("BlueprintPrivate"), bValue);
        }

        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
        AppendLine(Report, FString::Printf(TEXT("CHANGE: variable options: %s"), *Name));
        return true;
    }

    static bool AddMemberVariables(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Obj, bool bDryRun, FString& Report)
    {
        if (!Blueprint || !Obj.IsValid())
        {
            AppendLine(Report, TEXT("ERROR: add_member_variables received invalid Blueprint or JSON object."));
            return false;
        }

        const TArray<TSharedPtr<FJsonValue>>* Variables = GetMemberVariablesArray(Obj);
        if (!Variables || Variables->Num() == 0)
        {
            AppendLine(Report, TEXT("ERROR: add_member_variables requires variables[] or member_variables[]."));
            return false;
        }

        bool bOk = true;
        for (const TSharedPtr<FJsonValue>& Value : *Variables)
        {
            TSharedPtr<FJsonObject> VarObj = Value.IsValid() ? Value->AsObject() : nullptr;
            const FString Name = GetStringFieldSafe(VarObj, TEXT("name"));
            const FString Category = GetStringFieldSafe(VarObj, TEXT("category"), GetStringFieldSafe(VarObj, TEXT("folder")));
            const FEdGraphPinType PinType = MakePinTypeFromJson(VarObj);
            FString DefaultValue = NormalizeMemberVariableDefaultValue(PinType, GetVariableDefaultValue(VarObj));
            const bool bHasDefaultValue = VarObj.IsValid() && (VarObj->HasField(TEXT("default_value")) || VarObj->HasField(TEXT("default")));

            if (!IsSafeBlueprintIdentifier(Name))
            {
                AppendLine(Report, FString::Printf(TEXT("ERROR: invalid Blueprint member variable name '%s'."), *Name));
                bOk = false;
                continue;
            }

            if (!CanonicalizeStructMemberVariableDefaultValue(Blueprint, Name, PinType, bHasDefaultValue, DefaultValue, Report))
            {
                bOk = false;
                continue;
            }

            const bool bExplicitEnumBackedByte = IsExplicitEnumBackedByteDeclaration(VarObj);
            FString UnsafePinReason;
            if (!IsPinTypeSafeForBlueprintMemberImport(PinType, bExplicitEnumBackedByte, UnsafePinReason))
            {
                if (bExplicitEnumBackedByte && PinType.PinCategory == UEdGraphSchema_K2::PC_Byte)
                {
                    AppendLine(Report, FString::Printf(TEXT("N2C_PREFLIGHT_GUARD|code=enum_member_type_unresolved|variable=%s"), *Name));
                    AppendLine(Report, FString::Printf(TEXT("ERROR: enum-backed member variable '%s' has no resolvable UEnum subtype: %s."), *Name, *UnsafePinReason));
                    bOk = false;
                }
                else
                {
                    AppendLine(Report, FString::Printf(TEXT("WARNING: unsupported_but_safe_skipped member variable '%s': %s."), *Name, *UnsafePinReason));
                }
                continue;
            }

            if (BlueprintHasMemberVariable(Blueprint, Name))
            {
                AppendLine(Report, FString::Printf(TEXT("Variable already exists, skipped duplicate: %s"), *Name));
                bOk &= ApplyMemberVariableDefaultValue(Blueprint, Name, DefaultValue, bHasDefaultValue, bDryRun, Report);
                if (!Category.IsEmpty())
                {
                    if (bDryRun)
                    {
                        AppendLine(Report, FString::Printf(TEXT("DRY RUN CHANGE: variable category: %s -> %s"), *Name, *Category));
                    }
                    else
                    {
                        FBlueprintEditorUtils::SetBlueprintVariableCategory(Blueprint, FName(*Name), nullptr, FText::FromString(Category), true);
                        AppendLine(Report, FString::Printf(TEXT("CHANGE: variable category: %s -> %s"), *Name, *Category));
                    }
                }
                bOk &= ApplyMemberVariableOptions(Blueprint, VarObj, Name, bDryRun, Report);
                continue;
            }

            if (bDryRun)
            {
                AppendLine(Report, FString::Printf(TEXT("DRY RUN NEW: variable: %s"), *Name));
                continue;
            }

            UE_LOG(LogNodeToCode, Display, TEXT("N2C patch apply trace: adding member variable '%s' category='%s' container=%d"), *Name, *Category, static_cast<int32>(PinType.ContainerType));
            const bool bAdded = FBlueprintEditorUtils::AddMemberVariable(Blueprint, FName(*Name), PinType, DefaultValue);
            if (bAdded)
            {
                AppendLine(Report, FString::Printf(TEXT("NEW: variable: %s"), *Name));
                if (!Category.IsEmpty())
                {
                    FBlueprintEditorUtils::SetBlueprintVariableCategory(Blueprint, FName(*Name), nullptr, FText::FromString(Category), true);
                    AppendLine(Report, FString::Printf(TEXT("CHANGE: variable category: %s -> %s"), *Name, *Category));
                }
                bOk &= ApplyMemberVariableOptions(Blueprint, VarObj, Name, bDryRun, Report);
            }
            else
            {
                AppendLine(Report, FString::Printf(TEXT("ERROR: failed to add member variable: %s"), *Name));
                bOk = false;
            }
        }

        return bOk;
    }

    static int32 ReplaceSelfFunctionCallReferences(UBlueprint* Blueprint, const FString& OldName, const FString& NewName, FString& Report)
    {
        if (!Blueprint || OldName.IsEmpty() || NewName.IsEmpty() || OldName == NewName)
        {
            return 0;
        }

        TArray<UEdGraph*> GraphsToScan;
        GraphsToScan.Append(Blueprint->FunctionGraphs);
        GraphsToScan.Append(Blueprint->UbergraphPages);
        GraphsToScan.Append(Blueprint->MacroGraphs);

        int32 UpdatedCount = 0;
        const FName OldFName(*OldName);
        const FName NewFName(*NewName);

        for (UEdGraph* ScanGraph : GraphsToScan)
        {
            if (!ScanGraph)
            {
                continue;
            }

            for (UEdGraphNode* Node : ScanGraph->Nodes)
            {
                UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node);
                if (!Call)
                {
                    continue;
                }

                if (Call->FunctionReference.GetMemberName() == OldFName)
                {
                    Call->Modify();
                    Call->FunctionReference.SetSelfMember(NewFName);
                    Call->ReconstructNode();
                    ++UpdatedCount;
                }
            }
        }

        if (UpdatedCount > 0)
        {
            AppendLine(Report, FString::Printf(TEXT("Updated self function call references: %s -> %s (%d node(s))."), *OldName, *NewName, UpdatedCount));
        }
        else
        {
            AppendLine(Report, FString::Printf(TEXT("No self function call references required update for: %s -> %s."), *OldName, *NewName));
        }

        return UpdatedCount;
    }


    static UEdGraph* FindTargetGraphForPatch(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& ActionObj);


    static void CollectNodeMatchStrings(UEdGraphNode* Node, TArray<FString>& OutStrings)
    {
        if (!Node)
        {
            return;
        }

        OutStrings.Add(Node->GetName());
        OutStrings.Add(Node->GetClass() ? Node->GetClass()->GetName() : TEXT(""));
        OutStrings.Add(Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
        OutStrings.Add(Node->GetNodeTitle(ENodeTitleType::MenuTitle).ToString());
        OutStrings.Add(Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());

        if (FNameProperty* NameProp = FindFProperty<FNameProperty>(Node->GetClass(), TEXT("CustomFunctionName")))
        {
            OutStrings.Add(NameProp->GetPropertyValue_InContainer(Node).ToString());
        }
        if (FNameProperty* NameProp = FindFProperty<FNameProperty>(Node->GetClass(), TEXT("EventSignatureName")))
        {
            OutStrings.Add(NameProp->GetPropertyValue_InContainer(Node).ToString());
        }
        if (FStrProperty* StrProp = FindFProperty<FStrProperty>(Node->GetClass(), TEXT("FunctionName")))
        {
            OutStrings.Add(StrProp->GetPropertyValue_InContainer(Node));
        }
    }

    static bool NodeMatchesAnyName(UEdGraphNode* Node, const TArray<FString>& Names)
    {
        if (!Node || Names.Num() == 0)
        {
            return false;
        }

        TArray<FString> NodeStrings;
        CollectNodeMatchStrings(Node, NodeStrings);
        for (const FString& Wanted : Names)
        {
            if (Wanted.IsEmpty())
            {
                continue;
            }
            for (const FString& Candidate : NodeStrings)
            {
                if (Candidate.IsEmpty())
                {
                    continue;
                }
                if (Candidate == Wanted || Candidate.Contains(Wanted))
                {
                    return true;
                }
            }
        }
        return false;
    }

    static FString GetBestNodeLabel(UEdGraphNode* Node)
    {
        if (!Node)
        {
            return TEXT("<null>");
        }
        TArray<FString> NodeStrings;
        CollectNodeMatchStrings(Node, NodeStrings);
        for (const FString& Candidate : NodeStrings)
        {
            if (!Candidate.IsEmpty())
            {
                return Candidate;
            }
        }
        return Node->GetName();
    }

    static bool DeleteGraphNodesByName(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& ActionObj, bool bDryRun, FString& Report)
    {
        UEdGraph* Graph = FindTargetGraphForPatch(Blueprint, ActionObj);
        if (!Graph)
        {
            AppendLine(Report, TEXT("ERROR: target graph was not found for delete_graph_nodes."));
            return false;
        }

        TArray<FString> Names;
        GetStringArrayField(ActionObj, TEXT("names"), Names);
        if (Names.Num() == 0)
        {
            AppendLine(Report, TEXT("ERROR: delete_graph_nodes requires names[]."));
            return false;
        }

        TArray<UEdGraphNode*> NodesToDelete;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (NodeMatchesAnyName(Node, Names))
            {
                NodesToDelete.Add(Node);
            }
        }

        if (bDryRun)
        {
            AppendLine(Report, FString::Printf(TEXT("DRY RUN CHANGE: delete graph nodes: %s, matched=%d"), *Graph->GetName(), NodesToDelete.Num()));
            return true;
        }

        for (UEdGraphNode* Node : NodesToDelete)
        {
            if (!Node)
            {
                continue;
            }
            const FString Label = GetBestNodeLabel(Node);
            Node->Modify();
            BreakAllLinksOnNode(Node);
            FBlueprintEditorUtils::RemoveNode(Blueprint, Node, true);
            AppendLine(Report, FString::Printf(TEXT("CHANGE: deleted graph node: %s.%s"), *Graph->GetName(), *Label));
        }

        if (NodesToDelete.Num() == 0)
        {
            AppendLine(Report, FString::Printf(TEXT("WARNING: delete_graph_nodes matched no nodes in %s."), *Graph->GetName()));
        }

        Graph->NotifyGraphChanged();
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
        return true;
    }

    static bool RenameCustomEventNode(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& ActionObj, bool bDryRun, FString& Report)
    {
        UEdGraph* Graph = FindTargetGraphForPatch(Blueprint, ActionObj);
        if (!Graph)
        {
            AppendLine(Report, TEXT("ERROR: target graph was not found for rename_custom_event."));
            return false;
        }

        const FString OldName = GetStringFieldSafe(ActionObj, TEXT("old_name"), GetStringFieldSafe(ActionObj, TEXT("from")));
        const FString NewName = GetStringFieldSafe(ActionObj, TEXT("new_name"), GetStringFieldSafe(ActionObj, TEXT("to")));
        TArray<FString> OldNames;
        OldNames.Add(OldName);

        UEdGraphNode* TargetNode = nullptr;
        UEdGraphNode* ExistingNewNode = nullptr;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (!Node)
            {
                continue;
            }
            if (NodeMatchesAnyName(Node, OldNames))
            {
                TargetNode = Node;
            }
            TArray<FString> NewNames;
            NewNames.Add(NewName);
            if (NodeMatchesAnyName(Node, NewNames))
            {
                ExistingNewNode = Node;
            }
        }

        if (!TargetNode)
        {
            if (ExistingNewNode)
            {
                AppendLine(Report, FString::Printf(TEXT("CustomEvent rename already applied: %s"), *NewName));
                return true;
            }
            AppendLine(Report, FString::Printf(TEXT("WARNING: rename_custom_event source node not found: %s"), *OldName));
            return true;
        }

        if (bDryRun)
        {
            AppendLine(Report, FString::Printf(TEXT("DRY RUN CHANGE: CustomEvent rename: %s -> %s"), *OldName, *NewName));
            return true;
        }

        TargetNode->Modify();
        if (FNameProperty* NameProp = FindFProperty<FNameProperty>(TargetNode->GetClass(), TEXT("CustomFunctionName")))
        {
            NameProp->SetPropertyValue_InContainer(TargetNode, FName(*NewName));
        }
        else
        {
            AppendLine(Report, FString::Printf(TEXT("WARNING: node matched for rename_custom_event but has no CustomFunctionName property: %s"), *GetBestNodeLabel(TargetNode)));
        }
        TargetNode->ReconstructNode();
        Graph->NotifyGraphChanged();
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
        AppendLine(Report, FString::Printf(TEXT("CHANGE: CustomEvent rename: %s -> %s"), *OldName, *NewName));
        return true;
    }

    static bool RenameMemberVariable(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& ActionObj, bool bDryRun, FString& Report)
    {
        const FString OldName = GetStringFieldSafe(ActionObj, TEXT("old_variable_name"), GetStringFieldSafe(ActionObj, TEXT("from")));
        const FString NewName = GetStringFieldSafe(ActionObj, TEXT("new_variable_name"), GetStringFieldSafe(ActionObj, TEXT("to")));
        const FString Category = GetStringFieldSafe(ActionObj, TEXT("category"), GetStringFieldSafe(ActionObj, TEXT("folder")));

        if (!BlueprintHasMemberVariable(Blueprint, OldName))
        {
            if (BlueprintHasMemberVariable(Blueprint, NewName))
            {
                AppendLine(Report, FString::Printf(TEXT("Variable rename already applied: %s"), *NewName));
                return true;
            }
            AppendLine(Report, FString::Printf(TEXT("ERROR: rename_variable source variable not found: %s"), *OldName));
            return false;
        }

        if (BlueprintHasMemberVariable(Blueprint, NewName))
        {
            AppendLine(Report, FString::Printf(TEXT("ERROR: rename_variable destination already exists: %s"), *NewName));
            return false;
        }

        if (bDryRun)
        {
            AppendLine(Report, FString::Printf(TEXT("DRY RUN CHANGE: variable rename: %s -> %s"), *OldName, *NewName));
            return true;
        }

        FBlueprintEditorUtils::RenameMemberVariable(Blueprint, FName(*OldName), FName(*NewName));
        if (!Category.IsEmpty())
        {
            FBlueprintEditorUtils::SetBlueprintVariableCategory(Blueprint, FName(*NewName), nullptr, FText::FromString(Category), true);
        }
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
        AppendLine(Report, FString::Printf(TEXT("CHANGE: variable rename: %s -> %s"), *OldName, *NewName));
        if (!Category.IsEmpty())
        {
            AppendLine(Report, FString::Printf(TEXT("CHANGE: variable category: %s -> %s"), *NewName, *Category));
        }
        return true;
    }

    static void GetActionNameList(const TSharedPtr<FJsonObject>& ActionObj, const TCHAR* ArrayFieldA, const TCHAR* ArrayFieldB, const TCHAR* SingleFieldA, const TCHAR* SingleFieldB, TArray<FString>& OutNames)
    {
        OutNames.Reset();
        GetStringArrayField(ActionObj, ArrayFieldA, OutNames);
        GetStringArrayField(ActionObj, ArrayFieldB, OutNames);
        const FString SingleName = GetStringFieldSafe(ActionObj, SingleFieldA, GetStringFieldSafe(ActionObj, SingleFieldB));
        if (!SingleName.IsEmpty())
        {
            OutNames.Add(SingleName);
        }

        TSet<FString> Seen;
        for (int32 Index = OutNames.Num() - 1; Index >= 0; --Index)
        {
            const FString Clean = OutNames[Index].TrimStartAndEnd();
            if (Clean.IsEmpty() || Seen.Contains(Clean))
            {
                OutNames.RemoveAt(Index);
                continue;
            }
            OutNames[Index] = Clean;
            Seen.Add(Clean);
        }
    }

    static bool DeleteMemberVariablesAction(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& ActionObj, bool bDryRun, FString& Report)
    {
        TArray<FString> Names;
        GetActionNameList(ActionObj, TEXT("variable_names"), TEXT("names"), TEXT("variable_name"), TEXT("name"), Names);
        if (Names.Num() == 0)
        {
            AppendLine(Report, TEXT("ERROR: delete_variable requires variable_name/name/variable_names[]."));
            return false;
        }

        bool bChanged = false;
        for (const FString& Name : Names)
        {
            if (!IsSafeBlueprintIdentifier(Name))
            {
                AppendLine(Report, FString::Printf(TEXT("ERROR: delete_variable contains unsafe variable name: %s"), *Name));
                return false;
            }

            if (!BlueprintHasMemberVariable(Blueprint, Name))
            {
                AppendLine(Report, FString::Printf(TEXT("Variable delete skipped, not found/already deleted: %s"), *Name));
                continue;
            }

            if (bDryRun)
            {
                AppendLine(Report, FString::Printf(TEXT("DRY RUN CHANGE: variable delete: %s"), *Name));
                continue;
            }

            FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, FName(*Name));
            AppendLine(Report, FString::Printf(TEXT("CHANGE: variable deleted: %s"), *Name));
            bChanged = true;
        }

        if (bChanged && !bDryRun)
        {
            FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
        }
        return true;
    }

    static bool DeleteFunctionGraphsAction(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& ActionObj, bool bDryRun, FString& Report)
    {
        TArray<FString> Names;
        GetActionNameList(ActionObj, TEXT("function_names"), TEXT("names"), TEXT("function_name"), TEXT("name"), Names);
        if (Names.Num() == 0)
        {
            AppendLine(Report, TEXT("ERROR: delete_function requires function_name/name/function_names[]."));
            return false;
        }

        bool bChanged = false;
        for (const FString& Name : Names)
        {
            if (!IsSafeBlueprintIdentifier(Name))
            {
                AppendLine(Report, FString::Printf(TEXT("ERROR: delete_function contains unsafe function name: %s"), *Name));
                return false;
            }

            UEdGraph* Graph = FindFunctionGraph(Blueprint, Name);
            if (!Graph)
            {
                AppendLine(Report, FString::Printf(TEXT("Function delete skipped, not found/already deleted: %s"), *Name));
                continue;
            }

            if (bDryRun)
            {
                AppendLine(Report, FString::Printf(TEXT("DRY RUN CHANGE: function delete: %s"), *Name));
                continue;
            }

            Graph->Modify();
            FBlueprintEditorUtils::RemoveGraph(Blueprint, Graph, EGraphRemoveFlags::Recompile);
            AppendLine(Report, FString::Printf(TEXT("CHANGE: function deleted: %s"), *Name));
            bChanged = true;
        }

        if (bChanged && !bDryRun)
        {
            FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
        }
        return true;
    }

    static bool DeleteEventDispatchersAction(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& ActionObj, bool bDryRun, FString& Report)
    {
        TArray<FString> Names;
        GetActionNameList(ActionObj, TEXT("dispatcher_names"), TEXT("names"), TEXT("dispatcher_name"), TEXT("name"), Names);
        if (Names.Num() == 0)
        {
            AppendLine(Report, TEXT("ERROR: delete_event_dispatcher requires dispatcher_name/name/dispatcher_names[]."));
            return false;
        }

        bool bChanged = false;
        for (const FString& Name : Names)
        {
            if (!IsSafeBlueprintIdentifier(Name))
            {
                AppendLine(Report, FString::Printf(TEXT("ERROR: delete_event_dispatcher contains unsafe dispatcher name: %s"), *Name));
                return false;
            }

            if (!BlueprintHasMemberVariable(Blueprint, Name))
            {
                AppendLine(Report, FString::Printf(TEXT("Event Dispatcher delete skipped, not found/already deleted: %s"), *Name));
                continue;
            }

            if (bDryRun)
            {
                AppendLine(Report, FString::Printf(TEXT("DRY RUN CHANGE: Event Dispatcher delete: %s"), *Name));
                continue;
            }

            FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, FName(*Name));
            AppendLine(Report, FString::Printf(TEXT("CHANGE: Event Dispatcher deleted: %s"), *Name));
            bChanged = true;
        }

        if (bChanged && !bDryRun)
        {
            FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
        }
        return true;
    }

    static bool DeleteMacroGraphsAction(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& ActionObj, bool bDryRun, FString& Report)
    {
        TArray<FString> Names;
        GetActionNameList(ActionObj, TEXT("macro_names"), TEXT("names"), TEXT("macro_name"), TEXT("name"), Names);
        if (Names.Num() == 0)
        {
            AppendLine(Report, TEXT("ERROR: delete_macro requires macro_name/name/macro_names[]."));
            return false;
        }

        bool bChanged = false;
        for (const FString& Name : Names)
        {
            if (!IsSafeBlueprintIdentifier(Name))
            {
                AppendLine(Report, FString::Printf(TEXT("ERROR: delete_macro contains unsafe macro name: %s"), *Name));
                return false;
            }

            UEdGraph* Graph = FindMacroGraph(Blueprint, Name);
            if (!Graph)
            {
                AppendLine(Report, FString::Printf(TEXT("Macro delete skipped, not found/already deleted: %s"), *Name));
                continue;
            }

            if (bDryRun)
            {
                AppendLine(Report, FString::Printf(TEXT("DRY RUN CHANGE: macro delete: %s"), *Name));
                continue;
            }

            Graph->Modify();
            FBlueprintEditorUtils::RemoveGraph(Blueprint, Graph, EGraphRemoveFlags::Recompile);
            AppendLine(Report, FString::Printf(TEXT("CHANGE: macro deleted: %s"), *Name));
            bChanged = true;
        }

        if (bChanged && !bDryRun)
        {
            FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
        }
        return true;
    }

    static bool AddEventDispatchers(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& ActionObj, bool bDryRun, FString& Report)
    {
        TArray<TSharedPtr<FJsonValue>> DispatcherValues;
        const TArray<TSharedPtr<FJsonValue>>* Dispatchers = GetArrayFieldSafe(ActionObj, TEXT("dispatchers"));
        if (Dispatchers)
        {
            DispatcherValues = *Dispatchers;
        }
        else
        {
            TSharedPtr<FJsonObject> Single = MakeShared<FJsonObject>();
            Single->SetStringField(TEXT("name"), GetStringFieldSafe(ActionObj, TEXT("name")));
            Single->SetStringField(TEXT("category"), GetStringFieldSafe(ActionObj, TEXT("category"), TEXT("N2C/Event Dispatchers")));
            DispatcherValues.Add(MakeShared<FJsonValueObject>(Single));
        }

        bool bOk = true;
        for (const TSharedPtr<FJsonValue>& Value : DispatcherValues)
        {
            TSharedPtr<FJsonObject> DispatcherObj = Value.IsValid() ? Value->AsObject() : nullptr;
            const FString Name = GetStringFieldSafe(DispatcherObj, TEXT("name"));
            const FString Category = GetStringFieldSafe(DispatcherObj, TEXT("category"), GetStringFieldSafe(ActionObj, TEXT("category"), TEXT("N2C/Event Dispatchers")));
            if (!IsSafeBlueprintIdentifier(Name))
            {
                AppendLine(Report, FString::Printf(TEXT("ERROR: invalid Event Dispatcher name: %s"), *Name));
                bOk = false;
                continue;
            }
            if (BlueprintHasMemberVariable(Blueprint, Name))
            {
                AppendLine(Report, FString::Printf(TEXT("Event Dispatcher already exists, skipped duplicate: %s"), *Name));
                continue;
            }
            if (bDryRun)
            {
                AppendLine(Report, FString::Printf(TEXT("DRY RUN NEW: Event Dispatcher: %s"), *Name));
                continue;
            }

            // Match UE4.27 FBlueprintEditor::AddNewDelegate using public APIs. A raw
            // PC_MCDelegate variable is insufficient: the signature graph and its
            // function terminators must exist before Blueprint compilation.
            const FName DispatcherName(*Name);
            FEdGraphPinType DelegateType;
            DelegateType.PinCategory = UEdGraphSchema_K2::PC_MCDelegate;
            if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, DispatcherName, DelegateType))
            {
                AppendLine(Report, FString::Printf(TEXT("ERROR: failed to add Event Dispatcher member variable: %s"), *Name));
                bOk = false;
                continue;
            }

            UEdGraph* SignatureGraph = FBlueprintEditorUtils::CreateNewGraph(
                Blueprint,
                DispatcherName,
                UEdGraph::StaticClass(),
                UEdGraphSchema_K2::StaticClass());
            if (!SignatureGraph)
            {
                FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, DispatcherName);
                AppendLine(Report, FString::Printf(TEXT("ERROR: failed to create Event Dispatcher signature graph: %s"), *Name));
                bOk = false;
                continue;
            }

            SignatureGraph->bEditable = false;
            const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
            K2Schema->CreateDefaultNodesForGraph(*SignatureGraph);
            K2Schema->CreateFunctionGraphTerminators(*SignatureGraph, static_cast<UClass*>(nullptr));
            K2Schema->AddExtraFunctionFlags(SignatureGraph, FUNC_BlueprintCallable | FUNC_BlueprintEvent | FUNC_Public);
            K2Schema->MarkFunctionEntryAsEditable(SignatureGraph, true);
            Blueprint->DelegateSignatureGraphs.Add(SignatureGraph);
            if (!Category.IsEmpty())
            {
                FBlueprintEditorUtils::SetBlueprintVariableCategory(Blueprint, DispatcherName, nullptr, FText::FromString(Category), true);
            }
            AppendLine(Report, FString::Printf(TEXT("NEW: Event Dispatcher with signature graph: %s"), *Name));
        }
        if (!bDryRun)
        {
            FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
        }
        return bOk;
    }

    static bool ImplementInterface(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& ActionObj, bool bDryRun, FString& Report)
    {
        const FString InterfacePath = GetStringFieldSafe(ActionObj, TEXT("interface_class_path"), GetStringFieldSafe(ActionObj, TEXT("interface_path")));
        UClass* InterfaceClass = ResolveBlueprintInterfaceClass(InterfacePath);
        if (!Blueprint || !InterfaceClass || !InterfaceClass->HasAnyClassFlags(CLASS_Interface))
        {
            AppendLine(Report, FString::Printf(TEXT("ERROR: implement_interface could not resolve Blueprint interface '%s'."), *InterfacePath));
            return false;
        }

        const bool bAlreadyImplemented = Blueprint->ImplementedInterfaces.ContainsByPredicate([InterfaceClass](const FBPInterfaceDescription& Desc)
        {
            return Desc.Interface == InterfaceClass;
        });
        if (bDryRun)
        {
            AppendLine(Report, FString::Printf(TEXT("DRY RUN %s: interface '%s'."), bAlreadyImplemented ? TEXT("CHANGE") : TEXT("NEW"), *InterfaceClass->GetPathName()));
            return true;
        }
        if (!bAlreadyImplemented && !FBlueprintEditorUtils::ImplementNewInterface(Blueprint, InterfaceClass->GetFName()))
        {
            AppendLine(Report, FString::Printf(TEXT("ERROR: UE4.27 failed to implement interface '%s'."), *InterfaceClass->GetPathName()));
            return false;
        }
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
        AppendLine(Report, FString::Printf(TEXT("%s: interface '%s'."), bAlreadyImplemented ? TEXT("UNCHANGED") : TEXT("NEW"), *InterfaceClass->GetPathName()));
        return true;
    }

    static UK2Node_Tunnel* FindBoundaryTunnel(UEdGraph* Graph, const FString& Role)
    {
        if (!Graph) return nullptr;
        for (UEdGraphNode* Node : Graph->Nodes) if (UK2Node_Tunnel* Tunnel = Cast<UK2Node_Tunnel>(Node))
        {
            if (Tunnel->IsA<UK2Node_Composite>()) continue;
            if (Role == TEXT("entry") && Tunnel->bCanHaveOutputs && !Tunnel->bCanHaveInputs) return Tunnel;
            if (Role == TEXT("exit") && Tunnel->bCanHaveInputs && !Tunnel->bCanHaveOutputs) return Tunnel;
        }
        return nullptr;
    }

    static bool ApplySchemaPinDefaultValue(
        UEdGraphPin* Pin,
        UObject* OwningObject,
        const FString& AuthoredDefault,
        const FString& Context,
        FString& Report,
        bool bNotifyNode = true);

    static bool SynchronizeBoundaryTunnel(UK2Node_Tunnel* Tunnel, const TSharedPtr<FJsonObject>& NodeObj, FString& Report)
    {
        if (!Tunnel || !NodeObj.IsValid()) return false;
        const TArray<TSharedPtr<FJsonValue>>* Signature = GetArrayFieldSafe(NodeObj, TEXT("user_defined_pin_signature"));
        if (!Signature) return RejectBoundary(Report, TEXT("tunnel_signature_mismatch"), TEXT("missing ordered tunnel signature"));
        const TArray<TSharedPtr<FUserPinInfo>> Existing = Tunnel->UserDefinedPins;
        for (const TSharedPtr<FUserPinInfo>& Pin : Existing) if (Pin.IsValid()) Tunnel->RemoveUserDefinedPin(Pin);
        for (const TSharedPtr<FJsonValue>& Value : *Signature)
        {
            const TSharedPtr<FJsonObject> PinObj = Value.IsValid() ? Value->AsObject() : nullptr;
            const FString Name = GetStringFieldSafe(PinObj, TEXT("name")); const FString DirectionText = GetStringFieldSafe(PinObj, TEXT("direction")).ToLower();
            if (Name.IsEmpty()) return RejectBoundary(Report, TEXT("tunnel_signature_mismatch"), TEXT("tunnel signature pin name is empty"));
            EEdGraphPinDirection Direction = DirectionText == TEXT("input") ? EGPD_Input : DirectionText == TEXT("output") ? EGPD_Output : GetStringFieldSafe(NodeObj, TEXT("tunnel_role")) == TEXT("entry") ? EGPD_Output : EGPD_Input;
            UEdGraphPin* Pin = Tunnel->CreateUserDefinedPin(FName(*Name), MakePinTypeFromJson(PinObj), Direction);
            if (!Pin) return RejectBoundary(Report, TEXT("tunnel_signature_mismatch"), FString::Printf(TEXT("UE4.27 rejected boundary pin '%s'"), *Name));
            const FString DefaultObjectPath = GetStringFieldSafe(PinObj, TEXT("default_object"));
            const FString DefaultText = GetStringFieldSafe(PinObj, TEXT("default_text"));
            const FString DefaultValue = GetStringFieldSafe(PinObj, TEXT("default_value"));
            const bool bHasAuthoredDefault = PinObj->HasField(TEXT("default_value")) ||
                PinObj->HasField(TEXT("default_text")) || PinObj->HasField(TEXT("default_object"));
            const FString AuthoredDefault = !DefaultObjectPath.IsEmpty() ? DefaultObjectPath :
                (!DefaultText.IsEmpty() ? DefaultText : DefaultValue);
            if (bHasAuthoredDefault && !ApplySchemaPinDefaultValue(
                    Pin,
                    Tunnel,
                    AuthoredDefault,
                    FString::Printf(TEXT("boundary.%s"), *Name),
                    Report,
                    false))
            {
                return RejectBoundary(Report, TEXT("tunnel_default_invalid"),
                    FString::Printf(TEXT("UE4.27 rejected boundary default for pin '%s'"), *Name));
            }
        }
        return true;
    }

    static bool ApplyBoundaryGraphBody(UBlueprint* Blueprint, UEdGraph* Graph, const TSharedPtr<FJsonObject>& ActionObj, TMap<FString, UEdGraphNode*>& NodesById, FString& Report)
    {
        const TArray<TSharedPtr<FJsonValue>>* Nodes = GetArrayFieldSafe(ActionObj, TEXT("nodes"));
        if (!Nodes) return false;
        int32 Index = 0;
        for (const TSharedPtr<FJsonValue>& Value : *Nodes)
        {
            const TSharedPtr<FJsonObject> NodeObj = Value.IsValid() ? Value->AsObject() : nullptr; if (!NodeObj.IsValid()) { ++Index; continue; }
            const FString Id = GetStringFieldSafe(NodeObj, TEXT("id"), FString::Printf(TEXT("BoundaryNode_%d"), Index)); const FString NodeType = GetStringFieldSafe(NodeObj, TEXT("type"), GetStringFieldSafe(NodeObj, TEXT("class"))).ToLower();
            if (NodeType.Contains(TEXT("tunnel")) && !NodeType.Contains(TEXT("composite")))
            {
                UK2Node_Tunnel* Tunnel = FindBoundaryTunnel(Graph, GetStringFieldSafe(NodeObj, TEXT("tunnel_role"))); if (!SynchronizeBoundaryTunnel(Tunnel, NodeObj, Report)) return false; NodesById.Add(Id, Tunnel);
            }
            else if (UEdGraphNode* Created = CreatePatchNode(Graph, Blueprint, NodeObj, Index, Report)) { if (!ApplyNodePinDefaults(Created, NodeObj, Report)) return false; NodesById.Add(Id, Created); }
            else return RejectBoundary(Report, TEXT("graph_boundary_variant_unsupported"), FString::Printf(TEXT("inner node '%s' could not be constructed"), *NodeType));
            ++Index;
        }
        if (!ConnectEdges(Graph, NodesById, ActionObj, Report)) return false;
        return true;
    }

    static bool AddMacroGraphAction(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& ActionObj, bool bDryRun, FString& Report)
    {
        const FString MacroName = GetStringFieldSafe(ActionObj, TEXT("macro_name"), GetStringFieldSafe(ActionObj, TEXT("name")));
        const FString Category = GetStringFieldSafe(ActionObj, TEXT("category"), GetStringFieldSafe(ActionObj, TEXT("folder"), TEXT("N2C/Macros")));

        UEdGraph* Graph = FindMacroGraph(Blueprint, MacroName);

        if (bDryRun)
        {
            AppendLine(Report, FString::Printf(TEXT("DRY RUN NEW: macro: %s"), *MacroName));
            return true;
        }

        if (!Graph) { Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, FName(*MacroName), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass()); FBlueprintEditorUtils::AddMacroGraph(Blueprint, Graph, true, nullptr); }
        else if (GetBoolFieldSafe(ActionObj, TEXT("replace_body"), false)) { const TArray<UEdGraphNode*> ExistingNodes = Graph->Nodes; for (UEdGraphNode* Node : ExistingNodes) if (Node && !Node->IsA<UK2Node_Tunnel>()) FBlueprintEditorUtils::RemoveNode(Blueprint, Node, true); }
        TMap<FString, UEdGraphNode*> NodesById;
        if (!ApplyBoundaryGraphBody(Blueprint, Graph, ActionObj, NodesById, Report)) return false;
        if (!Category.IsEmpty())
        {
            FBlueprintEditorUtils::SetBlueprintFunctionOrMacroCategory(Graph, FText::FromString(Category), true);
        }
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
        AppendLine(Report, FString::Printf(TEXT("CHANGE: macro graph boundary/body synchronized: %s"), *MacroName));
        if (!Category.IsEmpty())
        {
            AppendLine(Report, FString::Printf(TEXT("CHANGE: macro category: %s -> %s"), *MacroName, *Category));
        }
        return true;
    }

    static bool RenameFunctionGraph(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& ActionObj, bool bDryRun, FString& Report)
    {
        const FString OldName = GetStringFieldSafe(ActionObj, TEXT("old_function_name"), GetStringFieldSafe(ActionObj, TEXT("from")));
        const FString NewName = GetStringFieldSafe(ActionObj, TEXT("new_function_name"), GetStringFieldSafe(ActionObj, TEXT("to")));
        const FString Category = GetStringFieldSafe(ActionObj, TEXT("category"), GetStringFieldSafe(ActionObj, TEXT("folder")));

        UEdGraph* Graph = FindFunctionGraph(Blueprint, OldName);
        if (!Graph)
        {
            if (FindFunctionGraph(Blueprint, NewName))
            {
                AppendLine(Report, FString::Printf(TEXT("Function rename already applied: %s"), *NewName));
                return true;
            }
            AppendLine(Report, FString::Printf(TEXT("ERROR: rename_function source not found: %s"), *OldName));
            return false;
        }
        if (FindFunctionGraph(Blueprint, NewName))
        {
            AppendLine(Report, FString::Printf(TEXT("ERROR: rename_function destination already exists: %s"), *NewName));
            return false;
        }

        if (bDryRun)
        {
            AppendLine(Report, FString::Printf(TEXT("DRY RUN CHANGE: function rename: %s -> %s"), *OldName, *NewName));
            return true;
        }

        Graph->Modify();
        FBlueprintEditorUtils::RenameGraph(Graph, NewName);
        ReplaceSelfFunctionCallReferences(Blueprint, OldName, NewName, Report);

        if (!Category.IsEmpty())
        {
            FBlueprintEditorUtils::SetBlueprintFunctionOrMacroCategory(Graph, FText::FromString(Category), true);
        }

        AppendLine(Report, FString::Printf(TEXT("CHANGE: function rename: %s -> %s"), *OldName, *NewName));
        if (!Category.IsEmpty())
        {
            AppendLine(Report, FString::Printf(TEXT("CHANGE: function category: %s -> %s"), *NewName, *Category));
        }
        return true;
    }

    static bool SetFunctionCategory(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& ActionObj, bool bDryRun, FString& Report)
    {
        const FString FunctionName = GetActionFunctionName(ActionObj);
        const FString Category = GetStringFieldSafe(ActionObj, TEXT("category"), GetStringFieldSafe(ActionObj, TEXT("folder")));
        UEdGraph* Graph = FindFunctionGraph(Blueprint, FunctionName);
        if (!Graph)
        {
            AppendLine(Report, FString::Printf(TEXT("ERROR: set_function_category target not found: %s"), *FunctionName));
            return false;
        }

        if (bDryRun)
        {
            AppendLine(Report, FString::Printf(TEXT("DRY RUN CHANGE: function category: %s -> %s"), *FunctionName, *Category));
            return true;
        }

        Graph->Modify();
        FBlueprintEditorUtils::SetBlueprintFunctionOrMacroCategory(Graph, FText::FromString(Category), true);
        AppendLine(Report, FString::Printf(TEXT("CHANGE: function category: %s -> %s"), *FunctionName, *Category));
        return true;
    }

    static bool TryGetNestedBoolOption(const TSharedPtr<FJsonObject>& Obj, const FString& ObjectFieldName, const FString& FieldName, bool& OutValue)
    {
        if (TryGetBoolFieldSafe(Obj, FieldName, OutValue))
        {
            return true;
        }

        const TSharedPtr<FJsonObject>* NestedObj = nullptr;
        if (Obj.IsValid() && Obj->TryGetObjectField(ObjectFieldName, NestedObj) && NestedObj && NestedObj->IsValid())
        {
            return (*NestedObj)->TryGetBoolField(FieldName, OutValue);
        }
        return false;
    }

    static bool TryGetNestedStringOption(const TSharedPtr<FJsonObject>& Obj, const FString& ObjectFieldName, const FString& FieldName, FString& OutValue)
    {
        if (Obj.IsValid() && Obj->TryGetStringField(FieldName, OutValue))
        {
            return true;
        }

        const TSharedPtr<FJsonObject>* NestedObj = nullptr;
        if (Obj.IsValid() && Obj->TryGetObjectField(ObjectFieldName, NestedObj) && NestedObj && NestedObj->IsValid())
        {
            return (*NestedObj)->TryGetStringField(FieldName, OutValue);
        }
        return false;
    }

    static bool ActionHasFunctionOptions(const TSharedPtr<FJsonObject>& ActionObj)
    {
        bool bBoolValue = false;
        FString StringValue;
        return TryGetNestedStringOption(ActionObj, TEXT("function_flags"), TEXT("access"), StringValue) ||
               TryGetNestedBoolOption(ActionObj, TEXT("function_flags"), TEXT("pure"), bBoolValue) ||
               TryGetNestedBoolOption(ActionObj, TEXT("function_flags"), TEXT("const"), bBoolValue);
    }

    static bool GetFunctionEntryFlags(UK2Node_FunctionEntry* Entry, uint64& OutFlags)
    {
        OutFlags = 0;
        if (!Entry)
        {
            return false;
        }
        if (FUInt32Property* UInt32Prop = FindFProperty<FUInt32Property>(Entry->GetClass(), TEXT("FunctionFlags")))
        {
            OutFlags = UInt32Prop->GetPropertyValue_InContainer(Entry);
            return true;
        }
        if (FIntProperty* IntProp = FindFProperty<FIntProperty>(Entry->GetClass(), TEXT("FunctionFlags")))
        {
            OutFlags = static_cast<uint64>(IntProp->GetPropertyValue_InContainer(Entry));
            return true;
        }
        if (FUInt64Property* UInt64Prop = FindFProperty<FUInt64Property>(Entry->GetClass(), TEXT("FunctionFlags")))
        {
            OutFlags = UInt64Prop->GetPropertyValue_InContainer(Entry);
            return true;
        }

        // UE4.27 stores user-declared Blueprint function flags on ExtraFlags.
        // Keep FunctionFlags support first for newer/other engine branches, then fall back.
        if (FUInt32Property* ExtraUInt32Prop = FindFProperty<FUInt32Property>(Entry->GetClass(), TEXT("ExtraFlags")))
        {
            OutFlags = ExtraUInt32Prop->GetPropertyValue_InContainer(Entry);
            return true;
        }
        if (FIntProperty* ExtraIntProp = FindFProperty<FIntProperty>(Entry->GetClass(), TEXT("ExtraFlags")))
        {
            OutFlags = static_cast<uint64>(ExtraIntProp->GetPropertyValue_InContainer(Entry));
            return true;
        }
        if (FUInt64Property* ExtraUInt64Prop = FindFProperty<FUInt64Property>(Entry->GetClass(), TEXT("ExtraFlags")))
        {
            OutFlags = ExtraUInt64Prop->GetPropertyValue_InContainer(Entry);
            return true;
        }
        return false;
    }

    static bool SetFunctionEntryFlags(UK2Node_FunctionEntry* Entry, uint64 Flags)
    {
        if (!Entry)
        {
            return false;
        }
        if (FUInt32Property* UInt32Prop = FindFProperty<FUInt32Property>(Entry->GetClass(), TEXT("FunctionFlags")))
        {
            UInt32Prop->SetPropertyValue_InContainer(Entry, static_cast<uint32>(Flags));
            return true;
        }
        if (FIntProperty* IntProp = FindFProperty<FIntProperty>(Entry->GetClass(), TEXT("FunctionFlags")))
        {
            IntProp->SetPropertyValue_InContainer(Entry, static_cast<int32>(Flags));
            return true;
        }
        if (FUInt64Property* UInt64Prop = FindFProperty<FUInt64Property>(Entry->GetClass(), TEXT("FunctionFlags")))
        {
            UInt64Prop->SetPropertyValue_InContainer(Entry, Flags);
            return true;
        }

        if (FUInt32Property* ExtraUInt32Prop = FindFProperty<FUInt32Property>(Entry->GetClass(), TEXT("ExtraFlags")))
        {
            ExtraUInt32Prop->SetPropertyValue_InContainer(Entry, static_cast<uint32>(Flags));
            return true;
        }
        if (FIntProperty* ExtraIntProp = FindFProperty<FIntProperty>(Entry->GetClass(), TEXT("ExtraFlags")))
        {
            ExtraIntProp->SetPropertyValue_InContainer(Entry, static_cast<int32>(Flags));
            return true;
        }
        if (FUInt64Property* ExtraUInt64Prop = FindFProperty<FUInt64Property>(Entry->GetClass(), TEXT("ExtraFlags")))
        {
            ExtraUInt64Prop->SetPropertyValue_InContainer(Entry, Flags);
            return true;
        }
        return false;
    }

    static bool ApplyFunctionOptions(UBlueprint* Blueprint, UEdGraph* Graph, const TSharedPtr<FJsonObject>& ActionObj, const FString& FunctionName, bool bDryRun, FString& Report)
    {
        if (!ActionHasFunctionOptions(ActionObj))
        {
            return true;
        }

        if (bDryRun)
        {
            AppendLine(Report, FString::Printf(TEXT("DRY RUN CHANGE: function options: %s"), *FunctionName));
            return true;
        }

        UK2Node_FunctionEntry* Entry = FindOrCreateEntryNode(Graph, FunctionName);
        if (!Entry)
        {
            AppendLine(Report, FString::Printf(TEXT("WARNING: function options skipped because entry node was not found: %s"), *FunctionName));
            return false;
        }

        uint64 Flags = 0;
        if (!GetFunctionEntryFlags(Entry, Flags))
        {
            AppendLine(Report, FString::Printf(TEXT("WARNING: function options skipped because FunctionFlags/ExtraFlags property was not found: %s"), *FunctionName));
            return false;
        }

        Entry->Modify();

        FString Access;
        if (TryGetNestedStringOption(ActionObj, TEXT("function_flags"), TEXT("access"), Access))
        {
            const FString LowerAccess = Access.ToLower();
            Flags &= ~(FUNC_Public | FUNC_Protected | FUNC_Private);
            if (LowerAccess == TEXT("private"))
            {
                Flags |= FUNC_Private;
            }
            else if (LowerAccess == TEXT("protected"))
            {
                Flags |= FUNC_Protected;
            }
            else
            {
                Flags |= FUNC_Public;
            }
        }

        bool bValue = false;
        if (TryGetNestedBoolOption(ActionObj, TEXT("function_flags"), TEXT("pure"), bValue))
        {
            if (bValue) { Flags |= FUNC_BlueprintPure; }
            else { Flags &= ~FUNC_BlueprintPure; }
        }
        if (TryGetNestedBoolOption(ActionObj, TEXT("function_flags"), TEXT("const"), bValue))
        {
            if (bValue) { Flags |= FUNC_Const; }
            else { Flags &= ~FUNC_Const; }
        }

        if (!SetFunctionEntryFlags(Entry, Flags))
        {
            AppendLine(Report, FString::Printf(TEXT("WARNING: function options failed to save FunctionFlags: %s"), *FunctionName));
            return false;
        }

        if (Blueprint)
        {
            FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
        }
        AppendLine(Report, FString::Printf(TEXT("CHANGE: function options: %s"), *FunctionName));
        return true;
    }

    static FString NormalizeLoosePinName(FString Value)
    {
        Value.TrimStartAndEndInline();
        Value.ReplaceInline(TEXT(" "), TEXT(""));
        Value.ReplaceInline(TEXT("_"), TEXT(""));
        Value.ReplaceInline(TEXT("-"), TEXT(""));
        // UE display names for array pins are often "[ 0]" while JSON
        // patches usually say "0", "[0]" or "Option 0".  Strip
        // brackets and punctuation so loose pin matching can connect these
        // nodes instead of silently skipping edges/defaults.
        Value.ReplaceInline(TEXT("["), TEXT(""));
        Value.ReplaceInline(TEXT("]"), TEXT(""));
        Value.ReplaceInline(TEXT("("), TEXT(""));
        Value.ReplaceInline(TEXT(")"), TEXT(""));
        Value.ReplaceInline(TEXT("."), TEXT(""));
        Value.ReplaceInline(TEXT(","), TEXT(""));
        return Value.ToLower();
    }

    static bool IsLikelyUserDefinedStructInternalPinName(const FString& PinName, const FString& WantedName)
    {
        const FString NormalizedPin = NormalizeLoosePinName(PinName);
        const FString NormalizedWanted = NormalizeLoosePinName(WantedName);
        return !NormalizedPin.IsEmpty()
            && !NormalizedWanted.IsEmpty()
            && NormalizedPin.StartsWith(NormalizedWanted)
            && PinName.Contains(TEXT("_"));
    }

    static UEdGraphPin* FindPinByLooseName(UEdGraphNode* Node, const FString& Name, EEdGraphPinDirection Direction)
    {
        if (!Node)
        {
            return nullptr;
        }

        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin || Pin->Direction != Direction)
            {
                continue;
            }

            // Crash guard 2026-07-11: do not call GetDisplayName() on freshly
            // created K2 signature nodes during apply. In UE4.27 some BlueprintGraph
            // nodes can dereference incomplete signature metadata from GetDisplayName()
            // before the Blueprint has been structurally rebuilt. PinName is stable
            // enough for importer-created links/defaults.
            if (Pin->PinName.ToString() == Name)
            {
                return Pin;
            }
        }

        const FString NormalizedName = NormalizeLoosePinName(Name);
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin || Pin->Direction != Direction)
            {
                continue;
            }
            if (NormalizeLoosePinName(Pin->PinName.ToString()) == NormalizedName ||
                IsLikelyUserDefinedStructInternalPinName(Pin->PinName.ToString(), Name))
            {
                return Pin;
            }
        }

        // Friendly aliases used by generated patches.  Keep this based on PinName only;
        // GetDisplayName() can crash on freshly created FunctionEntry/FunctionResult pins
        // before UE4.27 has rebuilt the signature metadata.
        const FString Lower = Name.ToLower();

        // UE4.27 names the GetArrayItem index pin "Dimension 1" even though
        // exported and AI-authored patches commonly call it "Index". Resolve
        // only this node-specific alias; unrelated missing pin_defaults remain
        // strict preflight failures.
        if (UK2Node_GetArrayItem* ArrayItem = Cast<UK2Node_GetArrayItem>(Node))
        {
            if (Direction == EGPD_Input &&
                (Lower == TEXT("index") || Lower == TEXT("dimension1") || Lower == TEXT("dimension_1")))
            {
                return ArrayItem->GetIndexPin();
            }
        }

        // UE4.27 UK2Node_Knot uses canonical internal names InputPin/OutputPin.
        // AI-authored patches often use the visible shorthand Input/Output. Accept
        // those aliases, but authoritative JSON documentation requires the canonical
        // names so exports and imports stay round-trip stable.
        if (UK2Node_Knot* Knot = Cast<UK2Node_Knot>(Node))
        {
            if (Direction == EGPD_Input && (Lower == TEXT("input") || Lower == TEXT("inputpin")))
            {
                return Knot->GetInputPin();
            }
            if (Direction == EGPD_Output && (Lower == TEXT("output") || Lower == TEXT("outputpin")))
            {
                return Knot->GetOutputPin();
            }
        }

        // UE4.27 UK2Node_CreateDelegate exposes the event output with the
        // internal PinName "OutputDelegate" and only shows "Event" in the UI.
        // Older/manual JSON commonly used "Delegate". Accept that alias so
        // strict sandbox preflight validates the real UE4 node instead of
        // reporting a false edge_pin_missing error. New exports and fixtures
        // must use the canonical OutputDelegate identity.
        if (UK2Node_CreateDelegate* CreateDelegate = Cast<UK2Node_CreateDelegate>(Node))
        {
            if (Direction == EGPD_Output &&
                (Lower == TEXT("delegate") || Lower == TEXT("event") ||
                 Lower == TEXT("outputdelegate") || Lower == TEXT("output_delegate")))
            {
                return CreateDelegate->GetDelegateOutPin();
            }
            if (Direction == EGPD_Input &&
                (Lower == TEXT("object") || Lower == TEXT("self") ||
                 Lower == TEXT("inputobject") || Lower == TEXT("input_object")))
            {
                return CreateDelegate->GetObjectInPin();
            }
        }

        // UE4.27 generic MakeStruct/BreakStruct pins are named after the
        // concrete UScriptStruct (StructType->GetFName()), not ReturnValue or
        // Struct. Older generated patches used those friendly aliases. Resolve
        // them only when the node has exactly one struct pin in the requested
        // direction, while authoritative JSON must use the exported PinName.
        const bool bStructValueAlias =
            Lower == TEXT("return") || Lower == TEXT("result") ||
            Lower == TEXT("returnvalue") || Lower == TEXT("return_value") ||
            Lower == TEXT("struct") || Lower == TEXT("structvalue") ||
            Lower == TEXT("struct_value") || Lower == TEXT("value");
        if (bStructValueAlias && Direction == EGPD_Output && Node->IsA<UK2Node_MakeStruct>())
        {
            UEdGraphPin* Candidate = nullptr;
            for (UEdGraphPin* Pin : Node->Pins)
            {
                if (!Pin || Pin->Direction != EGPD_Output || Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Struct)
                {
                    continue;
                }
                if (Candidate)
                {
                    Candidate = nullptr;
                    break;
                }
                Candidate = Pin;
            }
            if (Candidate)
            {
                return Candidate;
            }
        }
        if (bStructValueAlias && Direction == EGPD_Input && Node->IsA<UK2Node_BreakStruct>())
        {
            UEdGraphPin* Candidate = nullptr;
            for (UEdGraphPin* Pin : Node->Pins)
            {
                if (!Pin || Pin->Direction != EGPD_Input || Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Struct)
                {
                    continue;
                }
                if (Candidate)
                {
                    Candidate = nullptr;
                    break;
                }
                Candidate = Pin;
            }
            if (Candidate)
            {
                return Candidate;
            }
        }

        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin || Pin->Direction != Direction)
            {
                continue;
            }
            const FString PinLower = Pin->PinName.ToString().ToLower();
            const FString PinNorm = NormalizeLoosePinName(Pin->PinName.ToString());
            if ((Lower == TEXT("then") && (PinLower == TEXT("then") || (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec && PinNorm.IsEmpty()))) ||
                ((Lower == TEXT("rowfound") || Lower == TEXT("row_found")) && PinLower == TEXT("then")) ||
                ((Lower == TEXT("exec") || Lower == TEXT("execute") || Lower == TEXT("execution")) && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) ||
                ((Lower == TEXT("completed") || Lower == TEXT("complete")) && PinLower == TEXT("completed")) ||
                ((Lower == TEXT("loopbody") || Lower == TEXT("body")) && PinLower == TEXT("loopbody")) ||
                ((Lower == TEXT("return") || Lower == TEXT("result") || Lower == TEXT("returnvalue") || Lower == TEXT("return_value") ||
                  Lower == TEXT("outrow") || Lower == TEXT("out_row") || Lower == TEXT("out row")) && PinLower == TEXT("returnvalue")) ||
                (Lower == TEXT("condition") && PinLower == TEXT("condition")) ||
                (Lower == TEXT("true") && PinLower == TEXT("then")) ||
                (Lower == TEXT("false") && PinLower == TEXT("else")))
            {
                return Pin;
            }
        }

        // SwitchEnum in UE4.27 usually has no "Default" output pin for fully
        // enumerated switches. Generated probe patches sometimes ask for Default;
        // map that to the first real enum exec output so the node can still compile
        // and the importer test verifies the constructor path without depending on
        // a specific enum case display name.
        if (Direction == EGPD_Output &&
            (Lower == TEXT("default") || Lower == TEXT("case") || Lower == TEXT("enumcase")) &&
            Node->IsA(UK2Node_SwitchEnum::StaticClass()))
        {
            for (UEdGraphPin* Pin : Node->Pins)
            {
                if (!Pin || Pin->Direction != EGPD_Output || Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
                {
                    continue;
                }
                const FString PinLower = Pin->PinName.ToString().ToLower();
                if (PinLower != TEXT("then") && PinLower != TEXT("execute") && PinLower != TEXT("exec"))
                {
                    return Pin;
                }
            }
        }

        // Friendly aliases for K2Node_Select option pins.  Bool selects expose
        // option display names as False/True, while generated patches often say
        // A/B, Option0/Option1, or Option 0/Option 1.
        if (Direction == EGPD_Input && (Lower == TEXT("a") || Lower == TEXT("option0") || Lower == TEXT("option 0") || Lower == TEXT("false") ||
                                        Lower == TEXT("b") || Lower == TEXT("option1") || Lower == TEXT("option 1") || Lower == TEXT("true")))
        {
            const bool bWantsSecond = (Lower == TEXT("b") || Lower == TEXT("option1") || Lower == TEXT("option 1") || Lower == TEXT("true"));
            for (UEdGraphPin* Pin : Node->Pins)
            {
                if (!Pin || Pin->Direction != EGPD_Input)
                {
                    continue;
                }
                const FString PinNorm = NormalizeLoosePinName(Pin->PinName.ToString());
                if (!bWantsSecond && PinNorm == TEXT("option0"))
                {
                    return Pin;
                }
                if (bWantsSecond && PinNorm == TEXT("option1"))
                {
                    return Pin;
                }
            }
        }

        if ((Lower == TEXT("struct") || Lower == TEXT("input") || Lower == TEXT("value")) && Direction == EGPD_Input)
        {
            for (UEdGraphPin* Pin : Node->Pins)
            {
                if (Pin && Pin->Direction == EGPD_Input && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
                {
                    return Pin;
                }
            }
        }

        return nullptr;
    }

    static bool IsArrayElementPinName(const FString& PinName);

    static bool IsWildcardPin(const UEdGraphPin* Pin)
    {
        return Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard;
    }

    static bool IsTypedPin(const UEdGraphPin* Pin)
    {
        return Pin && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Wildcard && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec;
    }

    static void AdoptLinkedPinTypeIfWildcard(UEdGraphPin* MaybeWildcardPin, const UEdGraphPin* SourcePin)
    {
        if (!IsWildcardPin(MaybeWildcardPin) || !IsTypedPin(SourcePin))
        {
            return;
        }

        MaybeWildcardPin->PinType = SourcePin->PinType;
    }

    static FEdGraphPinType MakeArrayOutputPinTypeFromElementType(const FEdGraphPinType& ElementType)
    {
        FEdGraphPinType ArrayType = ElementType;
        ArrayType.ContainerType = EPinContainerType::Array;
        return ArrayType;
    }

    static void ApplyMakeArrayElementType(UEdGraphNode* Node, const FEdGraphPinType& ElementType)
    {
        if (!Node)
        {
            return;
        }

        const FEdGraphPinType ArrayType = MakeArrayOutputPinTypeFromElementType(ElementType);
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin || Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Wildcard)
            {
                continue;
            }

            if (Pin->Direction == EGPD_Output)
            {
                Pin->PinType = ArrayType;
            }
            else if (IsArrayElementPinName(Pin->PinName.ToString()))
            {
                Pin->PinType = ElementType;
                GetDefault<UEdGraphSchema_K2>()->SetPinAutogeneratedDefaultValueBasedOnType(Pin);
            }
        }
    }

    static FString JsonValueToPinDefaultString(const TSharedPtr<FJsonValue>& Value)
    {
        if (!Value.IsValid())
        {
            return TEXT("");
        }

        switch (Value->Type)
        {
        case EJson::Boolean:
            return Value->AsBool() ? TEXT("true") : TEXT("false");
        case EJson::Number:
            return FString::SanitizeFloat(Value->AsNumber());
        case EJson::String:
            return Value->AsString();
        default:
            return TEXT("");
        }
    }

    static FString NormalizePinDefaultValue(const UEdGraphPin* Pin, FString DefaultValue)
    {
        DefaultValue.TrimStartAndEndInline();
        if (!Pin || DefaultValue.IsEmpty())
        {
            return DefaultValue;
        }

        const FName Category = Pin->PinType.PinCategory;

        // UE validates integer pin defaults strictly. JSON numeric values are serialized
        // through FString::SanitizeFloat(), so 3 can become "3.0" unless normalized here.
        if (Category == UEdGraphSchema_K2::PC_Int)
        {
            if (IsLooseNumericString(DefaultValue))
            {
                return FString::FromInt(FMath::RoundToInt(FCString::Atod(*DefaultValue)));
            }
            return DefaultValue;
        }

        // Plain byte pins may be numeric. Enum byte pins must keep enumerator names
        // such as NewEnumerator0 / EMyEnum::Value, so only normalize numeric strings.
        if (Category == UEdGraphSchema_K2::PC_Byte)
        {
            if (IsLooseNumericString(DefaultValue))
            {
                return FString::FromInt(FMath::RoundToInt(FCString::Atod(*DefaultValue)));
            }
            return DefaultValue;
        }

        if (Category == UEdGraphSchema_K2::PC_Boolean)
        {
            const FString Lower = DefaultValue.ToLower();
            if (Lower == TEXT("1") || Lower == TEXT("true") || Lower == TEXT("yes"))
            {
                return TEXT("true");
            }
            if (Lower == TEXT("0") || Lower == TEXT("false") || Lower == TEXT("no"))
            {
                return TEXT("false");
            }
        }

        if (Category == UEdGraphSchema_K2::PC_Struct)
        {
            UScriptStruct* StructType = Cast<UScriptStruct>(Pin->PinType.PinSubCategoryObject.Get());
            FString K2Default;
            UObject* Owner = Pin->GetOwningNodeUnchecked();
            if (TryCanonicalizeK2CustomStructDefault(StructType, DefaultValue, Owner, K2Default))
            {
                return K2Default;
            }
        }

        return DefaultValue;
    }

    static FString GetAuthoredPinDefaultString(UEdGraphPin* Pin, const TSharedPtr<FJsonValue>& Value)
    {
        if (!Pin || !Value.IsValid())
        {
            return FString();
        }
        return NormalizePinDefaultValue(Pin, JsonValueToPinDefaultString(Value));
    }

    static bool ApplySchemaPinDefaultValue(
        UEdGraphPin* Pin,
        UObject* OwningObject,
        const FString& AuthoredDefault,
        const FString& Context,
        FString& Report,
        bool bNotifyNode)
    {
        if (!Pin)
        {
            AppendLine(Report, FString::Printf(
                TEXT("N2C_RUNTIME_GUARD|code=pin_default_target_missing|context=%s"),
                *Context));
            AppendLine(Report, FString::Printf(
                TEXT("ERROR: authored default '%s' has no target pin in context '%s'."),
                *AuthoredDefault,
                *Context));
            return false;
        }

        const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
        const FString NormalizedDefault = NormalizePinDefaultValue(Pin, AuthoredDefault);
        FString UseDefaultValue;
        UObject* UseDefaultObject = nullptr;
        FText UseDefaultText;
        Schema->GetPinDefaultValuesFromString(
            Pin->PinType,
            OwningObject ? OwningObject : Pin->GetOwningNodeUnchecked(),
            NormalizedDefault,
            UseDefaultValue,
            UseDefaultObject,
            UseDefaultText,
            false);

        if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Wildcard)
        {
            const FString ValidationError = Schema->IsPinDefaultValid(
                Pin,
                UseDefaultValue,
                UseDefaultObject,
                UseDefaultText);
            if (!ValidationError.IsEmpty())
            {
                AppendLine(Report, FString::Printf(
                    TEXT("N2C_RUNTIME_GUARD|code=pin_default_invalid|node=%s|pin=%s|authored=%s|canonical=%s|object=%s|context=%s"),
                    Pin->GetOwningNodeUnchecked() ? *Pin->GetOwningNodeUnchecked()->GetClass()->GetName() : TEXT("<none>"),
                    *Pin->PinName.ToString(),
                    *NormalizedDefault,
                    *UseDefaultValue,
                    UseDefaultObject ? *UseDefaultObject->GetPathName() : TEXT(""),
                    *Context));
                AppendLine(Report, FString::Printf(
                    TEXT("ERROR: node pin '%s' default '%s' is invalid in UE4.27 after schema conversion: %s"),
                    *Context,
                    *NormalizedDefault,
                    *ValidationError));
                return false;
            }
        }

        Pin->DefaultValue = MoveTemp(UseDefaultValue);
        Pin->DefaultObject = UseDefaultObject;
        Pin->DefaultTextValue = MoveTemp(UseDefaultText);
        if (bNotifyNode)
        {
            if (UEdGraphNode* OwningNode = Pin->GetOwningNodeUnchecked())
            {
                OwningNode->PinDefaultValueChanged(Pin);
            }
        }
        return true;
    }

    static bool IsArrayElementPinName(const FString& PinName)
    {
        FString Normalized = NormalizeLoosePinName(PinName);
        if (Normalized.IsEmpty())
        {
            return false;
        }
        for (int32 Index = 0; Index < Normalized.Len(); ++Index)
        {
            if (!FChar::IsDigit(Normalized[Index]))
            {
                return false;
            }
        }
        return true;
    }


    static bool IsReservedNodeJsonField(const FString& FieldName)
    {
        return FieldName == TEXT("id") ||
               FieldName == TEXT("type") ||
               FieldName == TEXT("class") ||
               FieldName == TEXT("name") ||
               FieldName == TEXT("title") ||
               FieldName == TEXT("pos_x") ||
               FieldName == TEXT("pos_y") ||
               FieldName == TEXT("function_name") ||
               FieldName == TEXT("function_path") ||
               FieldName == TEXT("member_name") ||
               FieldName == TEXT("variable_name") ||
               FieldName == TEXT("enum_path") ||
               FieldName == TEXT("enum") ||
               FieldName == TEXT("type_path") ||
               FieldName == TEXT("asset_path") ||
               FieldName == TEXT("object_path") ||
               FieldName == TEXT("struct_path") ||
               FieldName == TEXT("class_path") ||
               FieldName == TEXT("target_class") ||
               FieldName == TEXT("macro_name") ||
               FieldName == TEXT("macro_path") ||
               FieldName == TEXT("native_class") ||
               FieldName == TEXT("option_count") ||
               FieldName == TEXT("input_count") ||
               FieldName == TEXT("index_pin_type") ||
               FieldName == TEXT("value_pin_type") ||
               FieldName == TEXT("create_new") ||
               FieldName == TEXT("force_new") ||
               FieldName == TEXT("pin_defaults");
    }

    static bool ApplyNodePinDefaults(UEdGraphNode* Node, const TSharedPtr<FJsonObject>& NodeObj, FString& Report)
    {
        if (!Node || !NodeObj.IsValid())
        {
            return false;
        }

        const TSharedPtr<FJsonObject>* ValuePinTypeObj = nullptr;
        FEdGraphPinType FallbackValuePinType;
        const bool bHasFallbackValuePinType = NodeObj->TryGetObjectField(TEXT("value_pin_type"), ValuePinTypeObj) && ValuePinTypeObj && ValuePinTypeObj->IsValid();
        if (bHasFallbackValuePinType)
        {
            FallbackValuePinType = MakePinTypeFromJson(*ValuePinTypeObj);
        }

        auto ApplyAndValidate = [&](UEdGraphPin* Pin, const FString& JsonPinName, const TSharedPtr<FJsonValue>& Value) -> bool
        {
            if (!Pin)
            {
                return true;
            }

            const FString AuthoredDefault = GetAuthoredPinDefaultString(Pin, Value);
            if (bHasFallbackValuePinType &&
                Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard &&
                IsArrayElementPinName(Pin->PinName.ToString()))
            {
                Pin->DefaultObject = nullptr;
                Pin->DefaultTextValue = FText::GetEmpty();
                Pin->DefaultValue = NormalizeMemberVariableDefaultValue(FallbackValuePinType, AuthoredDefault);
                Node->PinDefaultValueChanged(Pin);
                return true;
            }

            return ApplySchemaPinDefaultValue(
                Pin,
                Node,
                AuthoredDefault,
                FString::Printf(TEXT("%s.%s"), *Node->GetClass()->GetName(), *JsonPinName),
                Report);
        };

        const TSharedPtr<FJsonObject>* DefaultsObj = nullptr;
        if (NodeObj->TryGetObjectField(TEXT("pin_defaults"), DefaultsObj) && DefaultsObj && DefaultsObj->IsValid())
        {
            for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*DefaultsObj)->Values)
            {
                UEdGraphPin* Pin = FindPinByLooseName(Node, Pair.Key, EGPD_Input);
                if (!Pin)
                {
                    AppendLine(Report, FString::Printf(
                        TEXT("N2C_PREFLIGHT_GUARD|code=pin_default_target_missing|node=%s|pin=%s"),
                        *Node->GetClass()->GetName(),
                        *Pair.Key));
                    AppendLine(Report, FString::Printf(
                        TEXT("ERROR: pin_defaults target '%s' was not found on node '%s'."),
                        *Pair.Key,
                        *Node->GetClass()->GetName()));
                    return false;
                }
                if (!ApplyAndValidate(Pin, Pair.Key, Pair.Value))
                {
                    return false;
                }
            }
        }

        // Convenience format: { "id":"Return", "type":"Return", "bResult": false }
        // Any non-reserved scalar field matching an input pin becomes its default value.
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : NodeObj->Values)
        {
            if (IsReservedNodeJsonField(Pair.Key))
            {
                continue;
            }
            if (!Pair.Value.IsValid() || (Pair.Value->Type != EJson::Boolean && Pair.Value->Type != EJson::Number && Pair.Value->Type != EJson::String))
            {
                continue;
            }
            if (UEdGraphPin* Pin = FindPinByLooseName(Node, Pair.Key, EGPD_Input))
            {
                if (!ApplyAndValidate(Pin, Pair.Key, Pair.Value))
                {
                    return false;
                }
            }
        }
        return true;
    }

    static UFunction* ResolveFunctionByNameInClassPath(const FString& ClassPath, const FString& FunctionName);

    static UFunction* ResolveFunction(const TSharedPtr<FJsonObject>& NodeObj, UBlueprint* Blueprint)
    {
        FString FunctionPath = GetStringFieldSafe(NodeObj, TEXT("function_path"));
        if (!FunctionPath.IsEmpty())
        {
            if (UFunction* Function = FindObject<UFunction>(ANY_PACKAGE, *FunctionPath))
            {
                return Function;
            }
            if (UFunction* Function = LoadObject<UFunction>(nullptr, *FunctionPath))
            {
                return Function;
            }
        }

        const FString FunctionName = GetStringFieldSafe(NodeObj, TEXT("function_name"), GetStringFieldSafe(NodeObj, TEXT("member_name"), GetStringFieldSafe(NodeObj, TEXT("name"))));
        if (FunctionName.IsEmpty())
        {
            return nullptr;
        }

        // Static Blueprint library calls such as UKismetMathLibrary::MakeTransform
        // are often specified as { class_path, function_name } in N2C patches.
        // Resolve that form before falling back to self/parent Blueprint functions.
        const FString OwnerClassPath = GetStringFieldSafe(NodeObj, TEXT("class_path"), GetStringFieldSafe(NodeObj, TEXT("owner_class_path")));
        if (!OwnerClassPath.IsEmpty())
        {
            if (UFunction* Function = ResolveFunctionByNameInClassPath(OwnerClassPath, FunctionName))
            {
                return Function;
            }
        }

        if (Blueprint && Blueprint->GeneratedClass)
        {
            if (UFunction* Function = Blueprint->GeneratedClass->FindFunctionByName(FName(*FunctionName)))
            {
                return Function;
            }
        }
        // Freshly added Blueprint functions may exist only on the skeleton class until the
        // Blueprint is fully compiled. Check SkeletonGeneratedClass as well so a later
        // action in the same patch can call a helper function created by an earlier action.
        if (Blueprint && Blueprint->SkeletonGeneratedClass)
        {
            if (UFunction* Function = Blueprint->SkeletonGeneratedClass->FindFunctionByName(FName(*FunctionName)))
            {
                return Function;
            }
        }
        if (Blueprint && Blueprint->ParentClass)
        {
            if (UFunction* Function = Blueprint->ParentClass->FindFunctionByName(FName(*FunctionName)))
            {
                return Function;
            }
        }
        return nullptr;
    }

    static UClass* ResolveNativeK2NodeClass(const FString& RequestedClassName)
    {
        FString ClassName = RequestedClassName;
        ClassName.TrimStartAndEndInline();
        if (ClassName.IsEmpty())
        {
            return nullptr;
        }

        if (ClassName.StartsWith(TEXT("/Script/")))
        {
            if (UClass* ExactClass = LoadObject<UClass>(nullptr, *ClassName))
            {
                return ExactClass;
            }
        }

        if (!ClassName.StartsWith(TEXT("K2Node_")))
        {
            ClassName = TEXT("K2Node_") + ClassName;
        }

        if (UClass* FoundClass = FindObject<UClass>(ANY_PACKAGE, *ClassName))
        {
            return FoundClass;
        }

        const FString BlueprintGraphPath = TEXT("/Script/BlueprintGraph.") + ClassName;
        if (UClass* LoadedClass = LoadObject<UClass>(nullptr, *BlueprintGraphPath))
        {
            return LoadedClass;
        }

        return nullptr;
    }

    static UEdGraphNode* CreateNativeK2NodeUnfinished(UEdGraph* Graph, const FString& NativeClassName, int32 PosX, int32 PosY, FString& Report)
    {
        if (!Graph)
        {
            return nullptr;
        }

        UClass* NodeClass = ResolveNativeK2NodeClass(NativeClassName);
        if (!NodeClass || !NodeClass->IsChildOf(UEdGraphNode::StaticClass()))
        {
            AppendLine(Report, FString::Printf(TEXT("WARNING: native K2 node class was not found or is not an EdGraphNode: %s"), *NativeClassName));
            return nullptr;
        }

        UEdGraphNode* Node = NewObject<UEdGraphNode>(Graph, NodeClass);
        if (!Node)
        {
            AppendLine(Report, FString::Printf(TEXT("WARNING: failed to allocate native K2 node: %s"), *NativeClassName));
            return nullptr;
        }

        Node->SetFlags(RF_Transactional);
        Node->NodePosX = PosX;
        Node->NodePosY = PosY;
        Graph->AddNode(Node, true, false);
        return Node;
    }

    static void FinishNativeK2Node(UEdGraphNode* Node)
    {
        if (!Node)
        {
            return;
        }
        Node->CreateNewGuid();
        Node->PostPlacedNewNode();
        Node->AllocateDefaultPins();
        Node->ReconstructNode();
    }

    static void FinishNativeK2NodePinsFirstNoReconstruct(UEdGraphNode* Node)
    {
        if (!Node)
        {
            return;
        }

        // Some UE4.27 K2 nodes, especially K2Node_AddComponent, read their
        // own pins from PostPlacedNewNode(). The generic finish order used by
        // many simple nodes calls PostPlacedNewNode before AllocateDefaultPins,
        // which trips K2Node_AddComponent.h line 62 (FoundPin != 0). For these
        // template-backed nodes we allocate pins first and avoid ReconstructNode
        // while the template context is still fresh.
        Node->CreateNewGuid();
        Node->AllocateDefaultPins();
        Node->PostPlacedNewNode();
    }

    static bool SetPinDefaultObjectByName(UEdGraphNode* Node, const TCHAR* PinName, UObject* DefaultObject, FString& Report)
    {
        if (!Node || !PinName || !DefaultObject)
        {
            return false;
        }
        if (UEdGraphPin* Pin = Node->FindPin(FName(PinName)))
        {
            return ApplySchemaPinDefaultValue(
                Pin,
                Node,
                DefaultObject->GetPathName(),
                FString::Printf(TEXT("%s.%s"), *Node->GetClass()->GetName(), PinName),
                Report);
        }
        return false;
    }

    static bool SetStructPropertyFromTextByName(UObject* Object, const TCHAR* PropertyName, const FString& Text)
    {
        if (!Object || !PropertyName || Text.IsEmpty()) return false;
        if (FStructProperty* StructProp = FindFProperty<FStructProperty>(Object->GetClass(), PropertyName))
        {
            return StructProp->ImportText(*Text, StructProp->ContainerPtrToValuePtr<void>(Object), PPF_None, Object) != nullptr;
        }
        return false;
    }

    static void ApplyGetEnumeratorNameContext(UEdGraphNode* Node, UEnum* Enum)
    {
        if (!Node || !Enum) return;
        UEdGraphPin* EnumeratorPin = Node->FindPin(TEXT("Enumerator"));
        if (EnumeratorPin)
        {
            EnumeratorPin->PinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
            EnumeratorPin->PinType.PinSubCategoryObject = Enum;
            EnumeratorPin->DefaultValue = Enum->GetNameStringByIndex(0);
        }
    }

    static void ApplyEnumLiteralContext(UEdGraphNode* Node, UEnum* Enum)
    {
        if (!Node || !Enum) return;
        const FString FirstValue = Enum->GetNameStringByIndex(0);
        for (const FName PinName : { FName(TEXT("Enum")), UEdGraphSchema_K2::PN_ReturnValue })
        {
            if (UEdGraphPin* Pin = Node->FindPin(PinName))
            {
                Pin->PinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
                Pin->PinType.PinSubCategoryObject = Enum;
                if (Pin->Direction == EGPD_Input) Pin->DefaultValue = FirstValue;
            }
        }
    }
    static void SetPinDefaultByName(UEdGraphNode* Node, const TCHAR* PinName, const FString& DefaultValue)
    {
        if (!Node || !PinName)
        {
            return;
        }
        if (UEdGraphPin* Pin = Node->FindPin(FName(PinName)))
        {
            Pin->DefaultValue = DefaultValue;
        }
    }

    static bool SetObjectPropertyByName(UObject* Object, const TCHAR* PropertyName, UObject* Value)
    {
        if (!Object || !PropertyName)
        {
            return false;
        }
        if (FObjectPropertyBase* ObjectProp = FindFProperty<FObjectPropertyBase>(Object->GetClass(), PropertyName))
        {
            ObjectProp->SetObjectPropertyValue_InContainer(Object, Value);
            return true;
        }
        return false;
    }

    static bool SetNamePropertyByName(UObject* Object, const TCHAR* PropertyName, const FString& Value)
    {
        if (!Object || !PropertyName || Value.IsEmpty())
        {
            return false;
        }
        if (FNameProperty* NameProp = FindFProperty<FNameProperty>(Object->GetClass(), PropertyName))
        {
            NameProp->SetPropertyValue_InContainer(Object, FName(*Value));
            return true;
        }
        return false;
    }

    static bool GetNamePropertyByName(UObject* Object, const TCHAR* PropertyName, FName& OutValue)
    {
        OutValue = NAME_None;
        if (!Object || !PropertyName)
        {
            return false;
        }
        if (FNameProperty* NameProp = FindFProperty<FNameProperty>(Object->GetClass(), PropertyName))
        {
            OutValue = NameProp->GetPropertyValue_InContainer(Object);
            return true;
        }
        return false;
    }

    static bool SetStringPropertyByName(UObject* Object, const TCHAR* PropertyName, const FString& Value)
    {
        if (!Object || !PropertyName)
        {
            return false;
        }
        if (FStrProperty* StrProp = FindFProperty<FStrProperty>(Object->GetClass(), PropertyName))
        {
            StrProp->SetPropertyValue_InContainer(Object, Value);
            return true;
        }
        return false;
    }

    static bool SetIntPropertyByName(UObject* Object, const TCHAR* PropertyName, int32 Value)
    {
        if (!Object || !PropertyName)
        {
            return false;
        }
        if (FIntProperty* IntProp = FindFProperty<FIntProperty>(Object->GetClass(), PropertyName))
        {
            IntProp->SetPropertyValue_InContainer(Object, Value);
            return true;
        }
        return false;
    }


    static bool SetBytePropertyByName(UObject* Object, const TCHAR* PropertyName, uint8 Value)
    {
        if (!Object || !PropertyName) return false;
        if (FByteProperty* ByteProp = FindFProperty<FByteProperty>(Object->GetClass(), PropertyName))
        {
            ByteProp->SetPropertyValue_InContainer(Object, Value);
            return true;
        }
        return false;
    }

    static bool SetBoolPropertyByName(UObject* Object, const TCHAR* PropertyName, bool bValue)
    {
        if (!Object || !PropertyName) return false;
        if (FBoolProperty* BoolProp = FindFProperty<FBoolProperty>(Object->GetClass(), PropertyName))
        {
            BoolProp->SetPropertyValue_InContainer(Object, bValue);
            return true;
        }
        return false;
    }
    static bool SetPinTypePropertyByName(UObject* Object, const TCHAR* PropertyName, const FEdGraphPinType& Value)
    {
        if (!Object || !PropertyName)
        {
            return false;
        }
        if (FStructProperty* StructProp = FindFProperty<FStructProperty>(Object->GetClass(), PropertyName))
        {
            StructProp->CopyCompleteValue(StructProp->ContainerPtrToValuePtr<void>(Object), &Value);
            return true;
        }
        return false;
    }

    static UClass* ResolveClassFromJson(const TSharedPtr<FJsonObject>& Obj)
    {
        FString ClassPath = GetStringFieldSafe(Obj, TEXT("class_path"));
        if (ClassPath.IsEmpty())
        {
            ClassPath = GetStringFieldSafe(Obj, TEXT("target_class"));
        }
        if (ClassPath.IsEmpty())
        {
            ClassPath = GetStringFieldSafe(Obj, TEXT("result_class_path"));
        }
        if (ClassPath.IsEmpty())
        {
            ClassPath = GetTypeObjectPathFromJson(Obj);
        }
        if (ClassPath.IsEmpty())
        {
            return nullptr;
        }
        if (UClass* ClassObj = FindObject<UClass>(ANY_PACKAGE, *ClassPath))
        {
            return ClassObj;
        }
        return LoadObject<UClass>(nullptr, *ClassPath);
    }

    static UFunction* ResolveFunctionByNameInClassPath(const FString& ClassPath, const FString& FunctionName)
    {
        if (ClassPath.IsEmpty() || FunctionName.IsEmpty())
        {
            return nullptr;
        }
        UClass* OwnerClass = FindObject<UClass>(ANY_PACKAGE, *ClassPath);
        if (!OwnerClass)
        {
            OwnerClass = LoadObject<UClass>(nullptr, *ClassPath);
        }
        return OwnerClass ? OwnerClass->FindFunctionByName(FName(*FunctionName)) : nullptr;
    }

    static UEdGraphNode* CreateCallFunctionNode(UEdGraph* Graph, UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& NodeObj, int32 PosX, int32 PosY, FString& Report, const FString& FallbackFunctionName = TEXT(""))
    {
        FGraphNodeCreator<UK2Node_CallFunction> Creator(*Graph);
        UK2Node_CallFunction* Call = Creator.CreateNode();
        UFunction* Function = ResolveFunction(NodeObj, Blueprint);
        FString FunctionName = GetStringFieldSafe(NodeObj, TEXT("function_name"), GetStringFieldSafe(NodeObj, TEXT("member_name"), GetStringFieldSafe(NodeObj, TEXT("name"))));
        if (!FallbackFunctionName.IsEmpty() && FunctionName.IsEmpty())
        {
            FunctionName = FallbackFunctionName;
        }
        if (!Function && !FallbackFunctionName.IsEmpty())
        {
            Function = ResolveFunctionByNameInClassPath(TEXT("/Script/Engine.KismetArrayLibrary"), FallbackFunctionName);
            if (!Function)
            {
                Function = ResolveFunctionByNameInClassPath(TEXT("/Script/Engine.KismetSystemLibrary"), FallbackFunctionName);
            }
        }

        if (Function)
        {
            Call->SetFromFunction(Function);
        }
        else if (!FunctionName.IsEmpty())
        {
            Call->FunctionReference.SetSelfMember(FName(*FunctionName));
            AppendLine(Report, FString::Printf(TEXT("WARNING: function '%s' was not resolved at import time; created self-member call."), *FunctionName));
        }
        else
        {
            AppendLine(Report, TEXT("WARNING: CallFunction node skipped because no function_name/member_name was provided."));
            return nullptr;
        }
        Call->NodePosX = PosX;
        Call->NodePosY = PosY;
        Creator.Finalize();
        return Call;
    }


    static UFunction* ResolveFunctionFromClassList(const TArray<FString>& ClassPaths, const FString& FunctionName)
    {
        if (FunctionName.IsEmpty())
        {
            return nullptr;
        }
        for (const FString& ClassPath : ClassPaths)
        {
            if (UFunction* Function = ResolveFunctionByNameInClassPath(ClassPath, FunctionName))
            {
                return Function;
            }
        }
        return nullptr;
    }

    static UEdGraphNode* CreateCallFunctionSubclassNode(UEdGraph* Graph, UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& NodeObj, const FString& NativeClassName, const FString& DefaultClassPath, const FString& FallbackFunctionName, int32 PosX, int32 PosY, FString& Report)
    {
        UEdGraphNode* RawNode = CreateNativeK2NodeUnfinished(Graph, NativeClassName, PosX, PosY, Report);
        UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(RawNode);
        if (!RawNode || !Call)
        {
            AppendLine(Report, FString::Printf(TEXT("WARNING: %s unavailable; falling back to plain CallFunction."), *NativeClassName));
            return CreateCallFunctionNode(Graph, Blueprint, NodeObj, PosX, PosY, Report, FallbackFunctionName);
        }

        UFunction* Function = ResolveFunction(NodeObj, Blueprint);
        FString FunctionName = GetStringFieldSafe(NodeObj, TEXT("function_name"), GetStringFieldSafe(NodeObj, TEXT("member_name"), GetStringFieldSafe(NodeObj, TEXT("name"))));
        if (FunctionName.IsEmpty())
        {
            FunctionName = FallbackFunctionName;
        }
        FString ClassPath = GetStringFieldSafe(NodeObj, TEXT("class_path"));
        if (ClassPath.IsEmpty())
        {
            ClassPath = DefaultClassPath;
        }
        if (!Function)
        {
            Function = ResolveFunctionByNameInClassPath(ClassPath, FunctionName);
        }

        if (Function)
        {
            Call->SetFromFunction(Function);
        }
        else if (!FunctionName.IsEmpty())
        {
            UClass* OwnerClass = nullptr;
            if (!ClassPath.IsEmpty())
            {
                OwnerClass = FindObject<UClass>(ANY_PACKAGE, *ClassPath);
                if (!OwnerClass)
                {
                    OwnerClass = LoadObject<UClass>(nullptr, *ClassPath);
                }
            }
            if (OwnerClass)
            {
                Call->FunctionReference.SetExternalMember(FName(*FunctionName), OwnerClass);
            }
            else
            {
                Call->FunctionReference.SetSelfMember(FName(*FunctionName));
            }
            AppendLine(Report, FString::Printf(TEXT("WARNING: %s function '%s' was not fully resolved; node was created with function reference fallback."), *NativeClassName, *FunctionName));
        }
        else
        {
            AppendLine(Report, FString::Printf(TEXT("WARNING: %s skipped because function_name/member_name is empty."), *NativeClassName));
            RawNode->DestroyNode();
            return nullptr;
        }

        if (UK2Node_CallFunctionOnMember* MemberCall = Cast<UK2Node_CallFunctionOnMember>(RawNode))
        {
            const FString MemberName = GetStringFieldSafe(NodeObj, TEXT("member_variable_name"), GetStringFieldSafe(NodeObj, TEXT("member_name_to_call_on")));
            const FString MemberOwnerPath = GetStringFieldSafe(NodeObj, TEXT("member_variable_owner_class"));
            if (MemberName.IsEmpty())
            {
                AppendLine(Report, TEXT("WARNING: K2Node_CallFunctionOnMember skipped because member_variable_name is empty."));
                RawNode->DestroyNode();
                return nullptr;
            }
            UClass* MemberOwner = MemberOwnerPath.IsEmpty() ? nullptr : FindObject<UClass>(ANY_PACKAGE, *MemberOwnerPath);
            if (!MemberOwner && !MemberOwnerPath.IsEmpty())
            {
                MemberOwner = LoadObject<UClass>(nullptr, *MemberOwnerPath);
            }
            if (MemberOwner)
            {
                MemberCall->MemberVariableToCallOn.SetExternalMember(FName(*MemberName), MemberOwner);
            }
            else
            {
                MemberCall->MemberVariableToCallOn.SetSelfMember(FName(*MemberName));
            }
        }

        FinishNativeK2Node(RawNode);
        return RawNode;
    }

    static UEdGraphNode* CreateGetArrayItemNode(UEdGraph* Graph, const TSharedPtr<FJsonObject>& NodeObj, int32 PosX, int32 PosY)
    {
        FGraphNodeCreator<UK2Node_GetArrayItem> Creator(*Graph);
        UK2Node_GetArrayItem* Node = Creator.CreateNode();
        // SetDesiredReturnType is declared by BlueprintGraph but not exported from
        // UE4.27, so calling it from a plugin produces LNK2019.  Set the serialized
        // desired state before Finalize() instead; AllocateDefaultPins reads it.
        if (FBoolProperty* ReturnByRef = FindFProperty<FBoolProperty>(Node->GetClass(), TEXT("bReturnByRefDesired")))
        {
            ReturnByRef->SetPropertyValue_InContainer(Node, GetBoolFieldSafe(NodeObj, TEXT("return_by_ref"), false));
        }
        Node->NodePosX = PosX;
        Node->NodePosY = PosY;
        Creator.Finalize();
        return Node;
    }

    static UEdGraphNode* CreateBuiltinEventNode(UEdGraph* Graph, UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& NodeObj, int32 PosX, int32 PosY, FString& Report)
    {
        const FString EventName = GetStringFieldSafe(NodeObj, TEXT("event_name"), GetStringFieldSafe(NodeObj, TEXT("member_name"), GetStringFieldSafe(NodeObj, TEXT("name"))));
        const FString OwnerPath = GetStringFieldSafe(NodeObj, TEXT("event_owner_class"), GetStringFieldSafe(NodeObj, TEXT("function_owner_class"), GetStringFieldSafe(NodeObj, TEXT("owner_class_path"))));
        if (EventName.IsEmpty())
        {
            AppendLine(Report, TEXT("WARNING: built-in event skipped because event_name/member_name is empty."));
            return nullptr;
        }

        UClass* OwnerClass = OwnerPath.IsEmpty() ? (Blueprint ? Blueprint->ParentClass : nullptr) : FindObject<UClass>(ANY_PACKAGE, *OwnerPath);
        if (!OwnerClass && !OwnerPath.IsEmpty())
        {
            OwnerClass = LoadObject<UClass>(nullptr, *OwnerPath);
        }
        if (!OwnerClass || !OwnerClass->FindFunctionByName(FName(*EventName)))
        {
            AppendLine(Report, FString::Printf(TEXT("WARNING: built-in event '%s' could not be resolved on '%s'; skipped before graph mutation."), *EventName, *OwnerPath));
            return nullptr;
        }

        // Prefer an existing compatible event in a patch graph.  Blueprint permits only
        // one implementation of a given parent/interface event.
        for (UEdGraphNode* Existing : Graph->Nodes)
        {
            if (UK2Node_Event* ExistingEvent = Cast<UK2Node_Event>(Existing))
            {
                if (ExistingEvent->EventReference.GetMemberName() == FName(*EventName))
                {
                    AppendLine(Report, FString::Printf(TEXT("CHANGE: reused existing built-in event '%s'."), *EventName));
                    return ExistingEvent;
                }
            }
        }

        FGraphNodeCreator<UK2Node_Event> Creator(*Graph);
        UK2Node_Event* Node = Creator.CreateNode();
        Node->EventReference.SetExternalMember(FName(*EventName), OwnerClass);
        Node->bOverrideFunction = true;
        Node->NodePosX = PosX;
        Node->NodePosY = PosY;
        Creator.Finalize();
        return Node;
    }

    static UEdGraphNode* CreateBinaryOperatorNode(UEdGraph* Graph, UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& NodeObj, int32 PosX, int32 PosY, FString& Report)
    {
        FString FunctionName = GetStringFieldSafe(NodeObj, TEXT("function_name"), GetStringFieldSafe(NodeObj, TEXT("member_name"), GetStringFieldSafe(NodeObj, TEXT("name"))));
        if (FunctionName.IsEmpty())
        {
            FunctionName = TEXT("BooleanAND");
        }
        return CreateCallFunctionSubclassNode(Graph, Blueprint, NodeObj, TEXT("K2Node_CommutativeAssociativeBinaryOperator"), TEXT("/Script/Engine.KismetMathLibrary"), FunctionName, PosX, PosY, Report);
    }

    static UEdGraphNode* CreateEnumComparisonNode(UEdGraph* Graph, UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& NodeObj, const FString& NativeClassName, int32 PosX, int32 PosY, FString& Report)
    {
        UEdGraphNode* Node = CreateNativeK2NodeUnfinished(Graph, NativeClassName, PosX, PosY, Report);
        if (!Node)
        {
            return nullptr;
        }
        UEnum* EnumType = Cast<UEnum>(ResolveTypeObjectFromJson(NodeObj));
        FinishNativeK2Node(Node);
        if (EnumType)
        {
            const FString Selected = GetStringFieldSafe(NodeObj, TEXT("enum_value"), GetStringFieldSafe(NodeObj, TEXT("selected_value"), EnumType->GetNameStringByIndex(0)));
            for (UEdGraphPin* Pin : Node->Pins)
            {
                if (!Pin || Pin->Direction != EGPD_Input || Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Byte) continue;
                Pin->PinType.PinSubCategoryObject = EnumType;
                if (Pin->DefaultValue.IsEmpty()) Pin->DefaultValue = Selected;
            }
        }
        return Node;
    }

    static UDataTable* ResolveDataTableFromJson(const TSharedPtr<FJsonObject>& NodeObj)
    {
        if (!NodeObj.IsValid())
        {
            return nullptr;
        }

        FString DataTablePath = GetStringFieldSafe(NodeObj, TEXT("data_table_path"), GetStringFieldSafe(NodeObj, TEXT("data_table")));
        if (DataTablePath.IsEmpty())
        {
            const TSharedPtr<FJsonObject>* DefaultsObj = nullptr;
            if (NodeObj->TryGetObjectField(TEXT("pin_defaults"), DefaultsObj) && DefaultsObj && DefaultsObj->IsValid())
            {
                DataTablePath = GetStringFieldSafe(*DefaultsObj, TEXT("DataTable"), GetStringFieldSafe(*DefaultsObj, TEXT("Data Table")));
            }
        }
        if (DataTablePath.IsEmpty())
        {
            return nullptr;
        }

        if (UDataTable* Existing = FindObject<UDataTable>(ANY_PACKAGE, *DataTablePath))
        {
            return Existing;
        }
        if (UDataTable* Loaded = LoadObject<UDataTable>(nullptr, *DataTablePath))
        {
            return Loaded;
        }
        return Cast<UDataTable>(StaticLoadObject(UDataTable::StaticClass(), nullptr, *DataTablePath));
    }

    static UEdGraphNode* CreateGetDataTableRowNode(UEdGraph* Graph, const TSharedPtr<FJsonObject>& NodeObj, int32 PosX, int32 PosY, FString& Report)
    {
        const bool bTablePinLinked = GetBoolFieldSafe(NodeObj, TEXT("data_table_pin_linked"), false);
        UDataTable* DataTable = ResolveDataTableFromJson(NodeObj);
        const FString RequestedRowStructPath = GetStringFieldSafe(NodeObj, TEXT("row_struct_path"), GetStringFieldSafe(NodeObj, TEXT("struct_path")));
        UScriptStruct* RequestedRowStruct = RequestedRowStructPath.IsEmpty() ? nullptr : FindObject<UScriptStruct>(ANY_PACKAGE, *RequestedRowStructPath);
        if (!RequestedRowStruct && !RequestedRowStructPath.IsEmpty()) RequestedRowStruct = LoadObject<UScriptStruct>(nullptr, *RequestedRowStructPath);
        if ((!bTablePinLinked && (!DataTable || !DataTable->GetRowStruct())) || (bTablePinLinked && !RequestedRowStruct))
        {
            AppendLine(Report, FString::Printf(TEXT("N2C_PREFLIGHT_GUARD|code=datatable_identity_missing|variant=%s"), bTablePinLinked ? TEXT("linked") : TEXT("literal")));
            AppendLine(Report, TEXT("WARNING: typed GetDataTableRow skipped because its literal table or linked-pin row struct identity did not resolve."));
            return nullptr;
        }

        if (!bTablePinLinked && !RequestedRowStructPath.IsEmpty() && DataTable->GetRowStruct()->GetPathName() != RequestedRowStructPath)
        {
            AppendLine(Report, FString::Printf(TEXT("WARNING: typed GetDataTableRow skipped because requested row struct '%s' does not match DataTable row struct '%s'."), *RequestedRowStructPath, *DataTable->GetRowStruct()->GetPathName()));
            return nullptr;
        }

        FGraphNodeCreator<UK2Node_GetDataTableRow> Creator(*Graph);
        UK2Node_GetDataTableRow* Node = Creator.CreateNode();
        Node->NodePosX = PosX;
        Node->NodePosY = PosY;
        Creator.Finalize();

        UEdGraphPin* DataTablePin = Node->GetDataTablePin();
        if (!DataTablePin)
        {
            AppendLine(Report, TEXT("WARNING: typed GetDataTableRow allocation produced no DataTable input pin."));
            Node->DestroyNode();
            return nullptr;
        }

        if (!bTablePinLinked)
        {
            if (!ApplySchemaPinDefaultValue(
                    DataTablePin,
                    Node,
                    DataTable->GetPathName(),
                    TEXT("K2Node_GetDataTableRow.DataTable"),
                    Report))
            {
                Node->DestroyNode();
                return nullptr;
            }
        }
        else
        {
            DataTablePin->DefaultObject = nullptr;
            DataTablePin->DefaultValue.Empty();
            if (UEdGraphPin* ResultPin = Node->GetResultPin())
            {
                ResultPin->PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
                ResultPin->PinType.PinSubCategoryObject = RequestedRowStruct;
            }
        }

        if (UEdGraphPin* RowNamePin = Node->GetRowNamePin())
        {
            const FString RowName = GetStringFieldSafe(NodeObj, TEXT("row_name_default"));
            if (!RowName.IsEmpty() && !ApplySchemaPinDefaultValue(
                    RowNamePin,
                    Node,
                    RowName,
                    TEXT("K2Node_GetDataTableRow.RowName"),
                    Report))
            {
                Node->DestroyNode();
                return nullptr;
            }
        }

        UScriptStruct* ExpectedRowStruct = bTablePinLinked ? RequestedRowStruct : const_cast<UScriptStruct*>(DataTable->GetRowStruct());
        UEdGraphPin* ResultPin = Node->GetResultPin();
        if (!ResultPin || ResultPin->PinType.PinSubCategoryObject.Get() != ExpectedRowStruct)
        {
            Node->ReconstructNode();
            ResultPin = Node->GetResultPin();
        }
        if (!ResultPin || ResultPin->PinType.PinSubCategoryObject.Get() != ExpectedRowStruct)
        {
            AppendLine(Report, FString::Printf(TEXT("WARNING: typed GetDataTableRow failed to allocate Out Row as '%s'."), *ExpectedRowStruct->GetPathName()));
            Node->DestroyNode();
            return nullptr;
        }

        AppendLine(Report, FString::Printf(TEXT("CHANGE: typed GetDataTableRow created: variant='%s' table='%s' row_struct='%s'."), bTablePinLinked ? TEXT("linked") : TEXT("literal"), DataTable ? *DataTable->GetPathName() : TEXT("<linked>"), *ExpectedRowStruct->GetPathName()));
        return Node;
    }

    static UEdGraphNode* CreateCallArrayFunctionNode(UEdGraph* Graph, UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& NodeObj, int32 PosX, int32 PosY, FString& Report, const FString& FallbackFunctionName)
    {
        // UE4.27 array library functions such as Array_Length are wildcard/custom-thunk nodes.
        // If they are imported as a plain UK2Node_CallFunction, the connected array pin may
        // look typed until the asset is reopened, then it reverts to wildcard and the Blueprint
        // compiles with "Target Array is undetermined". Create the BlueprintGraph
        // K2Node_CallArrayFunction class instead, while still using UK2Node_CallFunction APIs.
        UEdGraphNode* RawNode = CreateNativeK2NodeUnfinished(Graph, TEXT("K2Node_CallArrayFunction"), PosX, PosY, Report);
        UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(RawNode);
        if (!RawNode || !Call)
        {
            AppendLine(Report, FString::Printf(TEXT("WARNING: K2Node_CallArrayFunction was unavailable for %s; falling back to plain CallFunction."), *FallbackFunctionName));
            return CreateCallFunctionNode(Graph, Blueprint, NodeObj, PosX, PosY, Report, FallbackFunctionName);
        }

        UFunction* Function = ResolveFunction(NodeObj, Blueprint);
        FString FunctionName = GetStringFieldSafe(NodeObj, TEXT("function_name"), GetStringFieldSafe(NodeObj, TEXT("member_name"), GetStringFieldSafe(NodeObj, TEXT("name"))));
        if (!FallbackFunctionName.IsEmpty() && FunctionName.IsEmpty())
        {
            FunctionName = FallbackFunctionName;
        }
        if (!Function && !FallbackFunctionName.IsEmpty())
        {
            Function = ResolveFunctionByNameInClassPath(TEXT("/Script/Engine.KismetArrayLibrary"), FallbackFunctionName);
        }

        if (Function)
        {
            Call->SetFromFunction(Function);
        }
        else if (!FunctionName.IsEmpty())
        {
            Call->FunctionReference.SetExternalMember(FName(*FunctionName), ResolveClassFromJson(NodeObj) ? ResolveClassFromJson(NodeObj) : LoadObject<UClass>(nullptr, TEXT("/Script/Engine.KismetArrayLibrary")));
            AppendLine(Report, FString::Printf(TEXT("WARNING: array function '%s' was not fully resolved; created external array function call."), *FunctionName));
        }
        else
        {
            AppendLine(Report, TEXT("WARNING: CallArrayFunction node skipped because no function name was provided."));
            RawNode->DestroyNode();
            return nullptr;
        }

        FinishNativeK2Node(RawNode);
        return RawNode;
    }

    static FString NormalizeMacroName(FString MacroName)
    {
        MacroName.TrimStartAndEndInline();
        const FString Lower = MacroName.ToLower().Replace(TEXT(" "), TEXT("")).Replace(TEXT("_"), TEXT(""));
        if (Lower == TEXT("isvalid")) return TEXT("IsValid");
        if (Lower == TEXT("doonce")) return TEXT("DoOnce");
        if (Lower == TEXT("gate")) return TEXT("Gate");
        if (Lower == TEXT("flipflop")) return TEXT("FlipFlop");
        if (Lower == TEXT("foreachloop")) return TEXT("ForEachLoop");
        if (Lower == TEXT("foreachloopwithbreak")) return TEXT("ForEachLoopWithBreak");
        if (Lower == TEXT("forloop")) return TEXT("ForLoop");
        if (Lower == TEXT("forloopwithbreak")) return TEXT("ForLoopWithBreak");
        if (Lower == TEXT("whileloop")) return TEXT("WhileLoop");
        return MacroName;
    }

    static UEdGraph* FindMacroGraphInBlueprint(UBlueprint* MacroBlueprint, const FString& MacroName)
    {
        if (!MacroBlueprint)
        {
            return nullptr;
        }
        for (UEdGraph* MacroGraph : MacroBlueprint->MacroGraphs)
        {
            if (MacroGraph && (MacroGraph->GetName() == MacroName || MacroGraph->GetName().Equals(MacroName, ESearchCase::IgnoreCase)))
            {
                return MacroGraph;
            }
        }
        return nullptr;
    }

    static UEdGraph* FindMacroGraphByName(const FString& RequestedMacroName, const TSharedPtr<FJsonObject>& NodeObj = nullptr, UBlueprint* OwningBlueprint = nullptr)
    {
        const FString MacroName = NormalizeMacroName(RequestedMacroName);
        if (MacroName.IsEmpty())
        {
            return nullptr;
        }

        if (NodeObj.IsValid())
        {
            const FString DurableOwnerPath = GetStringFieldSafe(NodeObj, TEXT("macro_owner_path"));
            const FString DurableGraphPath = GetStringFieldSafe(NodeObj, TEXT("macro_graph_path"));
            if (!DurableOwnerPath.IsEmpty() || !DurableGraphPath.IsEmpty())
            {
                UEdGraph* DurableGraph = nullptr; FString ErrorCode, Detail;
                return FN2CMacroReference::ResolveAndValidate(NodeObj, OwningBlueprint, DurableGraph, ErrorCode, Detail) ? DurableGraph : nullptr;
            }
            FString MacroGraphPath = GetStringFieldSafe(NodeObj, TEXT("macro_graph_path"), GetStringFieldSafe(NodeObj, TEXT("macro_path")));
            if (!MacroGraphPath.IsEmpty())
            {
                if (UEdGraph* ExactGraph = FindObject<UEdGraph>(ANY_PACKAGE, *MacroGraphPath))
                {
                    return ExactGraph;
                }
                if (UEdGraph* LoadedGraph = LoadObject<UEdGraph>(nullptr, *MacroGraphPath))
                {
                    return LoadedGraph;
                }
            }

            const FString MacroBlueprintPath = GetStringFieldSafe(NodeObj, TEXT("macro_blueprint_path"), GetStringFieldSafe(NodeObj, TEXT("owner_blueprint_path")));
            if (!MacroBlueprintPath.IsEmpty())
            {
                UBlueprint* MacroBlueprint = FindObject<UBlueprint>(ANY_PACKAGE, *MacroBlueprintPath);
                if (!MacroBlueprint)
                {
                    MacroBlueprint = LoadObject<UBlueprint>(nullptr, *MacroBlueprintPath);
                }
                if (UEdGraph* ProjectMacro = FindMacroGraphInBlueprint(MacroBlueprint, MacroName))
                {
                    return ProjectMacro;
                }
            }
        }

        if (UEdGraph* LocalMacro = FindMacroGraphInBlueprint(OwningBlueprint, MacroName))
        {
            return LocalMacro;
        }

        TArray<FString> MacroBlueprintPaths;
        MacroBlueprintPaths.Add(TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros"));
        MacroBlueprintPaths.Add(TEXT("/Engine/EngineResources/StandardMacros.StandardMacros"));

        for (const FString& MacroBlueprintPath : MacroBlueprintPaths)
        {
            UBlueprint* MacroBlueprint = LoadObject<UBlueprint>(nullptr, *MacroBlueprintPath);
            if (UEdGraph* MacroGraph = FindMacroGraphInBlueprint(MacroBlueprint, MacroName))
            {
                return MacroGraph;
            }
        }
        return nullptr;
    }

    static UEdGraphNode* CreateMacroInstanceNode(UEdGraph* Graph, const FString& RequestedMacroName, int32 PosX, int32 PosY, FString& Report, const TSharedPtr<FJsonObject>& NodeObj = nullptr, UBlueprint* OwningBlueprint = nullptr)
    {
        const FString MacroName = NormalizeMacroName(RequestedMacroName);
        UEdGraph* MacroGraph = FindMacroGraphByName(MacroName, NodeObj, OwningBlueprint);
        if (!MacroGraph)
        {
            AppendLine(Report, FString::Printf(TEXT("WARNING: macro graph was not found: %s"), *MacroName));
            return nullptr;
        }

        FGraphNodeCreator<UK2Node_MacroInstance> Creator(*Graph);
        UK2Node_MacroInstance* MacroNode = Creator.CreateNode();
        MacroNode->SetMacroGraph(MacroGraph);
        const TSharedPtr<FJsonObject>* ResolvedWildcard = nullptr;
        if (NodeObj.IsValid() && NodeObj->TryGetObjectField(TEXT("resolved_wildcard_type"), ResolvedWildcard) && ResolvedWildcard && ResolvedWildcard->IsValid())
        {
            MacroNode->ResolvedWildcardType = MakePinTypeFromJson(*ResolvedWildcard);
        }
        MacroNode->NodePosX = PosX;
        MacroNode->NodePosY = PosY;
        Creator.Finalize();
        MacroNode->ReconstructNode();
        const TArray<TSharedPtr<FJsonValue>>* PinContract = nullptr;
        if (NodeObj.IsValid() && NodeObj->TryGetArrayField(TEXT("instance_pin_contract"), PinContract) && PinContract)
        {
            for (const TSharedPtr<FJsonValue>& PinValue : *PinContract)
            {
                const TSharedPtr<FJsonObject> PinObject = PinValue.IsValid() ? PinValue->AsObject() : nullptr;
                if (!PinObject.IsValid()) continue;
                const FString PinName = GetStringFieldSafe(PinObject, TEXT("name"));
                const FString Direction = GetStringFieldSafe(PinObject, TEXT("direction"));
                const TSharedPtr<FJsonObject>* PinTypeObject = nullptr;
                if (!PinObject->TryGetObjectField(TEXT("pin_type"), PinTypeObject) || !PinTypeObject || !PinTypeObject->IsValid()) continue;
                for (UEdGraphPin* Pin : MacroNode->Pins)
                {
                    if (Pin && Pin->PinName.ToString() == PinName && ((Direction == TEXT("input") && Pin->Direction == EGPD_Input) || (Direction == TEXT("output") && Pin->Direction == EGPD_Output)))
                    {
                        Pin->PinType = MakePinTypeFromJson(*PinTypeObject);
                        break;
                    }
                }
            }
        }
        AppendLine(Report, FString::Printf(TEXT("CHANGE: MacroInstance created: %s (%s)."), *MacroName, *MacroGraph->GetPathName()));
        return MacroNode;
    }

    static UEdGraphNode* CreateStructBackedNode(UEdGraph* Graph, const TSharedPtr<FJsonObject>& NodeObj, const FString& NativeClassName, int32 PosX, int32 PosY, FString& Report)
    {
        UObject* TypeObject = ResolveTypeObjectFromJson(NodeObj);
        UScriptStruct* StructType = Cast<UScriptStruct>(TypeObject);
        if (!StructType)
        {
            AppendLine(Report, FString::Printf(TEXT("WARNING: struct-backed node skipped because struct type could not be resolved from '%s'."), *GetTypeObjectPathFromJson(NodeObj)));
            return nullptr;
        }

        if ((NativeClassName == TEXT("K2Node_MakeStruct") || NativeClassName == TEXT("K2Node_BreakStruct")) && ShouldGuardGenericStructMakeBreak(StructType))
        {
            AppendLine(Report, FString::Printf(TEXT("WARNING: %s guarded/skipped for native make/break struct '%s'. Use the specialized Kismet make/break function instead."), *NativeClassName, *StructType->GetPathName()));
            return nullptr;
        }

        UEdGraphNode* Node = CreateNativeK2NodeUnfinished(Graph, NativeClassName, PosX, PosY, Report);
        if (!Node)
        {
            return nullptr;
        }
        SetObjectPropertyByName(Node, TEXT("StructType"), StructType);
        FinishNativeK2Node(Node);
        return Node;
    }

    struct FN2CRequestedStructField
    {
        FString Label;
        TArray<FString> NormalizedNames;
        FString GuidDigits;
    };

    static FString NormalizeGuidDigits(FString Value)
    {
        Value.ReplaceInline(TEXT("-"), TEXT(""));
        Value.ReplaceInline(TEXT("{"), TEXT(""));
        Value.ReplaceInline(TEXT("}"), TEXT(""));
        Value.TrimStartAndEndInline();
        return Value.ToLower();
    }

    static bool IsAllDigits(const FString& Value)
    {
        if (Value.IsEmpty()) return false;
        for (const TCHAR Ch : Value)
        {
            if (!FChar::IsDigit(Ch)) return false;
        }
        return true;
    }

    static bool IsAllHexDigits(const FString& Value)
    {
        if (Value.IsEmpty()) return false;
        for (const TCHAR Ch : Value)
        {
            if (!FChar::IsHexDigit(Ch)) return false;
        }
        return true;
    }

    static FString UserDefinedStructFriendlyBase(const FString& InternalName)
    {
        int32 LastUnderscore = INDEX_NONE;
        if (!InternalName.FindLastChar(TEXT('_'), LastUnderscore) || LastUnderscore <= 0)
        {
            return InternalName;
        }

        const FString GuidSuffix = InternalName.Mid(LastUnderscore + 1);
        if (GuidSuffix.Len() != 32 || !IsAllHexDigits(GuidSuffix))
        {
            return InternalName;
        }

        const FString BeforeGuid = InternalName.Left(LastUnderscore);
        int32 IndexUnderscore = INDEX_NONE;
        if (!BeforeGuid.FindLastChar(TEXT('_'), IndexUnderscore) || IndexUnderscore <= 0)
        {
            return InternalName;
        }

        const FString GeneratedIndex = BeforeGuid.Mid(IndexUnderscore + 1);
        return IsAllDigits(GeneratedIndex) ? BeforeGuid.Left(IndexUnderscore) : InternalName;
    }

    static void AddNormalizedStructFieldName(TArray<FString>& Names, const FString& RawName)
    {
        if (RawName.IsEmpty()) return;
        const FString Normalized = NormalizeLoosePinName(RawName);
        if (!Normalized.IsEmpty()) Names.AddUnique(Normalized);
        const FString FriendlyBase = NormalizeLoosePinName(UserDefinedStructFriendlyBase(RawName));
        if (!FriendlyBase.IsEmpty()) Names.AddUnique(FriendlyBase);
    }

    static void AddRequestedStructField(
        TArray<FN2CRequestedStructField>& Requests,
        const FString& RawName,
        const FString& PersistentGuid = FString())
    {
        if (RawName.IsEmpty() && PersistentGuid.IsEmpty()) return;

        FN2CRequestedStructField Incoming;
        Incoming.Label = RawName.IsEmpty() ? PersistentGuid : RawName;
        AddNormalizedStructFieldName(Incoming.NormalizedNames, RawName);
        Incoming.GuidDigits = NormalizeGuidDigits(PersistentGuid);

        for (FN2CRequestedStructField& Existing : Requests)
        {
            bool bSame = !Incoming.GuidDigits.IsEmpty() && Incoming.GuidDigits == Existing.GuidDigits;
            if (!bSame)
            {
                for (const FString& Name : Incoming.NormalizedNames)
                {
                    if (Existing.NormalizedNames.Contains(Name))
                    {
                        bSame = true;
                        break;
                    }
                }
            }
            if (bSame)
            {
                for (const FString& Name : Incoming.NormalizedNames) Existing.NormalizedNames.AddUnique(Name);
                if (Existing.GuidDigits.IsEmpty()) Existing.GuidDigits = Incoming.GuidDigits;
                return;
            }
        }

        Requests.Add(MoveTemp(Incoming));
    }

    static void CollectRequestedStructFields(
        const TSharedPtr<FJsonObject>& NodeObj,
        TArray<FN2CRequestedStructField>& OutFields)
    {
        if (!NodeObj.IsValid()) return;

        const TArray<TSharedPtr<FJsonValue>>* IdentityValues = nullptr;
        if (NodeObj->TryGetArrayField(TEXT("member_pin_identity"), IdentityValues) && IdentityValues)
        {
            for (const TSharedPtr<FJsonValue>& Value : *IdentityValues)
            {
                const TSharedPtr<FJsonObject> Identity = Value.IsValid() ? Value->AsObject() : nullptr;
                AddRequestedStructField(
                    OutFields,
                    GetStringFieldSafe(Identity, TEXT("pin_name")),
                    GetStringFieldSafe(Identity, TEXT("persistent_guid")));
            }
        }

        TArray<FString> FieldNames;
        GetStringArrayField(NodeObj, TEXT("show_fields"), FieldNames);
        GetStringArrayField(NodeObj, TEXT("fields_to_set"), FieldNames);
        for (const FString& FieldName : FieldNames)
        {
            AddRequestedStructField(OutFields, FieldName);
        }

        const TSharedPtr<FJsonObject>* FieldsObj = nullptr;
        if (NodeObj->TryGetObjectField(TEXT("fields"), FieldsObj) && FieldsObj && FieldsObj->IsValid())
        {
            for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*FieldsObj)->Values)
            {
                AddRequestedStructField(OutFields, Pair.Key);
            }
        }

        const TSharedPtr<FJsonObject>* DefaultsObj = nullptr;
        if (NodeObj->TryGetObjectField(TEXT("pin_defaults"), DefaultsObj) && DefaultsObj && DefaultsObj->IsValid())
        {
            for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*DefaultsObj)->Values)
            {
                const FString Normalized = NormalizeLoosePinName(Pair.Key);
                if (Normalized != TEXT("structref") && Normalized != TEXT("structout") &&
                    Normalized != TEXT("execute") && Normalized != TEXT("then"))
                {
                    AddRequestedStructField(OutFields, Pair.Key);
                }
            }
        }
    }

    static bool RequestedStructFieldMatchesNames(
        const FN2CRequestedStructField& Request,
        const FString& InternalName,
        const FString& FriendlyName)
    {
        const FString NormalizedInternal = NormalizeLoosePinName(InternalName);
        const FString NormalizedFriendly = NormalizeLoosePinName(FriendlyName);
        const FString NormalizedBase = NormalizeLoosePinName(UserDefinedStructFriendlyBase(InternalName));
        const FString CompactInternal = NormalizeGuidDigits(InternalName);

        if (!Request.GuidDigits.IsEmpty() && CompactInternal.Contains(Request.GuidDigits))
        {
            return true;
        }

        for (const FString& RequestedName : Request.NormalizedNames)
        {
            if (RequestedName == NormalizedInternal || RequestedName == NormalizedFriendly ||
                RequestedName == NormalizedBase ||
                IsLikelyUserDefinedStructInternalPinName(InternalName, RequestedName))
            {
                return true;
            }
        }
        return false;
    }

    static bool RequestedStructFieldMatchesProperty(
        const FN2CRequestedStructField& Request,
        const FProperty* Property)
    {
        if (!Property) return false;
        return RequestedStructFieldMatchesNames(
            Request,
            Property->GetName(),
            Property->GetDisplayNameText().ToString());
    }

    static bool ValidateRequestedStructFields(
        UScriptStruct* StructType,
        const TSharedPtr<FJsonObject>& NodeObj,
        FString& Report)
    {
        if (!StructType || !NodeObj.IsValid()) return false;

        TArray<FN2CRequestedStructField> Requests;
        CollectRequestedStructFields(NodeObj, Requests);
        const bool bShowAll = GetBoolFieldSafe(NodeObj, TEXT("show_all_fields"), false);
        if (Requests.Num() == 0)
        {
            if (bShowAll) return true;
            AppendLine(Report, TEXT("N2C_PREFLIGHT_GUARD|code=struct_member_identity_missing"));
            AppendLine(Report, TEXT("ERROR: SetFieldsInStruct requires member_pin_identity, show_fields, fields_to_set, fields, pin_defaults or show_all_fields before mutation."));
            return false;
        }

        bool bAllMatched = true;
        for (const FN2CRequestedStructField& Request : Requests)
        {
            bool bMatched = false;
            for (TFieldIterator<FProperty> It(StructType); It; ++It)
            {
                if (RequestedStructFieldMatchesProperty(Request, *It))
                {
                    bMatched = true;
                    break;
                }
            }
            if (!bMatched)
            {
                bAllMatched = false;
                AppendLine(Report, FString::Printf(
                    TEXT("N2C_PREFLIGHT_GUARD|code=struct_member_mismatch|member=%s"),
                    *Request.Label));
                AppendLine(Report, FString::Printf(
                    TEXT("ERROR: SetFieldsInStruct member '%s' was not found in '%s'. Friendly name, internal name and persistent GUID were checked before mutation."),
                    *Request.Label,
                    *StructType->GetPathName()));
            }
        }
        return bAllMatched;
    }

    static UEdGraphNode* CreateSetFieldsInStructNode(UEdGraph* Graph, const TSharedPtr<FJsonObject>& NodeObj, int32 PosX, int32 PosY, FString& Report)
    {
        UScriptStruct* StructType = Cast<UScriptStruct>(ResolveTypeObjectFromJson(NodeObj));
        if (!StructType)
        {
            AppendLine(Report, FString::Printf(TEXT("ERROR: SetFieldsInStruct could not resolve struct type from '%s'."), *GetTypeObjectPathFromJson(NodeObj)));
            return nullptr;
        }

        if (!ValidateRequestedStructFields(StructType, NodeObj, Report))
        {
            return nullptr;
        }

        FGraphNodeCreator<UK2Node_SetFieldsInStruct> Creator(*Graph);
        UK2Node_SetFieldsInStruct* Node = Creator.CreateNode();
        Node->StructType = StructType;
        Node->NodePosX = PosX;
        Node->NodePosY = PosY;
        Creator.Finalize();

        TArray<FN2CRequestedStructField> Requests;
        CollectRequestedStructFields(NodeObj, Requests);
        const bool bShowAll = GetBoolFieldSafe(NodeObj, TEXT("show_all_fields"), false);
        TArray<bool> Matched;
        Matched.Init(false, Requests.Num());

        int32 ShownCount = 0;
        for (FOptionalPinFromProperty& OptionalPin : Node->ShowPinForProperties)
        {
            bool bShow = bShowAll;
            for (int32 RequestIndex = 0; RequestIndex < Requests.Num(); ++RequestIndex)
            {
                if (RequestedStructFieldMatchesNames(
                    Requests[RequestIndex],
                    OptionalPin.PropertyName.ToString(),
                    OptionalPin.PropertyFriendlyName))
                {
                    Matched[RequestIndex] = true;
                    bShow = true;
                }
            }
            OptionalPin.bShowPin = bShow;
            ShownCount += bShow ? 1 : 0;
        }
        Node->ReconstructNode();

        bool bAllMatched = true;
        for (int32 Index = 0; Index < Requests.Num(); ++Index)
        {
            if (!Matched[Index])
            {
                bAllMatched = false;
                AppendLine(Report, FString::Printf(
                    TEXT("ERROR: SetFieldsInStruct failed to expose requested member '%s' after node allocation."),
                    *Requests[Index].Label));
            }
        }

        if (!bAllMatched || (Requests.Num() > 0 && ShownCount == 0))
        {
            Node->DestroyNode();
            return nullptr;
        }

        AppendLine(Report, FString::Printf(
            TEXT("CHANGE: SetFieldsInStruct created: struct='%s' shown_fields=%d requested_fields=%d."),
            *StructType->GetPathName(),
            ShownCount,
            Requests.Num()));
        return Node;
    }

    static FString JsonScalarToImportText(const TSharedPtr<FJsonValue>& Value)
    {
        if (!Value.IsValid())
        {
            return TEXT("");
        }
        switch (Value->Type)
        {
        case EJson::Boolean:
            return Value->AsBool() ? TEXT("True") : TEXT("False");
        case EJson::Number:
            return FString::SanitizeFloat(Value->AsNumber());
        case EJson::String:
            return Value->AsString();
        default:
            return TEXT("");
        }
    }

    static int32 ApplyTemplateDefaults(UObject* Target, const TSharedPtr<FJsonObject>& DefaultsObj, FString& Report)
    {
        if (!Target || !DefaultsObj.IsValid())
        {
            return 0;
        }

        int32 Applied = 0;
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : DefaultsObj->Values)
        {
            FProperty* Property = FindFProperty<FProperty>(Target->GetClass(), FName(*Pair.Key));
            if (!Property)
            {
                AppendLine(Report, FString::Printf(TEXT("WARNING: component/template property not found: %s.%s"), *Target->GetClass()->GetPathName(), *Pair.Key));
                continue;
            }

            const FString ImportText = JsonScalarToImportText(Pair.Value);
            if (ImportText.IsEmpty() && Pair.Value.IsValid() && Pair.Value->Type != EJson::String)
            {
                AppendLine(Report, FString::Printf(TEXT("WARNING: component/template property '%s' requires scalar or Unreal ImportText string."), *Pair.Key));
                continue;
            }

            void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Target);
            if (Property->ImportText(*ImportText, ValuePtr, PPF_None, Target))
            {
                ++Applied;
            }
            else
            {
                AppendLine(Report, FString::Printf(TEXT("WARNING: failed to import component/template property %s='%s'."), *Pair.Key, *ImportText));
            }
        }
        return Applied;
    }

    static bool TryReadVectorValue(const TSharedPtr<FJsonObject>& Obj, const FString& FieldName, FVector& OutValue)
    {
        if (!Obj.IsValid())
        {
            return false;
        }
        const TSharedPtr<FJsonObject>* ValueObj = nullptr;
        if (Obj->TryGetObjectField(FieldName, ValueObj) && ValueObj && ValueObj->IsValid())
        {
            double X = 0.0, Y = 0.0, Z = 0.0;
            (*ValueObj)->TryGetNumberField(TEXT("x"), X);
            (*ValueObj)->TryGetNumberField(TEXT("y"), Y);
            (*ValueObj)->TryGetNumberField(TEXT("z"), Z);
            OutValue = FVector((float)X, (float)Y, (float)Z);
            return true;
        }
        FString TextValue;
        if (Obj->TryGetStringField(FieldName, TextValue))
        {
            return OutValue.InitFromString(TextValue);
        }
        return false;
    }

    static bool TryReadRotatorValue(const TSharedPtr<FJsonObject>& Obj, const FString& FieldName, FRotator& OutValue)
    {
        if (!Obj.IsValid())
        {
            return false;
        }
        const TSharedPtr<FJsonObject>* ValueObj = nullptr;
        if (Obj->TryGetObjectField(FieldName, ValueObj) && ValueObj && ValueObj->IsValid())
        {
            double Pitch = 0.0, Yaw = 0.0, Roll = 0.0;
            (*ValueObj)->TryGetNumberField(TEXT("pitch"), Pitch);
            (*ValueObj)->TryGetNumberField(TEXT("yaw"), Yaw);
            (*ValueObj)->TryGetNumberField(TEXT("roll"), Roll);
            OutValue = FRotator((float)Pitch, (float)Yaw, (float)Roll);
            return true;
        }
        FString TextValue;
        if (Obj->TryGetStringField(FieldName, TextValue))
        {
            return OutValue.InitFromString(TextValue);
        }
        return false;
    }

    static void ApplySceneComponentTransform(UActorComponent* ComponentTemplate, const TSharedPtr<FJsonObject>& ComponentObj)
    {
        USceneComponent* SceneComponent = Cast<USceneComponent>(ComponentTemplate);
        if (!SceneComponent || !ComponentObj.IsValid())
        {
            return;
        }

        FVector Location;
        if (TryReadVectorValue(ComponentObj, TEXT("relative_location"), Location))
        {
            SceneComponent->SetRelativeLocation_Direct(Location);
        }
        FRotator Rotation;
        if (TryReadRotatorValue(ComponentObj, TEXT("relative_rotation"), Rotation))
        {
            SceneComponent->SetRelativeRotation_Direct(Rotation);
        }
        FVector Scale;
        if (TryReadVectorValue(ComponentObj, TEXT("relative_scale"), Scale) || TryReadVectorValue(ComponentObj, TEXT("relative_scale3d"), Scale))
        {
            SceneComponent->SetRelativeScale3D_Direct(Scale);
        }
    }

    static UClass* ResolveComponentClassFromJson(const TSharedPtr<FJsonObject>& NodeObj)
    {
        FString ClassPath = GetStringFieldSafe(NodeObj, TEXT("component_class"));
        if (ClassPath.IsEmpty())
        {
            ClassPath = GetStringFieldSafe(NodeObj, TEXT("template_class"));
        }
        if (ClassPath.IsEmpty())
        {
            ClassPath = GetStringFieldSafe(NodeObj, TEXT("target_class"));
        }
        if (ClassPath.IsEmpty())
        {
            ClassPath = GetStringFieldSafe(NodeObj, TEXT("class_path"));
        }
        if (ClassPath.IsEmpty())
        {
            return nullptr;
        }
        if (UClass* FoundClass = FindObject<UClass>(ANY_PACKAGE, *ClassPath))
        {
            return FoundClass;
        }
        return LoadObject<UClass>(nullptr, *ClassPath);
    }

    static bool IsSCSNodeInSubtree(const USCS_Node* Root, const USCS_Node* Candidate)
    {
        if (!Root || !Candidate)
        {
            return false;
        }
        if (Root == Candidate)
        {
            return true;
        }
        for (USCS_Node* Child : Root->GetChildNodes())
        {
            if (IsSCSNodeInSubtree(Child, Candidate))
            {
                return true;
            }
        }
        return false;
    }

    static USCS_Node* FindSCSNodeByNameRecursive(const TArray<USCS_Node*>& Nodes, const FName& VariableName)
    {
        if (VariableName.IsNone())
        {
            return nullptr;
        }
        for (USCS_Node* Node : Nodes)
        {
            if (!Node)
            {
                continue;
            }
            if (Node->GetVariableName() == VariableName)
            {
                return Node;
            }
            if (USCS_Node* Child = FindSCSNodeByNameRecursive(Node->GetChildNodes(), VariableName))
            {
                return Child;
            }
        }
        return nullptr;
    }

    static USCS_Node* FindOrCreateSimpleSCSComponent(UBlueprint* Blueprint, UClass* ComponentClass, const FString& ComponentName, FString& Report)
    {
        if (!Blueprint || !ComponentClass)
        {
            AppendLine(Report, TEXT("WARNING: AddComponent skipped because Blueprint or component class is null."));
            return nullptr;
        }

        const FName ComponentFName(*ComponentName);
        if (ComponentFName.IsNone())
        {
            AppendLine(Report, TEXT("WARNING: AddComponent skipped because component_name/template_name is empty."));
            return nullptr;
        }

        if (!Blueprint->SimpleConstructionScript)
        {
            Blueprint->SimpleConstructionScript = NewObject<USimpleConstructionScript>(Blueprint, TEXT("SimpleConstructionScript"));
            Blueprint->SimpleConstructionScript->SetFlags(RF_Transactional);
            AppendLine(Report, TEXT("CHANGE: Created missing SimpleConstructionScript."));
        }

        USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
        if (USCS_Node* ExistingNode = FindSCSNodeByNameRecursive(SCS->GetRootNodes(), ComponentFName))
        {
            return ExistingNode;
        }

        USCS_Node* NewNode = SCS->CreateNode(ComponentClass, ComponentFName);
        if (!NewNode)
        {
            AppendLine(Report, FString::Printf(TEXT("WARNING: AddComponent failed to create SCS node '%s' class='%s'."), *ComponentName, *ComponentClass->GetPathName()));
            return nullptr;
        }

        NewNode->SetFlags(RF_Transactional);
        SCS->AddNode(NewNode);
        AppendLine(Report, FString::Printf(TEXT("CHANGE: Component template created: %s (%s)."), *ComponentName, *ComponentClass->GetPathName()));
        return NewNode;
    }

    struct FN2CSCSImportRecord
    {
        TSharedPtr<FJsonObject> Object;
        FString ParentName;
    };

    static bool ImportSCSHierarchy(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& ActionObj, bool bDryRun, FString& Report)
    {
        if (!Blueprint || !ActionObj.IsValid())
        {
            return false;
        }
        const TArray<TSharedPtr<FJsonValue>>* Components = GetArrayFieldSafe(ActionObj, TEXT("components"));
        if (!Components || Components->Num() == 0)
        {
            AppendLine(Report, TEXT("ERROR: SCS hierarchy import requires components[]."));
            return false;
        }

        TArray<FN2CSCSImportRecord> Records;
        TFunction<void(const TArray<TSharedPtr<FJsonValue>>&, const FString&)> FlattenComponents;
        FlattenComponents = [&](const TArray<TSharedPtr<FJsonValue>>& Values, const FString& ParentName)
        {
            for (const TSharedPtr<FJsonValue>& Value : Values)
            {
                TSharedPtr<FJsonObject> ComponentObj = Value.IsValid() ? Value->AsObject() : nullptr;
                if (!ComponentObj.IsValid())
                {
                    continue;
                }
                FString ExplicitParent = GetStringFieldSafe(ComponentObj, TEXT("parent_name"), GetStringFieldSafe(ComponentObj, TEXT("attach_parent"), ParentName));
                FN2CSCSImportRecord Record;
                Record.Object = ComponentObj;
                Record.ParentName = ExplicitParent;
                Records.Add(Record);

                const FString ComponentName = GetStringFieldSafe(ComponentObj, TEXT("component_name"), GetStringFieldSafe(ComponentObj, TEXT("name")));
                const TArray<TSharedPtr<FJsonValue>>* Children = GetArrayFieldSafe(ComponentObj, TEXT("children"));
                if (Children)
                {
                    FlattenComponents(*Children, ComponentName);
                }
            }
        };
        FlattenComponents(*Components, TEXT(""));

        // Validate the complete requested hierarchy before touching the SCS.
        // This avoids a half-mutated component tree when a later record contains
        // a bad class, duplicate name, unknown parent or cycle.
        bool bPreflightOk = Records.Num() > 0;
        TSet<FString> IncomingNames;
        TMap<FString, FString> IncomingParents;
        for (const FN2CSCSImportRecord& Record : Records)
        {
            const FString ComponentName = GetStringFieldSafe(Record.Object, TEXT("component_name"), GetStringFieldSafe(Record.Object, TEXT("name")));
            UClass* ComponentClass = ResolveComponentClassFromJson(Record.Object);
            if (!IsSafeBlueprintIdentifier(ComponentName))
            {
                AppendLine(Report, FString::Printf(TEXT("ERROR: invalid SCS component name '%s'. Rejected before mutation."), *ComponentName));
                bPreflightOk = false;
                continue;
            }
            if (IncomingNames.Contains(ComponentName))
            {
                AppendLine(Report, FString::Printf(TEXT("ERROR: duplicate SCS component name '%s'. Rejected before mutation."), *ComponentName));
                bPreflightOk = false;
                continue;
            }
            IncomingNames.Add(ComponentName);
            IncomingParents.Add(ComponentName, Record.ParentName);

            if (!ComponentClass || !ComponentClass->IsChildOf(UActorComponent::StaticClass()) ||
                !FKismetEditorUtilities::IsClassABlueprintSpawnableComponent(ComponentClass))
            {
                AppendLine(Report, FString::Printf(TEXT("ERROR: SCS component '%s' has invalid/non-spawnable class '%s'. Rejected before mutation."), *ComponentName, *GetStringFieldSafe(Record.Object, TEXT("component_class"))));
                bPreflightOk = false;
                continue;
            }

            if (Blueprint->SimpleConstructionScript)
            {
                if (USCS_Node* Existing = FindSCSNodeByNameRecursive(Blueprint->SimpleConstructionScript->GetRootNodes(), FName(*ComponentName)))
                {
                    if (!Existing->ComponentTemplate || !Existing->ComponentTemplate->IsA(ComponentClass))
                    {
                        AppendLine(Report, FString::Printf(TEXT("ERROR: existing SCS component '%s' class differs from incoming class '%s'; destructive replacement is not allowed. Rejected before mutation."), *ComponentName, *ComponentClass->GetPathName()));
                        bPreflightOk = false;
                    }
                }
            }
        }

        for (const FN2CSCSImportRecord& Record : Records)
        {
            const FString ComponentName = GetStringFieldSafe(Record.Object, TEXT("component_name"), GetStringFieldSafe(Record.Object, TEXT("name")));
            if (Record.ParentName.IsEmpty())
            {
                continue;
            }
            if (Record.ParentName == ComponentName)
            {
                AppendLine(Report, FString::Printf(TEXT("ERROR: SCS component '%s' cannot parent itself. Rejected before mutation."), *ComponentName));
                bPreflightOk = false;
                continue;
            }
            const bool bParentIncoming = IncomingNames.Contains(Record.ParentName);
            const bool bParentExisting = Blueprint->SimpleConstructionScript &&
                FindSCSNodeByNameRecursive(Blueprint->SimpleConstructionScript->GetRootNodes(), FName(*Record.ParentName));
            if (!bParentIncoming && !bParentExisting)
            {
                AppendLine(Report, FString::Printf(TEXT("ERROR: SCS parent '%s' for '%s' was not found. Rejected before mutation."), *Record.ParentName, *ComponentName));
                bPreflightOk = false;
            }
        }

        for (const TPair<FString, FString>& Pair : IncomingParents)
        {
            TSet<FString> Seen;
            FString Cursor = Pair.Key;
            while (!Cursor.IsEmpty() && IncomingParents.Contains(Cursor))
            {
                if (Seen.Contains(Cursor))
                {
                    AppendLine(Report, FString::Printf(TEXT("ERROR: circular SCS parent chain detected at '%s'. Rejected before mutation."), *Cursor));
                    bPreflightOk = false;
                    break;
                }
                Seen.Add(Cursor);
                Cursor = IncomingParents.FindRef(Cursor);
            }
        }

        if (!bPreflightOk)
        {
            return false;
        }

        if (bDryRun)
        {
            AppendLine(Report, FString::Printf(TEXT("DRY RUN CHANGE: SCS hierarchy import/update components=%d."), Records.Num()));
            return true;
        }

        Blueprint->Modify();
        if (!Blueprint->SimpleConstructionScript)
        {
            Blueprint->SimpleConstructionScript = NewObject<USimpleConstructionScript>(Blueprint, TEXT("SimpleConstructionScript"));
            Blueprint->SimpleConstructionScript->SetFlags(RF_Transactional);
        }
        USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
        SCS->Modify();

        TMap<FString, USCS_Node*> NodesByName;
        bool bOk = true;
        for (const FN2CSCSImportRecord& Record : Records)
        {
            const FString ComponentName = GetStringFieldSafe(Record.Object, TEXT("component_name"), GetStringFieldSafe(Record.Object, TEXT("name")));
            UClass* ComponentClass = ResolveComponentClassFromJson(Record.Object);
            if (ComponentName.IsEmpty() || !ComponentClass || !ComponentClass->IsChildOf(UActorComponent::StaticClass()))
            {
                AppendLine(Report, FString::Printf(TEXT("ERROR: invalid SCS component record name='%s' class='%s'."), *ComponentName, *GetStringFieldSafe(Record.Object, TEXT("component_class"))));
                bOk = false;
                continue;
            }

            USCS_Node* SCSNode = FindOrCreateSimpleSCSComponent(Blueprint, ComponentClass, ComponentName, Report);
            if (!SCSNode || !SCSNode->ComponentTemplate)
            {
                bOk = false;
                continue;
            }
            if (!SCSNode->ComponentTemplate->IsA(ComponentClass))
            {
                AppendLine(Report, FString::Printf(TEXT("ERROR: existing SCS component '%s' has class '%s', incoming class is '%s'; destructive class replacement is not performed."), *ComponentName, *SCSNode->ComponentTemplate->GetClass()->GetPathName(), *ComponentClass->GetPathName()));
                bOk = false;
                continue;
            }

            SCSNode->Modify();
            SCSNode->ComponentTemplate->Modify();
            const TSharedPtr<FJsonObject>* DefaultsObj = nullptr;
            if (!(Record.Object->TryGetObjectField(TEXT("template_defaults"), DefaultsObj) && DefaultsObj && DefaultsObj->IsValid()))
            {
                Record.Object->TryGetObjectField(TEXT("component_defaults"), DefaultsObj);
            }
            const int32 AppliedDefaults = (DefaultsObj && DefaultsObj->IsValid()) ? ApplyTemplateDefaults(SCSNode->ComponentTemplate, *DefaultsObj, Report) : 0;
            ApplySceneComponentTransform(SCSNode->ComponentTemplate, Record.Object);
            SCSNode->AttachToName = FName(*GetStringFieldSafe(Record.Object, TEXT("attach_socket"), GetStringFieldSafe(Record.Object, TEXT("socket_name"))));
            NodesByName.Add(ComponentName, SCSNode);
            AppendLine(Report, FString::Printf(TEXT("CHANGE: SCS component updated: %s defaults=%d socket='%s'."), *ComponentName, AppliedDefaults, *SCSNode->AttachToName.ToString()));
        }

        // Parent in a second pass so nested records may appear in any order.
        for (const FN2CSCSImportRecord& Record : Records)
        {
            const FString ComponentName = GetStringFieldSafe(Record.Object, TEXT("component_name"), GetStringFieldSafe(Record.Object, TEXT("name")));
            USCS_Node* Node = NodesByName.FindRef(ComponentName);
            if (!Node || Record.ParentName.IsEmpty())
            {
                continue;
            }

            USCS_Node* ParentNode = NodesByName.FindRef(Record.ParentName);
            if (!ParentNode)
            {
                ParentNode = FindSCSNodeByNameRecursive(SCS->GetRootNodes(), FName(*Record.ParentName));
            }
            if (!ParentNode || ParentNode == Node || IsSCSNodeInSubtree(Node, ParentNode))
            {
                AppendLine(Report, FString::Printf(TEXT("ERROR: invalid/circular SCS parent '%s' for component '%s'."), *Record.ParentName, *ComponentName));
                bOk = false;
                continue;
            }
            if (!Node->ComponentTemplate->IsA<USceneComponent>() || !ParentNode->ComponentTemplate->IsA<USceneComponent>())
            {
                AppendLine(Report, FString::Printf(TEXT("WARNING: SCS parent ignored for non-scene component: %s -> %s."), *ComponentName, *Record.ParentName));
                continue;
            }

            USCS_Node* CurrentParent = SCS->FindParentNode(Node);
            if (CurrentParent == ParentNode)
            {
                // Local SCS tree parentage is represented by ChildNodes. These
                // fields are reserved for native/inherited parent references;
                // setting them to the same local parent triggers UE4.27
                // ValidateSceneRootNodes line 481 on repeat import.
                Node->bIsParentComponentNative = false;
                Node->ParentComponentOrVariableName = NAME_None;
                Node->ParentComponentOwnerClassName = NAME_None;
                continue;
            }
            if (CurrentParent)
            {
                CurrentParent->RemoveChildNode(Node, false);
            }
            else
            {
                // A node without an SCS parent is a root node in the current tree.
                // Remove it from the root set before adding it under the new parent.
                SCS->RemoveNode(Node, false);
            }
            ParentNode->AddChildNode(Node, true);
            Node->bIsParentComponentNative = false;
            Node->ParentComponentOrVariableName = NAME_None;
            Node->ParentComponentOwnerClassName = NAME_None;
            AppendLine(Report, FString::Printf(TEXT("CHANGE: SCS attachment: %s -> %s socket='%s'."), *ComponentName, *Record.ParentName, *Node->AttachToName.ToString()));
        }

        SCS->FixupRootNodeParentReferences();
        SCS->ValidateSceneRootNodes();
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
        return bOk;
    }

    static double GetJsonNumberSafe(const TSharedPtr<FJsonObject>& Obj, const FString& FieldName, double DefaultValue = 0.0)
    {
        if (!Obj.IsValid())
        {
            return DefaultValue;
        }
        double Value = DefaultValue;
        return Obj->TryGetNumberField(FieldName, Value) ? Value : DefaultValue;
    }

    static void AddFloatCurveKeys(UCurveFloat* Curve, const TArray<TSharedPtr<FJsonValue>>* Keys)
    {
        if (!Curve || !Keys)
        {
            return;
        }
        for (const TSharedPtr<FJsonValue>& KeyValue : *Keys)
        {
            const TSharedPtr<FJsonObject> KeyObj = KeyValue.IsValid() ? KeyValue->AsObject() : nullptr;
            if (!KeyObj.IsValid())
            {
                continue;
            }
            const float Time = (float)GetJsonNumberSafe(KeyObj, TEXT("time"));
            const float Value = (float)GetJsonNumberSafe(KeyObj, TEXT("value"));
            const FKeyHandle Handle = Curve->FloatCurve.AddKey(Time, Value);
            const FString Interp = GetStringFieldSafe(KeyObj, TEXT("interp"), TEXT("linear")).ToLower();
            if (Interp == TEXT("constant")) Curve->FloatCurve.SetKeyInterpMode(Handle, RCIM_Constant);
            else if (Interp == TEXT("cubic") || Interp == TEXT("auto")) Curve->FloatCurve.SetKeyInterpMode(Handle, RCIM_Cubic);
            else Curve->FloatCurve.SetKeyInterpMode(Handle, RCIM_Linear);
        }
    }

    static FVector ReadVectorKeyValue(const TSharedPtr<FJsonObject>& KeyObj)
    {
        const TSharedPtr<FJsonObject>* ValueObj = nullptr;
        if (KeyObj.IsValid() && KeyObj->TryGetObjectField(TEXT("value"), ValueObj) && ValueObj && ValueObj->IsValid())
        {
            return FVector(
                (float)GetJsonNumberSafe(*ValueObj, TEXT("x")),
                (float)GetJsonNumberSafe(*ValueObj, TEXT("y")),
                (float)GetJsonNumberSafe(*ValueObj, TEXT("z")));
        }
        return FVector(
            (float)GetJsonNumberSafe(KeyObj, TEXT("x")),
            (float)GetJsonNumberSafe(KeyObj, TEXT("y")),
            (float)GetJsonNumberSafe(KeyObj, TEXT("z")));
    }

    static FLinearColor ReadColorKeyValue(const TSharedPtr<FJsonObject>& KeyObj)
    {
        const TSharedPtr<FJsonObject>* ValueObj = nullptr;
        TSharedPtr<FJsonObject> ColorObj = KeyObj;
        if (KeyObj.IsValid() && KeyObj->TryGetObjectField(TEXT("value"), ValueObj) && ValueObj && ValueObj->IsValid())
        {
            ColorObj = *ValueObj;
        }
        return FLinearColor(
            (float)GetJsonNumberSafe(ColorObj, TEXT("r")),
            (float)GetJsonNumberSafe(ColorObj, TEXT("g")),
            (float)GetJsonNumberSafe(ColorObj, TEXT("b")),
            (float)GetJsonNumberSafe(ColorObj, TEXT("a"), 1.0));
    }

    static void ApplyRichCurveInterp(FRichCurve& Curve, FKeyHandle Handle, const TSharedPtr<FJsonObject>& KeyObj)
    {
        const FString Interp = GetStringFieldSafe(KeyObj, TEXT("interp"), TEXT("linear")).ToLower();
        if (Interp == TEXT("constant")) Curve.SetKeyInterpMode(Handle, RCIM_Constant);
        else if (Interp == TEXT("cubic") || Interp == TEXT("auto")) Curve.SetKeyInterpMode(Handle, RCIM_Cubic);
        else Curve.SetKeyInterpMode(Handle, RCIM_Linear);
    }

    static UTimelineTemplate* BuildTimelineTemplate(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& NodeObj, const FName TimelineName, FString& Report)
    {
        if (!Blueprint || TimelineName.IsNone() || !Blueprint->GeneratedClass)
        {
            return nullptr;
        }

        UTimelineTemplate* Template = Blueprint->FindTimelineTemplateByVariableName(TimelineName);
        if (!Template)
        {
            Template = FBlueprintEditorUtils::AddNewTimeline(Blueprint, TimelineName);
        }
        if (!Template)
        {
            AppendLine(Report, FString::Printf(TEXT("WARNING: failed to create TimelineTemplate '%s'."), *TimelineName.ToString()));
            return nullptr;
        }

        Template->Modify();
        Template->TimelineLength = (float)GetJsonNumberSafe(NodeObj, TEXT("length"), GetJsonNumberSafe(NodeObj, TEXT("timeline_length"), 1.0));
        const FString LengthMode = GetStringFieldSafe(NodeObj, TEXT("length_mode"), TEXT("timeline_length")).ToLower();
        Template->LengthMode = (LengthMode == TEXT("last_key") || LengthMode == TEXT("last_key_frame")) ? TL_LastKeyFrame : TL_TimelineLength;
        Template->bAutoPlay = GetBoolFieldSafe(NodeObj, TEXT("auto_play"), GetBoolFieldSafe(NodeObj, TEXT("autoplay"), false));
        Template->bLoop = GetBoolFieldSafe(NodeObj, TEXT("loop"), GetBoolFieldSafe(NodeObj, TEXT("looping"), false));
        Template->bReplicated = GetBoolFieldSafe(NodeObj, TEXT("replicated"), false);
        Template->bIgnoreTimeDilation = GetBoolFieldSafe(NodeObj, TEXT("ignore_time_dilation"), GetBoolFieldSafe(NodeObj, TEXT("ignore_global_time_dilation"), false));

        while (Template->GetNumDisplayTracks() > 0)
        {
            Template->RemoveDisplayTrack(0);
        }
        Template->EventTracks.Reset();
        Template->FloatTracks.Reset();
        Template->VectorTracks.Reset();
        Template->LinearColorTracks.Reset();

        auto MakeCurveName = [&](const FString& TrackName, const TCHAR* Suffix) -> FName
        {
            return MakeUniqueObjectName(Blueprint, UObject::StaticClass(), FName(*FString::Printf(TEXT("%s_%s_%s"), *TimelineName.ToString(), *TrackName, Suffix)));
        };

        if (const TArray<TSharedPtr<FJsonValue>>* Tracks = GetArrayFieldSafe(NodeObj, TEXT("float_tracks")))
        {
            for (const TSharedPtr<FJsonValue>& TrackValue : *Tracks)
            {
                const TSharedPtr<FJsonObject> TrackObj = TrackValue.IsValid() ? TrackValue->AsObject() : nullptr;
                const FString TrackName = GetStringFieldSafe(TrackObj, TEXT("name"));
                if (!TrackObj.IsValid() || TrackName.IsEmpty()) continue;
                FTTFloatTrack Track;
                Track.CurveFloat = NewObject<UCurveFloat>(Blueprint, MakeCurveName(TrackName, TEXT("FloatCurve")), RF_Transactional);
                Track.SetTrackName(FName(*TrackName), Template);
                AddFloatCurveKeys(Track.CurveFloat, GetArrayFieldSafe(TrackObj, TEXT("keys")));
                const int32 TrackIndex = Template->FloatTracks.Add(Track);
                Template->AddDisplayTrack(FTTTrackId(FTTTrackBase::TT_FloatInterp, TrackIndex));
            }
        }

        if (const TArray<TSharedPtr<FJsonValue>>* Tracks = GetArrayFieldSafe(NodeObj, TEXT("vector_tracks")))
        {
            for (const TSharedPtr<FJsonValue>& TrackValue : *Tracks)
            {
                const TSharedPtr<FJsonObject> TrackObj = TrackValue.IsValid() ? TrackValue->AsObject() : nullptr;
                const FString TrackName = GetStringFieldSafe(TrackObj, TEXT("name"));
                if (!TrackObj.IsValid() || TrackName.IsEmpty()) continue;
                FTTVectorTrack Track;
                Track.CurveVector = NewObject<UCurveVector>(Blueprint, MakeCurveName(TrackName, TEXT("VectorCurve")), RF_Transactional);
                Track.SetTrackName(FName(*TrackName), Template);
                if (const TArray<TSharedPtr<FJsonValue>>* Keys = GetArrayFieldSafe(TrackObj, TEXT("keys")))
                {
                    for (const TSharedPtr<FJsonValue>& KeyValue : *Keys)
                    {
                        const TSharedPtr<FJsonObject> KeyObj = KeyValue.IsValid() ? KeyValue->AsObject() : nullptr;
                        if (!KeyObj.IsValid()) continue;
                        const float Time = (float)GetJsonNumberSafe(KeyObj, TEXT("time"));
                        const FVector V = ReadVectorKeyValue(KeyObj);
                        const float Values[3] = { V.X, V.Y, V.Z };
                        for (int32 Axis = 0; Axis < 3; ++Axis)
                        {
                            FKeyHandle Handle = Track.CurveVector->FloatCurves[Axis].AddKey(Time, Values[Axis]);
                            ApplyRichCurveInterp(Track.CurveVector->FloatCurves[Axis], Handle, KeyObj);
                        }
                    }
                }
                const int32 TrackIndex = Template->VectorTracks.Add(Track);
                Template->AddDisplayTrack(FTTTrackId(FTTTrackBase::TT_VectorInterp, TrackIndex));
            }
        }

        if (const TArray<TSharedPtr<FJsonValue>>* Tracks = GetArrayFieldSafe(NodeObj, TEXT("color_tracks")))
        {
            for (const TSharedPtr<FJsonValue>& TrackValue : *Tracks)
            {
                const TSharedPtr<FJsonObject> TrackObj = TrackValue.IsValid() ? TrackValue->AsObject() : nullptr;
                const FString TrackName = GetStringFieldSafe(TrackObj, TEXT("name"));
                if (!TrackObj.IsValid() || TrackName.IsEmpty()) continue;
                FTTLinearColorTrack Track;
                Track.CurveLinearColor = NewObject<UCurveLinearColor>(Blueprint, MakeCurveName(TrackName, TEXT("ColorCurve")), RF_Transactional);
                Track.SetTrackName(FName(*TrackName), Template);
                if (const TArray<TSharedPtr<FJsonValue>>* Keys = GetArrayFieldSafe(TrackObj, TEXT("keys")))
                {
                    for (const TSharedPtr<FJsonValue>& KeyValue : *Keys)
                    {
                        const TSharedPtr<FJsonObject> KeyObj = KeyValue.IsValid() ? KeyValue->AsObject() : nullptr;
                        if (!KeyObj.IsValid()) continue;
                        const float Time = (float)GetJsonNumberSafe(KeyObj, TEXT("time"));
                        const FLinearColor C = ReadColorKeyValue(KeyObj);
                        const float Values[4] = { C.R, C.G, C.B, C.A };
                        for (int32 Channel = 0; Channel < 4; ++Channel)
                        {
                            FKeyHandle Handle = Track.CurveLinearColor->FloatCurves[Channel].AddKey(Time, Values[Channel]);
                            ApplyRichCurveInterp(Track.CurveLinearColor->FloatCurves[Channel], Handle, KeyObj);
                        }
                    }
                }
                const int32 TrackIndex = Template->LinearColorTracks.Add(Track);
                Template->AddDisplayTrack(FTTTrackId(FTTTrackBase::TT_LinearColorInterp, TrackIndex));
            }
        }

        if (const TArray<TSharedPtr<FJsonValue>>* Tracks = GetArrayFieldSafe(NodeObj, TEXT("event_tracks")))
        {
            for (const TSharedPtr<FJsonValue>& TrackValue : *Tracks)
            {
                const TSharedPtr<FJsonObject> TrackObj = TrackValue.IsValid() ? TrackValue->AsObject() : nullptr;
                const FString TrackName = GetStringFieldSafe(TrackObj, TEXT("name"));
                if (!TrackObj.IsValid() || TrackName.IsEmpty()) continue;
                FTTEventTrack Track;
                Track.CurveKeys = NewObject<UCurveFloat>(Blueprint, MakeCurveName(TrackName, TEXT("EventCurve")), RF_Transactional);
                Track.SetTrackName(FName(*TrackName), Template);
                if (const TArray<TSharedPtr<FJsonValue>>* Keys = GetArrayFieldSafe(TrackObj, TEXT("keys")))
                {
                    int32 EventIndex = 0;
                    for (const TSharedPtr<FJsonValue>& KeyValue : *Keys)
                    {
                        const TSharedPtr<FJsonObject> KeyObj = KeyValue.IsValid() ? KeyValue->AsObject() : nullptr;
                        if (!KeyObj.IsValid()) continue;
                        Track.CurveKeys->FloatCurve.AddKey((float)GetJsonNumberSafe(KeyObj, TEXT("time")), (float)EventIndex++);
                    }
                }
                const int32 TrackIndex = Template->EventTracks.Add(Track);
                Template->AddDisplayTrack(FTTTrackId(FTTTrackBase::TT_Event, TrackIndex));
            }
        }

        AppendLine(Report, FString::Printf(TEXT("CHANGE: TimelineTemplate rebuilt: %s float=%d vector=%d color=%d event=%d."),
            *TimelineName.ToString(), Template->FloatTracks.Num(), Template->VectorTracks.Num(), Template->LinearColorTracks.Num(), Template->EventTracks.Num()));
        return Template;
    }

    static UEdGraphNode* CreateTimelineNode(UEdGraph* Graph, UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& NodeObj, int32 PosX, int32 PosY, FString& Report)
    {
        const UEdGraphSchema_K2* K2Schema = Graph ? Cast<UEdGraphSchema_K2>(Graph->GetSchema()) : nullptr;
        if (!Graph || !Blueprint || !K2Schema || K2Schema->GetGraphType(Graph) != GT_Ubergraph)
        {
            AppendLine(Report, TEXT("WARNING: Timeline skipped because it is only valid in an Ubergraph/EventGraph."));
            return nullptr;
        }
        FString TimelineNameString = GetStringFieldSafe(NodeObj, TEXT("timeline_name"), GetStringFieldSafe(NodeObj, TEXT("name"), TEXT("N2C_Timeline")));
        const FName TimelineName(*TimelineNameString);
        UTimelineTemplate* Template = BuildTimelineTemplate(Blueprint, NodeObj, TimelineName, Report);
        if (!Template)
        {
            return nullptr;
        }

        for (UEdGraphNode* ExistingNode : Graph->Nodes)
        {
            UK2Node_Timeline* ExistingTimeline = Cast<UK2Node_Timeline>(ExistingNode);
            if (ExistingTimeline && ExistingTimeline->TimelineName == TimelineName)
            {
                ExistingTimeline->Modify();
                ExistingTimeline->NodePosX = PosX;
                ExistingTimeline->NodePosY = PosY;
                ExistingTimeline->TimelineGuid = Template->TimelineGuid;
                ExistingTimeline->bAutoPlay = Template->bAutoPlay;
                ExistingTimeline->bLoop = Template->bLoop;
                ExistingTimeline->bReplicated = Template->bReplicated;
                ExistingTimeline->bIgnoreTimeDilation = Template->bIgnoreTimeDilation;
                ExistingTimeline->ReconstructNode();
                return ExistingTimeline;
            }
        }

        UK2Node_Timeline* Node = NewObject<UK2Node_Timeline>(Graph);
        Node->SetFlags(RF_Transactional);
        Node->TimelineName = TimelineName;
        Node->TimelineGuid = Template->TimelineGuid;
        Node->bAutoPlay = Template->bAutoPlay;
        Node->bLoop = Template->bLoop;
        Node->bReplicated = Template->bReplicated;
        Node->bIgnoreTimeDilation = Template->bIgnoreTimeDilation;
        Node->NodePosX = PosX;
        Node->NodePosY = PosY;
        Graph->AddNode(Node, true, false);
        Node->CreateNewGuid();
        Node->AllocateDefaultPins();
        Node->ReconstructNode();
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
        AppendLine(Report, FString::Printf(TEXT("CHANGE: Timeline K2 node created: %s."), *TimelineNameString));
        return Node;
    }

    static UEdGraphNode* CreateAddComponentNode(UEdGraph* Graph, UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& NodeObj, int32 PosX, int32 PosY, FString& Report)
    {
        if (!Graph || !Blueprint || !Blueprint->GeneratedClass)
        {
            AppendLine(Report, TEXT("WARNING: AddComponent skipped because graph/Blueprint/GeneratedClass is unavailable."));
            return nullptr;
        }
        if (!FBlueprintEditorUtils::IsActorBased(Blueprint))
        {
            AppendLine(Report, TEXT("WARNING: AddComponent skipped because the target Blueprint is not Actor-based."));
            return nullptr;
        }

        UClass* ComponentClass = ResolveComponentClassFromJson(NodeObj);
        if (!ComponentClass || !ComponentClass->IsChildOf(UActorComponent::StaticClass()) ||
            !FKismetEditorUtilities::IsClassABlueprintSpawnableComponent(ComponentClass))
        {
            AppendLine(Report, FString::Printf(TEXT("WARNING: AddComponent skipped because component_class is not a Blueprint-spawnable UActorComponent class: %s"), *GetStringFieldSafe(NodeObj, TEXT("component_class"))));
            return nullptr;
        }

        UK2Node_AddComponent* Node = NewObject<UK2Node_AddComponent>(Graph);
        Node->SetFlags(RF_Transactional);
        Node->NodePosX = PosX;
        Node->NodePosY = PosY;
        Node->TemplateType = ComponentClass;
        UFunction* AddComponentFunction = AActor::StaticClass()->FindFunctionByName(UK2Node_AddComponent::GetAddComponentFunctionName());
        if (!AddComponentFunction)
        {
            AppendLine(Report, TEXT("WARNING: AddComponent skipped because AActor::AddComponent was not found."));
            return nullptr;
        }
        Node->FunctionReference.SetFromField<UFunction>(AddComponentFunction, true);
        Graph->AddNode(Node, true, false);

        // UE4.27 AddComponent reads required pins from PostPlacedNewNode, therefore
        // allocate the function pins only after TemplateType/FunctionReference are set.
        FinishNativeK2NodePinsFirstNoReconstruct(Node);

        Blueprint->Modify();
        Blueprint->GeneratedClass->Modify();
        // Match UE4.27 FEdGraphSchemaAction_K2AddComponent: NAME_None lets
        // NewObject assign a unique template name without calling the
        // non-exported MakeNewComponentTemplateName helper.
        const FName TemplateObjectName = NAME_None;
        UActorComponent* ComponentTemplate = NewObject<UActorComponent>(
            Blueprint->GeneratedClass,
            ComponentClass,
            TemplateObjectName,
            RF_ArchetypeObject | RF_Public | RF_Transactional);
        if (!ComponentTemplate)
        {
            AppendLine(Report, TEXT("WARNING: AddComponent failed to allocate the component template."));
            Node->DestroyNode();
            return nullptr;
        }

        const TSharedPtr<FJsonObject>* DefaultsObj = nullptr;
        if (!(NodeObj->TryGetObjectField(TEXT("template_defaults"), DefaultsObj) && DefaultsObj && DefaultsObj->IsValid()))
        {
            NodeObj->TryGetObjectField(TEXT("component_defaults"), DefaultsObj);
        }
        const int32 DefaultsApplied = (DefaultsObj && DefaultsObj->IsValid()) ? ApplyTemplateDefaults(ComponentTemplate, *DefaultsObj, Report) : 0;
        ApplySceneComponentTransform(ComponentTemplate, NodeObj);
        Blueprint->ComponentTemplates.Add(ComponentTemplate);

        if (UEdGraphPin* TemplateNamePin = Node->GetTemplateNamePinChecked())
        {
            TemplateNamePin->DefaultValue = ComponentTemplate->GetName();
        }
        Node->ReconstructNode();
        if (UEdGraphPin* ReturnPin = Node->GetReturnValuePin())
        {
            ReturnPin->PinType.PinSubCategoryObject = ComponentClass;
        }

        AppendLine(Report, FString::Printf(TEXT("CHANGE: AddComponent K2 node created: class='%s' template='%s' defaults=%d."), *ComponentClass->GetPathName(), *ComponentTemplate->GetName(), DefaultsApplied));
        return Node;
    }

    static FMulticastDelegateProperty* ResolveMulticastDelegateProperty(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& NodeObj, UClass*& OutOwnerClass)
    {
        OutOwnerClass = nullptr;
        const FString DelegateName = GetStringFieldSafe(NodeObj, TEXT("delegate_name"), GetStringFieldSafe(NodeObj, TEXT("member_name"), GetStringFieldSafe(NodeObj, TEXT("name"))));
        if (DelegateName.IsEmpty())
        {
            return nullptr;
        }

        TArray<UClass*> CandidateClasses;
        const FString OwnerPath = GetStringFieldSafe(NodeObj, TEXT("delegate_owner_class_path"), GetStringFieldSafe(NodeObj, TEXT("owner_class_path"), GetStringFieldSafe(NodeObj, TEXT("class_path"))));
        if (!OwnerPath.IsEmpty())
        {
            UClass* ExplicitClass = FindObject<UClass>(ANY_PACKAGE, *OwnerPath);
            if (!ExplicitClass)
            {
                ExplicitClass = LoadObject<UClass>(nullptr, *OwnerPath);
            }
            if (ExplicitClass)
            {
                CandidateClasses.Add(ExplicitClass);
            }
        }
        if (Blueprint)
        {
            CandidateClasses.AddUnique(Blueprint->SkeletonGeneratedClass);
            CandidateClasses.AddUnique(Blueprint->GeneratedClass);
            CandidateClasses.AddUnique(Blueprint->ParentClass);
        }

        for (UClass* CandidateClass : CandidateClasses)
        {
            if (!CandidateClass)
            {
                continue;
            }
            if (FMulticastDelegateProperty* Property = FindFProperty<FMulticastDelegateProperty>(CandidateClass, FName(*DelegateName)))
            {
                OutOwnerClass = CandidateClass;
                return Property;
            }
        }
        return nullptr;
    }

    template<typename TDelegateNode>
    static UEdGraphNode* CreateMulticastDelegateNode(UEdGraph* Graph, UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& NodeObj, int32 PosX, int32 PosY, FString& Report, const TCHAR* FriendlyType)
    {
        UClass* OwnerClass = nullptr;
        FMulticastDelegateProperty* DelegateProperty = ResolveMulticastDelegateProperty(Blueprint, NodeObj, OwnerClass);
        if (!DelegateProperty || !OwnerClass)
        {
            AppendLine(Report, FString::Printf(TEXT("WARNING: %s skipped because delegate property '%s' could not be resolved on the requested/self class."), FriendlyType, *GetStringFieldSafe(NodeObj, TEXT("delegate_name"), GetStringFieldSafe(NodeObj, TEXT("name")))));
            return nullptr;
        }

        FGraphNodeCreator<TDelegateNode> Creator(*Graph);
        TDelegateNode* Node = Creator.CreateNode();
        const bool bSelfContext = Blueprint && (OwnerClass == Blueprint->SkeletonGeneratedClass || OwnerClass == Blueprint->GeneratedClass);
        Node->SetFromProperty(DelegateProperty, bSelfContext, OwnerClass);
        Node->NodePosX = PosX;
        Node->NodePosY = PosY;
        // UK2Node_AssignDelegate::PostPlacedNewNode immediately reads its
        // delegate pin. FGraphNodeCreator::Finalize calls PostPlacedNewNode
        // before allocating pins, so raw class creation must allocate here.
        Node->AllocateDefaultPins();
        Creator.Finalize();
        AppendLine(Report, FString::Printf(TEXT("CHANGE: %s created for delegate '%s'."), FriendlyType, *DelegateProperty->GetName()));
        return Node;
    }

    static UEdGraphNode* CreateComponentBoundEventNode(UEdGraph* Graph, UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& NodeObj, int32 PosX, int32 PosY, FString& Report)
    {
        if (!Blueprint)
        {
            return nullptr;
        }

        const FString ComponentName = GetStringFieldSafe(NodeObj, TEXT("component_property_name"), GetStringFieldSafe(NodeObj, TEXT("component_name")));
        const FString EventName = GetStringFieldSafe(NodeObj, TEXT("delegate_name"),
            GetStringFieldSafe(NodeObj, TEXT("delegate_property_name"),
                GetStringFieldSafe(NodeObj, TEXT("event_name"), GetStringFieldSafe(NodeObj, TEXT("name")))));
        if (ComponentName.IsEmpty() || EventName.IsEmpty())
        {
            AppendLine(Report, TEXT("WARNING: ComponentBoundEvent requires component_name/component_property_name and delegate_name/event_name."));
            return nullptr;
        }

        TArray<UClass*> BlueprintClasses;
        BlueprintClasses.Add(Blueprint->SkeletonGeneratedClass);
        BlueprintClasses.Add(Blueprint->GeneratedClass);
        FObjectProperty* ComponentProperty = nullptr;
        for (UClass* BlueprintClass : BlueprintClasses)
        {
            if (BlueprintClass)
            {
                ComponentProperty = FindFProperty<FObjectProperty>(BlueprintClass, FName(*ComponentName));
                if (ComponentProperty)
                {
                    break;
                }
            }
        }
        if (!ComponentProperty || !ComponentProperty->PropertyClass)
        {
            AppendLine(Report, FString::Printf(TEXT("WARNING: ComponentBoundEvent skipped because component property '%s' is not available on the compiled/skeleton Blueprint class. Compile once after adding SCS components, then re-run this patch."), *ComponentName));
            return nullptr;
        }

        FMulticastDelegateProperty* DelegateProperty = FindFProperty<FMulticastDelegateProperty>(ComponentProperty->PropertyClass, FName(*EventName));
        if (!DelegateProperty)
        {
            AppendLine(Report, FString::Printf(TEXT("WARNING: ComponentBoundEvent skipped because '%s' has no multicast delegate '%s'."), *ComponentProperty->PropertyClass->GetPathName(), *EventName));
            return nullptr;
        }

        FGraphNodeCreator<UK2Node_ComponentBoundEvent> Creator(*Graph);
        UK2Node_ComponentBoundEvent* Node = Creator.CreateNode();
        Node->InitializeComponentBoundEventParams(ComponentProperty, DelegateProperty);
        Node->NodePosX = PosX;
        Node->NodePosY = PosY;
        Creator.Finalize();
        AppendLine(Report, FString::Printf(TEXT("CHANGE: ComponentBoundEvent created: %s.%s."), *ComponentName, *EventName));
        return Node;
    }

    static UEdGraphNode* CreateCreateDelegateNode(UEdGraph* Graph, const TSharedPtr<FJsonObject>& NodeObj, int32 PosX, int32 PosY, FString& Report)
    {
        const FString FunctionName = GetStringFieldSafe(NodeObj, TEXT("function_name"), GetStringFieldSafe(NodeObj, TEXT("event_name")));
        if (FunctionName.IsEmpty())
        {
            AppendLine(Report, TEXT("WARNING: CreateDelegate skipped because function_name/event_name is empty."));
            return nullptr;
        }
        FGraphNodeCreator<UK2Node_CreateDelegate> Creator(*Graph);
        UK2Node_CreateDelegate* Node = Creator.CreateNode();
        Node->NodePosX = PosX;
        Node->NodePosY = PosY;
        Creator.Finalize();

        // UE4.27 CreateDelegate cannot validate the selected function until its
        // OutputDelegate pin is connected and therefore has a delegate signature.
        // Calling HandleAnyChangeWithoutNotifying() here clears SelectedFunctionName
        // on an unlinked node. Keep the requested name intact; ConnectEdges performs
        // the first signature-aware stabilization after all links exist. This also
        // supports a handler function created earlier in the same patch, because the
        // compiler validates it after generated functions are materialized.
        Node->SetFunction(FName(*FunctionName));
        AppendLine(Report, FString::Printf(TEXT("CHANGE: CreateDelegate created for function '%s'; selection deferred until delegate links exist."), *FunctionName));
        return Node;
    }

    static bool ValidateHighRiskNodeContext(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& NodeObj, bool bFunctionGraph, FString& Report)
    {
        if (!NodeObj.IsValid())
        {
            return false;
        }
        FString Type = GetStringFieldSafe(NodeObj, TEXT("type"), GetStringFieldSafe(NodeObj, TEXT("class")));
        const FString Lower = Type.ToLower();


        if (Lower.Contains(TEXT("makestruct")) || Lower.Contains(TEXT("breakstruct")) ||
            Lower.Contains(TEXT("setfieldsinstruct")) || Lower.Contains(TEXT("setmembersinstruct")) ||
            Lower == TEXT("make_struct") || Lower == TEXT("break_struct"))
        {
            UScriptStruct* StructType = Cast<UScriptStruct>(ResolveTypeObjectFromJson(NodeObj));
            if (!StructType)
            {
                AppendLine(Report, TEXT("N2C_PREFLIGHT_GUARD|code=struct_identity_missing"));
                AppendLine(Report, TEXT("ERROR: struct node requires a resolvable struct_path before mutation."));
                return false;
            }
            if ((Lower.Contains(TEXT("makestruct")) || Lower.Contains(TEXT("breakstruct")) ||
                 Lower == TEXT("make_struct") || Lower == TEXT("break_struct")) &&
                ShouldGuardGenericStructMakeBreak(StructType))
            {
                AppendLine(Report, FString::Printf(TEXT("ERROR: unsafe generic Make/Break request for native struct '%s'. Use the specialized Kismet function/node instead. Rejected before mutation."), *StructType->GetPathName()));
                return false;
            }
            const bool bSetFieldsNode = Lower.Contains(TEXT("setfieldsinstruct")) ||
                Lower.Contains(TEXT("setmembersinstruct"));
            if (bSetFieldsNode)
            {
                if (!ValidateRequestedStructFields(StructType, NodeObj, Report))
                {
                    return false;
                }
            }
            else
            {
                const TArray<TSharedPtr<FJsonValue>>* Members = nullptr;
                if (NodeObj->TryGetArrayField(TEXT("member_pin_identity"), Members) && Members)
                {
                    for (const TSharedPtr<FJsonValue>& MemberValue : *Members)
                    {
                        const TSharedPtr<FJsonObject> Member = MemberValue.IsValid() ? MemberValue->AsObject() : nullptr;
                        FN2CRequestedStructField Request;
                        Request.Label = GetStringFieldSafe(Member, TEXT("pin_name"));
                        AddNormalizedStructFieldName(Request.NormalizedNames, Request.Label);
                        Request.GuidDigits = NormalizeGuidDigits(GetStringFieldSafe(Member, TEXT("persistent_guid")));

                        bool bMatched = Request.NormalizedNames.Num() == 0 && Request.GuidDigits.IsEmpty();
                        for (TFieldIterator<FProperty> It(StructType); !bMatched && It; ++It)
                        {
                            bMatched = RequestedStructFieldMatchesProperty(Request, *It);
                        }
                        if (!bMatched)
                        {
                            AppendLine(Report, FString::Printf(
                                TEXT("N2C_PREFLIGHT_GUARD|code=struct_member_mismatch|member=%s"),
                                *Request.Label));
                            AppendLine(Report, FString::Printf(
                                TEXT("ERROR: struct member '%s' was not found in '%s'. Friendly name, internal name and persistent GUID were checked before mutation."),
                                *Request.Label,
                                *StructType->GetPathName()));
                            return false;
                        }
                    }
                }
            }
        }

        if (bFunctionGraph &&
            (Lower == TEXT("delay") || Lower == TEXT("latent_delay") || Lower.Contains(TEXT("delay")) ||
             Lower == TEXT("timeline") || Lower == TEXT("timeline_node") || Lower.Contains(TEXT("timeline")) ||
             Lower == TEXT("componentboundevent") || Lower == TEXT("component_bound_event") || Lower.Contains(TEXT("componentboundevent"))))
        {
            AppendLine(Report, FString::Printf(TEXT("ERROR: node type '%s' is Ubergraph/EventGraph-only and cannot be imported into a function graph. Rejected before mutation."), *Type));
            return false;
        }

        if (bFunctionGraph && (Lower == TEXT("callfunction") || Lower == TEXT("call_function") || Lower.Contains(TEXT("callfunction"))))
        {
            if (UFunction* Function = ResolveFunction(NodeObj, Blueprint))
            {
                if (Function->HasMetaData(FName(TEXT("Latent"))))
                {
                    AppendLine(Report, FString::Printf(TEXT("ERROR: latent function '%s' cannot be imported into a Blueprint function graph. Use EventGraph/macro context. Rejected before mutation."), *Function->GetPathName()));
                    return false;
                }
            }
        }

        if (Lower == TEXT("getdatatablerow") || Lower == TEXT("get_data_table_row") || Lower.Contains(TEXT("getdatatablerow")))
        {
            const bool bTablePinLinked = GetBoolFieldSafe(NodeObj, TEXT("data_table_pin_linked"), false);
            UDataTable* Table = ResolveDataTableFromJson(NodeObj);
            const FString RequestedStruct = GetStringFieldSafe(NodeObj, TEXT("row_struct_path"), GetStringFieldSafe(NodeObj, TEXT("struct_path")));
            UScriptStruct* LinkedRowStruct = RequestedStruct.IsEmpty() ? nullptr : FindObject<UScriptStruct>(ANY_PACKAGE, *RequestedStruct);
            if (!LinkedRowStruct && !RequestedStruct.IsEmpty()) LinkedRowStruct = LoadObject<UScriptStruct>(nullptr, *RequestedStruct);
            if ((!bTablePinLinked && (!Table || !Table->GetRowStruct())) || (bTablePinLinked && !LinkedRowStruct))
            {
                AppendLine(Report, FString::Printf(TEXT("N2C_PREFLIGHT_GUARD|code=datatable_identity_missing|variant=%s"), bTablePinLinked ? TEXT("linked") : TEXT("literal")));
                AppendLine(Report, TEXT("ERROR: typed GetDataTableRow requires a resolvable literal table or linked-pin row_struct_path. Rejected before mutation."));
                return false;
            }
            if (!bTablePinLinked && !RequestedStruct.IsEmpty() && RequestedStruct != Table->GetRowStruct()->GetPathName())
            {
                AppendLine(Report, FString::Printf(TEXT("ERROR: GetDataTableRow row_struct_path '%s' does not match '%s'. Rejected before mutation."), *RequestedStruct, *Table->GetRowStruct()->GetPathName()));
                return false;
            }
        }

        if (Lower == TEXT("setfieldsinstruct") || Lower == TEXT("setmembersinstruct") || Lower.Contains(TEXT("setfieldsinstruct")) || Lower.Contains(TEXT("setmembersinstruct")))
        {
            if (!Cast<UScriptStruct>(ResolveTypeObjectFromJson(NodeObj)))
            {
                AppendLine(Report, TEXT("ERROR: SetFieldsInStruct requires a resolvable struct_path/type_path. Rejected before mutation."));
                return false;
            }
        }

        if (Lower == TEXT("macroinstance") || Lower == TEXT("macro_instance") || Lower.Contains(TEXT("macroinstance")) ||
            Lower == TEXT("isvalid") || Lower == TEXT("is_valid") || Lower == TEXT("doonce") || Lower == TEXT("do_once") ||
            Lower == TEXT("gate") || Lower == TEXT("flipflop") || Lower == TEXT("flip_flop") || Lower == TEXT("foreachloopwithbreak") ||
            Lower == TEXT("for_each_loop_with_break") || Lower == TEXT("whileloop") || Lower == TEXT("while_loop"))
        {
            const FString MacroName = GetStringFieldSafe(NodeObj, TEXT("macro_name"), GetStringFieldSafe(NodeObj, TEXT("name"), Type));
            UEdGraph* ResolvedMacroGraph = nullptr; FString MacroErrorCode, MacroDetail;
            const bool bHasDurableIdentity = !GetStringFieldSafe(NodeObj, TEXT("macro_owner_path")).IsEmpty() || !GetStringFieldSafe(NodeObj, TEXT("macro_graph_path")).IsEmpty();
            const bool bMacroResolved = bHasDurableIdentity ? FN2CMacroReference::ResolveAndValidate(NodeObj, Blueprint, ResolvedMacroGraph, MacroErrorCode, MacroDetail) : FindMacroGraphByName(MacroName, NodeObj, Blueprint) != nullptr;
            if (!bMacroResolved)
            {
                if (MacroErrorCode.IsEmpty()) MacroErrorCode = TEXT("macro_graph_missing");
                AppendLine(Report, FString::Printf(TEXT("N2C_MACRO_REFERENCE_RESOLVE|result=FAIL|code=%s|macro=%s|detail=%s"), *MacroErrorCode, *MacroName, *MacroDetail));
                AppendLine(Report, FString::Printf(TEXT("ERROR: MacroInstance '%s' could not resolve its exact macro graph. Rejected before mutation."), *MacroName));
                return false;
            }
            AppendLine(Report, FString::Printf(TEXT("N2C_MACRO_REFERENCE_RESOLVE|result=PASS|macro=%s|graph=%s"), *MacroName, ResolvedMacroGraph ? *ResolvedMacroGraph->GetPathName() : TEXT("legacy")));
        }

        if (Lower == TEXT("addcomponent") || Lower == TEXT("add_component") || Lower.Contains(TEXT("addcomponent")))
        {
            UClass* ComponentClass = ResolveComponentClassFromJson(NodeObj);
            if (!Blueprint || !FBlueprintEditorUtils::IsActorBased(Blueprint) || !Blueprint->GeneratedClass || !ComponentClass ||
                !ComponentClass->IsChildOf(UActorComponent::StaticClass()) || !FKismetEditorUtilities::IsClassABlueprintSpawnableComponent(ComponentClass))
            {
                AppendLine(Report, TEXT("ERROR: AddComponent requires an Actor Blueprint with GeneratedClass and a valid UActorComponent component_class. Rejected before mutation."));
                return false;
            }
        }

        if (Lower == TEXT("componentboundevent") || Lower == TEXT("component_bound_event") || Lower.Contains(TEXT("componentboundevent")))
        {
            const FString ComponentName = GetStringFieldSafe(NodeObj, TEXT("component_property_name"), GetStringFieldSafe(NodeObj, TEXT("component_name")));
            const FString EventName = GetStringFieldSafe(NodeObj, TEXT("delegate_name"),
            GetStringFieldSafe(NodeObj, TEXT("delegate_property_name"),
                GetStringFieldSafe(NodeObj, TEXT("event_name"), GetStringFieldSafe(NodeObj, TEXT("name")))));
            FObjectProperty* ComponentProperty = nullptr;
            if (Blueprint)
            {
                if (Blueprint->SkeletonGeneratedClass) ComponentProperty = FindFProperty<FObjectProperty>(Blueprint->SkeletonGeneratedClass, FName(*ComponentName));
                if (!ComponentProperty && Blueprint->GeneratedClass) ComponentProperty = FindFProperty<FObjectProperty>(Blueprint->GeneratedClass, FName(*ComponentName));
            }
            if (!ComponentProperty || !ComponentProperty->PropertyClass || !FindFProperty<FMulticastDelegateProperty>(ComponentProperty->PropertyClass, FName(*EventName)))
            {
                AppendLine(Report, FString::Printf(TEXT("ERROR: ComponentBoundEvent could not resolve compiled component '%s' and delegate '%s'. Compile after SCS import, then run the event patch. Rejected before mutation."), *ComponentName, *EventName));
                return false;
            }
        }

        // P1 native nodes have editor-visible context that must be supplied before
        // allocating pins.  Reject incomplete records here, before Apply clears or
        // mutates a graph; this keeps incomplete project exports transactional.
        const bool bEnumP1 = Lower.Contains(TEXT("getenumeratornameasstring")) ||
            Lower.Contains(TEXT("foreachelementinenum")) || Lower.Contains(TEXT("castbytetoenum")) ||
            Lower.Contains(TEXT("enumliteral"));
        const bool bEnumSpecialized = bEnumP1 || Lower.Contains(TEXT("switchenum")) ||
            Lower.Contains(TEXT("enumequality")) || Lower.Contains(TEXT("enuminequality"));
        UEnum* ResolvedEnum = bEnumSpecialized ? Cast<UEnum>(ResolveTypeObjectFromJson(NodeObj)) : nullptr;
        if (bEnumSpecialized && !ResolvedEnum)
        {
            AppendLine(Report, TEXT("N2C_PREFLIGHT_GUARD|code=enum_identity_missing"));
            AppendLine(Report, FString::Printf(TEXT("ERROR: enum node '%s' requires a resolvable enum_path before mutation."), *Type));
            return false;
        }
        const TArray<TSharedPtr<FJsonValue>>* EnumCases = nullptr;
        if (ResolvedEnum && NodeObj->TryGetArrayField(TEXT("enum_cases"), EnumCases) && EnumCases)
        {
            for (const TSharedPtr<FJsonValue>& CaseValue : *EnumCases)
            {
                const FString CaseName = CaseValue.IsValid() ? CaseValue->AsString() : FString();
                if (!CaseName.IsEmpty() && ResolvedEnum->GetIndexByNameString(CaseName) == INDEX_NONE)
                {
                    AppendLine(Report, FString::Printf(TEXT("N2C_PREFLIGHT_GUARD|code=enum_case_mismatch|case=%s"), *CaseName));
                    return false;
                }
            }
        }
        if (ResolvedEnum)
        {
            const FString SelectedValue = GetStringFieldSafe(NodeObj, TEXT("enum_value"), GetStringFieldSafe(NodeObj, TEXT("selected_value")));
            if (!SelectedValue.IsEmpty() && ResolvedEnum->GetIndexByNameString(SelectedValue) == INDEX_NONE)
            {
                AppendLine(Report, FString::Printf(TEXT("N2C_PREFLIGHT_GUARD|code=enum_case_mismatch|case=%s"), *SelectedValue));
                return false;
            }
        }
        if (Lower.Contains(TEXT("createwidget")))
        {
            const bool bClassPinLinked = GetBoolFieldSafe(NodeObj, TEXT("class_pin_linked"), false);
            UClass* WidgetClass = ResolveClassFromJson(NodeObj);
            if (!WidgetClass || !WidgetClass->IsChildOf(UUserWidget::StaticClass()))
            {
                AppendLine(Report, FString::Printf(TEXT("N2C_PREFLIGHT_GUARD|code=create_widget_class_identity_missing|variant=%s"), bClassPinLinked ? TEXT("linked") : TEXT("literal")));
                AppendLine(Report, TEXT("ERROR: CreateWidget requires a resolvable literal class_path or linked result_class_path before mutation."));
                return false;
            }
        }
        if (Lower.Contains(TEXT("inputaction")) && GetStringFieldSafe(NodeObj, TEXT("input_action_name")).IsEmpty())
        {
            AppendLine(Report, TEXT("N2C_PREFLIGHT_GUARD|code=input_action_identity_missing"));
            AppendLine(Report, TEXT("ERROR: InputAction requires input_action_name before mutation."));
            return false;
        }
        if (Lower.Contains(TEXT("inputaxis")) && !Lower.Contains(TEXT("key")) && GetStringFieldSafe(NodeObj, TEXT("input_axis_name")).IsEmpty())
        {
            AppendLine(Report, TEXT("ERROR: InputAxisEvent requires input_axis_name before mutation."));
            return false;
        }
        if ((Lower.Contains(TEXT("inputkey")) || Lower.Contains(TEXT("inputaxiskey"))) && GetStringFieldSafe(NodeObj, TEXT("key_name")).IsEmpty())
        {
            AppendLine(Report, TEXT("N2C_PREFLIGHT_GUARD|code=input_key_identity_missing"));
            AppendLine(Report, TEXT("ERROR: key input event requires key_name before mutation."));
            return false;
        }
        if ((Lower.Contains(TEXT("callmaterialparametercollection")) || Lower.Contains(TEXT("message"))) && !ResolveFunction(NodeObj, Blueprint))
        {
            AppendLine(Report, FString::Printf(TEXT("ERROR: P1 call node '%s' requires a resolvable function_path before mutation."), *Type));
            return false;
        }
        const bool bMCDelegateNode = Lower.Contains(TEXT("adddelegate")) || Lower.Contains(TEXT("assigndelegate")) || Lower.Contains(TEXT("removedelegate")) ||
            Lower.Contains(TEXT("calldelegate")) || Lower.Contains(TEXT("cleardelegate")) || Lower == TEXT("binddelegate") || Lower == TEXT("bind_delegate") ||
            Lower == TEXT("unbinddelegate") || Lower == TEXT("unbind_delegate");
        if (bMCDelegateNode)
        {
            UClass* OwnerClass = nullptr;
            if (!ResolveMulticastDelegateProperty(Blueprint, NodeObj, OwnerClass))
            {
                AppendLine(Report, FString::Printf(TEXT("N2C_PREFLIGHT_GUARD|code=delegate_identity_mismatch|node=%s"), *Type));
                AppendLine(Report, FString::Printf(TEXT("ERROR: delegate node '%s' could not resolve delegate property '%s'. Rejected before mutation."), *Type, *GetStringFieldSafe(NodeObj, TEXT("delegate_name"), GetStringFieldSafe(NodeObj, TEXT("name")))));
                return false;
            }
        }

        return true;
    }

    static UEdGraphNode* CreatePatchNode(UEdGraph* Graph, UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& NodeObj, int32 Index, FString& Report)
    {
        if (!Graph || !NodeObj.IsValid())
        {
            return nullptr;
        }

        FString Type = GetStringFieldSafe(NodeObj, TEXT("type"));
        if (Type.IsEmpty())
        {
            Type = GetStringFieldSafe(NodeObj, TEXT("class"));
        }
        const FString Lower = Type.ToLower();

        const int32 PosX = GetStringFieldSafe(NodeObj, TEXT("pos_x")).IsEmpty() ? 300 + Index * 280 : FCString::Atoi(*GetStringFieldSafe(NodeObj, TEXT("pos_x")));
        const int32 PosY = GetStringFieldSafe(NodeObj, TEXT("pos_y")).IsEmpty() ? 0 : FCString::Atoi(*GetStringFieldSafe(NodeObj, TEXT("pos_y")));

        if (Lower == TEXT("animgraphnode_localrefpose") || Lower == TEXT("local_ref_pose") || Lower == TEXT("localrefpose"))
        {
            if (!Graph->GetSchema()->IsA(UAnimationGraphSchema::StaticClass()))
            {
                AppendLine(Report, TEXT("WARNING: LocalRefPose is only valid in an Animation Graph."));
                return nullptr;
            }
            FGraphNodeCreator<UAnimGraphNode_LocalRefPose> Creator(*Graph);
            UAnimGraphNode_LocalRefPose* Node = Creator.CreateNode();
            Node->NodePosX = PosX;
            Node->NodePosY = PosY;
            Creator.Finalize();
            return Node;
        }

        const FString TraceGraphName = Graph ? Graph->GetName() : FString(TEXT("<null>"));
        UE_LOG(LogNodeToCode, Display, TEXT("N2C patch apply trace: create node request type='%s' graph='%s'"), *Type, *TraceGraphName);

        if (Lower == TEXT("addcomponent") || Lower == TEXT("add_component") || Lower.Contains(TEXT("addcomponent")))
        {
            return CreateAddComponentNode(Graph, Blueprint, NodeObj, PosX, PosY, Report);
        }

        if (Lower == TEXT("delay") || Lower == TEXT("latent_delay") || Lower.Contains(TEXT("delay")))
        {
            return CreateCallFunctionNode(Graph, Blueprint, NodeObj, PosX, PosY, Report, TEXT("Delay"));
        }

        if (Lower == TEXT("getdatatablerow") || Lower == TEXT("get_data_table_row") || Lower.Contains(TEXT("getdatatablerow")))
        {
            return CreateGetDataTableRowNode(Graph, NodeObj, PosX, PosY, Report);
        }

        if (Lower == TEXT("setfieldsinstruct") || Lower == TEXT("setmembersinstruct") ||
            Lower == TEXT("set_fields_in_struct") || Lower == TEXT("set_members_in_struct") ||
            Lower.Contains(TEXT("setfieldsinstruct")) || Lower.Contains(TEXT("setmembersinstruct")))
        {
            return CreateSetFieldsInStructNode(Graph, NodeObj, PosX, PosY, Report);
        }

        if (Lower == TEXT("k2node_knot") || Lower == TEXT("knot") || Lower == TEXT("reroute") || Lower.Contains(TEXT("reroute")))
        {
            // Keep reroutes as real UE4.27 knot nodes.  Their wildcard type is
            // propagated by ConnectEdges, preserving every exported node ID.
            FGraphNodeCreator<UK2Node_Knot> Creator(*Graph);
            UK2Node_Knot* Node = Creator.CreateNode();
            Node->NodePosX = PosX;
            Node->NodePosY = PosY;
            Creator.Finalize();
            return Node;
        }
        if (Lower == TEXT("k2node_getarrayitem") || Lower == TEXT("getarrayitem") || Lower == TEXT("get_array_item"))
        {
            return CreateGetArrayItemNode(Graph, NodeObj, PosX, PosY);
        }
        if (Lower == TEXT("k2node_self") || Lower == TEXT("self"))
        {
            UEdGraphNode* Node = CreateNativeK2NodeUnfinished(Graph, TEXT("K2Node_Self"), PosX, PosY, Report);
            FinishNativeK2Node(Node);
            return Node;
        }
        if (Lower == TEXT("k2node_callparentfunction") || Lower == TEXT("callparentfunction") || Lower == TEXT("call_parent_function"))
        {
            return CreateCallFunctionSubclassNode(Graph, Blueprint, NodeObj, TEXT("K2Node_CallParentFunction"), TEXT(""), TEXT(""), PosX, PosY, Report);
        }
        if (Lower == TEXT("k2node_callfunctiononmember") || Lower == TEXT("callfunctiononmember") || Lower == TEXT("call_function_on_member"))
        {
            return CreateCallFunctionSubclassNode(Graph, Blueprint, NodeObj, TEXT("K2Node_CallFunctionOnMember"), TEXT(""), TEXT(""), PosX, PosY, Report);
        }
        if (Lower == TEXT("k2node_event") || Lower == TEXT("event") || Lower == TEXT("builtin_event"))
        {
            return CreateBuiltinEventNode(Graph, Blueprint, NodeObj, PosX, PosY, Report);
        }

        if (IsGuardedSafeSkippedNodeType(Type))
        {
            AppendLine(Report, FString::Printf(TEXT("WARNING: unsupported_but_safe_skipped node type '%s'. Import skipped it to avoid UE4.27 BlueprintGraph crash; export/dry-run coverage remains available."), *Type));
            return nullptr;
        }

        if (Lower.Contains(TEXT("functionentry")) || Lower == TEXT("entry"))
        {
            return FindOrCreateEntryNode(Graph, Graph->GetName());
        }
        if (Lower.Contains(TEXT("functionresult")) || Lower == TEXT("return"))
        {
            if (GetBoolFieldSafe(NodeObj, TEXT("create_new"), false) || GetBoolFieldSafe(NodeObj, TEXT("force_new"), false))
            {
                return CreateAdditionalResultNode(Graph, Graph->GetName(), PosX, PosY);
            }
            return FindOrCreateResultNode(Graph, Graph->GetName());
        }
        if (Lower == TEXT("binaryoperator") || Lower == TEXT("binary_operator") ||
            Lower == TEXT("commutativeassociativebinaryoperator") || Lower.Contains(TEXT("commutativeassociativebinaryoperator")))
        {
            return CreateBinaryOperatorNode(Graph, Blueprint, NodeObj, PosX, PosY, Report);
        }
        if (Lower == TEXT("branch") || Lower.Contains(TEXT("ifthenelse")))
        {
            FGraphNodeCreator<UK2Node_IfThenElse> Creator(*Graph);
            UK2Node_IfThenElse* Branch = Creator.CreateNode();
            Branch->NodePosX = PosX;
            Branch->NodePosY = PosY;
            Creator.Finalize();
            return Branch;
        }
        if (Lower == TEXT("sequence") || Lower.Contains(TEXT("executionsequence")))
        {
            UEdGraphNode* Node = CreateNativeK2NodeUnfinished(Graph, TEXT("K2Node_ExecutionSequence"), PosX, PosY, Report);
            FinishNativeK2Node(Node);
            return Node;
        }
        if (Lower == TEXT("select") || Lower.Contains(TEXT("k2node_select")))
        {
            UEdGraphNode* Node = CreateNativeK2NodeUnfinished(Graph, TEXT("K2Node_Select"), PosX, PosY, Report);
            if (Node)
            {
                const TSharedPtr<FJsonObject>* IndexPinTypeObj = nullptr;
                if (NodeObj->TryGetObjectField(TEXT("index_pin_type"), IndexPinTypeObj) && IndexPinTypeObj && IndexPinTypeObj->IsValid())
                {
                    SetPinTypePropertyByName(Node, TEXT("IndexPinType"), MakePinTypeFromJson(*IndexPinTypeObj));
                }
                SetIntPropertyByName(Node, TEXT("NumOptionPins"), FCString::Atoi(*GetStringFieldSafe(NodeObj, TEXT("option_count"), TEXT("2"))));
            }
            FinishNativeK2Node(Node);
            return Node;
        }
        if (Lower == TEXT("makearray") || Lower == TEXT("make_array") || Lower.Contains(TEXT("makearray")))
        {
            UEdGraphNode* Node = CreateNativeK2NodeUnfinished(Graph, TEXT("K2Node_MakeArray"), PosX, PosY, Report);
            SetIntPropertyByName(Node, TEXT("NumInputs"), FCString::Atoi(*GetStringFieldSafe(NodeObj, TEXT("input_count"), TEXT("1"))));

            const TSharedPtr<FJsonObject>* ValuePinTypeObj = nullptr;
            if (NodeObj->TryGetObjectField(TEXT("value_pin_type"), ValuePinTypeObj) && ValuePinTypeObj && ValuePinTypeObj->IsValid())
            {
                const FEdGraphPinType ElementType = MakePinTypeFromJson(*ValuePinTypeObj);
                SetPinTypePropertyByName(Node, TEXT("OutputType"), MakeArrayOutputPinTypeFromElementType(ElementType));
                FinishNativeK2Node(Node);
                ApplyMakeArrayElementType(Node, ElementType);
            }
            else
            {
                FinishNativeK2Node(Node);
            }
            return Node;
        }
        if (Lower == TEXT("macroinstance") || Lower == TEXT("macro_instance") || Lower.Contains(TEXT("macroinstance")))
        {
            return CreateMacroInstanceNode(Graph, GetStringFieldSafe(NodeObj, TEXT("macro_name"), GetStringFieldSafe(NodeObj, TEXT("name"), TEXT("IsValid"))), PosX, PosY, Report, NodeObj, Blueprint);
        }
        if (Lower == TEXT("isvalid") || Lower == TEXT("is_valid"))
        {
            return CreateMacroInstanceNode(Graph, GetStringFieldSafe(NodeObj, TEXT("macro_name"), TEXT("IsValid")), PosX, PosY, Report, NodeObj, Blueprint);
        }
        if (Lower == TEXT("incrementint") || Lower == TEXT("increment_int"))
        {
            return CreateMacroInstanceNode(Graph, GetStringFieldSafe(NodeObj, TEXT("macro_name"), TEXT("IncrementInt")), PosX, PosY, Report, NodeObj, Blueprint);
        }
        if (Lower == TEXT("foreachloop") || Lower == TEXT("for_each_loop"))
        {
            return CreateMacroInstanceNode(Graph, GetStringFieldSafe(NodeObj, TEXT("macro_name"), TEXT("ForEachLoop")), PosX, PosY, Report, NodeObj, Blueprint);
        }
        if (Lower == TEXT("foreachloopwithbreak") || Lower == TEXT("for_each_loop_with_break"))
        {
            return CreateMacroInstanceNode(Graph, GetStringFieldSafe(NodeObj, TEXT("macro_name"), TEXT("ForEachLoopWithBreak")), PosX, PosY, Report, NodeObj, Blueprint);
        }
        if (Lower == TEXT("flipflop") || Lower == TEXT("flip_flop"))
        {
            return CreateMacroInstanceNode(Graph, GetStringFieldSafe(NodeObj, TEXT("macro_name"), TEXT("FlipFlop")), PosX, PosY, Report, NodeObj, Blueprint);
        }
        if (Lower == TEXT("forloop") || Lower == TEXT("for_loop"))
        {
            return CreateMacroInstanceNode(Graph, GetStringFieldSafe(NodeObj, TEXT("macro_name"), TEXT("ForLoop")), PosX, PosY, Report, NodeObj, Blueprint);
        }
        if (Lower == TEXT("forloopwithbreak") || Lower == TEXT("for_loop_with_break"))
        {
            return CreateMacroInstanceNode(Graph, GetStringFieldSafe(NodeObj, TEXT("macro_name"), TEXT("ForLoopWithBreak")), PosX, PosY, Report, NodeObj, Blueprint);
        }
        if (Lower == TEXT("whileloop") || Lower == TEXT("while_loop"))
        {
            return CreateMacroInstanceNode(Graph, GetStringFieldSafe(NodeObj, TEXT("macro_name"), TEXT("WhileLoop")), PosX, PosY, Report, NodeObj, Blueprint);
        }
        if (Lower == TEXT("doonce") || Lower == TEXT("do_once"))
        {
            return CreateMacroInstanceNode(Graph, GetStringFieldSafe(NodeObj, TEXT("macro_name"), TEXT("DoOnce")), PosX, PosY, Report, NodeObj, Blueprint);
        }
        if (Lower == TEXT("gate"))
        {
            return CreateMacroInstanceNode(Graph, GetStringFieldSafe(NodeObj, TEXT("macro_name"), TEXT("Gate")), PosX, PosY, Report, NodeObj, Blueprint);
        }
        if (Lower == TEXT("arrayget") || Lower == TEXT("array_get"))
        {
            return CreateCallArrayFunctionNode(Graph, Blueprint, NodeObj, PosX, PosY, Report, TEXT("Array_Get"));
        }
        if (Lower == TEXT("arrayadd") || Lower == TEXT("array_add"))
        {
            return CreateCallArrayFunctionNode(Graph, Blueprint, NodeObj, PosX, PosY, Report, TEXT("Array_Add"));
        }
        if (Lower == TEXT("arrayaddunique") || Lower == TEXT("array_add_unique"))
        {
            return CreateCallArrayFunctionNode(Graph, Blueprint, NodeObj, PosX, PosY, Report, TEXT("Array_AddUnique"));
        }
        if (Lower == TEXT("arrayclear") || Lower == TEXT("array_clear"))
        {
            return CreateCallArrayFunctionNode(Graph, Blueprint, NodeObj, PosX, PosY, Report, TEXT("Array_Clear"));
        }
        if (Lower == TEXT("arraycontains") || Lower == TEXT("array_contains"))
        {
            return CreateCallArrayFunctionNode(Graph, Blueprint, NodeObj, PosX, PosY, Report, TEXT("Array_Contains"));
        }
        if (Lower == TEXT("arrayfind") || Lower == TEXT("array_find"))
        {
            return CreateCallArrayFunctionNode(Graph, Blueprint, NodeObj, PosX, PosY, Report, TEXT("Array_Find"));
        }
        if (Lower == TEXT("arrayset") || Lower == TEXT("array_set"))
        {
            return CreateCallArrayFunctionNode(Graph, Blueprint, NodeObj, PosX, PosY, Report, TEXT("Array_Set"));
        }
        if (Lower == TEXT("arrayremove") || Lower == TEXT("array_remove"))
        {
            return CreateCallArrayFunctionNode(Graph, Blueprint, NodeObj, PosX, PosY, Report, TEXT("Array_Remove"));
        }
        if (Lower == TEXT("arrayremoveitem") || Lower == TEXT("array_remove_item"))
        {
            return CreateCallArrayFunctionNode(Graph, Blueprint, NodeObj, PosX, PosY, Report, TEXT("Array_RemoveItem"));
        }
        if (Lower == TEXT("arraylength") || Lower == TEXT("array_length"))
        {
            return CreateCallArrayFunctionNode(Graph, Blueprint, NodeObj, PosX, PosY, Report, TEXT("Array_Length"));
        }
        if (Lower == TEXT("callarrayfunction") || Lower == TEXT("call_array_function") || Lower.Contains(TEXT("callarrayfunction")))
        {
            const FString FallbackName = GetStringFieldSafe(NodeObj, TEXT("function_name"), TEXT("Array_Length"));
            return CreateCallArrayFunctionNode(Graph, Blueprint, NodeObj, PosX, PosY, Report, FallbackName);
        }
        if (Lower == TEXT("switchenum") || Lower == TEXT("switch_enum") || Lower.Contains(TEXT("switchenum")))
        {
            UObject* TypeObject = ResolveTypeObjectFromJson(NodeObj);
            UEnum* EnumType = Cast<UEnum>(TypeObject);
            if (!EnumType)
            {
                AppendLine(Report, FString::Printf(TEXT("WARNING: SwitchEnum node skipped because enum type could not be resolved from '%s'."), *GetTypeObjectPathFromJson(NodeObj)));
                return nullptr;
            }

            FGraphNodeCreator<UK2Node_SwitchEnum> Creator(*Graph);
            UK2Node_SwitchEnum* SwitchEnum = Creator.CreateNode();
            SwitchEnum->NodePosX = PosX;
            SwitchEnum->NodePosY = PosY;

            // Use reflection instead of direct field access so this stays safe across minor UE4.27 forks.
            if (FObjectProperty* EnumProp = FindFProperty<FObjectProperty>(SwitchEnum->GetClass(), TEXT("Enum")))
            {
                EnumProp->SetObjectPropertyValue_InContainer(SwitchEnum, EnumType);
            }
            else
            {
                AppendLine(Report, TEXT("WARNING: SwitchEnum node created but Enum property was not found."));
            }

            Creator.Finalize();
            SwitchEnum->ReconstructNode();
            return SwitchEnum;
        }
        if (Lower == TEXT("enumequality") || Lower == TEXT("enum_equality") || Lower.Contains(TEXT("enumequality")))
        {
            return CreateEnumComparisonNode(Graph, Blueprint, NodeObj, TEXT("K2Node_EnumEquality"), PosX, PosY, Report);
        }
        if (Lower == TEXT("enuminequality") || Lower == TEXT("enum_inequality") || Lower.Contains(TEXT("enuminequality")))
        {
            return CreateEnumComparisonNode(Graph, Blueprint, NodeObj, TEXT("K2Node_EnumInequality"), PosX, PosY, Report);
        }
        if (Lower == TEXT("getdatatablerow") || Lower == TEXT("get_data_table_row") || Lower.Contains(TEXT("getdatatablerow")))
        {
            return CreateGetDataTableRowNode(Graph, NodeObj, PosX, PosY, Report);
        }
        if (Lower == TEXT("callfunction") || Lower == TEXT("call_function") || Lower.Contains(TEXT("callfunction")) ||
            Lower == TEXT("interfacecall") || Lower == TEXT("interface_call"))
        {
            return CreateCallFunctionNode(Graph, Blueprint, NodeObj, PosX, PosY, Report);
        }
        if (Lower == TEXT("breakstruct") || Lower == TEXT("break_struct") || Lower.Contains(TEXT("breakstruct")))
        {
            UObject* TypeObject = ResolveTypeObjectFromJson(NodeObj);
            UScriptStruct* StructType = Cast<UScriptStruct>(TypeObject);
            if (!StructType)
            {
                AppendLine(Report, FString::Printf(TEXT("WARNING: BreakStruct node skipped because struct type could not be resolved from '%s'."), *GetTypeObjectPathFromJson(NodeObj)));
                return nullptr;
            }

            if (ShouldGuardGenericStructMakeBreak(StructType))
            {
                AppendLine(Report, FString::Printf(TEXT("WARNING: BreakStruct guarded/skipped for native make/break struct '%s'. Use the specialized Kismet break function instead."), *StructType->GetPathName()));
                return nullptr;
            }

            FGraphNodeCreator<UK2Node_BreakStruct> Creator(*Graph);
            UK2Node_BreakStruct* BreakStruct = Creator.CreateNode();
            BreakStruct->StructType = StructType;
            BreakStruct->NodePosX = PosX;
            BreakStruct->NodePosY = PosY;
            Creator.Finalize();
            BreakStruct->ReconstructNode();
            return BreakStruct;
        }
        if (Lower == TEXT("makestruct") || Lower == TEXT("make_struct") || Lower.Contains(TEXT("makestruct")))
        {
            return CreateStructBackedNode(Graph, NodeObj, TEXT("K2Node_MakeStruct"), PosX, PosY, Report);
        }
        if (Lower == TEXT("setfieldsinstruct") || Lower == TEXT("setmembersinstruct") ||
            Lower == TEXT("set_fields_in_struct") || Lower == TEXT("set_members_in_struct") ||
            Lower.Contains(TEXT("setfieldsinstruct")) || Lower.Contains(TEXT("setmembersinstruct")))
        {
            return CreateSetFieldsInStructNode(Graph, NodeObj, PosX, PosY, Report);
        }
        if (Lower == TEXT("customevent") || Lower == TEXT("custom_event") || Lower.Contains(TEXT("customevent")))
        {
            UEdGraphNode* Node = CreateNativeK2NodeUnfinished(Graph, TEXT("K2Node_CustomEvent"), PosX, PosY, Report);
            const FString EventName = GetStringFieldSafe(NodeObj, TEXT("event_name"), GetStringFieldSafe(NodeObj, TEXT("custom_function_name"), GetStringFieldSafe(NodeObj, TEXT("name"))));
            SetNamePropertyByName(Node, TEXT("CustomFunctionName"), EventName);
            FinishNativeK2Node(Node);
            return Node;
        }
        if (Lower == TEXT("spawnactorfromclass") || Lower == TEXT("spawn_actor_from_class") || Lower.Contains(TEXT("spawnactorfromclass")))
        {
            UEdGraphNode* Node = CreateNativeK2NodeUnfinished(Graph, TEXT("K2Node_SpawnActorFromClass"), PosX, PosY, Report);
            FinishNativeK2Node(Node);
            return Node;
        }
        if (Lower == TEXT("timeline") || Lower == TEXT("timeline_node") || Lower.Contains(TEXT("timeline")))
        {
            return CreateTimelineNode(Graph, Blueprint, NodeObj, PosX, PosY, Report);
        }
        if (Lower == TEXT("addcomponent") || Lower == TEXT("add_component") || Lower.Contains(TEXT("addcomponent")))
        {
            return CreateAddComponentNode(Graph, Blueprint, NodeObj, PosX, PosY, Report);
        }
        if (Lower == TEXT("dynamiccast") || Lower == TEXT("castto") || Lower == TEXT("cast_to") || Lower.Contains(TEXT("dynamiccast")))
        {
            UEdGraphNode* Node = CreateNativeK2NodeUnfinished(Graph, TEXT("K2Node_DynamicCast"), PosX, PosY, Report);
            UClass* TargetClass = ResolveClassFromJson(NodeObj);
            SetObjectPropertyByName(Node, TEXT("TargetType"), TargetClass);
            FinishNativeK2Node(Node);
            return Node;
        }
        if (Lower == TEXT("componentboundevent") || Lower == TEXT("component_bound_event") || Lower.Contains(TEXT("componentboundevent")))
        {
            return CreateComponentBoundEventNode(Graph, Blueprint, NodeObj, PosX, PosY, Report);
        }
        if (Lower == TEXT("assigndelegate") || Lower == TEXT("assign_delegate") || Lower.Contains(TEXT("assigndelegate")))
        {
            return CreateMulticastDelegateNode<UK2Node_AssignDelegate>(Graph, Blueprint, NodeObj, PosX, PosY, Report, TEXT("AssignDelegate"));
        }
        if (Lower == TEXT("adddelegate") || Lower == TEXT("binddelegate") || Lower == TEXT("bind_delegate") || Lower.Contains(TEXT("adddelegate")))
        {
            return CreateMulticastDelegateNode<UK2Node_AddDelegate>(Graph, Blueprint, NodeObj, PosX, PosY, Report, TEXT("AddDelegate"));
        }
        if (Lower == TEXT("removedelegate") || Lower == TEXT("remove_delegate") || Lower == TEXT("unbinddelegate") || Lower == TEXT("unbind_delegate") || Lower.Contains(TEXT("removedelegate")))
        {
            return CreateMulticastDelegateNode<UK2Node_RemoveDelegate>(Graph, Blueprint, NodeObj, PosX, PosY, Report, TEXT("RemoveDelegate"));
        }
        if (Lower == TEXT("calldelegate") || Lower == TEXT("call_delegate") || Lower.Contains(TEXT("calldelegate")))
        {
            return CreateMulticastDelegateNode<UK2Node_CallDelegate>(Graph, Blueprint, NodeObj, PosX, PosY, Report, TEXT("CallDelegate"));
        }
        if (Lower == TEXT("cleardelegate") || Lower == TEXT("clear_delegate") || Lower.Contains(TEXT("cleardelegate")))
        {
            return CreateMulticastDelegateNode<UK2Node_ClearDelegate>(Graph, Blueprint, NodeObj, PosX, PosY, Report, TEXT("ClearDelegate"));
        }
        if (Lower == TEXT("createdelegate") || Lower == TEXT("create_delegate") || Lower.Contains(TEXT("createdelegate")))
        {
            return CreateCreateDelegateNode(Graph, NodeObj, PosX, PosY, Report);
        }
        if (Lower == TEXT("variableget") || Lower == TEXT("variable_get") || Lower.Contains(TEXT("variableget")))
        {
            const FString VarName = GetStringFieldSafe(NodeObj, TEXT("variable_name"), GetStringFieldSafe(NodeObj, TEXT("member_name"), GetStringFieldSafe(NodeObj, TEXT("name"))));
            if (VarName.IsEmpty())
            {
                AppendLine(Report, TEXT("WARNING: VariableGet node skipped because variable_name/member_name is empty."));
                return nullptr;
            }
            FGraphNodeCreator<UK2Node_VariableGet> Creator(*Graph);
            UK2Node_VariableGet* Var = Creator.CreateNode();
            Var->VariableReference.SetSelfMember(FName(*VarName));
            Var->NodePosX = PosX;
            Var->NodePosY = PosY;
            Creator.Finalize();
            return Var;
        }
        if (Lower == TEXT("variableset") || Lower == TEXT("variable_set") || Lower.Contains(TEXT("variableset")))
        {
            const FString VarName = GetStringFieldSafe(NodeObj, TEXT("variable_name"), GetStringFieldSafe(NodeObj, TEXT("member_name"), GetStringFieldSafe(NodeObj, TEXT("name"))));
            if (VarName.IsEmpty())
            {
                AppendLine(Report, TEXT("WARNING: VariableSet node skipped because variable_name/member_name is empty."));
                return nullptr;
            }
            FGraphNodeCreator<UK2Node_VariableSet> Creator(*Graph);
            UK2Node_VariableSet* Var = Creator.CreateNode();
            Var->VariableReference.SetSelfMember(FName(*VarName));
            Var->NodePosX = PosX;
            Var->NodePosY = PosY;
            Creator.Finalize();
            return Var;
        }

        // P1 native constructors use the same reflected context as the UE4.27 editor.
        FString P1NativeClass;
        if (Lower.Contains(TEXT("getenumeratornameasstring"))) P1NativeClass = TEXT("K2Node_GetEnumeratorNameAsString");
        else if (Lower.Contains(TEXT("createwidget"))) P1NativeClass = TEXT("K2Node_CreateWidget");
        else if (Lower.Contains(TEXT("inputaction"))) P1NativeClass = TEXT("K2Node_InputAction");
        else if (Lower.Contains(TEXT("inputaxis"))) P1NativeClass = Lower.Contains(TEXT("key")) ? TEXT("K2Node_InputAxisKeyEvent") : TEXT("K2Node_InputAxisEvent");
        else if (Lower.Contains(TEXT("inputkey"))) P1NativeClass = TEXT("K2Node_InputKey");
        else if (Lower.Contains(TEXT("inputtouch"))) P1NativeClass = TEXT("K2Node_InputTouchEvent");
        else if (Lower.Contains(TEXT("foreachelementinenum"))) P1NativeClass = TEXT("K2Node_ForEachElementInEnum");
        else if (Lower.Contains(TEXT("easefunction"))) P1NativeClass = TEXT("K2Node_EaseFunction");
        else if (Lower.Contains(TEXT("castbytetoenum"))) P1NativeClass = TEXT("K2Node_CastByteToEnum");
        else if (Lower.Contains(TEXT("enumliteral"))) P1NativeClass = TEXT("K2Node_EnumLiteral");
        if (!P1NativeClass.IsEmpty())
        {
            UEdGraphNode* Node = CreateNativeK2NodeUnfinished(Graph, P1NativeClass, PosX, PosY, Report);
            if (!Node) return nullptr;
            const FString InputEvent = GetStringFieldSafe(NodeObj, TEXT("input_event"), TEXT("Pressed"));
            int32 InputEventValue = InputEvent.Equals(TEXT("Released"), ESearchCase::IgnoreCase) ? 1 : InputEvent.Equals(TEXT("Repeat"), ESearchCase::IgnoreCase) ? 2 : InputEvent.Equals(TEXT("DoubleClick"), ESearchCase::IgnoreCase) ? 3 : InputEvent.Equals(TEXT("Axis"), ESearchCase::IgnoreCase) ? 4 : 0;
            double NumericInputEvent = 0.0;
            if (NodeObj->TryGetNumberField(TEXT("input_event_value"), NumericInputEvent)) InputEventValue = static_cast<int32>(NumericInputEvent);
            SetBoolPropertyByName(Node, TEXT("bConsumeInput"), GetBoolFieldSafe(NodeObj, TEXT("consume_input"), true));
            SetBoolPropertyByName(Node, TEXT("bExecuteWhenPaused"), GetBoolFieldSafe(NodeObj, TEXT("execute_when_paused"), false));
            SetBoolPropertyByName(Node, TEXT("bOverrideParentBinding"), GetBoolFieldSafe(NodeObj, TEXT("override_parent_binding"), true));
            if (P1NativeClass == TEXT("K2Node_InputAction"))
            {
                SetNamePropertyByName(Node, TEXT("InputActionName"), GetStringFieldSafe(NodeObj, TEXT("input_action_name")));
            }
            else if (P1NativeClass == TEXT("K2Node_InputAxisEvent"))
            {
                SetNamePropertyByName(Node, TEXT("InputAxisName"), GetStringFieldSafe(NodeObj, TEXT("input_axis_name")));
            }
            else if (P1NativeClass == TEXT("K2Node_InputAxisKeyEvent"))
            {
                SetStructPropertyFromTextByName(Node, TEXT("AxisKey"), GetStringFieldSafe(NodeObj, TEXT("key_name")));
            }
            else if (P1NativeClass == TEXT("K2Node_InputKey"))
            {
                SetStructPropertyFromTextByName(Node, TEXT("InputKey"), GetStringFieldSafe(NodeObj, TEXT("key_name")));
                SetBoolPropertyByName(Node, TEXT("bShift"), GetBoolFieldSafe(NodeObj, TEXT("shift"), false));
                SetBoolPropertyByName(Node, TEXT("bControl"), GetBoolFieldSafe(NodeObj, TEXT("ctrl"), false));
                SetBoolPropertyByName(Node, TEXT("bAlt"), GetBoolFieldSafe(NodeObj, TEXT("alt"), false));
                SetBoolPropertyByName(Node, TEXT("bCommand"), GetBoolFieldSafe(NodeObj, TEXT("cmd"), false));
            }
            else if (P1NativeClass == TEXT("K2Node_InputTouchEvent"))
            {
                SetBytePropertyByName(Node, TEXT("InputKeyEvent"), static_cast<uint8>(InputEventValue));
            }
            if (P1NativeClass == TEXT("K2Node_CreateWidget"))
            {
                UClass* WidgetClass = ResolveClassFromJson(NodeObj);
                const bool bClassPinLinked = GetBoolFieldSafe(NodeObj, TEXT("class_pin_linked"), false);
                FinishNativeK2Node(Node);
                if (!bClassPinLinked)
                {
                    bool bClassDefaultApplied = false;
                    if (WidgetClass)
                    {
                        const bool bWidgetTypeApplied = SetPinDefaultObjectByName(Node, TEXT("WidgetType"), WidgetClass, Report);
                        const bool bClassPinApplied = SetPinDefaultObjectByName(Node, TEXT("Class"), WidgetClass, Report);
                        bClassDefaultApplied = bWidgetTypeApplied || bClassPinApplied;
                    }
                    if (!bClassDefaultApplied)
                    {
                        AppendLine(Report, TEXT("N2C_RUNTIME_GUARD|code=create_widget_class_default_invalid"));
                        AppendLine(Report, TEXT("ERROR: UE4.27 rejected the CreateWidget class literal before graph mutation."));
                        Node->DestroyNode();
                        return nullptr;
                    }
                    Node->ReconstructNode();
                }
                else if (UEdGraphPin* ResultPin = FindPinByLooseName(Node, TEXT("ReturnValue"), EGPD_Output))
                {
                    ResultPin->PinType.PinCategory = UEdGraphSchema_K2::PC_Object;
                    ResultPin->PinType.PinSubCategoryObject = WidgetClass;
                }
                return Node;
            }
            const bool bNeedsEnum = P1NativeClass == TEXT("K2Node_GetEnumeratorNameAsString") || P1NativeClass == TEXT("K2Node_ForEachElementInEnum") || P1NativeClass == TEXT("K2Node_CastByteToEnum") || P1NativeClass == TEXT("K2Node_EnumLiteral");
            UEnum* EnumContext = bNeedsEnum ? Cast<UEnum>(ResolveTypeObjectFromJson(NodeObj)) : nullptr;
            if (EnumContext) SetObjectPropertyByName(Node, TEXT("Enum"), EnumContext);
            FinishNativeK2Node(Node);
            if (P1NativeClass == TEXT("K2Node_EnumLiteral"))
            {
                ApplyEnumLiteralContext(Node, EnumContext);
                const FString SelectedValue = GetStringFieldSafe(NodeObj, TEXT("enum_value"), GetStringFieldSafe(NodeObj, TEXT("selected_value")));
                if (!SelectedValue.IsEmpty()) SetPinDefaultByName(Node, TEXT("Enum"), SelectedValue);
            }
            else if (P1NativeClass == TEXT("K2Node_GetEnumeratorNameAsString"))
            {
                // This node owns no Enum UPROPERTY. Its enum lives on the raw
                // Enumerator pin, so set it after its final UE4.27 allocation
                // and deliberately do not reconstruct the node again.
                ApplyGetEnumeratorNameContext(Node, EnumContext);
            }
            return Node;
        }
        if (Lower.Contains(TEXT("callmaterialparametercollection")) || Lower.Contains(TEXT("message")))
        {
            const FString NativeClass = Lower.Contains(TEXT("message")) ? TEXT("K2Node_Message") : TEXT("K2Node_CallMaterialParameterCollectionFunction");
            return CreateCallFunctionSubclassNode(Graph, Blueprint, NodeObj, NativeClass, TEXT(""), TEXT(""), PosX, PosY, Report);
        }
        AppendLine(Report, FString::Printf(TEXT("WARNING: unsupported node type '%s'. Node was not created."), *Type));
        return nullptr;
    }

    static bool ConnectEdges(UEdGraph* Graph, const TMap<FString, UEdGraphNode*>& NodesById, const TSharedPtr<FJsonObject>& ActionObj, FString& Report)
    {
        if (!Graph || !ActionObj.IsValid())
        {
            AppendLine(Report, TEXT("ERROR: edge connection requires a valid graph and action."));
            return false;
        }

        const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(Graph->GetSchema());
        if (!Schema)
        {
            AppendLine(Report, TEXT("ERROR: graph schema is not K2; runtime links cannot be connected."));
            return false;
        }

        struct FN2CLinkedDataTableExpectation
        {
            UK2Node_GetDataTableRow* Node = nullptr;
            UScriptStruct* RowStruct = nullptr;
            FString NodeId;
        };
        TArray<FN2CLinkedDataTableExpectation> LinkedDataTables;

        const TArray<TSharedPtr<FJsonValue>>* NodeValues = GetArrayFieldSafe(ActionObj, TEXT("nodes"));
        if (NodeValues)
        {
            for (const TSharedPtr<FJsonValue>& NodeValue : *NodeValues)
            {
                const TSharedPtr<FJsonObject> NodeObj = NodeValue.IsValid() ? NodeValue->AsObject() : nullptr;
                const FString Type = GetStringFieldSafe(NodeObj, TEXT("type"), GetStringFieldSafe(NodeObj, TEXT("class"))).ToLower();
                if (!Type.Contains(TEXT("getdatatablerow")) || !GetBoolFieldSafe(NodeObj, TEXT("data_table_pin_linked"), false))
                {
                    continue;
                }

                const FString NodeId = GetStringFieldSafe(NodeObj, TEXT("id"));
                UK2Node_GetDataTableRow* GetRow = Cast<UK2Node_GetDataTableRow>(NodesById.FindRef(NodeId));
                const FString RowStructPath = GetStringFieldSafe(NodeObj, TEXT("row_struct_path"), GetStringFieldSafe(NodeObj, TEXT("struct_path")));
                UScriptStruct* RowStruct = RowStructPath.IsEmpty() ? nullptr : FindObject<UScriptStruct>(ANY_PACKAGE, *RowStructPath);
                if (!RowStruct && !RowStructPath.IsEmpty()) RowStruct = LoadObject<UScriptStruct>(nullptr, *RowStructPath);
                if (!GetRow || !RowStruct || !GetRow->GetResultPin())
                {
                    AppendLine(Report, FString::Printf(
                        TEXT("ERROR: code=datatable_identity_missing; linked GetDataTableRow '%s' lost its node or row-struct identity before edge creation."),
                        *NodeId));
                    return false;
                }

                UEdGraphPin* ResultPin = GetRow->GetResultPin();
                ResultPin->PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
                ResultPin->PinType.PinSubCategoryObject = RowStruct;

                FN2CLinkedDataTableExpectation Expectation;
                Expectation.Node = GetRow;
                Expectation.RowStruct = RowStruct;
                Expectation.NodeId = NodeId;
                LinkedDataTables.Add(Expectation);
            }
        }

        bool bOk = true;
        auto ConnectArray = [&](const FString& FieldName)
        {
            const TArray<TSharedPtr<FJsonValue>>* Edges = GetArrayFieldSafe(ActionObj, FieldName);
            if (!Edges) return;

            for (const TSharedPtr<FJsonValue>& Value : *Edges)
            {
                const TSharedPtr<FJsonObject> EdgeObj = Value.IsValid() ? Value->AsObject() : nullptr;
                if (!EdgeObj.IsValid())
                {
                    AppendLine(Report, FString::Printf(TEXT("ERROR: invalid edge object in %s."), *FieldName));
                    bOk = false;
                    continue;
                }

                const FString FromId = GetStringFieldSafe(EdgeObj, TEXT("from_node_id"), GetStringFieldSafe(EdgeObj, TEXT("from")));
                const FString ToId = GetStringFieldSafe(EdgeObj, TEXT("to_node_id"), GetStringFieldSafe(EdgeObj, TEXT("to")));
                const FString FromPinName = GetStringFieldSafe(EdgeObj, TEXT("from_pin"), TEXT("Then"));
                const FString ToPinName = GetStringFieldSafe(EdgeObj, TEXT("to_pin"), TEXT("Execute"));

                UEdGraphNode* FromNode = NodesById.FindRef(FromId);
                UEdGraphNode* ToNode = NodesById.FindRef(ToId);
                if (!FromNode || !ToNode)
                {
                    AppendLine(Report, FString::Printf(
                        TEXT("N2C_RUNTIME_GUARD|code=edge_node_missing|from=%s|to=%s"),
                        *FromId, *ToId));
                    AppendLine(Report, FString::Printf(
                        TEXT("ERROR: runtime edge could not be created because node id was not found: %s -> %s."),
                        *FromId, *ToId));
                    bOk = false;
                    continue;
                }

                UEdGraphPin* FromPin = FindPinByLooseName(FromNode, FromPinName, EGPD_Output);
                UEdGraphPin* ToPin = FindPinByLooseName(ToNode, ToPinName, EGPD_Input);
                if (!FromPin || !ToPin)
                {
                    AppendLine(Report, FString::Printf(
                        TEXT("N2C_RUNTIME_GUARD|code=edge_pin_missing|from=%s.%s|to=%s.%s"),
                        *FromId, *FromPinName, *ToId, *ToPinName));
                    AppendLine(Report, FString::Printf(
                        TEXT("ERROR: runtime edge pin was not found: %s.%s -> %s.%s."),
                        *FromId, *FromPinName, *ToId, *ToPinName));
                    bOk = false;
                    continue;
                }

                UScriptStruct* ExpectedDataTableRowStruct = nullptr;
                if (UK2Node_GetDataTableRow* GetRow = Cast<UK2Node_GetDataTableRow>(ToNode))
                {
                    if (ToPin == GetRow->GetDataTablePin())
                    {
                        for (const FN2CLinkedDataTableExpectation& Expectation : LinkedDataTables)
                        {
                            if (Expectation.Node == GetRow)
                            {
                                ExpectedDataTableRowStruct = Expectation.RowStruct;
                                break;
                            }
                        }
                    }
                }

                UClass* PreservedConstructResultClass = nullptr;
                if (ToPin->PinName == TEXT("Class") || ToPin->PinName == TEXT("WidgetType"))
                {
                    if (UEdGraphPin* ResultPin = FindPinByLooseName(ToNode, TEXT("ReturnValue"), EGPD_Output))
                    {
                        PreservedConstructResultClass = Cast<UClass>(ResultPin->PinType.PinSubCategoryObject.Get());
                    }
                }

                const bool bAlreadyConnected = FromPin->LinkedTo.Contains(ToPin) && ToPin->LinkedTo.Contains(FromPin);
                if (!bAlreadyConnected && !Schema->TryCreateConnection(FromPin, ToPin))
                {
                    AppendLine(Report, FString::Printf(
                        TEXT("N2C_RUNTIME_GUARD|code=edge_connection_failed|from=%s.%s|to=%s.%s"),
                        *FromId, *FromPinName, *ToId, *ToPinName));
                    AppendLine(Report, FString::Printf(
                        TEXT("ERROR: UE4.27 rejected runtime edge: %s.%s -> %s.%s."),
                        *FromId, *FromPinName, *ToId, *ToPinName));
                    bOk = false;
                    continue;
                }

                AdoptLinkedPinTypeIfWildcard(FromPin, ToPin);
                AdoptLinkedPinTypeIfWildcard(ToPin, FromPin);
                if (ToPin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
                {
                    ToPin->DefaultValue.Empty();
                    ToPin->DefaultObject = nullptr;
                    ToPin->DefaultTextValue = FText::GetEmpty();
                }

                FromNode->PinConnectionListChanged(FromPin);
                ToNode->PinConnectionListChanged(ToPin);
                FromNode->NodeConnectionListChanged();
                ToNode->NodeConnectionListChanged();

                // A linked generic DataTable input cannot infer row type from the
                // DataTable pin. Preserve the explicit row_struct_path after every
                // UE4.27 connection callback; the typed result link becomes the
                // durable authority after save/reopen.
                if (ExpectedDataTableRowStruct)
                {
                    if (UK2Node_GetDataTableRow* GetRow = Cast<UK2Node_GetDataTableRow>(ToNode))
                    {
                        if (UEdGraphPin* ResultPin = GetRow->GetResultPin())
                        {
                            ResultPin->PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
                            ResultPin->PinType.PinSubCategoryObject = ExpectedDataTableRowStruct;
                        }
                    }
                }

                if (PreservedConstructResultClass)
                {
                    if (UEdGraphPin* ResultPin = FindPinByLooseName(ToNode, TEXT("ReturnValue"), EGPD_Output))
                    {
                        ResultPin->PinType.PinCategory = UEdGraphSchema_K2::PC_Object;
                        ResultPin->PinType.PinSubCategoryObject = PreservedConstructResultClass;
                    }
                }

                if (ToPin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec && ToPin->LinkedTo.Num() > 0)
                {
                    ToPin->DefaultValue.Empty();
                    ToPin->DefaultObject = nullptr;
                    ToPin->DefaultTextValue = FText::GetEmpty();
                }
                if (bAlreadyConnected)
                {
                    AppendLine(Report, FString::Printf(
                        TEXT("CHANGE: Graph edge reused: %s.%s -> %s.%s"),
                        *FromId, *FromPinName, *ToId, *ToPinName));
                }
                else
                {
                    AppendLine(Report, FString::Printf(
                        TEXT("CHANGE: Graph edge connected: %s.%s -> %s.%s"),
                        *FromId, *FromPinName, *ToId, *ToPinName));
                }
            }
        };

        ConnectArray(TEXT("exec_edges"));
        ConnectArray(TEXT("data_edges"));
        ConnectArray(TEXT("edges"));

        // CreateDelegate selection is only meaningful after OutputDelegate has
        // inherited a concrete signature from Add/Remove/Assign Delegate. Validate
        // this before compile so a missing connection cannot degrade into the opaque
        // UE4 errors "missing a function/event name" and "bad or unknown type".
        for (const TPair<FString, UEdGraphNode*>& Pair : NodesById)
        {
            UK2Node_CreateDelegate* CreateDelegate = Cast<UK2Node_CreateDelegate>(Pair.Value);
            if (!CreateDelegate)
            {
                continue;
            }

            UEdGraphPin* DelegateOut = CreateDelegate->GetDelegateOutPin();
            if (!DelegateOut || DelegateOut->LinkedTo.Num() == 0 || !CreateDelegate->GetDelegateSignature())
            {
                AppendLine(Report, FString::Printf(
                    TEXT("N2C_RUNTIME_GUARD|code=create_delegate_signature_missing|node=%s|function=%s"),
                    *Pair.Key,
                    *CreateDelegate->GetFunctionName().ToString()));
                AppendLine(Report, FString::Printf(
                    TEXT("ERROR: CreateDelegate '%s' must connect OutputDelegate to a typed delegate input before compile."),
                    *Pair.Key));
                bOk = false;
                continue;
            }

            CreateDelegate->HandleAnyChangeWithoutNotifying();
            if (CreateDelegate->GetFunctionName().IsNone())
            {
                AppendLine(Report, FString::Printf(
                    TEXT("N2C_RUNTIME_GUARD|code=create_delegate_function_selection_lost|node=%s"),
                    *Pair.Key));
                AppendLine(Report, FString::Printf(
                    TEXT("ERROR: CreateDelegate '%s' lost its selected function after delegate signature stabilization."),
                    *Pair.Key));
                bOk = false;
                continue;
            }

            AppendLine(Report, FString::Printf(
                TEXT("CHANGE: CreateDelegate signature stabilized: %s -> %s."),
                *Pair.Key,
                *CreateDelegate->GetFunctionName().ToString()));
        }

        for (const FN2CLinkedDataTableExpectation& Expectation : LinkedDataTables)
        {
            UEdGraphPin* ResultPin = Expectation.Node ? Expectation.Node->GetResultPin() : nullptr;
            if (!ResultPin || ResultPin->LinkedTo.Num() == 0)
            {
                AppendLine(Report, FString::Printf(
                    TEXT("N2C_RUNTIME_GUARD|code=datatable_linked_output_untyped|node=%s"),
                    *Expectation.NodeId));
                AppendLine(Report, FString::Printf(
                    TEXT("ERROR: linked GetDataTableRow '%s' requires ReturnValue/Out Row to connect to a typed '%s' struct pin so UE4.27 can persist the row type."),
                    *Expectation.NodeId,
                    Expectation.RowStruct ? *Expectation.RowStruct->GetPathName() : TEXT("<null>")));
                bOk = false;
                continue;
            }

            ResultPin->PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
            ResultPin->PinType.PinSubCategoryObject = Expectation.RowStruct;
            Expectation.Node->NotifyPinConnectionListChanged(ResultPin);
            ResultPin = Expectation.Node->GetResultPin();
            if (!ResultPin || ResultPin->PinType.PinCategory != UEdGraphSchema_K2::PC_Struct ||
                ResultPin->PinType.PinSubCategoryObject.Get() != Expectation.RowStruct)
            {
                AppendLine(Report, FString::Printf(
                    TEXT("N2C_RUNTIME_GUARD|code=datatable_row_type_not_persistent|node=%s"),
                    *Expectation.NodeId));
                AppendLine(Report, FString::Printf(
                    TEXT("ERROR: linked GetDataTableRow '%s' did not retain row struct '%s' after connection callbacks."),
                    *Expectation.NodeId,
                    *Expectation.RowStruct->GetPathName()));
                bOk = false;
            }
        }

        return bOk;
    }

    static void SeedExistingGraphPatchAliases(UEdGraph* Graph, TMap<FString, UEdGraphNode*>& NodesById)
    {
        if (!Graph)
        {
            return;
        }

        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (!Node)
            {
                continue;
            }

            if (UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
            {
                NodesById.Add(TEXT("Entry"), Entry);
                NodesById.Add(TEXT("GraphEntry"), Entry);
                NodesById.Add(TEXT("ConstructionScriptEntry"), Entry);
                NodesById.Add(TEXT("UserConstructionScript"), Entry);
                const FString EntryMember = Entry->FunctionReference.GetMemberName().ToString();
                if (!EntryMember.IsEmpty())
                {
                    NodesById.Add(EntryMember, Entry);
                }
            }
            else if (UK2Node_Event* Event = Cast<UK2Node_Event>(Node))
            {
                const FString EventMember = Event->EventReference.GetMemberName().ToString();
                if (!EventMember.IsEmpty())
                {
                    NodesById.Add(EventMember, Event);
                }
            }
            else if (UK2Node_CustomEvent* CustomEvent = Cast<UK2Node_CustomEvent>(Node))
            {
                const FString EventName = CustomEvent->CustomFunctionName.ToString();
                if (!EventName.IsEmpty())
                {
                    NodesById.Add(EventName, CustomEvent);
                }
            }
            if (UK2Node_Tunnel* Tunnel = Cast<UK2Node_Tunnel>(Node))
            {
                if (!Tunnel->IsA<UK2Node_Composite>() && Tunnel->bCanHaveOutputs && !Tunnel->bCanHaveInputs) { NodesById.Add(TEXT("Entry"), Tunnel); NodesById.Add(TEXT("TunnelEntry"), Tunnel); }
                if (!Tunnel->IsA<UK2Node_Composite>() && Tunnel->bCanHaveInputs && !Tunnel->bCanHaveOutputs) { NodesById.Add(TEXT("Exit"), Tunnel); NodesById.Add(TEXT("TunnelExit"), Tunnel); }
            }
        }
    }

    static UEdGraph* FindTargetGraphForPatch(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& ActionObj)
    {
        if (!Blueprint)
        {
            return nullptr;
        }

        const FString GraphName = GetStringFieldSafe(ActionObj, TEXT("graph_name"), GetStringFieldSafe(ActionObj, TEXT("target_graph"), TEXT("EventGraph")));
        const FString GraphNameLower = GraphName.ToLower();
        TArray<UEdGraph*> GraphsToScan;
        auto AddGraphUnique = [&GraphsToScan](UEdGraph* Graph)
        {
            if (Graph && !GraphsToScan.Contains(Graph))
            {
                GraphsToScan.Add(Graph);
            }
        };
        for (UEdGraph* Graph : Blueprint->UbergraphPages) { AddGraphUnique(Graph); }
        for (UEdGraph* Graph : Blueprint->FunctionGraphs) { AddGraphUnique(Graph); }
        for (UEdGraph* Graph : Blueprint->MacroGraphs) { AddGraphUnique(Graph); }
        for (UEdGraph* Graph : Blueprint->DelegateSignatureGraphs) { AddGraphUnique(Graph); }
        for (const FBPInterfaceDescription& InterfaceDesc : Blueprint->ImplementedInterfaces)
        {
            for (UEdGraph* Graph : InterfaceDesc.Graphs)
            {
                AddGraphUnique(Graph);
            }
        }
        TArray<UObject*> ChildObjects;
        GetObjectsWithOuter(Blueprint, ChildObjects, true);
        for (UObject* ChildObject : ChildObjects)
        {
            AddGraphUnique(Cast<UEdGraph>(ChildObject));
        }
        if (Blueprint->GetOutermost())
        {
            for (TObjectIterator<UEdGraph> It; It; ++It)
            {
                UEdGraph* Graph = *It;
                if (Graph && Graph->GetOutermost() == Blueprint->GetOutermost())
                {
                    AddGraphUnique(Graph);
                }
            }
        }

        for (UEdGraph* Graph : GraphsToScan)
        {
            if (!Graph)
            {
                continue;
            }
            const FString Candidate = Graph->GetName();
            const FString CandidateLower = Candidate.ToLower();
            if (Candidate == GraphName || CandidateLower == GraphNameLower ||
                (GraphNameLower.Contains(TEXT("construction")) && CandidateLower.Contains(TEXT("construction"))))
            {
                return Graph;
            }
        }
        return nullptr;
    }

    static bool ValidateSpecialGraphPatch(UBlueprint* Blueprint, UEdGraph* Graph, const FString& ActionType, FString& Report)
    {
        if (IsWidgetGraphPatchAction(ActionType))
        {
            if (!Cast<UWidgetBlueprint>(Blueprint) || !Cast<UEdGraphSchema_K2>(Graph ? Graph->GetSchema() : nullptr))
            {
                AppendLine(Report, TEXT("ERROR: patch_widget_graph requires a Widget Blueprint K2 graph."));
                return false;
            }
        }
        else if (IsAnimationGraphPatchAction(ActionType))
        {
            if (!Cast<UAnimBlueprint>(Blueprint) || !Graph || !Graph->GetSchema()->IsA(UAnimationGraphSchema::StaticClass()))
            {
                AppendLine(Report, TEXT("ERROR: patch_animation_graph requires an Animation Blueprint graph using UAnimationGraphSchema."));
                return false;
            }
        }
        return true;
    }

    static bool CreateOrReplaceCollapsedGraph(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& ActionObj, bool bDryRun, FString& Report)
    {
        UEdGraph* ParentGraph = FindTargetGraphForPatch(Blueprint, ActionObj);
        const FString CollapsedName = GetStringFieldSafe(ActionObj, TEXT("collapsed_graph_name"), GetStringFieldSafe(ActionObj, TEXT("name")));
        if (!ParentGraph || !Cast<UEdGraphSchema_K2>(ParentGraph->GetSchema()))
        {
            AppendLine(Report, TEXT("ERROR: collapsed graph requires an existing K2 parent graph."));
            return false;
        }

        UK2Node_Composite* Composite = nullptr;
        for (UEdGraphNode* ExistingNode : ParentGraph->Nodes)
        {
            UK2Node_Composite* Candidate = Cast<UK2Node_Composite>(ExistingNode);
            if (Candidate && Candidate->BoundGraph && Candidate->BoundGraph->GetName() == CollapsedName)
            {
                Composite = Candidate;
                break;
            }
        }

        if (bDryRun)
        {
            AppendLine(Report, FString::Printf(TEXT("DRY RUN %s: collapsed graph '%s' in '%s'."),
                Composite ? TEXT("CHANGE") : TEXT("NEW"), *CollapsedName, *ParentGraph->GetName()));
            return true;
        }

        if (!Composite)
        {
            FGraphNodeCreator<UK2Node_Composite> Creator(*ParentGraph);
            Composite = Creator.CreateNode();
            Composite->NodePosX = FCString::Atoi(*GetStringFieldSafe(ActionObj, TEXT("pos_x"), TEXT("300")));
            Composite->NodePosY = FCString::Atoi(*GetStringFieldSafe(ActionObj, TEXT("pos_y"), TEXT("0")));
            Composite->bCanRenameNode = true;
            Creator.Finalize();
            if (!Composite->BoundGraph)
            {
                AppendLine(Report, TEXT("ERROR: UE4.27 failed to create the composite bound graph."));
                Composite->DestroyNode();
                return false;
            }
            FBlueprintEditorUtils::RenameGraph(Composite->BoundGraph, CollapsedName);
        }

        UEdGraph* BoundGraph = Composite->BoundGraph;
        BoundGraph->Modify();
        if (GetStringFieldSafe(ActionObj, TEXT("type")) == TEXT("replace_collapsed_graph") || GetBoolFieldSafe(ActionObj, TEXT("replace_body"), true))
        {
            TArray<UEdGraphNode*> ExistingNodes = BoundGraph->Nodes;
            for (UEdGraphNode* ExistingNode : ExistingNodes)
            {
                if (ExistingNode && !ExistingNode->IsA<UK2Node_Tunnel>())
                {
                    FBlueprintEditorUtils::RemoveNode(Blueprint, ExistingNode, true);
                }
            }
        }

        TMap<FString, UEdGraphNode*> NodesById;
        if (UK2Node_Tunnel* Entry = Composite->GetEntryNode())
        {
            NodesById.Add(TEXT("Entry"), Entry);
            NodesById.Add(TEXT("CollapsedEntry"), Entry);
        }
        if (UK2Node_Tunnel* Exit = Composite->GetExitNode())
        {
            NodesById.Add(TEXT("Exit"), Exit);
            NodesById.Add(TEXT("CollapsedExit"), Exit);
        }

        const TArray<TSharedPtr<FJsonValue>>* Nodes = GetArrayFieldSafe(ActionObj, TEXT("nodes"));
        const int32 CreatedCount = Nodes ? Nodes->Num() - 2 : 0;
        if (!ApplyBoundaryGraphBody(Blueprint, BoundGraph, ActionObj, NodesById, Report)) return false;
        Composite->ReconstructNode();

        // Outer connections are a separate explicit phase after the BoundGraph and
        // tunnel pins exist. Existing aliases let a patch reconnect ordinary outer
        // nodes without recreating them.
        TMap<FString, UEdGraphNode*> OuterNodes;
        SeedExistingGraphPatchAliases(ParentGraph, OuterNodes);
        const FString CompositeId = GetStringFieldSafe(ActionObj, TEXT("composite_node_id"), TEXT("Composite"));
        OuterNodes.Add(CompositeId, Composite);
        TSharedPtr<FJsonObject> OuterEdges = MakeShared<FJsonObject>();
        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (ActionObj->TryGetArrayField(TEXT("outer_exec_edges"), Values) && Values) OuterEdges->SetArrayField(TEXT("exec_edges"), *Values);
        if (ActionObj->TryGetArrayField(TEXT("outer_data_edges"), Values) && Values) OuterEdges->SetArrayField(TEXT("data_edges"), *Values);
        if (ActionObj->TryGetArrayField(TEXT("outer_edges"), Values) && Values) OuterEdges->SetArrayField(TEXT("edges"), *Values);
        if (!ConnectEdges(ParentGraph, OuterNodes, OuterEdges, Report)) return false;
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
        AppendLine(Report, FString::Printf(TEXT("CHANGE: collapsed graph '%s' created/updated with %d inner node(s)."), *CollapsedName, CreatedCount));
        return true;
    }

    static bool ValidateSpawnActorTransformContract(const TSharedPtr<FJsonObject>& ActionObj, FString& Report)
    {
        if (!ActionObj.IsValid())
        {
            return true;
        }

        TSet<FString> SpawnNodeIds;
        const TArray<TSharedPtr<FJsonValue>>* Nodes = GetArrayFieldSafe(ActionObj, TEXT("nodes"));
        if (Nodes)
        {
            int32 Index = 0;
            for (const TSharedPtr<FJsonValue>& Value : *Nodes)
            {
                const TSharedPtr<FJsonObject> NodeObj = Value.IsValid() ? Value->AsObject() : nullptr;
                const FString Type = GetStringFieldSafe(NodeObj, TEXT("type"), GetStringFieldSafe(NodeObj, TEXT("class"))).ToLower();
                if (Type == TEXT("spawnactorfromclass") || Type == TEXT("spawn_actor_from_class") || Type.Contains(TEXT("spawnactorfromclass")))
                {
                    SpawnNodeIds.Add(GetStringFieldSafe(NodeObj, TEXT("id"), FString::Printf(TEXT("GraphPatchNode_%d"), Index)));
                }
                ++Index;
            }
        }

        if (SpawnNodeIds.Num() == 0)
        {
            return true;
        }

        TSet<FString> LinkedSpawnTransforms;
        auto ScanEdges = [&](const TCHAR* FieldName)
        {
            const TArray<TSharedPtr<FJsonValue>>* Edges = GetArrayFieldSafe(ActionObj, FieldName);
            if (!Edges)
            {
                return;
            }
            for (const TSharedPtr<FJsonValue>& Value : *Edges)
            {
                const TSharedPtr<FJsonObject> EdgeObj = Value.IsValid() ? Value->AsObject() : nullptr;
                if (!EdgeObj.IsValid())
                {
                    continue;
                }
                const FString ToId = GetStringFieldSafe(EdgeObj, TEXT("to_node_id"), GetStringFieldSafe(EdgeObj, TEXT("to")));
                const FString ToPin = GetStringFieldSafe(EdgeObj, TEXT("to_pin"));
                if (SpawnNodeIds.Contains(ToId) && NormalizeLoosePinName(ToPin) == NormalizeLoosePinName(TEXT("SpawnTransform")))
                {
                    LinkedSpawnTransforms.Add(ToId);
                }
            }
        };
        ScanEdges(TEXT("data_edges"));
        ScanEdges(TEXT("edges"));

        bool bOk = true;
        for (const FString& SpawnNodeId : SpawnNodeIds)
        {
            if (!LinkedSpawnTransforms.Contains(SpawnNodeId))
            {
                AppendLine(Report, FString::Printf(
                    TEXT("N2C_PREFLIGHT_GUARD|code=spawn_transform_link_missing|node=%s"),
                    *SpawnNodeId));
                AppendLine(Report, FString::Printf(
                    TEXT("ERROR: SpawnActorFromClass '%s' requires SpawnTransform to be connected to MakeTransform or another transform output. UE4.27 expands it to by-ref function parameters and rejects literal/unconnected defaults during compile."),
                    *SpawnNodeId));
                bOk = false;
            }
        }
        return bOk;
    }

    static FString BuildStableGraphPatchScope(UEdGraph* Graph, const TSharedPtr<FJsonObject>& ActionObj)
    {
        FString ExplicitScope = GetStringFieldSafe(ActionObj, TEXT("import_scope"),
            GetStringFieldSafe(ActionObj, TEXT("action_id"), GetStringFieldSafe(ActionObj, TEXT("import_id"))));
        if (!ExplicitScope.IsEmpty())
        {
            return ExplicitScope;
        }

        TArray<FString> NodeIds;
        if (const TArray<TSharedPtr<FJsonValue>>* Nodes = GetArrayFieldSafe(ActionObj, TEXT("nodes")))
        {
            int32 Index = 0;
            for (const TSharedPtr<FJsonValue>& Value : *Nodes)
            {
                const TSharedPtr<FJsonObject> NodeObj = Value.IsValid() ? Value->AsObject() : nullptr;
                NodeIds.Add(GetStringFieldSafe(NodeObj, TEXT("id"), FString::Printf(TEXT("GraphPatchNode_%d"), Index++)));
            }
        }
        NodeIds.Sort();
        return FString::Printf(TEXT("%s|%s|%s"),
            Graph ? *Graph->GetName() : TEXT("<null>"),
            *GetStringFieldSafe(ActionObj, TEXT("type")),
            *FString::Join(NodeIds, TEXT(",")));
    }

    static FGuid MakeStableGraphPatchNodeGuid(UEdGraph* Graph, const TSharedPtr<FJsonObject>& ActionObj, const FString& NodeId)
    {
        const FString Seed = FString::Printf(TEXT("N2C_GRAPH_PATCH_V1|%s|%s|%s"),
            Graph ? *Graph->GetPathName() : TEXT("<null>"),
            *BuildStableGraphPatchScope(Graph, ActionObj),
            *NodeId);
        const FString Digits = FMD5::HashAnsiString(*Seed);
        FGuid Guid;
        return FGuid::ParseExact(Digits, EGuidFormats::Digits, Guid) ? Guid : FGuid();
    }

    static UEdGraphNode* FindGraphNodeByStableGuid(UEdGraph* Graph, const FGuid& Guid)
    {
        if (!Graph || !Guid.IsValid())
        {
            return nullptr;
        }
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (Node && Node->NodeGuid == Guid)
            {
                return Node;
            }
        }
        return nullptr;
    }

    static bool PatchExistingGraph(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& ActionObj, bool bDryRun, FString& Report)
    {
        UEdGraph* Graph = FindTargetGraphForPatch(Blueprint, ActionObj);
        if (!Graph)
        {
            AppendLine(Report, TEXT("ERROR: target graph was not found for graph patch."));
            return false;
        }

        const FString ActionType = GetStringFieldSafe(ActionObj, TEXT("type"));
        if (!ValidateSpecialGraphPatch(Blueprint, Graph, ActionType, Report))
        {
            return false;
        }
        if (!ValidateSpawnActorTransformContract(ActionObj, Report))
        {
            return false;
        }

        if (bDryRun)
        {
            const TArray<TSharedPtr<FJsonValue>>* Nodes = GetArrayFieldSafe(ActionObj, TEXT("nodes"));
            AppendLine(Report, FString::Printf(TEXT("DRY RUN CHANGE: graph patch: %s, nodes=%d"), *Graph->GetName(), Nodes ? Nodes->Num() : 0));
            return true;
        }

        Graph->Modify();
        TMap<FString, UEdGraphNode*> NodesById;
        SeedExistingGraphPatchAliases(Graph, NodesById);
        for (UEdGraphNode* ExistingNode : Graph->Nodes)
        {
            if (Cast<UAnimGraphNode_Root>(ExistingNode))
            {
                NodesById.Add(TEXT("Root"), ExistingNode);
                NodesById.Add(TEXT("OutputPose"), ExistingNode);
            }
        }
        const TArray<TSharedPtr<FJsonValue>>* Nodes = GetArrayFieldSafe(ActionObj, TEXT("nodes"));
        int32 CreatedCount = 0;
        int32 SafeSkippedCount = 0;
        int32 ExpectedCount = Nodes ? Nodes->Num() : 0;
        if (Nodes)
        {
            int32 Index = 0;
            for (const TSharedPtr<FJsonValue>& Value : *Nodes)
            {
                TSharedPtr<FJsonObject> NodeObj = Value.IsValid() ? Value->AsObject() : nullptr;
                if (!NodeObj.IsValid())
                {
                    ++Index;
                    continue;
                }
                const FString NodeId = GetStringFieldSafe(NodeObj, TEXT("id"), FString::Printf(TEXT("GraphPatchNode_%d"), Index));
                FString Type = GetStringFieldSafe(NodeObj, TEXT("type"));
                if (Type.IsEmpty())
                {
                    Type = GetStringFieldSafe(NodeObj, TEXT("class"));
                }
                const FGuid StableNodeGuid = MakeStableGraphPatchNodeGuid(Graph, ActionObj, NodeId);
                UEdGraphNode* CreatedNode = FindGraphNodeByStableGuid(Graph, StableNodeGuid);
                const bool bReusedNode = CreatedNode != nullptr;
                const FString LowerType = Type.ToLower();
                if (!CreatedNode && LowerType.Contains(TEXT("tunnel")) && !LowerType.Contains(TEXT("composite")))
                {
                    CreatedNode = FindBoundaryTunnel(Graph, GetStringFieldSafe(NodeObj, TEXT("tunnel_role")));
                }
                if (CreatedNode && LowerType.Contains(TEXT("tunnel")) && !LowerType.Contains(TEXT("composite")))
                {
                    if (!SynchronizeBoundaryTunnel(Cast<UK2Node_Tunnel>(CreatedNode), NodeObj, Report)) return false;
                }
                else if (!CreatedNode)
                {
                    CreatedNode = CreatePatchNode(Graph, Blueprint, NodeObj, Index, Report);
                }
                if (CreatedNode)
                {
                    CreatedNode->Modify();
                    if (StableNodeGuid.IsValid())
                    {
                        CreatedNode->NodeGuid = StableNodeGuid;
                    }
                    if (!ApplyNodePinDefaults(CreatedNode, NodeObj, Report))
                    {
                        return false;
                    }
                    NodesById.Add(NodeId, CreatedNode);
                    ++CreatedCount;
                    if (bReusedNode)
                    {
                        AppendLine(Report, FString::Printf(TEXT("CHANGE: Graph patch reused node: %s.%s"), *Graph->GetName(), *NodeId));
                    }
                }
                else if (IsGuardedSafeSkippedNodeType(Type))
                {
                    ++SafeSkippedCount;
                }
                ++Index;
            }
        }

        if ((CreatedCount + SafeSkippedCount) != ExpectedCount)
        {
            AppendLine(Report, FString::Printf(
                TEXT("ERROR: graph patch '%s' created %d/%d requested node(s); edge creation was not attempted."),
                *Graph->GetName(),
                CreatedCount + SafeSkippedCount,
                ExpectedCount));
            return false;
        }
        if (!ConnectEdges(Graph, NodesById, ActionObj, Report)) return false;
        AppendLine(Report, FString::Printf(TEXT("CHANGE: Graph patch summary: %s created %d/%d node(s), safe-skipped %d guarded node(s)."), *Graph->GetName(), CreatedCount, ExpectedCount, SafeSkippedCount));
        Graph->NotifyGraphChanged();
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
        return true;
    }

    static void ApplyFunctionMetadata(UEdGraph* Graph, const TSharedPtr<FJsonObject>& ActionObj, FString& Report)
    {
        if (!Graph || !ActionObj.IsValid()) return;
        FKismetUserDeclaredFunctionMetadata* Metadata = FBlueprintEditorUtils::GetGraphFunctionMetaData(Graph);
        if (!Metadata) return;
        const FString Tooltip = GetStringFieldSafe(ActionObj, TEXT("tooltip"), GetStringFieldSafe(ActionObj, TEXT("description")));
        const FString Keywords = GetStringFieldSafe(ActionObj, TEXT("keywords"));
        if (!Tooltip.IsEmpty())
        {
            Metadata->ToolTip = FText::FromString(Tooltip);
            AppendLine(Report, FString::Printf(TEXT("CHANGE: function ToolTip updated: %s"), *Graph->GetName()));
        }
        if (!Keywords.IsEmpty()) Metadata->Keywords = FText::FromString(Keywords);
    }
    static bool CreateOrReplaceFunction(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& ActionObj, bool bDryRun, FString& Report)
    {
        const FString FunctionName = GetStringFieldSafe(ActionObj, TEXT("function_name"), GetStringFieldSafe(ActionObj, TEXT("name")));
        if (FunctionName.IsEmpty())
        {
            AppendLine(Report, TEXT("ERROR: action is missing function_name."));
            return false;
        }

        const FString ActionType = GetStringFieldSafe(ActionObj, TEXT("type"));
        const bool bReplace = ActionType == TEXT("replace_function_body") || GetBoolFieldSafe(ActionObj, TEXT("replace_body"), true);

        if (bDryRun)
        {
            UEdGraph* ExistingGraph = FindFunctionGraph(Blueprint, FunctionName);
            if (ExistingGraph)
            {
                AppendLine(Report, FString::Printf(TEXT("DRY RUN CHANGE: function: %s"), *FunctionName));
            }
            else
            {
                AppendLine(Report, FString::Printf(TEXT("DRY RUN NEW: function: %s"), *FunctionName));
            }

            const FString FunctionCategoryDryRun = GetStringFieldSafe(ActionObj, TEXT("category"), GetStringFieldSafe(ActionObj, TEXT("folder")));
            if (!FunctionCategoryDryRun.IsEmpty())
            {
                AppendLine(Report, FString::Printf(TEXT("DRY RUN CHANGE: function category: %s -> %s"), *FunctionName, *FunctionCategoryDryRun));
            }
            if (ActionHasFunctionOptions(ActionObj))
            {
                AppendLine(Report, FString::Printf(TEXT("DRY RUN CHANGE: function options: %s"), *FunctionName));
            }
            if (bReplace)
            {
                CountStaleSignaturePins(Blueprint, ActionObj, Report);
            }
            return true;
        }

        UEdGraph* Graph = FindFunctionGraph(Blueprint, FunctionName);
        if (!Graph)
        {
            Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, FName(*FunctionName), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());

            // UE4.27 exposes AddFunctionGraph as a templated helper. Passing nullptr directly
            // makes MSVC unable to infer SignatureType, so the template type is explicit.
            UFunction* SignatureFunction = nullptr;
            FBlueprintEditorUtils::AddFunctionGraph<UFunction>(Blueprint, Graph, true, SignatureFunction);

            AppendLine(Report, FString::Printf(TEXT("NEW: function: %s"), *FunctionName));
        }
        else
        {
            AppendLine(Report, FString::Printf(TEXT("Using existing function graph: %s"), *FunctionName));
            if (bReplace)
            {
                ClearFunctionBodyPreservingSignature(Blueprint, Graph, Report);
                CleanupFunctionSignaturePins(Graph, ActionObj, FunctionName, Report);
            }
        }

        AddSignaturePins(Graph, ActionObj, FunctionName, Report);
        if (bReplace && CountStaleSignaturePins(Blueprint, ActionObj, Report) > 0)
        {
            AppendLine(Report, FString::Printf(TEXT("ERROR: stale signature pins remain after replace-body synchronization for function '%s'. Apply aborted."), *FunctionName));
            return false;
        }
        AddLocalVariables(Blueprint, Graph, ActionObj, FunctionName, Report);

        const FString FunctionCategory = GetStringFieldSafe(ActionObj, TEXT("category"), GetStringFieldSafe(ActionObj, TEXT("folder")));
        if (!FunctionCategory.IsEmpty())
        {
            FBlueprintEditorUtils::SetBlueprintFunctionOrMacroCategory(Graph, FText::FromString(FunctionCategory), true);
            AppendLine(Report, FString::Printf(TEXT("CHANGE: function category: %s -> %s"), *FunctionName, *FunctionCategory));
        }

        ApplyFunctionOptions(Blueprint, Graph, ActionObj, FunctionName, bDryRun, Report);
        ApplyFunctionMetadata(Graph, ActionObj, Report);

        TMap<FString, UEdGraphNode*> NodesById;
        if (UK2Node_FunctionEntry* Entry = FindOrCreateEntryNode(Graph, FunctionName))
        {
            NodesById.Add(TEXT("Entry"), Entry);
            NodesById.Add(TEXT("N1"), Entry);
        }
        if (UK2Node_FunctionResult* Result = FindOrCreateResultNode(Graph, FunctionName))
        {
            NodesById.Add(TEXT("Return"), Result);
        }

        const TArray<TSharedPtr<FJsonValue>>* Nodes = GetArrayFieldSafe(ActionObj, TEXT("nodes"));
        bool bAllNodesCreated = true;
        if (Nodes)
        {
            int32 Index = 0;
            int32 ResultPatchCount = 0;
            TArray<UK2Node_FunctionResult*> ExistingResultNodes;
            for (UEdGraphNode* ExistingNode : Graph->Nodes)
            {
                if (UK2Node_FunctionResult* ExistingResult = Cast<UK2Node_FunctionResult>(ExistingNode))
                {
                    ExistingResultNodes.Add(ExistingResult);
                }
            }
            for (const TSharedPtr<FJsonValue>& Value : *Nodes)
            {
                TSharedPtr<FJsonObject> NodeObj = Value.IsValid() ? Value->AsObject() : nullptr;
                if (!NodeObj.IsValid())
                {
                    continue;
                }
                const FString NodeId = GetStringFieldSafe(NodeObj, TEXT("id"), FString::Printf(TEXT("PatchNode_%d"), Index));
                FString PatchNodeType = GetStringFieldSafe(NodeObj, TEXT("type"), GetStringFieldSafe(NodeObj, TEXT("class"))).ToLower();
                const bool bResultNode = PatchNodeType == TEXT("return") || PatchNodeType.Contains(TEXT("functionresult"));
                UEdGraphNode* CreatedNode = nullptr;
                if (bResultNode)
                {
                    const int32 ResultIndex = ResultPatchCount++;
                    CreatedNode = ExistingResultNodes.IsValidIndex(ResultIndex)
                        ? ExistingResultNodes[ResultIndex]
                        : CreateAdditionalResultNode(Graph, FunctionName, Index * 240, Index * 80);
                }
                else
                {
                    CreatedNode = CreatePatchNode(Graph, Blueprint, NodeObj, Index, Report);
                }
                if (CreatedNode)
                {
                    UE_LOG(LogNodeToCode, Display, TEXT("N2C patch apply trace: applying defaults node_id='%s' graph='%s'"), *NodeId, *FunctionName);
                    if (!ApplyNodePinDefaults(CreatedNode, NodeObj, Report))
                    {
                        bAllNodesCreated = false;
                        AppendLine(Report, FString::Printf(TEXT("ERROR: function '%s' node '%s' has an invalid pin default."), *FunctionName, *NodeId));
                        ++Index;
                        continue;
                    }
                    UE_LOG(LogNodeToCode, Display, TEXT("N2C patch apply trace: defaults done node_id='%s' graph='%s'"), *NodeId, *FunctionName);
                    NodesById.Add(NodeId, CreatedNode);
                }
                else
                {
                    bAllNodesCreated = false;
                    AppendLine(Report, FString::Printf(
                        TEXT("ERROR: function '%s' node '%s' could not be created; function patch stopped."),
                        *FunctionName,
                        *NodeId));
                }
                ++Index;
            }
        }

        const int32 ExpectedNodeCount = Nodes ? Nodes->Num() : 0;
        AppendLine(Report, FString::Printf(TEXT("Function patch summary: %s created/mapped %d/%d node id(s)."), *FunctionName, NodesById.Num(), ExpectedNodeCount));
        if (!bAllNodesCreated)
        {
            return false;
        }
        UE_LOG(LogNodeToCode, Display, TEXT("N2C patch apply trace: connect edges begin function='%s'"), *FunctionName);
        if (!ConnectEdges(Graph, NodesById, ActionObj, Report)) return false;
        UE_LOG(LogNodeToCode, Display, TEXT("N2C patch apply trace: connect edges done function='%s'"), *FunctionName);

        UE_LOG(LogNodeToCode, Display, TEXT("N2C patch apply trace: notify graph changed function='%s'"), *FunctionName);
        Graph->NotifyGraphChanged();
        UE_LOG(LogNodeToCode, Display, TEXT("N2C patch apply trace: mark blueprint modified function='%s'"), *FunctionName);
        // AddFunctionGraph/AddLocalVariable/SetFunction... calls above already mark the Blueprint
        // structurally when needed. Calling MarkBlueprintAsStructurallyModified repeatedly while
        // freshly created FunctionEntry/FunctionResult pins still have transient signature metadata
        // can crash UE4.27 BlueprintGraph. Mark dirty here and let the final compile rebuild structure.
        FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
        return true;
    }

    static void PostImportGraphRepair(UBlueprint* Blueprint, FString& Report)
    {
        if (!Blueprint)
        {
            return;
        }

        TArray<UEdGraph*> Graphs;
        auto AddGraphUnique = [&Graphs](UEdGraph* Graph)
        {
            if (Graph && !Graphs.Contains(Graph))
            {
                Graphs.Add(Graph);
            }
        };
        for (UEdGraph* Graph : Blueprint->FunctionGraphs) { AddGraphUnique(Graph); }
        for (UEdGraph* Graph : Blueprint->UbergraphPages) { AddGraphUnique(Graph); }
        for (UEdGraph* Graph : Blueprint->MacroGraphs) { AddGraphUnique(Graph); }
        for (UEdGraph* Graph : Blueprint->DelegateSignatureGraphs) { AddGraphUnique(Graph); }

        int32 FixedReciprocalLinks = 0;
        int32 RemovedBrokenLinks = 0;
        int32 WildcardPropagated = 0;
        bool bChangedAnyGraph = false;
        for (UEdGraph* Graph : Graphs)
        {
            if (!Graph)
            {
                continue;
            }

            bool bGraphChanged = false;
            UE_LOG(LogNodeToCode, Display, TEXT("N2C patch apply trace: post-repair scan graph='%s' nodes=%d"), *Graph->GetName(), Graph->Nodes.Num());
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (!Node)
                {
                    continue;
                }

                // UE4.27 can crash inside BlueprintGraph when freshly-created
                // FunctionEntry/FunctionResult nodes are explicitly notified before the
                // Blueprint compiler has rebuilt their signature metadata. We still scan
                // their links below, but we do not call NodeConnectionListChanged(),
                // PinConnectionListChanged(nullptr), ReconstructNode() or per-graph
                // NotifyGraphChanged() from this repair pass. The final CompileBlueprint
                // call owns that rebuild.
                const bool bIsFreshFunctionSignatureNode =
                    Node->IsA<UK2Node_FunctionEntry>() || Node->IsA<UK2Node_FunctionResult>();

                for (UEdGraphPin* Pin : Node->Pins)
                {
                    if (!Pin)
                    {
                        continue;
                    }

                    for (int32 LinkIndex = Pin->LinkedTo.Num() - 1; LinkIndex >= 0; --LinkIndex)
                    {
                        UEdGraphPin* LinkedPin = Pin->LinkedTo[LinkIndex];
                        if (!LinkedPin || !LinkedPin->GetOwningNode())
                        {
                            Pin->LinkedTo.RemoveAt(LinkIndex);
                            ++RemovedBrokenLinks;
                            bGraphChanged = true;
                            continue;
                        }
                        if (!LinkedPin->LinkedTo.Contains(Pin))
                        {
                            LinkedPin->LinkedTo.Add(Pin);
                            ++FixedReciprocalLinks;
                            bGraphChanged = true;
                        }

                        // Lightweight wildcard propagation: when one side is still wildcard after
                        // import and the other side is typed, copy the typed side. Do not touch
                        // FunctionEntry/FunctionResult signature pins here; their pin types are
                        // rebuilt by BlueprintGraph from function metadata during compile and
                        // touching them in this phase caused the 0x108 BlueprintGraph crash.
                        const bool bLinkedIsFreshFunctionSignatureNode =
                            LinkedPin->GetOwningNode()->IsA<UK2Node_FunctionEntry>() || LinkedPin->GetOwningNode()->IsA<UK2Node_FunctionResult>();
                        if (!bIsFreshFunctionSignatureNode && !bLinkedIsFreshFunctionSignatureNode)
                        {
                            if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard && LinkedPin->PinType.PinCategory != UEdGraphSchema_K2::PC_Wildcard)
                            {
                                Pin->PinType = LinkedPin->PinType;
                                ++WildcardPropagated;
                                bGraphChanged = true;
                            }
                            else if (LinkedPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Wildcard)
                            {
                                LinkedPin->PinType = Pin->PinType;
                                ++WildcardPropagated;
                                bGraphChanged = true;
                            }
                        }
                    }
                }
            }

            bChangedAnyGraph |= bGraphChanged;
        }

        if (FixedReciprocalLinks > 0 || RemovedBrokenLinks > 0 || WildcardPropagated > 0)
        {
            AppendLine(Report, FString::Printf(TEXT("Post-import graph repair: reciprocal_links_fixed=%d broken_links_removed=%d wildcard_types_propagated=%d. Deferred graph notifications to final compile."), FixedReciprocalLinks, RemovedBrokenLinks, WildcardPropagated));
        }
        else
        {
            AppendLine(Report, TEXT("Post-import graph repair: no broken reciprocal links detected. Deferred graph notifications to final compile."));
        }
        UE_LOG(LogNodeToCode, Display, TEXT("N2C patch apply trace: post-import graph repair done changed=%d"), bChangedAnyGraph ? 1 : 0);
    }

    static void EnsureUniqueCustomEventNames(UBlueprint* Blueprint, FString& Report)
    {
        if (!Blueprint)
        {
            return;
        }

        TArray<UEdGraph*> GraphsToScan;
        GraphsToScan.Append(Blueprint->UbergraphPages);
        GraphsToScan.Append(Blueprint->FunctionGraphs);
        GraphsToScan.Append(Blueprint->MacroGraphs);

        TSet<FName> UsedNames;
        int32 RenameCount = 0;
        for (UEdGraph* Graph : GraphsToScan)
        {
            if (!Graph)
            {
                continue;
            }

            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (!Node || !Node->GetClass() || !Node->GetClass()->GetName().Contains(TEXT("K2Node_CustomEvent")))
                {
                    continue;
                }

                FName EventName;
                if (!GetNamePropertyByName(Node, TEXT("CustomFunctionName"), EventName) || EventName.IsNone())
                {
                    continue;
                }

                if (!UsedNames.Contains(EventName))
                {
                    UsedNames.Add(EventName);
                    continue;
                }

                FString BaseName = EventName.ToString();
                FString NewName;
                int32 Suffix = 1;
                do
                {
                    NewName = FString::Printf(TEXT("%s_N2CDuplicate_%d"), *BaseName, Suffix++);
                }
                while (UsedNames.Contains(FName(*NewName)));

                Node->Modify();
                SetNamePropertyByName(Node, TEXT("CustomFunctionName"), NewName);
                Node->ReconstructNode();
                UsedNames.Add(FName(*NewName));
                ++RenameCount;
                AppendLine(Report, FString::Printf(TEXT("WARNING: duplicate CustomEvent name '%s' was renamed to '%s'."), *BaseName, *NewName));
            }
        }

        if (RenameCount > 0)
        {
            FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
            AppendLine(Report, FString::Printf(TEXT("CustomEvent duplicate-name cleanup: renamed %d duplicate node(s)."), RenameCount));
        }
    }

    static bool ApplyPatchInternal(UBlueprint* Blueprint, const FString& PatchJson, bool bDryRun, FString& OutReport, bool bDeveloperOverride, bool bCompileAfterApply = true, bool bSandboxValidation = false, const FString& LogicalBlueprintPath = FString())
    {
        OutReport.Empty();
        if (!Blueprint)
        {
            AppendLine(OutReport, TEXT("ERROR: invalid Blueprint."));
            return false;
        }

        TSharedPtr<FJsonObject> Root;
        if (!ParseRoot(PatchJson, Root, OutReport))
        {
            return false;
        }

        FN2CPreflightResult CoverageResult;
        if (!FN2CCoverageClassifier::PreflightPatch(Root, bDeveloperOverride, CoverageResult))
        {
            AppendLine(OutReport, FString::Printf(TEXT("N2C_PREFLIGHT_RESULT|asset=%s|source_schema=N2C_PATCH_V1|policy=%s|runtime_blockers=%d|verification_gaps=%d|cosmetic_warnings=%d|allowed=0"), *Blueprint->GetPathName(), bDeveloperOverride ? TEXT("developer_override") : TEXT("strict"), CoverageResult.RuntimeBlockerCount, CoverageResult.VerificationGapCount, CoverageResult.CosmeticWarningCount));
            for (const FN2CCoverageIssue& Issue : CoverageResult.Issues)
            {
                if (!FN2CCoverageClassifier::AllowsApply(Issue, bDeveloperOverride))
                {
                    AppendLine(OutReport, FString::Printf(TEXT("N2C_COVERAGE_BLOCKER|asset=%s|node_class=%s|status=%s|reason=%s"), *Blueprint->GetPathName(), *Issue.NodeClass, *Issue.Status, *Issue.Reason));
                }
            }
            AppendLine(OutReport, TEXT("N2C_APPLY_BLOCKED_BEFORE_MUTATION|coverage preflight rejected the patch."));
            return false;
        }
        AppendLine(OutReport, FString::Printf(TEXT("N2C_PREFLIGHT_RESULT|asset=%s|source_schema=N2C_PATCH_V1|policy=%s|runtime_blockers=%d|verification_gaps=%d|cosmetic_warnings=%d|allowed=1"), *Blueprint->GetPathName(), bDeveloperOverride ? TEXT("developer_override") : TEXT("strict"), CoverageResult.RuntimeBlockerCount, CoverageResult.VerificationGapCount, CoverageResult.CosmeticWarningCount));
        if (CoverageResult.CosmeticWarningCount > 0)
        {
            AppendLine(OutReport, FString::Printf(TEXT("N2C_COVERAGE_WARNING|asset=%s|cosmetic_warnings=%d"), *Blueprint->GetPathName(), CoverageResult.CosmeticWarningCount));
        }

        const TArray<TSharedPtr<FJsonValue>>* Actions = GetArrayFieldSafe(Root, TEXT("actions"));
        const TArray<TSharedPtr<FJsonValue>>* RootVariables = GetMemberVariablesArray(Root);
        const int32 ActionCount = Actions ? Actions->Num() : 0;
        const int32 RootVariableCount = RootVariables ? RootVariables->Num() : 0;
        if (ActionCount == 0 && RootVariableCount == 0)
        {
            AppendLine(OutReport, TEXT("ERROR: actions[] is empty and no root variables were provided."));
            return false;
        }

        AppendLine(OutReport, FString::Printf(TEXT("Patch target Blueprint: %s"), *Blueprint->GetName()));
        AppendLine(OutReport, FString::Printf(TEXT("Actions: %d; Root variables: %d"), ActionCount, RootVariableCount));
        AppendLine(OutReport, TEXT("Importer compatibility: FunctionEntry/FunctionResult, Branch, Sequence, Select, MakeArray, BinaryOperator, CallArrayFunction, expanded MacroInstance lookup, SwitchEnum/EnumEquality/EnumInequality, typed GetDataTableRow, BreakStruct/MakeStruct, SetFieldsInStruct, SpawnActor, Cast, simple CallFunction, CustomEvent, existing-dispatcher delegate nodes, ComponentBoundEvent, variable get/set (including serialized Set/Map and resolved object/class/soft-reference defaults), AddComponent K2 node plus SCS hierarchy, EventGraph latent calls, Timeline template/track reconstruction, collapsed/interface/Construction Script graphs, Widget Blueprint K2 graphs, and Animation Blueprint AnimGraphs (Local Space Ref Pose) use dedicated UE4.27 importer paths. Brand-new Event Dispatcher signature creation and P5 Niagara production round-trip remain incomplete."));

        const FString DeclaredTarget = GetStringFieldSafe(Root, TEXT("target_blueprint"));
        if (!bSandboxValidation && !DeclaredTarget.IsEmpty() && DeclaredTarget != Blueprint->GetName() && DeclaredTarget != Blueprint->GetPathName())
        {
            AppendLine(OutReport, FString::Printf(TEXT("ERROR: target_blueprint mismatch. Patch='%s', current='%s' (%s)."), *DeclaredTarget, *Blueprint->GetName(), *Blueprint->GetPathName()));
            return false;
        }

        bool bValidationOk = true;
        if (RootVariableCount > 0)
        {
            bValidationOk &= ValidateMemberVariablesShape(Root, TEXT("root variables"), OutReport);
        }
        if (Actions)
        for (const TSharedPtr<FJsonValue>& Value : *Actions)
        {
            TSharedPtr<FJsonObject> ActionObj = Value.IsValid() ? Value->AsObject() : nullptr;
            bValidationOk &= ValidateActionShape(Blueprint, ActionObj, OutReport, LogicalBlueprintPath);
        }

        if (!bValidationOk)
        {
            AppendLine(OutReport, TEXT("Patch aborted: validation failed before any Blueprint mutation."));
            return false;
        }

        if (bDryRun)
        {
            AppendLine(OutReport, TEXT("Mode: dry run. Blueprint will not be modified."));
        }
        else if (!bSandboxValidation)
        {
            FString BackupPath;
            if (!BackupBlueprintAsset(Blueprint, BackupPath, OutReport))
            {
                AppendLine(OutReport, TEXT("Patch aborted before mutation."));
                return false;
            }
        }
        else
        {
            AppendLine(OutReport, TEXT("Mode: transient sandbox apply. Target Blueprint will not be modified."));
        }

        bool bAllOk = true;
        if (!bDryRun)
        {
            Blueprint->Modify();
        }

        // Optional root-level member variables support:
        // { "schema":"N2C_PATCH_V1", "variables":[...], "actions":[...] }
        if (RootVariableCount > 0)
        {
            if (!bDryRun)
            {
                UE_LOG(LogNodeToCode, Display, TEXT("N2C patch apply trace: root member variables begin"));
                AppendLine(OutReport, TEXT("APPLY TRACE: root member variables begin"));
            }
            bAllOk &= AddMemberVariables(Blueprint, Root, bDryRun, OutReport);
        }

        auto ApplyAction = [&](const TSharedPtr<FJsonObject>& ActionObj) -> bool
        {
            const FString ActionType = GetStringFieldSafe(ActionObj, TEXT("type"));
            if (!bDryRun)
            {
                UE_LOG(LogNodeToCode, Display, TEXT("N2C patch apply trace: action begin '%s'"), *ActionType);
                AppendLine(OutReport, FString::Printf(TEXT("APPLY TRACE: action begin: %s"), *ActionType));
            }
            if (IsMemberVariablesAction(ActionType))
            {
                return AddMemberVariables(Blueprint, ActionObj, bDryRun, OutReport);
            }
            if (ActionType == TEXT("rename_function"))
            {
                return RenameFunctionGraph(Blueprint, ActionObj, bDryRun, OutReport);
            }
            if (ActionType == TEXT("rename_variable"))
            {
                return RenameMemberVariable(Blueprint, ActionObj, bDryRun, OutReport);
            }
            if (ActionType == TEXT("delete_function") || ActionType == TEXT("delete_functions"))
            {
                return DeleteFunctionGraphsAction(Blueprint, ActionObj, bDryRun, OutReport);
            }
            if (ActionType == TEXT("delete_variable") || ActionType == TEXT("delete_variables") ||
                ActionType == TEXT("delete_member_variable") || ActionType == TEXT("delete_member_variables"))
            {
                return DeleteMemberVariablesAction(Blueprint, ActionObj, bDryRun, OutReport);
            }
            if (ActionType == TEXT("delete_event_dispatcher") || ActionType == TEXT("delete_event_dispatchers") ||
                ActionType == TEXT("delete_dispatcher") || ActionType == TEXT("delete_dispatchers"))
            {
                return DeleteEventDispatchersAction(Blueprint, ActionObj, bDryRun, OutReport);
            }
            if (ActionType == TEXT("delete_macro") || ActionType == TEXT("delete_macros"))
            {
                return DeleteMacroGraphsAction(Blueprint, ActionObj, bDryRun, OutReport);
            }
            if (ActionType == TEXT("delete_event_graph_nodes") || ActionType == TEXT("delete_graph_nodes"))
            {
                return DeleteGraphNodesByName(Blueprint, ActionObj, bDryRun, OutReport);
            }
            if (ActionType == TEXT("rename_custom_event"))
            {
                return RenameCustomEventNode(Blueprint, ActionObj, bDryRun, OutReport);
            }
            if (ActionType == TEXT("add_event_dispatcher") || ActionType == TEXT("add_event_dispatchers"))
            {
                return AddEventDispatchers(Blueprint, ActionObj, bDryRun, OutReport);
            }
            if (ActionType == TEXT("implement_interface"))
            {
                return ImplementInterface(Blueprint, ActionObj, bDryRun, OutReport);
            }
            if (ActionType == TEXT("import_scs_hierarchy") || ActionType == TEXT("add_scs_components") || ActionType == TEXT("update_scs_hierarchy"))
            {
                return ImportSCSHierarchy(Blueprint, ActionObj, bDryRun, OutReport);
            }
            if (ActionType == TEXT("create_collapsed_graph") || ActionType == TEXT("replace_collapsed_graph"))
            {
                return CreateOrReplaceCollapsedGraph(Blueprint, ActionObj, bDryRun, OutReport);
            }
            if (ActionType == TEXT("add_macro"))
            {
                return AddMacroGraphAction(Blueprint, ActionObj, bDryRun, OutReport);
            }
            if (ActionType == TEXT("set_function_category") || ActionType == TEXT("move_function_to_category"))
            {
                return SetFunctionCategory(Blueprint, ActionObj, bDryRun, OutReport);
            }
            if (IsEventGraphPatchAction(ActionType) || IsWidgetGraphPatchAction(ActionType) || IsAnimationGraphPatchAction(ActionType))
            {
                return PatchExistingGraph(Blueprint, ActionObj, bDryRun, OutReport);
            }
            return CreateOrReplaceFunction(Blueprint, ActionObj, bDryRun, OutReport);
        };

        if (Actions)
        for (const TSharedPtr<FJsonValue>& Value : *Actions)
        {
            TSharedPtr<FJsonObject> ActionObj = Value.IsValid() ? Value->AsObject() : nullptr;
            const bool bActionOk = ApplyAction(ActionObj);
            bAllOk &= bActionOk;
            if (!bActionOk && !bDryRun)
            {
                AppendLine(OutReport, TEXT("ERROR: patch apply stopped at the first failed action; later actions were not executed."));
                break;
            }
        }

        if (!bDryRun && bAllOk)
        {
            EnsureUniqueCustomEventNames(Blueprint, OutReport);
            UE_LOG(LogNodeToCode, Display, TEXT("N2C patch apply trace: post-import graph repair begin"));
            PostImportGraphRepair(Blueprint, OutReport);
            if (bCompileAfterApply)
            {
                UE_LOG(LogNodeToCode, Display, TEXT("N2C patch apply trace: compile begin"));
                FKismetEditorUtilities::CompileBlueprint(Blueprint);
                UE_LOG(LogNodeToCode, Display, TEXT("N2C patch apply trace: compile done status=%d"), static_cast<int32>(Blueprint->Status));
                if (Blueprint->Status == BS_Error)
                {
                    bAllOk = false;
                    AppendLine(OutReport, TEXT("ERROR: Blueprint compile finished with BS_Error. Transaction rollback will be requested by ApplyPatchToBlueprint."));
                }
                else
                {
                    AppendLine(OutReport, TEXT("Compile requested after patch apply. Check Unreal compiler output for any non-fatal warnings."));
                }
            }
            else
            {
                AppendLine(OutReport, TEXT("Apply completed; compile and save were deliberately deferred to the P0.3 verifier."));
            }
        }

        // A textual dry run cannot prove UE4.27 constructor pins or schema connection
        // compatibility. Apply the same patch to a transient duplicate Blueprint and
        // run real node allocation + edge connection there. This catches missing pin
        // names, rejected connections and constructor context failures before the real
        // FScopedTransaction touches the user's asset.
        if (bDryRun && bAllOk && !bSandboxValidation)
        {
            const FName SandboxName = MakeUniqueObjectName(
                GetTransientPackage(),
                Blueprint->GetClass(),
                FName(*(Blueprint->GetName() + TEXT("_N2CPreflightSandbox"))));
            UBlueprint* SandboxBlueprint = DuplicateObject<UBlueprint>(
                Blueprint,
                GetTransientPackage(),
                SandboxName);

            if (!SandboxBlueprint)
            {
                AppendLine(OutReport, TEXT("N2C_PREFLIGHT_GUARD|code=sandbox_duplicate_failed"));
                AppendLine(OutReport, TEXT("ERROR: strict dry run could not create a transient Blueprint sandbox. Patch rejected before mutation."));
                bAllOk = false;
            }
            else
            {
                SandboxBlueprint->SetFlags(RF_Transient);
                SandboxBlueprint->ClearFlags(RF_Public | RF_Standalone);
                if (SandboxBlueprint->GetOutermost())
                {
                    SandboxBlueprint->GetOutermost()->SetDirtyFlag(false);
                }

                FString SandboxReport;
                const bool bSandboxOk = ApplyPatchInternal(
                    SandboxBlueprint,
                    PatchJson,
                    false,
                    SandboxReport,
                    bDeveloperOverride,
                    false,
                    true,
                    Blueprint->GetPathName());
                if (!bSandboxOk)
                {
                    AppendLine(OutReport, TEXT("N2C_PREFLIGHT_GUARD|code=sandbox_apply_failed"));
                    AppendLine(OutReport, TEXT("ERROR: transient sandbox apply rejected node creation, pin resolution or edge compatibility before target mutation."));
                    AppendLine(OutReport, SandboxReport);
                    bAllOk = false;
                }
                else
                {
                    AppendLine(OutReport, TEXT("N2C_PREFLIGHT_SANDBOX_RESULT|result=PASS|mutation=transient_only|compile=deferred"));
                }
            }
        }

        AppendLine(OutReport, bAllOk ? TEXT("Patch processing finished.") : TEXT("Patch processing finished with errors/warnings."));
        return bAllOk;
    }
}

bool FN2CPatchImporter::PreflightPatch(UBlueprint* Blueprint, const FString& PatchJson, bool bDeveloperOverride, FN2CPreflightResult& OutResult, FString& OutReport)
{
    using namespace N2CPatchImporter_Private;
    OutReport.Empty();
    TSharedPtr<FJsonObject> Root;
    if (!ParseRoot(PatchJson, Root, OutReport))
    {
        OutResult = FN2CPreflightResult();
        return false;
    }
    bool bAllowed = FN2CCoverageClassifier::PreflightPatch(Root, bDeveloperOverride, OutResult);
    bool bNeedsLiveSemanticResolution = false;
    for (const FN2CCoverageIssue& Issue : OutResult.Issues)
    {
        if (!FN2CCoverageClassifier::AllowsApply(Issue, bDeveloperOverride))
        {
            FString BeforeCode, AfterCode;
            if (Issue.Reason.Split(TEXT("code="), &BeforeCode, &AfterCode))
            {
                FString GuardCode, AfterSeparator;
                if (!AfterCode.Split(TEXT(";"), &GuardCode, &AfterSeparator)) GuardCode = AfterCode;
                GuardCode.TrimStartAndEndInline();
                if (!GuardCode.IsEmpty()) AppendLine(OutReport, FString::Printf(TEXT("N2C_PREFLIGHT_GUARD|code=%s"), *GuardCode));
            }
            AppendLine(OutReport, FString::Printf(TEXT("N2C_COVERAGE_BLOCKER|node_class=%s|status=%s|reason=%s"), *Issue.NodeClass, *Issue.Status, *Issue.Reason));
        }
        const FString Lower = Issue.NodeClass.ToLower();
        bNeedsLiveSemanticResolution |= Lower.Contains(TEXT("macro")) || Lower.Contains(TEXT("struct")) ||
            Lower.Contains(TEXT("enum")) || Lower.Contains(TEXT("datatable")) || Lower.Contains(TEXT("delegate")) ||
            Lower.Contains(TEXT("componentboundevent")) || Lower.Contains(TEXT("createwidget")) ||
            Lower.Contains(TEXT("input")) || Lower.Contains(TEXT("message"));
    }
    const TArray<TSharedPtr<FJsonValue>>* PreflightActions = GetArrayFieldSafe(Root, TEXT("actions"));
    if (PreflightActions)
    {
        for (const TSharedPtr<FJsonValue>& ActionValue : *PreflightActions)
        {
            const TSharedPtr<FJsonObject> Action = ActionValue.IsValid() ? ActionValue->AsObject() : nullptr;
            const FString Type = GetStringFieldSafe(Action, TEXT("type"));
            bNeedsLiveSemanticResolution |= IsMemberVariablesAction(Type) ||
                Type == TEXT("add_or_replace_function") || Type == TEXT("replace_function_body");
        }
    }
    bNeedsLiveSemanticResolution |= GetMemberVariablesArray(Root) != nullptr;
    if (bAllowed && bNeedsLiveSemanticResolution)
    {
        FString SemanticReport;
        if (!ApplyPatchInternal(Blueprint, PatchJson, true, SemanticReport, bDeveloperOverride))
        {
            bAllowed = false;
            AppendLine(OutReport, SemanticReport);
        }
    }
    AppendLine(OutReport, FString::Printf(TEXT("N2C_PREFLIGHT_RESULT|asset=%s|source_schema=N2C_PATCH_V1|policy=%s|runtime_blockers=%d|verification_gaps=%d|cosmetic_warnings=%d|allowed=%d"), Blueprint ? *Blueprint->GetPathName() : TEXT("<null>"), bDeveloperOverride ? TEXT("developer_override") : TEXT("strict"), OutResult.RuntimeBlockerCount, OutResult.VerificationGapCount, OutResult.CosmeticWarningCount, bAllowed ? 1 : 0));
    return bAllowed;
}

namespace
{
    static FString N2CSanitizeBackupName(FString Value)
    {
        const FString Invalid = TEXT("/\\\\:.\\\"'` ,;()[]{}+-=*?!@#$%^&|<>\\t\\r\\n");
        for (int32 Index = 0; Index < Invalid.Len(); ++Index)
        {
            Value.ReplaceInline(*Invalid.Mid(Index, 1), TEXT("_"));
        }
        return Value.IsEmpty() ? TEXT("Blueprint") : Value;
    }

    static bool N2CSaveBlueprintPackage(UBlueprint* Blueprint, const FString& Context, FString& Report)
    {
        if (!Blueprint || !Blueprint->GetOutermost()) return false;
        UPackage* Package = Blueprint->GetOutermost();
        Package->MarkPackageDirty();

        TArray<UPackage*> Packages;
        Packages.Add(Package);
        const FEditorFileUtils::EPromptReturnCode SaveResult =
            FEditorFileUtils::PromptForCheckoutAndSave(Packages, false, false);
        if (SaveResult == FEditorFileUtils::PR_Success)
        {
            return true;
        }

        // UE4.27's unattended PromptForCheckoutAndSave route may return failure
        // without reaching SavePackage. The direct path below is the same API used
        // by the runtime verification harness and is intentionally commandlet-only;
        // interactive imports retain checkout/source-control handling above.
        if (!GIsRunningUnattendedScript && !FApp::IsUnattended())
        {
            return false;
        }

        const FString Filename = FPackageName::LongPackageNameToFilename(
            Package->GetName(),
            FPackageName::GetAssetPackageExtension());
        const bool bSaved = UPackage::SavePackage(
            Package,
            Blueprint,
            RF_Public | RF_Standalone,
            *Filename,
            GError,
            nullptr,
            false,
            true,
            SAVE_NoError);
        N2CPatchImporter_Private::AppendLine(
            Report,
            FString::Printf(
                TEXT("N2C_SAVE_FALLBACK|context=%s|prompt_result=%d|result=%s|package=%s|filename=%s"),
                *Context,
                static_cast<int32>(SaveResult),
                bSaved ? TEXT("PASS") : TEXT("FAIL"),
                *Package->GetName(),
                *Filename));
        return bSaved;
    }
    static bool N2CSaveCurrentBlueprintBeforeApply(UBlueprint* Blueprint, FString& Report)
    {
        if (!Blueprint || !Blueprint->GetOutermost()) return false;
        UPackage* Package = Blueprint->GetOutermost();
        if (!Package->IsDirty()) return true;

        if (!N2CSaveBlueprintPackage(Blueprint, TEXT("pre_apply"), Report))
        {
            N2CPatchImporter_Private::AppendLine(
                Report,
                TEXT("ERROR: current Blueprint has unsaved changes and could not be saved before N2C apply. Patch was rejected before mutation."));
            return false;
        }
        N2CPatchImporter_Private::AppendLine(
            Report,
            TEXT("Pre-apply save completed so backup and rollback baseline include the latest Blueprint state."));
        return true;
    }

    static bool N2CCreateBlueprintApplyBackup(UBlueprint* Blueprint, FString& OutBackupPath, FString& Report)
    {
        OutBackupPath.Empty();
        if (!Blueprint || !Blueprint->GetOutermost()) return false;

        FString SourceFilename;
        const FString PackageName = Blueprint->GetOutermost()->GetName();
        if (!FPackageName::DoesPackageExist(PackageName, nullptr, &SourceFilename) ||
            SourceFilename.IsEmpty())
        {
            N2CPatchImporter_Private::AppendLine(
                Report,
                FString::Printf(
                    TEXT("ERROR: cannot create pre-apply backup because package file does not exist: %s. Save the asset first."),
                    *PackageName));
            return false;
        }

        const FString BackupDir = FPaths::ProjectSavedDir() / TEXT("NodeToCode/Backups");
        IFileManager::Get().MakeDirectory(*BackupDir, true);
        const FString Extension = FPaths::GetExtension(SourceFilename, true);
        const FString Stamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
        OutBackupPath = BackupDir / FString::Printf(
            TEXT("N2C_%s_%s_pre_apply%s"),
            *N2CSanitizeBackupName(Blueprint->GetName()),
            *Stamp,
            *Extension);

        const int32 CopyResult = IFileManager::Get().Copy(
            *OutBackupPath,
            *SourceFilename,
            true,
            true);
        if (CopyResult != COPY_OK)
        {
            N2CPatchImporter_Private::AppendLine(
                Report,
                FString::Printf(
                    TEXT("ERROR: failed to create pre-apply backup '%s' from '%s' (copy result %d). Patch was rejected before mutation."),
                    *OutBackupPath,
                    *SourceFilename,
                    CopyResult));
            OutBackupPath.Empty();
            return false;
        }

        N2CPatchImporter_Private::AppendLine(
            Report,
            FString::Printf(TEXT("Backup created: %s"), *OutBackupPath));
        return true;
    }

    static bool N2CBuildRollbackSnapshot(
        UBlueprint* Blueprint,
        TSharedPtr<FJsonObject>& OutSnapshot,
        FString& OutHash,
        FString& Report,
        const TCHAR* Stage)
    {
        FString Error;
        if (!FN2CStructuralSnapshot::Build(Blueprint, OutSnapshot, OutHash, Error))
        {
            N2CPatchImporter_Private::AppendLine(
                Report,
                FString::Printf(
                    TEXT("ERROR: could not build %s structural snapshot: %s"),
                    Stage,
                    *Error));
            return false;
        }
        return true;
    }


    static bool N2CQueueAutomaticBackupRestore(
        UBlueprint* Blueprint,
        const FString& BackupPath,
        FString& OutManifestPath,
        FString& Report)
    {
        OutManifestPath.Empty();
        if (!Blueprint || !Blueprint->GetOutermost() || BackupPath.IsEmpty() ||
            !IFileManager::Get().FileExists(*BackupPath))
        {
            N2CPatchImporter_Private::AppendLine(
                Report,
                TEXT("ERROR: automatic disk-restore fallback could not be queued because the pre-apply backup is unavailable."));
            return false;
        }

        FString TargetFilename;
        const FString PackageName = Blueprint->GetOutermost()->GetName();
        if (!FPackageName::DoesPackageExist(PackageName, nullptr, &TargetFilename) ||
            TargetFilename.IsEmpty())
        {
            N2CPatchImporter_Private::AppendLine(
                Report,
                FString::Printf(
                    TEXT("ERROR: automatic disk-restore fallback could not resolve package file for '%s'."),
                    *PackageName));
            return false;
        }

        const FString PendingDir = FPaths::ProjectSavedDir() / TEXT("NodeToCode/Backups/PendingRestore");
        IFileManager::Get().MakeDirectory(*PendingDir, true);
        const FDateTime Now = FDateTime::Now();
        const FString Stamp = FString::Printf(TEXT("%s_%lld"), *Now.ToString(TEXT("%Y%m%d_%H%M%S")), Now.GetTicks());
        const FString SafeObjectName = N2CSanitizeBackupName(Blueprint->GetName());
        const FString SafePackageName = N2CSanitizeBackupName(PackageName);
        const FString Extension = FPaths::GetExtension(TargetFilename, true);
        const FString PendingBackupCopy = PendingDir / FString::Printf(
            TEXT("N2C_%s_%s_auto_rollback%s"),
            *SafeObjectName,
            *Stamp,
            *Extension);

        if (IFileManager::Get().Copy(*PendingBackupCopy, *BackupPath, true, true) != COPY_OK ||
            !IFileManager::Get().FileExists(*PendingBackupCopy))
        {
            N2CPatchImporter_Private::AppendLine(
                Report,
                FString::Printf(
                    TEXT("ERROR: automatic disk-restore fallback could not copy backup '%s' to '%s'."),
                    *BackupPath,
                    *PendingBackupCopy));
            return false;
        }

        OutManifestPath = PendingDir / FString::Printf(
            TEXT("%s__%s.restore"),
            *SafePackageName,
            *Stamp);
        FString Manifest;
        Manifest += FString::Printf(TEXT("TargetFilename=%s%s"), *TargetFilename, LINE_TERMINATOR);
        Manifest += FString::Printf(TEXT("PendingBackupCopy=%s%s"), *PendingBackupCopy, LINE_TERMINATOR);
        Manifest += FString::Printf(TEXT("OriginalBackupPath=%s%s"), *BackupPath, LINE_TERMINATOR);
        Manifest += FString::Printf(TEXT("RollbackPath=%s%s"), TEXT(""), LINE_TERMINATOR);
        Manifest += FString::Printf(TEXT("PackageName=%s%s"), *PackageName, LINE_TERMINATOR);
        Manifest += FString::Printf(TEXT("AssetPathName=%s%s"), *Blueprint->GetPathName(), LINE_TERMINATOR);
        Manifest += FString::Printf(TEXT("CreatedAt=%s%s"), *FDateTime::Now().ToString(), LINE_TERMINATOR);

        if (!FFileHelper::SaveStringToFile(Manifest, *OutManifestPath))
        {
            IFileManager::Get().Delete(*PendingBackupCopy, false, true, true);
            N2CPatchImporter_Private::AppendLine(
                Report,
                FString::Printf(
                    TEXT("ERROR: automatic disk-restore fallback could not write manifest '%s'."),
                    *OutManifestPath));
            OutManifestPath.Empty();
            return false;
        }

        N2CPatchImporter_Private::AppendLine(
            Report,
            FString::Printf(
                TEXT("N2C_ROLLBACK_FALLBACK|result=QUEUED|manifest=%s|backup=%s"),
                *OutManifestPath,
                *BackupPath));
        N2CPatchImporter_Private::AppendLine(
            Report,
            TEXT("Automatic disk restore was queued for the next UE startup. Close the editor without saving this Blueprint, then reopen the project."));
        return true;
    }

    static bool N2CPerformVerifiedRollback(
        UBlueprint* Blueprint,
        const FString& BeforeHash,
        const FString& BackupPath,
        const TCHAR* FailureStage,
        bool bResetDirtyFlag,
        FString& Report)
    {
        N2CPatchImporter_Private::AppendLine(
            Report,
            FString::Printf(
                TEXT("Rollback requested after '%s': applying editor Undo for the completed N2C transaction."),
                FailureStage));

        bool bUndoRequested = false;
        if (GEditor)
        {
            GEditor->UndoTransaction();
            bUndoRequested = true;
        }
        else
        {
            N2CPatchImporter_Private::AppendLine(
                Report,
                TEXT("ERROR: GEditor is unavailable; automatic transaction Undo could not run."));
        }

        if (Blueprint)
        {
            FKismetEditorUtilities::CompileBlueprint(Blueprint);
        }

        TSharedPtr<FJsonObject> AfterSnapshot;
        FString AfterHash;
        const bool bSnapshotBuilt = Blueprint && N2CBuildRollbackSnapshot(
            Blueprint,
            AfterSnapshot,
            AfterHash,
            Report,
            TEXT("post-undo"));
        const bool bRollbackVerified = bUndoRequested && bSnapshotBuilt && BeforeHash == AfterHash;

        if (bRollbackVerified)
        {
            if (bResetDirtyFlag && Blueprint && Blueprint->GetOutermost())
            {
                Blueprint->GetOutermost()->SetDirtyFlag(false);
            }
            N2CPatchImporter_Private::AppendLine(
                Report,
                FString::Printf(
                    TEXT("N2C_ROLLBACK_RESULT|result=PASS|stage=%s|method=editor_undo_structural_snapshot|before=%s|after=%s|backup=%s"),
                    FailureStage,
                    *BeforeHash,
                    *AfterHash,
                    BackupPath.IsEmpty() ? TEXT("<not-created>") : *BackupPath));
            N2CPatchImporter_Private::AppendLine(
                Report,
                TEXT("Rollback verified: Blueprint structure matches the pre-apply snapshot."));
            return true;
        }

        FString PendingManifest;
        const bool bFallbackQueued = N2CQueueAutomaticBackupRestore(
            Blueprint,
            BackupPath,
            PendingManifest,
            Report);
        N2CPatchImporter_Private::AppendLine(
            Report,
            FString::Printf(
                TEXT("N2C_ROLLBACK_RESULT|result=FAIL|stage=%s|method=editor_undo_structural_snapshot|before=%s|after=%s|backup=%s|fallback=%s"),
                FailureStage,
                *BeforeHash,
                AfterHash.IsEmpty() ? TEXT("<snapshot-failed>") : *AfterHash,
                BackupPath.IsEmpty() ? TEXT("<not-created>") : *BackupPath,
                bFallbackQueued ? TEXT("queued") : TEXT("not_queued")));
        N2CPatchImporter_Private::AppendLine(
            Report,
            bFallbackQueued
                ? TEXT("ERROR: in-memory Undo was not structurally verified. Do not save the Blueprint. A disk restore is queued for the next UE startup.")
                : TEXT("ERROR: rollback could not be verified and the disk fallback could not be queued. Do not save the Blueprint; restore the listed pre-apply backup manually."));
        return false;
    }
}

bool FN2CPatchImporter::ApplyPatchToBlueprint(UBlueprint* Blueprint, const FString& PatchJson, FString& OutReport, bool bDeveloperOverride, bool bCompileAndSave)
{
    using namespace N2CPatchImporter_Private;
    OutReport.Empty();

    if (!Blueprint || !Blueprint->GetOutermost())
    {
        AppendLine(OutReport, TEXT("ERROR: ApplyPatchToBlueprint requires a valid Blueprint package."));
        return false;
    }

    FString GateReport;
    // Run parser, schema, coverage and live semantic guards before a transaction.
    if (!ApplyPatchInternal(Blueprint, PatchJson, true, GateReport, bDeveloperOverride))
    {
        OutReport = GateReport;
        AppendLine(OutReport, TEXT("N2C_APPLY_BLOCKED_BEFORE_MUTATION|strict gate rejected the patch before FScopedTransaction."));
        return false;
    }

    if (bCompileAndSave && !N2CSaveCurrentBlueprintBeforeApply(Blueprint, OutReport))
    {
        AppendLine(OutReport, TEXT("N2C_APPLY_BLOCKED_BEFORE_MUTATION|pre-apply save failed."));
        return false;
    }

    FString BackupPath;
    if (bCompileAndSave && !N2CCreateBlueprintApplyBackup(Blueprint, BackupPath, OutReport))
    {
        AppendLine(OutReport, TEXT("N2C_APPLY_BLOCKED_BEFORE_MUTATION|pre-apply backup failed."));
        return false;
    }

    TSharedPtr<FJsonObject> BeforeSnapshot;
    FString BeforeHash;
    if (!N2CBuildRollbackSnapshot(Blueprint, BeforeSnapshot, BeforeHash, OutReport, TEXT("pre-apply")))
    {
        AppendLine(OutReport, TEXT("N2C_APPLY_BLOCKED_BEFORE_MUTATION|rollback baseline could not be captured."));
        return false;
    }

    bool bApplyOk = false;
    {
        FScopedTransaction Transaction(NSLOCTEXT("NodeToCode", "ApplyN2CPatch", "Apply N2C Blueprint Patch"));
        Blueprint->Modify();
        Blueprint->GetOutermost()->Modify();
        bApplyOk = ApplyPatchInternal(
            Blueprint,
            PatchJson,
            false,
            OutReport,
            bDeveloperOverride,
            bCompileAndSave);
#if WITH_DEV_AUTOMATION_TESTS
        if (bApplyOk)
        {
            TSharedPtr<FJsonObject> AutomationRoot;
            FString AutomationParseReport;
            if (ParseRoot(PatchJson, AutomationRoot, AutomationParseReport) &&
                GetBoolFieldSafe(AutomationRoot, TEXT("automation_force_failure_after_mutation"), false))
            {
                bApplyOk = false;
                AppendLine(OutReport, TEXT("N2C_TEST_FAULT_INJECTION|type=apply_failure_after_mutation"));
                AppendLine(OutReport, TEXT("ERROR: automation forced failure after real Blueprint mutation."));
            }
        }
#endif
        // Do not call Transaction.Cancel() on failure. Cancel only removes the
        // transaction record and leaves already-mutated UObject state behind.
        // Let the transaction close, then perform an actual editor Undo.
    }

    if (!bApplyOk)
    {
        N2CPerformVerifiedRollback(
            Blueprint,
            BeforeHash,
            BackupPath,
            TEXT("apply_or_compile"),
            bCompileAndSave,
            OutReport);
        return false;
    }

    if (bCompileAndSave)
    {
        if (!N2CSaveBlueprintPackage(Blueprint, TEXT("post_apply"), OutReport))
        {
            AppendLine(
                OutReport,
                TEXT("ERROR: patch compiled, but the Blueprint package could not be saved. Rollback will be requested."));
            N2CPerformVerifiedRollback(
                Blueprint,
                BeforeHash,
                BackupPath,
                TEXT("save"),
                true,
                OutReport);
            return false;
        }
        AppendLine(OutReport, TEXT("Saved Blueprint package after successful N2C patch apply."));
    }

    AppendLine(
        OutReport,
        FString::Printf(
            TEXT("N2C_APPLY_RESULT|result=PASS|backup=%s"),
            BackupPath.IsEmpty() ? TEXT("<not-created>") : *BackupPath));
    return true;
}

bool FN2CPatchImporter::DryRunPatch(UBlueprint* Blueprint, const FString& PatchJson, FString& OutReport, bool bDeveloperOverride)
{
    using namespace N2CPatchImporter_Private;
    return ApplyPatchInternal(Blueprint, PatchJson, true, OutReport, bDeveloperOverride);
}


