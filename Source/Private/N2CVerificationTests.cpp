// Copyright (c) 2025 Nick McClure (Protospatial). All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "AssetRegistryModule.h"
#include "Core/N2CPatchImporter.h"
#include "Core/N2CAIExport.h"
#include "Core/N2CCoverage.h"
#include "Core/N2CEditorIntegration.h"
#include "Core/N2CToolbarCommand.h"
#include "Core/N2CRoundTripVerification.h"
#include "Core/N2CStructuralSnapshot.h"
#include "Core/N2CMacroReference.h"
#include "EdGraphSchema_K2.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Components/BoxComponent.h"
#include "Engine/DataTable.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Animation/Skeleton.h"
#include "Engine/TimelineTemplate.h"
#include "Engine/UserDefinedStruct.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "AnimGraphNode_Root.h"
#include "AnimationGraphSchema.h"
#include "Blueprint/UserWidget.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Toolkits/AssetEditorManager.h"
#include "Kismet2/StructureEditorUtils.h"
#include "K2Node_Composite.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallFunctionOnMember.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Tunnel.h"
#include "PackageTools.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "K2Node_Variable.h"
#include "K2Node_CallParentFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_GetArrayItem.h"
#include "K2Node_Knot.h"
#include "K2Node_Self.h"
#include "Misc/FileHelper.h"
#include "Misc/DefaultValueHelper.h"
#include "Misc/PackageName.h"
#include "Models/N2CLogging.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/Interface.h"
#include "UObject/UnrealType.h"
#include "UserDefinedStructure/UserDefinedStructEditorData.h"
#include "WidgetBlueprint.h"

namespace N2CVerificationTests_Private
{
    static FString OneLine(FString Value)
    {
        Value.ReplaceInline(TEXT("\r"), TEXT(" "));
        Value.ReplaceInline(TEXT("\n"), TEXT(" "));
        Value.ReplaceInline(TEXT("|"), TEXT("/"));
        while (Value.ReplaceInline(TEXT("  "), TEXT(" ")) > 0)
        {
        }
        return Value.TrimStartAndEnd();
    }

    static UBlueprint* ResolveBlueprint(const FString& Target)
    {
        if (Target.StartsWith(TEXT("/")))
        {
            const FString ObjectPath = Target.Contains(TEXT("."))
                ? Target
                : Target + TEXT(".") + FPaths::GetBaseFilename(Target);
            return LoadObject<UBlueprint>(nullptr, *ObjectPath);
        }

        FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        TArray<FAssetData> Blueprints;
        AssetRegistryModule.Get().GetAssetsByClass(UBlueprint::StaticClass()->GetFName(), Blueprints, true);
        for (const FAssetData& Asset : Blueprints)
        {
            if (Asset.AssetName.ToString().Equals(Target, ESearchCase::IgnoreCase))
            {
                return Cast<UBlueprint>(Asset.GetAsset());
            }
        }
        return nullptr;
    }

    static bool SaveAsset(UObject* Asset)
    {
        if (!Asset)
        {
            return false;
        }
        UPackage* Package = Asset->GetOutermost();
        const FString Filename = FPackageName::LongPackageNameToFilename(
            Package->GetName(),
            FPackageName::GetAssetPackageExtension());
        Package->MarkPackageDirty();
        return UPackage::SavePackage(
            Package,
            Asset,
            RF_Public | RF_Standalone,
            *Filename,
            GError,
            nullptr,
            false,
            true,
            SAVE_NoError);
    }

    static int32 CountPendingRestoreManifests()
    {
        TArray<FString> Files;
        IFileManager::Get().FindFiles(
            Files,
            *FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("NodeToCode/Backups/PendingRestore/*.restore")),
            true,
            false);
        return Files.Num();
    }

    static int32 RemoveExistingP1NativeTestNodes(UBlueprint* Blueprint)
    {
        if (!Blueprint) return 0;
        const TSet<FString> P1Classes = {
            TEXT("K2Node_GetEnumeratorNameAsString"), TEXT("K2Node_CreateWidget"),
            TEXT("K2Node_InputActionEvent"), TEXT("K2Node_InputAxisEvent"),
            TEXT("K2Node_InputAxisKeyEvent"), TEXT("K2Node_InputKeyEvent"),
            TEXT("K2Node_InputTouchEvent"), TEXT("K2Node_ForEachElementInEnum"),
            TEXT("K2Node_EaseFunction"), TEXT("K2Node_CastByteToEnum"),
            TEXT("K2Node_EnumLiteral") };
        int32 Removed = 0;
        for (UEdGraph* Graph : Blueprint->UbergraphPages)
        {
            if (!Graph) continue;
            const TArray<UEdGraphNode*> ExistingNodes = Graph->Nodes;
            for (UEdGraphNode* Node : ExistingNodes)
            {
                if (Node && P1Classes.Contains(Node->GetClass()->GetName()))
                {
                    Node->DestroyNode();
                    ++Removed;
                }
            }
        }
        if (Removed > 0) FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
        return Removed;
    }
    static bool HasExpectedP1GetEnumeratorNameContext(UBlueprint* Blueprint)
    {
        if (!Blueprint) return false;
        for (UEdGraph* Graph : Blueprint->UbergraphPages)
        {
            if (!Graph) continue;
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (!Node || Node->GetClass()->GetName() != TEXT("K2Node_GetEnumeratorNameAsString")) continue;
                UEdGraphPin* EnumeratorPin = Node->FindPin(TEXT("Enumerator"));
                UEdGraphPin* ContextPin = EnumeratorPin && EnumeratorPin->LinkedTo.Num() > 0
                    ? EnumeratorPin->LinkedTo[0]
                    : EnumeratorPin;
                UEnum* Enum = ContextPin ? Cast<UEnum>(ContextPin->PinType.PinSubCategoryObject.Get()) : nullptr;
                if (!Enum || Enum->GetPathName() != TEXT("/Script/Engine.EInputEvent")) return false;
                if (EnumeratorPin->LinkedTo.Num() == 0)
                {
                    return EnumeratorPin->DefaultValue == Enum->GetNameStringByIndex(0);
                }
                UEdGraphNode* LiteralNode = ContextPin->GetOwningNode();
                UEdGraphPin* LiteralValuePin = LiteralNode ? LiteralNode->FindPin(TEXT("Enum")) : nullptr;
                return LiteralNode && LiteralNode->GetClass()->GetName() == TEXT("K2Node_EnumLiteral") &&
                    LiteralValuePin && LiteralValuePin->DefaultValue == Enum->GetNameStringByIndex(0);
            }
        }
        return false;
    }
    static bool HasExpectedP4ContainerDefaults(UBlueprint* Blueprint)
    {
        if (!Blueprint || !Blueprint->GeneratedClass)
        {
            return false;
        }

        UObject* CDO = Blueprint->GeneratedClass->GetDefaultObject();
        FSetProperty* SetProperty = FindFProperty<FSetProperty>(Blueprint->GeneratedClass, TEXT("N2C_P4_NameSet"));
        FMapProperty* MapProperty = FindFProperty<FMapProperty>(Blueprint->GeneratedClass, TEXT("N2C_P4_NameToInt"));
        FNameProperty* SetElementProperty = SetProperty ? CastField<FNameProperty>(SetProperty->ElementProp) : nullptr;
        FNameProperty* MapKeyProperty = MapProperty ? CastField<FNameProperty>(MapProperty->KeyProp) : nullptr;
        FIntProperty* MapValueProperty = MapProperty ? CastField<FIntProperty>(MapProperty->ValueProp) : nullptr;
        if (!CDO || !SetProperty || !MapProperty || !SetElementProperty || !MapKeyProperty || !MapValueProperty)
        {
            return false;
        }

        FScriptSetHelper SetHelper(SetProperty, SetProperty->ContainerPtrToValuePtr<void>(CDO));
        bool bHasAlpha = false;
        bool bHasBeta = false;
        for (int32 Index = 0; Index < SetHelper.GetMaxIndex(); ++Index)
        {
            if (SetHelper.IsValidIndex(Index))
            {
                const FName Value = SetElementProperty->GetPropertyValue(SetHelper.GetElementPtr(Index));
                bHasAlpha |= Value == TEXT("Alpha");
                bHasBeta |= Value == TEXT("Beta");
            }
        }

        FScriptMapHelper MapHelper(MapProperty, MapProperty->ContainerPtrToValuePtr<void>(CDO));
        bool bHasAlphaOne = false;
        bool bHasBetaTwo = false;
        for (int32 Index = 0; Index < MapHelper.GetMaxIndex(); ++Index)
        {
            if (MapHelper.IsValidIndex(Index))
            {
                const FName Key = MapKeyProperty->GetPropertyValue(MapHelper.GetKeyPtr(Index));
                const int32 Value = MapValueProperty->GetPropertyValue(MapHelper.GetValuePtr(Index));
                bHasAlphaOne |= Key == TEXT("Alpha") && Value == 1;
                bHasBetaTwo |= Key == TEXT("Beta") && Value == 2;
            }
        }

        return SetHelper.Num() == 2 && bHasAlpha && bHasBeta && MapHelper.Num() == 2 && bHasAlphaOne && bHasBetaTwo;
    }

    static UUserDefinedStruct* EnsureRowStruct()
    {
        static const TCHAR* ObjectPath = TEXT("/Game/N2C_Test/ST_N2C_Row.ST_N2C_Row");
        if (UUserDefinedStruct* Existing = LoadObject<UUserDefinedStruct>(nullptr, ObjectPath))
        {
            return Existing;
        }

        UPackage* Package = CreatePackage(TEXT("/Game/N2C_Test/ST_N2C_Row"));
        UUserDefinedStruct* Struct = FStructureEditorUtils::CreateUserDefinedStruct(
            Package,
            TEXT("ST_N2C_Row"),
            RF_Public | RF_Standalone | RF_Transactional);
        if (!Struct)
        {
            return nullptr;
        }
        FAssetRegistryModule::AssetCreated(Struct);

        TArray<FStructVariableDescription>& Variables = FStructureEditorUtils::GetVarDesc(Struct);
        if (Variables.Num() > 0)
        {
            FStructureEditorUtils::RenameVariable(Struct, Variables[0].VarGuid, TEXT("RowFlag"));
        }

        FEdGraphPinType FloatType;
        FloatType.PinCategory = UEdGraphSchema_K2::PC_Float;
        if (FStructureEditorUtils::AddVariable(Struct, FloatType))
        {
            TArray<FStructVariableDescription>& Updated = FStructureEditorUtils::GetVarDesc(Struct);
            FStructureEditorUtils::RenameVariable(Struct, Updated.Last().VarGuid, TEXT("RowValue"));
        }

        FEdGraphPinType StringType;
        StringType.PinCategory = UEdGraphSchema_K2::PC_String;
        if (FStructureEditorUtils::AddVariable(Struct, StringType))
        {
            TArray<FStructVariableDescription>& Updated = FStructureEditorUtils::GetVarDesc(Struct);
            FStructureEditorUtils::RenameVariable(Struct, Updated.Last().VarGuid, TEXT("RowLabel"));
        }
        FStructureEditorUtils::CompileStructure(Struct);
        return SaveAsset(Struct) ? Struct : nullptr;
    }

    static UDataTable* EnsureDataTable(UUserDefinedStruct* RowStruct)
    {
        static const TCHAR* ObjectPath = TEXT("/Game/N2C_Test/DT_N2C_Row.DT_N2C_Row");
        if (!RowStruct)
        {
            return nullptr;
        }

        UDataTable* DataTable = LoadObject<UDataTable>(nullptr, ObjectPath);
        if (!DataTable)
        {
            UPackage* Package = CreatePackage(TEXT("/Game/N2C_Test/DT_N2C_Row"));
            DataTable = NewObject<UDataTable>(
                Package,
                TEXT("DT_N2C_Row"),
                RF_Public | RF_Standalone | RF_Transactional);
            FAssetRegistryModule::AssetCreated(DataTable);
        }
        DataTable->RowStruct = RowStruct;
        if (!DataTable->GetRowNames().Contains(TEXT("RowA")))
        {
            DataTable->CreateTableFromJSONString(
                TEXT("[{\"Name\":\"RowA\",\"RowFlag\":false,\"RowValue\":1.0,\"RowLabel\":\"Automation\"}]"));
        }
        return SaveAsset(DataTable) ? DataTable : nullptr;
    }

    static UBlueprint* EnsureMacroLibrary()
    {
        static const TCHAR* ObjectPath = TEXT("/Game/N2C_Test/BPL_N2C_Macros.BPL_N2C_Macros");
        UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, ObjectPath);
        if (!Blueprint)
        {
            UPackage* Package = CreatePackage(TEXT("/Game/N2C_Test/BPL_N2C_Macros"));
            Blueprint = FKismetEditorUtilities::CreateBlueprint(
                UObject::StaticClass(),
                Package,
                TEXT("BPL_N2C_Macros"),
                BPTYPE_MacroLibrary,
                UBlueprint::StaticClass(),
                UBlueprintGeneratedClass::StaticClass(),
                TEXT("N2CVerification"));
            if (!Blueprint)
            {
                return nullptr;
            }
            FAssetRegistryModule::AssetCreated(Blueprint);
        }

        const FName MacroName(TEXT("N2C_TestProjectMacro"));
        const bool bExists = Blueprint->MacroGraphs.ContainsByPredicate([&](const UEdGraph* Graph)
        {
            return Graph && Graph->GetFName() == MacroName;
        });
        if (!bExists)
        {
            UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(
                Blueprint,
                MacroName,
                UEdGraph::StaticClass(),
                UEdGraphSchema_K2::StaticClass());
            FBlueprintEditorUtils::AddMacroGraph(Blueprint, Graph, true, nullptr);
            FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
            FKismetEditorUtilities::CompileBlueprint(Blueprint);
        }
        SaveAsset(Blueprint);
        return Blueprint;
    }

    static UBlueprint* EnsureP3Interface()
    {
        static const TCHAR* ObjectPath = TEXT("/Game/N2C_Test/BPI_N2C_P3.BPI_N2C_P3");
        UBlueprint* InterfaceBlueprint = LoadObject<UBlueprint>(nullptr, ObjectPath);
        if (!InterfaceBlueprint)
        {
            UPackage* Package = CreatePackage(TEXT("/Game/N2C_Test/BPI_N2C_P3"));
            InterfaceBlueprint = FKismetEditorUtilities::CreateBlueprint(
                UInterface::StaticClass(),
                Package,
                TEXT("BPI_N2C_P3"),
                BPTYPE_Interface,
                UBlueprint::StaticClass(),
                UBlueprintGeneratedClass::StaticClass(),
                TEXT("N2CVerification"));
            if (!InterfaceBlueprint)
            {
                return nullptr;
            }
            FAssetRegistryModule::AssetCreated(InterfaceBlueprint);
        }

        const FName FunctionName(TEXT("N2C_P3_InterfaceFunction"));
        const bool bExists = InterfaceBlueprint->FunctionGraphs.ContainsByPredicate([&](const UEdGraph* Graph)
        {
            return Graph && Graph->GetFName() == FunctionName;
        });
        if (!bExists)
        {
            UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(
                InterfaceBlueprint,
                FunctionName,
                UEdGraph::StaticClass(),
                UEdGraphSchema_K2::StaticClass());
            FBlueprintEditorUtils::AddFunctionGraph<UClass>(InterfaceBlueprint, Graph, true, nullptr);
            FKismetEditorUtilities::CompileBlueprint(InterfaceBlueprint);
        }
        FKismetEditorUtilities::CompileBlueprint(InterfaceBlueprint);
        UE_LOG(LogNodeToCode, Display, TEXT("N2C_VERIFY_P3_INTERFACE|asset=%s|class=%s|flags=%u"),
            *InterfaceBlueprint->GetPathName(),
            InterfaceBlueprint->GeneratedClass ? *InterfaceBlueprint->GeneratedClass->GetPathName() : TEXT("<null>"),
            InterfaceBlueprint->GeneratedClass ? static_cast<uint32>(InterfaceBlueprint->GeneratedClass->ClassFlags) : 0u);
        return SaveAsset(InterfaceBlueprint) ? InterfaceBlueprint : nullptr;
    }

    static UWidgetBlueprint* EnsureP3WidgetBlueprint()
    {
        static const TCHAR* ObjectPath = TEXT("/Game/N2C_Test/WBP_N2C_P3.WBP_N2C_P3");
        UWidgetBlueprint* Blueprint = LoadObject<UWidgetBlueprint>(nullptr, ObjectPath);
        if (!Blueprint)
        {
            UPackage* Package = CreatePackage(TEXT("/Game/N2C_Test/WBP_N2C_P3"));
            Blueprint = Cast<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(
                UUserWidget::StaticClass(), Package, TEXT("WBP_N2C_P3"), BPTYPE_Normal,
                UWidgetBlueprint::StaticClass(), UWidgetBlueprintGeneratedClass::StaticClass(), TEXT("N2CVerification")));
            if (Blueprint)
            {
                FAssetRegistryModule::AssetCreated(Blueprint);
            }
        }
        return Blueprint && SaveAsset(Blueprint) ? Blueprint : nullptr;
    }

    static UAnimBlueprint* EnsureP3AnimationBlueprint()
    {
        static const TCHAR* ObjectPath = TEXT("/Game/N2C_Test/ABP_N2C_P3.ABP_N2C_P3");
        UAnimBlueprint* Blueprint = LoadObject<UAnimBlueprint>(nullptr, ObjectPath);
        if (!Blueprint)
        {
            UPackage* Package = CreatePackage(TEXT("/Game/N2C_Test/ABP_N2C_P3"));
            Blueprint = Cast<UAnimBlueprint>(FKismetEditorUtilities::CreateBlueprint(
                UAnimInstance::StaticClass(), Package, TEXT("ABP_N2C_P3"), BPTYPE_Normal,
                UAnimBlueprint::StaticClass(), UAnimBlueprintGeneratedClass::StaticClass(), TEXT("N2CVerification")));
            if (Blueprint)
            {
                FAssetRegistryModule::AssetCreated(Blueprint);
            }
        }
        if (!Blueprint)
        {
            return nullptr;
        }

        if (!Blueprint->TargetSkeleton)
        {
            FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
            TArray<FAssetData> SkeletonAssets;
            AssetRegistryModule.Get().GetAssetsByClass(USkeleton::StaticClass()->GetFName(), SkeletonAssets, true);
            USkeleton* Skeleton = SkeletonAssets.Num() > 0 ? Cast<USkeleton>(SkeletonAssets[0].GetAsset()) : nullptr;
            if (!Skeleton)
            {
                return nullptr;
            }
            Blueprint->TargetSkeleton = Skeleton;
            if (UAnimBlueprintGeneratedClass* Generated = Cast<UAnimBlueprintGeneratedClass>(Blueprint->GeneratedClass))
            {
                Generated->TargetSkeleton = Skeleton;
            }
            if (UAnimBlueprintGeneratedClass* SkeletonGenerated = Cast<UAnimBlueprintGeneratedClass>(Blueprint->SkeletonGeneratedClass))
            {
                SkeletonGenerated->TargetSkeleton = Skeleton;
            }
        }
        return SaveAsset(Blueprint) ? Blueprint : nullptr;
    }

    static bool LoadPackPatch(const FString& RelativePath, FString& OutJson)
    {
        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("NodeToCode"));
        return Plugin.IsValid() && FFileHelper::LoadFileToString(
            OutJson,
            *FPaths::Combine(Plugin->GetBaseDir(), TEXT("examples/P0_P1_P2_v58"), RelativePath));
    }

    static UBlueprint* EnsureP0RuntimeBlueprint()
    {
        static const TCHAR* ObjectPath = TEXT("/Game/N2C_Test/BP_N2C_P0Runtime.BP_N2C_P0Runtime");
        UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, ObjectPath);
        if (!Blueprint)
        {
            UPackage* Package = CreatePackage(TEXT("/Game/N2C_Test/BP_N2C_P0Runtime"));
            Blueprint = FKismetEditorUtilities::CreateBlueprint(
                AActor::StaticClass(), Package, TEXT("BP_N2C_P0Runtime"), BPTYPE_Normal,
                UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(), TEXT("N2CVerification"));
            if (Blueprint)
            {
                FAssetRegistryModule::AssetCreated(Blueprint);
            }
        }
        if (!Blueprint)
        {
            return nullptr;
        }

        const FName MemberName(TEXT("N2C_P0_TargetActor"));
        const bool bHasMember = Blueprint->NewVariables.ContainsByPredicate([MemberName](const FBPVariableDescription& Var)
        {
            return Var.VarName == MemberName;
        });
        if (!bHasMember)
        {
            FEdGraphPinType ActorType;
            ActorType.PinCategory = UEdGraphSchema_K2::PC_Object;
            ActorType.PinSubCategoryObject = AActor::StaticClass();
            if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, MemberName, ActorType))
            {
                return nullptr;
            }
        }
        FKismetEditorUtilities::CompileBlueprint(Blueprint);
        // A freshly created Blueprint can transiently report BS_Error before its
        // first graph mutation in commandlet automation. The test below performs
        // the authoritative compile check after applying the P0 patch.
        return SaveAsset(Blueprint) ? Blueprint : nullptr;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FN2CP0ConnectivityRuntimeVerification,
    "NodeToCode.Verification.Legacy.P0ConnectivityRuntime",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FN2CP0ConnectivityRuntimeVerification::RunTest(const FString& Parameters)
{
    using namespace N2CVerificationTests_Private;

    UBlueprint* Blueprint = EnsureP0RuntimeBlueprint();
    if (!Blueprint)
    {
        AddError(TEXT("N2C_VERIFY_P0V65|case=setup|result=FAIL|reason=runtime_blueprint_setup_failed"));
        return false;
    }

    const FString PatchJson = TEXT(R"json({
        "schema":"N2C_PATCH_V1", "target_blueprint":"BP_N2C_P0Runtime", "actions":[{
            "type":"patch_graph", "graph_name":"EventGraph", "nodes":[
                {"id":"BeginPlay","type":"K2Node_Event","event_name":"ReceiveBeginPlay","event_owner_class":"/Script/Engine.Actor","event_is_override":true},
                {"id":"CallParent","type":"K2Node_CallParentFunction","function_name":"ReceiveBeginPlay","class_path":"/Script/Engine.Actor"},
                {"id":"Reroute","type":"K2Node_Knot"},
                {"id":"Self","type":"K2Node_Self"},
                {"id":"ArrayGet","type":"K2Node_GetArrayItem","return_by_ref":false},
                {"id":"MemberCall","type":"K2Node_CallFunctionOnMember","function_name":"K2_GetActorLocation","class_path":"/Script/Engine.Actor","member_variable_name":"N2C_P0_TargetActor"}
            ],
            "exec_edges":[
                {"from_node_id":"BeginPlay","from_pin":"Then","to_node_id":"CallParent","to_pin":"Execute"}
            ], "data_edges":[]
        }]
    })json");

    FString DryRunReport;
    FString ApplyReport;
    const bool bDryRun = FN2CPatchImporter::DryRunPatch(Blueprint, PatchJson, DryRunReport);
    const bool bApplied = bDryRun && FN2CPatchImporter::ApplyPatchToBlueprint(Blueprint, PatchJson, ApplyReport);

    UEdGraph* EventGraph = Blueprint->UbergraphPages.Num() > 0 ? Blueprint->UbergraphPages[0] : nullptr;
    UK2Node_Event* EventNode = nullptr;
    UK2Node_CallParentFunction* ParentNode = nullptr;
    UK2Node_Knot* KnotNode = nullptr;
    UK2Node_Self* SelfNode = nullptr;
    UK2Node_GetArrayItem* ArrayNode = nullptr;
    UK2Node_CallFunctionOnMember* MemberNode = nullptr;
    if (EventGraph)
    {
        for (UEdGraphNode* Node : EventGraph->Nodes)
        {
            EventNode = EventNode ? EventNode : Cast<UK2Node_Event>(Node);
            ParentNode = ParentNode ? ParentNode : Cast<UK2Node_CallParentFunction>(Node);
            KnotNode = KnotNode ? KnotNode : Cast<UK2Node_Knot>(Node);
            SelfNode = SelfNode ? SelfNode : Cast<UK2Node_Self>(Node);
            ArrayNode = ArrayNode ? ArrayNode : Cast<UK2Node_GetArrayItem>(Node);
            MemberNode = MemberNode ? MemberNode : Cast<UK2Node_CallFunctionOnMember>(Node);
        }
    }

    FKismetEditorUtilities::CompileBlueprint(Blueprint);
    FString ExportJson;
    FString ExportError;
    const bool bExported = FN2CAIExport::BuildBlueprintAIJson(Blueprint, ExportJson, ExportError);
    const bool bMetadataPresent = bExported &&
        ExportJson.Contains(TEXT("event_owner_class")) &&
        ExportJson.Contains(TEXT("member_variable_name")) &&
        ExportJson.Contains(TEXT("return_by_ref"));
    const bool bNodesValid = EventNode && EventNode->EventReference.GetMemberName() == TEXT("ReceiveBeginPlay") &&
        ParentNode && KnotNode && SelfNode && ArrayNode && MemberNode &&
        MemberNode->MemberVariableToCallOn.GetMemberName() == TEXT("N2C_P0_TargetActor");
    const bool bSaved = bApplied && bNodesValid && bMetadataPresent && Blueprint->Status != BS_Error && SaveAsset(Blueprint);

    UE_LOG(LogNodeToCode, Display,
        TEXT("N2C_VERIFY_P0V65_SUMMARY|result=%s|dry_run=%d|applied=%d|event=%d|parent=%d|knot=%d|self=%d|array=%d|member=%d|metadata=%d|compile_status=%d|saved=%d|apply_report=%s"),
        bSaved ? TEXT("PASS") : TEXT("FAIL"), bDryRun ? 1 : 0, bApplied ? 1 : 0,
        EventNode ? 1 : 0, ParentNode ? 1 : 0, KnotNode ? 1 : 0, SelfNode ? 1 : 0,
        ArrayNode ? 1 : 0, MemberNode ? 1 : 0, bMetadataPresent ? 1 : 0,
        static_cast<int32>(Blueprint->Status), bSaved ? 1 : 0, *OneLine(ApplyReport));
    if (!bSaved)
    {
        AddError(FString::Printf(TEXT("P0 v65 connectivity runtime verification failed. dry=%s apply=%s nodes=%s export=%s status=%d report=%s"),
            bDryRun ? TEXT("pass") : TEXT("fail"), bApplied ? TEXT("pass") : TEXT("fail"),
            bNodesValid ? TEXT("pass") : TEXT("fail"), bMetadataPresent ? TEXT("pass") : TEXT("fail"),
            static_cast<int32>(Blueprint->Status), *OneLine(ApplyReport)));
    }
    return bSaved;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FN2CP0ConnectivityReopenVerification,
    "NodeToCode.Verification.Legacy.P0ConnectivityReopen",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FN2CP0ConnectivityReopenVerification::RunTest(const FString& Parameters)
{
    using namespace N2CVerificationTests_Private;

    UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, TEXT("/Game/N2C_Test/BP_N2C_P0Runtime.BP_N2C_P0Runtime"));
    UEdGraph* EventGraph = Blueprint && Blueprint->UbergraphPages.Num() > 0 ? Blueprint->UbergraphPages[0] : nullptr;
    bool bEvent = false;
    bool bParent = false;
    bool bKnot = false;
    bool bSelf = false;
    bool bArray = false;
    bool bMember = false;
    if (EventGraph)
    {
        for (UEdGraphNode* Node : EventGraph->Nodes)
        {
            const UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node);
            bEvent |= EventNode && EventNode->EventReference.GetMemberName() == TEXT("ReceiveBeginPlay");
            bParent |= Cast<UK2Node_CallParentFunction>(Node) != nullptr;
            bKnot |= Cast<UK2Node_Knot>(Node) != nullptr;
            bSelf |= Cast<UK2Node_Self>(Node) != nullptr;
            bArray |= Cast<UK2Node_GetArrayItem>(Node) != nullptr;
            const UK2Node_CallFunctionOnMember* MemberNode = Cast<UK2Node_CallFunctionOnMember>(Node);
            bMember |= MemberNode && MemberNode->MemberVariableToCallOn.GetMemberName() == TEXT("N2C_P0_TargetActor");
        }
    }
    if (Blueprint)
    {
        FKismetEditorUtilities::CompileBlueprint(Blueprint);
    }
    FString ExportJson;
    FString ExportError;
    const bool bMetadata = Blueprint && FN2CAIExport::BuildBlueprintAIJson(Blueprint, ExportJson, ExportError) &&
        ExportJson.Contains(TEXT("event_owner_class")) && ExportJson.Contains(TEXT("member_variable_name")) && ExportJson.Contains(TEXT("return_by_ref"));
    const bool bOk = Blueprint && Blueprint->Status != BS_Error && bEvent && bParent && bKnot && bSelf && bArray && bMember && bMetadata;
    UE_LOG(LogNodeToCode, Display,
        TEXT("N2C_VERIFY_P0V65_REOPEN_SUMMARY|result=%s|event=%d|parent=%d|knot=%d|self=%d|array=%d|member=%d|metadata=%d|compile_status=%d"),
        bOk ? TEXT("PASS") : TEXT("FAIL"), bEvent ? 1 : 0, bParent ? 1 : 0, bKnot ? 1 : 0,
        bSelf ? 1 : 0, bArray ? 1 : 0, bMember ? 1 : 0, bMetadata ? 1 : 0,
        Blueprint ? static_cast<int32>(Blueprint->Status) : -1);
    if (!bOk)
    {
        AddError(TEXT("P0 v65 nodes or metadata did not survive reopen/compile."));
    }
    return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FN2CP0P2DryRunVerification,
    "NodeToCode.Verification.Legacy.P0P2DryRun",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FN2CP0P2DryRunVerification::RunTest(const FString& Parameters)
{
    using namespace N2CVerificationTests_Private;

    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("NodeToCode"));
    if (!Plugin.IsValid())
    {
        AddError(TEXT("N2C_VERIFY_FATAL|plugin=NodeToCode|reason=plugin_not_found"));
        return false;
    }

    const FString PackRoot = FPaths::Combine(Plugin->GetBaseDir(), TEXT("examples/P0_P1_P2_v58"));
    TArray<FString> PatchFiles;
    IFileManager::Get().FindFilesRecursive(PatchFiles, *PackRoot, TEXT("*.json"), true, false, false);
    PatchFiles.RemoveAll([](const FString& Path)
    {
        return FPaths::GetCleanFilename(Path).Equals(TEXT("manifest.json"), ESearchCase::IgnoreCase);
    });
    PatchFiles.Sort();

    int32 PassCount = 0;
    int32 ExpectedRejectPassCount = 0;
    int32 TemplateReadyCount = 0;
    int32 TemplateBlockedCount = 0;
    int32 FailCount = 0;
    for (const FString& PatchPath : PatchFiles)
    {
        const FString CaseName = FPaths::GetBaseFilename(PatchPath);
        const bool bExpectedReject = CaseName.Contains(TEXT("EXPECTED_REJECT"), ESearchCase::IgnoreCase);
        const bool bTemplate = CaseName.Contains(TEXT("TEMPLATE"), ESearchCase::IgnoreCase);

        FString PatchJson;
        if (!FFileHelper::LoadFileToString(PatchJson, *PatchPath))
        {
            ++FailCount;
            const FString Message = FString::Printf(TEXT("N2C_VERIFY_CASE|case=%s|result=FAIL|reason=file_read_failed"), *CaseName);
            AddError(Message);
            continue;
        }

        TSharedPtr<FJsonObject> Root;
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(PatchJson);
        if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
        {
            ++FailCount;
            const FString Message = FString::Printf(TEXT("N2C_VERIFY_CASE|case=%s|result=FAIL|reason=json_parse_failed"), *CaseName);
            AddError(Message);
            continue;
        }

        FString Target;
        Root->TryGetStringField(TEXT("target_blueprint"), Target);
        UBlueprint* Blueprint = ResolveBlueprint(Target);
        if (!Blueprint)
        {
            ++FailCount;
            const FString Message = FString::Printf(TEXT("N2C_VERIFY_CASE|case=%s|result=FAIL|reason=blueprint_not_found|target=%s"), *CaseName, *OneLine(Target));
            AddError(Message);
            continue;
        }

        FString Report;
        const bool bDryRunOk = FN2CPatchImporter::DryRunPatch(Blueprint, PatchJson, Report);
        FString Result;
        if (bExpectedReject)
        {
            if (!bDryRunOk)
            {
                Result = TEXT("EXPECTED_REJECT_PASS");
                ++ExpectedRejectPassCount;
            }
            else
            {
                Result = TEXT("FAIL");
                ++FailCount;
                AddError(FString::Printf(TEXT("Expected-reject case unexpectedly passed Dry Run: %s"), *CaseName));
            }
        }
        else if (bTemplate)
        {
            if (bDryRunOk)
            {
                Result = TEXT("TEMPLATE_READY_PASS");
                ++TemplateReadyCount;
            }
            else
            {
                Result = TEXT("TEMPLATE_BLOCKED");
                ++TemplateBlockedCount;
            }
        }
        else if (bDryRunOk)
        {
            Result = TEXT("PASS");
            ++PassCount;
        }
        else
        {
            Result = TEXT("FAIL");
            ++FailCount;
            AddError(FString::Printf(TEXT("PASS case failed Dry Run: %s -- %s"), *CaseName, *OneLine(Report)));
        }

        UE_LOG(LogNodeToCode, Display, TEXT("N2C_VERIFY_CASE|case=%s|result=%s|target=%s|report=%s"),
            *CaseName,
            *Result,
            *OneLine(Target),
            *OneLine(Report));
    }

    UE_LOG(LogNodeToCode, Display,
        TEXT("N2C_VERIFY_SUMMARY|total=%d|pass=%d|expected_reject_pass=%d|template_ready=%d|template_blocked=%d|fail=%d"),
        PatchFiles.Num(), PassCount, ExpectedRejectPassCount, TemplateReadyCount, TemplateBlockedCount, FailCount);

    return FailCount == 0;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FN2CP1TemplateApplyVerification,
    "NodeToCode.Verification.P1TemplateApply",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FN2CP1TemplateApplyVerification::RunTest(const FString& Parameters)
{
    using namespace N2CVerificationTests_Private;

    UBlueprint* Blueprint = ResolveBlueprint(TEXT("Test3"));
    if (!Blueprint)
    {
        AddError(TEXT("N2C_VERIFY_APPLY|case=setup|result=FAIL|reason=Test3_not_found"));
        return false;
    }

    UUserDefinedStruct* RowStruct = EnsureRowStruct();
    UDataTable* DataTable = EnsureDataTable(RowStruct);
    UBlueprint* MacroLibrary = EnsureMacroLibrary();
    if (!RowStruct || !DataTable || !MacroLibrary)
    {
        AddError(TEXT("N2C_VERIFY_APPLY|case=prerequisite_assets|result=FAIL|reason=asset_creation_failed"));
        return false;
    }
    UE_LOG(LogNodeToCode, Display, TEXT("N2C_VERIFY_APPLY|case=prerequisite_assets|result=PASS|struct=%s|datatable=%s|macro_library=%s"),
        *RowStruct->GetPathName(), *DataTable->GetPathName(), *MacroLibrary->GetPathName());

    const FString DispatcherPatch = TEXT("{\"schema\":\"N2C_PATCH_V1\",\"target_blueprint\":\"Test3\",\"actions\":[{\"type\":\"add_event_dispatcher\",\"name\":\"N2C_OnP1Test\"}]}");
    FString Report;
    if (!FN2CPatchImporter::ApplyPatchToBlueprint(Blueprint, DispatcherPatch, Report))
    {
        AddError(FString::Printf(TEXT("N2C_VERIFY_APPLY|case=dispatcher_prerequisite|result=FAIL|report=%s"), *OneLine(Report)));
        return false;
    }
    UE_LOG(LogNodeToCode, Display, TEXT("N2C_VERIFY_APPLY|case=dispatcher_prerequisite|result=PASS|report=%s"), *OneLine(Report));

    const TArray<FString> OrderedCases = {
        TEXT("P1/P1_01_GetDataTableRow_TYPED_TEMPLATE.json"),
        TEXT("P1/P1_02_SetFieldsInStruct_NATIVE_AND_USER_TEMPLATE.json"),
        TEXT("P1/P1_04_ProjectMacro_TEMPLATE.json"),
        TEXT("P1/P1_05_DelegateNodes_EXISTING_DISPATCHER_TEMPLATE.json"),
        TEXT("P2/P2_02_SCS_Hierarchy_PASS.json"),
        TEXT("P1/P1_06_ComponentBoundEvent_AFTER_SCS_TEMPLATE.json")
    };

    int32 FailCount = 0;
    for (const FString& RelativePath : OrderedCases)
    {
        FString PatchJson;
        if (!LoadPackPatch(RelativePath, PatchJson))
        {
            ++FailCount;
            AddError(FString::Printf(TEXT("N2C_VERIFY_APPLY|case=%s|result=FAIL|reason=file_read_failed"), *RelativePath));
            continue;
        }

        FString ApplyReport;
        const bool bApplied = FN2CPatchImporter::ApplyPatchToBlueprint(Blueprint, PatchJson, ApplyReport);
        const FString CaseName = FPaths::GetBaseFilename(RelativePath);
        UE_LOG(LogNodeToCode, Display, TEXT("N2C_VERIFY_APPLY|case=%s|result=%s|report=%s"),
            *CaseName,
            bApplied ? TEXT("PASS") : TEXT("FAIL"),
            *OneLine(ApplyReport));
        if (!bApplied)
        {
            ++FailCount;
            AddError(FString::Printf(TEXT("Template Apply failed: %s -- %s"), *CaseName, *OneLine(ApplyReport)));
        }
    }

    const TArray<FString> AdditionalPassCases = {
        TEXT("P0/P0_01_Pure_InternalExec_PASS.json"),
        TEXT("P0/P0_03_SignatureCleanup_STEP1_PASS.json"),
        TEXT("P0/P0_04_SignatureCleanup_STEP2_PASS.json"),
        TEXT("P1/P1_03_StandardMacros_PASS.json"),
        TEXT("P2/P2_01_AddComponent_K2_PASS.json"),
        TEXT("P2/P2_03_Delay_EventGraph_PASS.json"),
        TEXT("P2/P2_05_RetriggerableDelay_EventGraph_PASS.json"),
        TEXT("P2/P2_06_Timeline_Full_PASS.json")
    };
    for (const FString& RelativePath : AdditionalPassCases)
    {
        FString PatchJson;
        FString ApplyReport;
        const bool bLoaded = LoadPackPatch(RelativePath, PatchJson);
        const bool bApplied = bLoaded && FN2CPatchImporter::ApplyPatchToBlueprint(Blueprint, PatchJson, ApplyReport);
        const FString CaseName = FPaths::GetBaseFilename(RelativePath);
        UE_LOG(LogNodeToCode, Display, TEXT("N2C_VERIFY_APPLY|case=%s|result=%s|report=%s"),
            *CaseName, bApplied ? TEXT("PASS") : TEXT("FAIL"), *OneLine(ApplyReport));
        if (!bApplied)
        {
            ++FailCount;
            AddError(FString::Printf(TEXT("PASS Apply failed: %s -- %s"), *CaseName, *OneLine(ApplyReport)));
        }
    }

    const TArray<FString> ExpectedRejectCases = {
        TEXT("P0/P0_02_Pure_NoInternalExec_EXPECTED_REJECT.json"),
        TEXT("P0/P0_05_NativeGenericMake_EXPECTED_REJECT.json"),
        TEXT("P2/P2_04_Delay_InFunction_EXPECTED_REJECT.json")
    };
    for (const FString& RelativePath : ExpectedRejectCases)
    {
        FString PatchJson;
        FString RejectReport;
        const bool bLoaded = LoadPackPatch(RelativePath, PatchJson);
        const bool bRejected = bLoaded && !FN2CPatchImporter::DryRunPatch(Blueprint, PatchJson, RejectReport);
        const FString CaseName = FPaths::GetBaseFilename(RelativePath);
        UE_LOG(LogNodeToCode, Display, TEXT("N2C_VERIFY_APPLY|case=%s|result=%s|report=%s"),
            *CaseName, bRejected ? TEXT("EXPECTED_REJECT_PASS") : TEXT("FAIL"), *OneLine(RejectReport));
        if (!bRejected)
        {
            ++FailCount;
            AddError(FString::Printf(TEXT("Expected-reject case did not reject: %s"), *CaseName));
        }
    }

    if (!SaveAsset(Blueprint))
    {
        ++FailCount;
        AddError(TEXT("N2C_VERIFY_APPLY|case=save_Test3|result=FAIL"));
    }
    UE_LOG(LogNodeToCode, Display, TEXT("N2C_VERIFY_APPLY_SUMMARY|cases=%d|fail=%d|blueprint_status=%d"),
        OrderedCases.Num() + AdditionalPassCases.Num() + ExpectedRejectCases.Num() + 2,
        FailCount,
        static_cast<int32>(Blueprint->Status));
    return FailCount == 0;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FN2CFunctionDescriptionMetadataVerification,
    "NodeToCode.Verification.FunctionDescriptionMetadata",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FN2CFunctionDescriptionMetadataVerification::RunTest(const FString& Parameters)
{
    using namespace N2CVerificationTests_Private;
    UBlueprint* Blueprint = ResolveBlueprint(TEXT("Test3"));
    if (!Blueprint) { AddError(TEXT("N2C_VERIFY_FUNCTION_METADATA|result=FAIL|reason=Test3_not_found")); return false; }
    const FString Patch = TEXT(R"json({"schema":"N2C_PATCH_V1","target_blueprint":"Test3","actions":[{"type":"add_or_replace_function","function_name":"N2C_Description_Alias","category":"Combat|Damage","keywords":"damage armour modifier","description":"Calculates final damage after armour and active modifiers.","nodes":[{"id":"Entry","type":"Entry"},{"id":"Return","type":"Return"}],"exec_edges":[{"from_node_id":"Entry","from_pin":"then","to_node_id":"Return","to_pin":"execute"}]}]})json");
    FString Report;
    const bool bApplied = FN2CPatchImporter::ApplyPatchToBlueprint(Blueprint, Patch, Report);
    UEdGraph* Graph = nullptr;
    for (UEdGraph* Candidate : Blueprint->FunctionGraphs) if (Candidate && Candidate->GetName() == TEXT("N2C_Description_Alias")) { Graph = Candidate; break; }
    const FKismetUserDeclaredFunctionMetadata* Metadata = Graph ? FBlueprintEditorUtils::GetGraphFunctionMetaData(Graph) : nullptr;
    const bool bDescriptionStored = Metadata && Metadata->ToolTip.ToString() == TEXT("Calculates final damage after armour and active modifiers.");
    const FString TooltipPatch = TEXT(R"json({"schema":"N2C_PATCH_V1","target_blueprint":"Test3","actions":[{"type":"add_or_replace_function","function_name":"N2C_Description_Alias","tooltip":"Tooltip alias is stored in ToolTip.","nodes":[{"id":"Entry","type":"Entry"},{"id":"Return","type":"Return"}],"exec_edges":[{"from_node_id":"Entry","from_pin":"then","to_node_id":"Return","to_pin":"execute"}]}]})json");
    FString TooltipReport;
    const bool bAppliedTooltip = FN2CPatchImporter::ApplyPatchToBlueprint(Blueprint, TooltipPatch, TooltipReport);
    Metadata = Graph ? FBlueprintEditorUtils::GetGraphFunctionMetaData(Graph) : nullptr;
    const bool bTooltipStored = Metadata && Metadata->ToolTip.ToString() == TEXT("Tooltip alias is stored in ToolTip.");    FKismetEditorUtilities::CompileBlueprint(Blueprint);
    const bool bOk = bApplied && bDescriptionStored && bAppliedTooltip && bTooltipStored && Blueprint->Status != BS_Error && SaveAsset(Blueprint);
    UE_LOG(LogNodeToCode, Display, TEXT("N2C_VERIFY_FUNCTION_METADATA|result=%s|description=%d|tooltip=%d|compile_status=%d|report=%s"), bOk ? TEXT("PASS") : TEXT("FAIL"), bDescriptionStored ? 1 : 0, bTooltipStored ? 1 : 0, static_cast<int32>(Blueprint->Status), *OneLine(Report));
    if (!bOk) AddError(TEXT("Function description metadata Apply verification failed."));
    return bOk;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FN2CP1NativeConstructorsVerification,
    "NodeToCode.Verification.P1NativeConstructors",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FN2CP1NativeConstructorsVerification::RunTest(const FString& Parameters)
{
    using namespace N2CVerificationTests_Private;
    UBlueprint* Blueprint = ResolveBlueprint(TEXT("Test3"));
    if (!Blueprint) { AddError(TEXT("N2C_VERIFY_P1_NATIVE|result=FAIL|reason=Test3_not_found")); return false; }
    const int32 RemovedOldP1Nodes = RemoveExistingP1NativeTestNodes(Blueprint);
    if (RemovedOldP1Nodes > 0)
    {
        FKismetEditorUtilities::CompileBlueprint(Blueprint);
        if (Blueprint->Status == BS_Error || !SaveAsset(Blueprint))
        {
            AddError(TEXT("N2C_VERIFY_P1_NATIVE|result=FAIL|reason=old_test_node_cleanup_failed"));
            return false;
        }
    }
    const FString PatchJson = TEXT(R"json({"schema":"N2C_PATCH_V1","target_blueprint":"Test3","actions":[{"type":"patch_event_graph","nodes":[
        {"id":"P1_EnumName","type":"GetEnumeratorNameAsString","enum_path":"/Script/Engine.EInputEvent"},
        {"id":"P1_Widget","type":"CreateWidget","class_path":"/Script/UMG.UserWidget"},
        {"id":"P1_Action","type":"InputActionEvent","input_action_name":"Jump","input_event":"Pressed"},{"id":"P1_Axis","type":"InputAxisEvent","input_axis_name":"MoveForward"},{"id":"P1_AxisKey","type":"InputAxisKeyEvent","key_name":"MouseX"},{"id":"P1_Key","type":"InputKeyEvent","key_name":"SpaceBar","input_event":"Pressed"},{"id":"P1_Touch","type":"InputTouchEvent"},
        {"id":"P1_ForEach","type":"ForEachElementInEnum","enum_path":"/Script/Engine.EInputEvent"},{"id":"P1_Ease","type":"EaseFunction"},
        {"id":"P1_CastEnum","type":"CastByteToEnum","enum_path":"/Script/Engine.EInputEvent"},{"id":"P1_Literal","type":"EnumLiteral","enum_path":"/Script/Engine.EInputEvent"}
    ],"data_edges":[{"from_node_id":"P1_Literal","from_pin":"ReturnValue","to_node_id":"P1_EnumName","to_pin":"Enumerator"}]}]})json");
    FString Report;
    const bool bDryRun = FN2CPatchImporter::ApplyPatchToBlueprint(Blueprint, PatchJson, Report);
    const bool bEnumContext = bDryRun && HasExpectedP1GetEnumeratorNameContext(Blueprint);
    const bool bSaved = bEnumContext && SaveAsset(Blueprint);
    UE_LOG(LogNodeToCode, Display, TEXT("N2C_VERIFY_P1_NATIVE|case=apply_compile_save|result=%s|enum_context=%d|saved=%d|report=%s"), bDryRun && bEnumContext && bSaved ? TEXT("PASS") : TEXT("FAIL"), bEnumContext ? 1 : 0, bSaved ? 1 : 0, *OneLine(Report));
    const FString RejectPatch = TEXT(R"json({"schema":"N2C_PATCH_V1","target_blueprint":"Test3","actions":[{"type":"patch_event_graph","nodes":[{"id":"MissingMaterial","type":"CallMaterialParameterCollectionFunction"},{"id":"MissingMessage","type":"Message"}]}]})json");
    FString RejectReport;
    const bool bRejected = !FN2CPatchImporter::DryRunPatch(Blueprint, RejectPatch, RejectReport);
    UE_LOG(LogNodeToCode, Display, TEXT("N2C_VERIFY_P1_NATIVE|case=missing_function_metadata|result=%s|report=%s"), bRejected ? TEXT("EXPECTED_REJECT_PASS") : TEXT("FAIL"), *OneLine(RejectReport));
    if (!bDryRun) { AddError(FString::Printf(TEXT("P1 native constructor Apply/compile failed: %s"), *OneLine(Report))); }
    if (!bEnumContext) { AddError(TEXT("P1 GetEnumeratorNameAsString did not retain the selected enum literal.")); }
    if (!bSaved) { AddError(TEXT("P1 native constructor asset save failed.")); }
    if (!bRejected) { AddError(TEXT("P1 missing function metadata did not reject before mutation.")); }
    return bDryRun && bEnumContext && bSaved && bRejected;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FN2CReopenCompileVerification,
    "NodeToCode.Verification.ReopenCompile",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FN2CReopenCompileVerification::RunTest(const FString& Parameters)
{
    using namespace N2CVerificationTests_Private;

    UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, TEXT("/Game/Test/test3.test3"));
    if (!Blueprint)
    {
        AddError(TEXT("N2C_VERIFY_REOPEN|result=FAIL|reason=Test3_load_failed"));
        return false;
    }

    FKismetEditorUtilities::CompileBlueprint(Blueprint);
    int32 FailCount = 0;
    if (Blueprint->Status == BS_Error)
    {
        ++FailCount;
        AddError(TEXT("N2C_VERIFY_REOPEN|check=compile|result=FAIL|status=BS_Error"));
    }

    const bool bDispatcherFound = Blueprint->GeneratedClass &&
        FindFProperty<FMulticastDelegateProperty>(Blueprint->GeneratedClass, TEXT("N2C_OnP1Test")) != nullptr;
    const bool bSCSFound = Blueprint->SimpleConstructionScript &&
        Blueprint->SimpleConstructionScript->GetAllNodes().ContainsByPredicate([](const USCS_Node* Node)
        {
            return Node && Node->GetVariableName() == TEXT("N2C_P2_Box");
        });
    const bool bP1EnumNameFound = HasExpectedP1GetEnumeratorNameContext(Blueprint);
    const bool bTimelineFound = Blueprint->Timelines.ContainsByPredicate([](const UTimelineTemplate* Timeline)
    {
        return Timeline && Timeline->GetFName() == TEXT("N2C_P2_FullTimeline_Template");
    });

    if (!bDispatcherFound || !bSCSFound || !bTimelineFound || !bP1EnumNameFound)
    {
        ++FailCount;
        AddError(FString::Printf(TEXT("N2C_VERIFY_REOPEN|check=persisted_objects|result=FAIL|dispatcher=%d|scs=%d|timeline=%d|p1_enum_name=%d"),
            bDispatcherFound ? 1 : 0, bSCSFound ? 1 : 0, bTimelineFound ? 1 : 0, bP1EnumNameFound ? 1 : 0));
    }

    UE_LOG(LogNodeToCode, Display,
        TEXT("N2C_VERIFY_REOPEN_SUMMARY|result=%s|blueprint_status=%d|dispatcher=%d|scs_box=%d|timeline=%d|p1_enum_name=%d|fail=%d"),
        FailCount == 0 ? TEXT("PASS") : TEXT("FAIL"),
        static_cast<int32>(Blueprint->Status),
        bDispatcherFound ? 1 : 0,
        bSCSFound ? 1 : 0,
        bTimelineFound ? 1 : 0,
        bP1EnumNameFound ? 1 : 0,
        FailCount);
    return FailCount == 0;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FN2CP3GraphApplyVerification,
    "NodeToCode.Verification.P3GraphApply",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FN2CP3GraphApplyVerification::RunTest(const FString& Parameters)
{
    using namespace N2CVerificationTests_Private;
    UBlueprint* Blueprint = ResolveBlueprint(TEXT("Test3"));
    if (!Blueprint)
    {
        AddError(TEXT("N2C_VERIFY_P3|case=setup|result=FAIL|reason=Test3_not_found"));
        return false;
    }
    if (!EnsureP3Interface())
    {
        AddError(TEXT("N2C_VERIFY_P3|case=interface_prerequisite|result=FAIL"));
        return false;
    }
    UWidgetBlueprint* WidgetBlueprint = EnsureP3WidgetBlueprint();
    UAnimBlueprint* AnimationBlueprint = EnsureP3AnimationBlueprint();
    if (!WidgetBlueprint || !AnimationBlueprint)
    {
        AddError(TEXT("N2C_VERIFY_P3|case=special_graph_prerequisites|result=FAIL"));
        return false;
    }

    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("NodeToCode"));
    const FString P3Root = Plugin.IsValid()
        ? FPaths::Combine(Plugin->GetBaseDir(), TEXT("examples/P3_P4_P5/P3"))
        : FString();
    int32 FailCount = 0;
    const TArray<FString> PassFiles = {
        TEXT("P3_01_CollapsedGraph_PASS.json"),
        TEXT("P3_02_ConstructionScript_PASS.json")
    };
    for (const FString& Filename : PassFiles)
    {
        FString Json;
        FString Report;
        const bool bLoaded = FFileHelper::LoadFileToString(Json, *FPaths::Combine(P3Root, Filename));
        const bool bApplied = bLoaded && FN2CPatchImporter::ApplyPatchToBlueprint(Blueprint, Json, Report);
        UE_LOG(LogNodeToCode, Display, TEXT("N2C_VERIFY_P3|case=%s|result=%s|report=%s"),
            *FPaths::GetBaseFilename(Filename), bApplied ? TEXT("PASS") : TEXT("FAIL"), *OneLine(Report));
        if (!bApplied)
        {
            ++FailCount;
            AddError(FString::Printf(TEXT("P3 Apply failed: %s -- %s"), *Filename, *OneLine(Report)));
        }
    }

    FString RejectJson;
    FString RejectReport;
    const FString RejectFile = FPaths::Combine(P3Root, TEXT("P3_03_MissingGraph_EXPECTED_REJECT.json"));
    const bool bRejectLoaded = FFileHelper::LoadFileToString(RejectJson, *RejectFile);
    const bool bRejected = bRejectLoaded && !FN2CPatchImporter::DryRunPatch(Blueprint, RejectJson, RejectReport);
    UE_LOG(LogNodeToCode, Display, TEXT("N2C_VERIFY_P3|case=P3_03_MissingGraph_EXPECTED_REJECT|result=%s|report=%s"),
        bRejected ? TEXT("EXPECTED_REJECT_PASS") : TEXT("FAIL"), *OneLine(RejectReport));
    if (!bRejected)
    {
        ++FailCount;
        AddError(TEXT("P3 missing-graph case did not reject."));
    }

    const TArray<FString> InterfacePassFiles = {
        TEXT("P3_04_ImplementInterface_PASS.json"),
        TEXT("P3_05_InterfaceGraph_PASS.json")
    };
    for (const FString& Filename : InterfacePassFiles)
    {
        FString Json;
        FString Report;
        const bool bLoaded = FFileHelper::LoadFileToString(Json, *FPaths::Combine(P3Root, Filename));
        const bool bApplied = bLoaded && FN2CPatchImporter::ApplyPatchToBlueprint(Blueprint, Json, Report);
        UE_LOG(LogNodeToCode, Display, TEXT("N2C_VERIFY_P3|case=%s|result=%s|report=%s"),
            *FPaths::GetBaseFilename(Filename), bApplied ? TEXT("PASS") : TEXT("FAIL"), *OneLine(Report));
        if (!bApplied)
        {
            ++FailCount;
            AddError(FString::Printf(TEXT("P3 interface case failed: %s -- %s"), *Filename, *OneLine(Report)));
        }
    }

    const TArray<FString> SpecialGraphPassFiles = {
        TEXT("P3_06_WidgetGraph_PASS.json"),
        TEXT("P3_07_AnimationGraph_PASS.json")
    };
    for (const FString& Filename : SpecialGraphPassFiles)
    {
        FString Json;
        FString Report;
        const bool bLoaded = FFileHelper::LoadFileToString(Json, *FPaths::Combine(P3Root, Filename));
        UBlueprint* Target = Filename.Contains(TEXT("Widget")) ? static_cast<UBlueprint*>(WidgetBlueprint) : static_cast<UBlueprint*>(AnimationBlueprint);
        const bool bApplied = bLoaded && FN2CPatchImporter::ApplyPatchToBlueprint(Target, Json, Report);
        UE_LOG(LogNodeToCode, Display, TEXT("N2C_VERIFY_P3|case=%s|result=%s|report=%s"),
            *FPaths::GetBaseFilename(Filename), bApplied ? TEXT("PASS") : TEXT("FAIL"), *OneLine(Report));
        if (!bApplied)
        {
            ++FailCount;
            AddError(FString::Printf(TEXT("P3 special graph case failed: %s -- %s"), *Filename, *OneLine(Report)));
        }
    }

    FString WidgetRejectJson;
    FString WidgetRejectReport;
    const bool bWidgetRejectLoaded = FFileHelper::LoadFileToString(WidgetRejectJson,
        *FPaths::Combine(P3Root, TEXT("P3_08_WidgetGraphWrongAsset_EXPECTED_REJECT.json")));
    const bool bWidgetRejected = bWidgetRejectLoaded && !FN2CPatchImporter::DryRunPatch(Blueprint, WidgetRejectJson, WidgetRejectReport);
    if (!bWidgetRejected)
    {
        ++FailCount;
        AddError(TEXT("P3 Widget wrong-asset case did not reject before mutation."));
    }

    bool bCollapsedFound = false;
    for (UEdGraph* Graph : Blueprint->UbergraphPages)
    {
        if (!Graph)
        {
            continue;
        }
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            const UK2Node_Composite* Composite = Cast<UK2Node_Composite>(Node);
            if (Composite && Composite->BoundGraph && Composite->BoundGraph->GetName() == TEXT("N2C_P3_Collapsed"))
            {
                bCollapsedFound = true;
            }
        }
    }
    bool bAnimRootLinked = false;
    for (UEdGraph* Graph : AnimationBlueprint->FunctionGraphs)
    {
        if (Graph && Graph->GetSchema() && Graph->GetSchema()->IsA(UAnimationGraphSchema::StaticClass()) &&
            Graph->Nodes.ContainsByPredicate([](const UEdGraphNode* Node)
            {
                return Node && Node->GetClass()->GetName() == TEXT("AnimGraphNode_LocalRefPose");
            }))
        {
            bAnimRootLinked = true;
            break;
        }
    }
    if (!bCollapsedFound || !bAnimRootLinked || !SaveAsset(Blueprint) || !SaveAsset(WidgetBlueprint) || !SaveAsset(AnimationBlueprint))
    {
        ++FailCount;
        AddError(TEXT("N2C_VERIFY_P3|case=persistence|result=FAIL"));
    }
    UE_LOG(LogNodeToCode, Display, TEXT("N2C_VERIFY_P3_SUMMARY|result=%s|collapsed=%d|widget=%d|animation=%d|status=%d|fail=%d"),
        FailCount == 0 ? TEXT("PASS") : TEXT("FAIL"), bCollapsedFound ? 1 : 0, WidgetBlueprint ? 1 : 0,
        bAnimRootLinked ? 1 : 0, static_cast<int32>(Blueprint->Status), FailCount);
    return FailCount == 0;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FN2CP4DefaultsVerification,
    "NodeToCode.Verification.P4DefaultsReopen",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FN2CP4DefaultsVerification::RunTest(const FString& Parameters)
{
    using namespace N2CVerificationTests_Private;
    UBlueprint* Blueprint = ResolveBlueprint(TEXT("Test3"));
    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("NodeToCode"));
    FString Json;
    FString Report;
    const bool bLoaded = Blueprint && Plugin.IsValid() && FFileHelper::LoadFileToString(Json,
        *FPaths::Combine(Plugin->GetBaseDir(), TEXT("examples/P3_P4_P5/P4/P4_01_SetMapDefaults_PASS.json")));
    const bool bApplied = bLoaded && FN2CPatchImporter::ApplyPatchToBlueprint(Blueprint, Json, Report);
    const FBPVariableDescription* SetVar = nullptr;
    const FBPVariableDescription* MapVar = nullptr;
    if (Blueprint)
    {
        for (const FBPVariableDescription& Var : Blueprint->NewVariables)
        {
            SetVar = SetVar ? SetVar : (Var.VarName == TEXT("N2C_P4_NameSet") ? &Var : nullptr);
            MapVar = MapVar ? MapVar : (Var.VarName == TEXT("N2C_P4_NameToInt") ? &Var : nullptr);
        }
    }
    const bool bTypesOk = SetVar && MapVar &&
        SetVar->VarType.ContainerType == EPinContainerType::Set &&
        MapVar->VarType.ContainerType == EPinContainerType::Map &&
        MapVar->VarType.PinValueType.TerminalCategory == UEdGraphSchema_K2::PC_Int &&
        HasExpectedP4ContainerDefaults(Blueprint);
    const bool bSaved = bApplied && bTypesOk && SaveAsset(Blueprint);
    UE_LOG(LogNodeToCode, Display, TEXT("N2C_VERIFY_P4_SUMMARY|result=%s|set=%d|map=%d|cdo_defaults=%d|saved=%d|report=%s"),
        bSaved ? TEXT("PASS") : TEXT("FAIL"), SetVar ? 1 : 0, MapVar ? 1 : 0,
        HasExpectedP4ContainerDefaults(Blueprint) ? 1 : 0, bSaved ? 1 : 0, *OneLine(Report));
    if (!bSaved)
    {
        AddError(FString::Printf(TEXT("P4 Set/Map defaults verification failed: %s"), *OneLine(Report)));
    }
    return bSaved;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FN2CP4ReopenVerification,
    "NodeToCode.Verification.P4Reopen",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FN2CP4ReopenVerification::RunTest(const FString& Parameters)
{
    using namespace N2CVerificationTests_Private;
    UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, TEXT("/Game/Test/test3.test3"));
    if (!Blueprint)
    {
        AddError(TEXT("N2C_VERIFY_P4_REOPEN|result=FAIL|reason=Test3_load_failed"));
        return false;
    }
    FKismetEditorUtilities::CompileBlueprint(Blueprint);
    const FBPVariableDescription* SetVar = nullptr;
    const FBPVariableDescription* MapVar = nullptr;
    for (const FBPVariableDescription& Var : Blueprint->NewVariables)
    {
        SetVar = SetVar ? SetVar : (Var.VarName == TEXT("N2C_P4_NameSet") ? &Var : nullptr);
        MapVar = MapVar ? MapVar : (Var.VarName == TEXT("N2C_P4_NameToInt") ? &Var : nullptr);
    }
    const bool bOk = Blueprint->Status != BS_Error && SetVar && MapVar &&
        SetVar->VarType.ContainerType == EPinContainerType::Set &&
        MapVar->VarType.ContainerType == EPinContainerType::Map &&
        MapVar->VarType.PinValueType.TerminalCategory == UEdGraphSchema_K2::PC_Int &&
        HasExpectedP4ContainerDefaults(Blueprint);
    UE_LOG(LogNodeToCode, Display, TEXT("N2C_VERIFY_P4_REOPEN_SUMMARY|result=%s|set=%d|map=%d|cdo_defaults=%d|status=%d"),
        bOk ? TEXT("PASS") : TEXT("FAIL"), SetVar ? 1 : 0, MapVar ? 1 : 0,
        HasExpectedP4ContainerDefaults(Blueprint) ? 1 : 0, static_cast<int32>(Blueprint->Status));
    if (!bOk) { AddError(TEXT("P4 Set/Map defaults did not survive editor reopen/compile.")); }
    return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CP0CoveragePreflightVerification, "NodeToCode.Verification.P0CoveragePreflight", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FN2CP0CoveragePreflightVerification::RunTest(const FString&)
{
    const FString FixtureName = FString::Printf(TEXT("BP_N2C_CoveragePreflight_%lld"), FDateTime::UtcNow().GetTicks());
    UPackage* FixturePackage = CreatePackage(*FString::Printf(TEXT("/Temp/%s"), *FixtureName));
    if (FixturePackage) { FixturePackage->SetFlags(RF_Transient); }
    UBlueprint* Blueprint = FixturePackage ? FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(), FixturePackage, FName(*FixtureName), BPTYPE_Normal,
        UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(), TEXT("N2CVerification")) : nullptr;
    if (!Blueprint) { AddError(TEXT("N2C_VERIFY_COVERAGE|result=FAIL|reason=transient_fixture_creation_failed")); return false; }

    auto WithTarget = [Blueprint](const FString& Template)
    {
        FString Patch = Template;
        Patch.ReplaceInline(TEXT("\"Test3\""), *FString::Printf(TEXT("\"%s\""), *Blueprint->GetName()));
        return Patch;
    };

    auto Preflight = [Blueprint](const FString& Patch, bool bOverride, FN2CPreflightResult& Out, FString& Report)
    {
        return FN2CPatchImporter::PreflightPatch(Blueprint, Patch, bOverride, Out, Report);
    };

    const FString VerifiedPatch = WithTarget(TEXT(R"json({"schema":"N2C_PATCH_V1","target_blueprint":"Test3","actions":[{"type":"patch_event_graph","nodes":[{"id":"Knot","type":"K2Node_Knot"},{"id":"Self","type":"K2Node_Self"}]}]})json"));
    FN2CPreflightResult Verified; FString VerifiedReport;
    const bool bVerifiedAllowed = Preflight(VerifiedPatch, false, Verified, VerifiedReport) && Verified.RuntimeBlockerCount == 0 && Verified.VerificationGapCount == 0;

    const FString FunctionBoundaryPatch = WithTarget(TEXT(R"json({"schema":"N2C_PATCH_V1","target_blueprint":"Test3","actions":[{"type":"add_or_replace_function","function_name":"N2C_CoverageVerifiedBoundary","nodes":[{"id":"Entry","type":"Entry"},{"id":"Return","type":"Return"}],"exec_edges":[{"from_node_id":"Entry","from_pin":"then","to_node_id":"Return","to_pin":"execute"}],"data_edges":[]}]})json"));
    FN2CPreflightResult FunctionBoundary; FString FunctionBoundaryReport;
    const bool bFunctionBoundaryVerified = Preflight(FunctionBoundaryPatch, false, FunctionBoundary, FunctionBoundaryReport) && FunctionBoundary.RuntimeBlockerCount == 0 && FunctionBoundary.VerificationGapCount == 0;

    const FString CosmeticPatch = WithTarget(TEXT(R"json({"schema":"N2C_PATCH_V1","target_blueprint":"Test3","actions":[{"type":"patch_event_graph","nodes":[{"id":"Comment","type":"EdGraphNode_Comment"}]}]})json"));
    FN2CPreflightResult Cosmetic; FString CosmeticReport;
    const bool bCosmeticAllowed = Preflight(CosmeticPatch, false, Cosmetic, CosmeticReport) && Cosmetic.CosmeticWarningCount == 1 && !Cosmetic.bHasRuntimeBlockers;

    const FString GuardedPatch = WithTarget(TEXT(R"json({"schema":"N2C_PATCH_V1","target_blueprint":"Test3","actions":[{"type":"patch_event_graph","nodes":[{"id":"Call","type":"K2Node_CallFunction"}]}]})json"));
    FN2CPreflightResult Guarded; FString GuardedReport;
    const bool bGuardedBlocked = !Preflight(GuardedPatch, false, Guarded, GuardedReport) && Guarded.RuntimeBlockerCount == 1 && Guarded.Issues.Num() == 1 && Guarded.Issues[0].MissingMetadata.Contains(TEXT("function_path"));

    const FString MacroPatch = WithTarget(TEXT(R"json({"schema":"N2C_PATCH_V1","target_blueprint":"Test3","actions":[{"type":"patch_event_graph","nodes":[{"id":"Macro","type":"K2Node_MacroInstance"}]}]})json"));
    FN2CPreflightResult Macro; FString MacroReport;
    const bool bMacroPartial = !Preflight(MacroPatch, false, Macro, MacroReport) && Macro.Issues.Num() == 1 && Macro.Issues[0].Status == TEXT("partial") && Macro.Issues[0].MissingMetadata.Contains(TEXT("macro_owner_path")) && Macro.Issues[0].MissingMetadata.Contains(TEXT("macro_graph_path")) && Macro.Issues[0].MissingMetadata.Contains(TEXT("wildcard_pin_types"));

    const FString UnknownPatch = WithTarget(TEXT(R"json({"schema":"N2C_PATCH_V1","target_blueprint":"Test3","actions":[{"type":"patch_event_graph","nodes":[{"id":"Unknown","type":"K2Node_N2C_UnknownRuntime"}]}]})json"));
    int32 GraphCountBefore = Blueprint->UbergraphPages.Num() + Blueprint->FunctionGraphs.Num() + Blueprint->MacroGraphs.Num() + Blueprint->DelegateSignatureGraphs.Num();
    int32 NodeCountBefore = 0; for (UEdGraph* Graph : Blueprint->UbergraphPages) { if (Graph) { NodeCountBefore += Graph->Nodes.Num(); } }
    const bool bDirtyBefore = Blueprint->GetOutermost() && Blueprint->GetOutermost()->IsDirty(); FString UnknownApplyReport;
    const bool bUnknownRejected = !FN2CPatchImporter::ApplyPatchToBlueprint(Blueprint, UnknownPatch, UnknownApplyReport);
    int32 GraphCountAfter = Blueprint->UbergraphPages.Num() + Blueprint->FunctionGraphs.Num() + Blueprint->MacroGraphs.Num() + Blueprint->DelegateSignatureGraphs.Num();
    int32 NodeCountAfter = 0; for (UEdGraph* Graph : Blueprint->UbergraphPages) { if (Graph) { NodeCountAfter += Graph->Nodes.Num(); } }
    const bool bDirtyAfter = Blueprint->GetOutermost() && Blueprint->GetOutermost()->IsDirty();
    const bool bNoMutation = bUnknownRejected && GraphCountBefore == GraphCountAfter && NodeCountBefore == NodeCountAfter && bDirtyBefore == bDirtyAfter && UnknownApplyReport.Contains(TEXT("N2C_APPLY_BLOCKED_BEFORE_MUTATION"));

    const FString WrongSchema = TEXT(R"json({"schema":"N2C_AI_EXPORT_V2","actions":[]})json"); FString WrongSchemaReport;
    const bool bWrongSchemaRejected = !FN2CPatchImporter::ApplyPatchToBlueprint(Blueprint, WrongSchema, WrongSchemaReport) && WrongSchemaReport.Contains(TEXT("normalizer is P3"));

    FString ExportJson; FString ExportError; FString SidecarJson; FString SidecarReason; bool bStale = false; FString StaleReason;
    const bool bSidecarBuilt = FN2CAIExport::BuildBlueprintAIJson(Blueprint, ExportJson, ExportError) && FN2CCoverageClassifier::BuildBlueprintSidecar(Blueprint, ExportJson, SidecarJson, SidecarReason);
    const bool bStaleDetected = bSidecarBuilt && !FN2CCoverageClassifier::ValidateSidecarForSource(SidecarJson, ExportJson + TEXT(" "), bStale, StaleReason) && bStale && StaleReason.Contains(TEXT("source_hash"));
    TSharedPtr<FJsonObject> Aggregate; TArray<FString> AggregateInputs; AggregateInputs.Add(SidecarJson); AggregateInputs.Add(SidecarJson);
    const bool bAggregate = bSidecarBuilt && FN2CCoverageClassifier::BuildAggregateCoverage(AggregateInputs, Aggregate) && Aggregate.IsValid() && static_cast<int32>(Aggregate->GetNumberField(TEXT("asset_count"))) == 2;

    const bool bOk = bVerifiedAllowed && bFunctionBoundaryVerified && bCosmeticAllowed && bGuardedBlocked && bMacroPartial && bNoMutation && bWrongSchemaRejected && bStaleDetected && bAggregate;
    UE_LOG(LogNodeToCode, Display, TEXT("N2C_VERIFY_COVERAGE|result=%s|verified=%d|function_boundary_verified=%d|cosmetic=%d|guarded=%d|macro=%d|no_mutation=%d|wrong_schema=%d|stale=%d|aggregate=%d"), bOk ? TEXT("PASS") : TEXT("FAIL"), bVerifiedAllowed ? 1 : 0, bFunctionBoundaryVerified ? 1 : 0, bCosmeticAllowed ? 1 : 0, bGuardedBlocked ? 1 : 0, bMacroPartial ? 1 : 0, bNoMutation ? 1 : 0, bWrongSchemaRejected ? 1 : 0, bStaleDetected ? 1 : 0, bAggregate ? 1 : 0);
    if (!bOk) { AddError(TEXT("P0.2 coverage/preflight policy regression failed.")); }
    return bOk;
}
namespace N2CUnsupportedScan_Private
{
    struct FRecord
    {
        FString AssetPath, GraphPath, AssetOrigin, NodeClass, Variant, Status, ConstructorHandler, Fixture, VerificationGap, ExpectedLoss, LossKind, Reason;
        TArray<FString> RequiredMetadata, MissingMetadata;
        bool bReopenVerified = false;
        int32 Count = 0;
    };

    static TArray<TSharedPtr<FJsonValue>> StringArray(const TArray<FString>& Values)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        for (const FString& Value : Values) { Out.Add(MakeShared<FJsonValueString>(Value)); }
        return Out;
    }

    static FString JoinForKey(const TArray<FString>& Values) { return FString::Join(Values, TEXT(",")); }

    static FString GetAssetOrigin(const FString& AssetPath)
    {
        if (AssetPath.StartsWith(TEXT("/Game/"))) { return TEXT("game"); }
        if (AssetPath.StartsWith(TEXT("/Engine/"))) { return TEXT("engine"); }
        const int32 End = AssetPath.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, 1);
        const FString Mount = AssetPath.Mid(1, End == INDEX_NONE ? AssetPath.Len() - 1 : End - 1);
        for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetEnabledPlugins())
        {
            if (Plugin->GetName() == Mount)
            {
                return Plugin->GetLoadedFrom() == EPluginLoadedFrom::Project ? TEXT("project_plugin") : TEXT("engine_plugin");
            }
        }
        return TEXT("project_plugin");
    }

    static bool HasWildcardPin(const UEdGraphNode* Node)
    {
        if (!Node) { return false; }
        for (const UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard) { return true; }
        }
        return false;
    }

    static void SetKnownSupport(FRecord& Out, const TCHAR* Handler, const TCHAR* Reason)
    {
        Out.Status = TEXT("supported_untested");
        Out.ConstructorHandler = Handler;
        Out.Reason = Reason;
        if (!Out.Reason.Contains(TEXT("reopen fixture")))
        {
            Out.Reason += TEXT("; no matching reopen fixture");
        }
        Out.VerificationGap = TEXT("reopen_not_verified");
        Out.ExpectedLoss.Empty();
        Out.LossKind = TEXT("none");
    }

    static void SetGuarded(FRecord& Out, const TCHAR* Handler, const TCHAR* Reason, const TArray<FString>& Required, const TArray<FString>& Missing)
    {
        Out.Status = TEXT("guarded");
        Out.ConstructorHandler = Handler;
        Out.Reason = Reason;
        Out.RequiredMetadata = Required;
        Out.MissingMetadata = Missing;
        Out.ExpectedLoss = TEXT("rejected before mutation when required metadata is absent");
        Out.LossKind = TEXT("runtime_semantic");
    }

    static void ClassifyNode(const UBlueprint* Blueprint, const UEdGraph* Graph, const UEdGraphNode* Node, FRecord& Out)
    {
        Out = FRecord();
        Out.AssetPath = Blueprint ? Blueprint->GetPathName() : FString();
        Out.GraphPath = Graph ? Graph->GetPathName() : FString();
        Out.AssetOrigin = GetAssetOrigin(Out.AssetPath);
        Out.NodeClass = Node && Node->GetClass() ? Node->GetClass()->GetName() : TEXT("unknown");
        Out.Variant = Out.NodeClass;

        const bool bStandardMacrosBody = Out.AssetPath.StartsWith(TEXT("/Engine/EditorBlueprintResources/StandardMacros"));
        if (bStandardMacrosBody || Out.AssetOrigin == TEXT("engine"))
        {
            Out.Status = TEXT("dependency_only");
            Out.ConstructorHandler = bStandardMacrosBody ? TEXT("existing engine macro graph reference") : TEXT("existing engine asset reference");
            Out.Reason = bStandardMacrosBody
                ? TEXT("StandardMacros body is an existing engine dependency, not a project import target")
                : TEXT("engine Blueprint inventory is not part of the /Game production import target");
            Out.LossKind = TEXT("none");
            return;
        }

        FN2CCoverageIssue Issue;
        FN2CCoverageClassifier::ClassifyLiveNode(Blueprint, Graph, Node, Issue);
        Out.NodeClass = Issue.NodeClass;
        Out.Variant = Issue.Variant;
        Out.Status = Issue.Status;
        Out.ConstructorHandler = Issue.ConstructorHandler;
        Out.RequiredMetadata = Issue.RequiredMetadata;
        Out.MissingMetadata = Issue.MissingMetadata;
        Out.Fixture = Issue.VerificationFixture;
        Out.VerificationGap = Issue.VerificationGap;
        Out.bReopenVerified = Issue.bReopenVerified;
        Out.ExpectedLoss = Issue.ExpectedLoss;
        Out.LossKind = Issue.LossKind;
        Out.Reason = Issue.Reason;
    }
    static TSharedPtr<FJsonObject> RecordToJson(const FRecord& Record, const bool bIncludeLocation)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        if (bIncludeLocation)
        {
            Obj->SetStringField(TEXT("asset_path"), Record.AssetPath);
            Obj->SetStringField(TEXT("graph_path"), Record.GraphPath);
        }
        Obj->SetStringField(TEXT("asset_origin"), Record.AssetOrigin);
        Obj->SetStringField(TEXT("node_class"), Record.NodeClass);
        Obj->SetStringField(TEXT("variant"), Record.Variant);
        Obj->SetNumberField(TEXT("count"), Record.Count);
        Obj->SetStringField(TEXT("status"), Record.Status);
        Obj->SetStringField(TEXT("constructor_handler"), Record.ConstructorHandler);
        Obj->SetArrayField(TEXT("required_metadata"), StringArray(Record.RequiredMetadata));
        Obj->SetArrayField(TEXT("missing_metadata"), StringArray(Record.MissingMetadata));
        Obj->SetStringField(TEXT("verification_fixture"), Record.Fixture);
        if (!Record.VerificationGap.IsEmpty()) { Obj->SetStringField(TEXT("verification_gap"), Record.VerificationGap); }
        Obj->SetBoolField(TEXT("reopen_verified"), Record.bReopenVerified);
        Obj->SetStringField(TEXT("expected_loss"), Record.ExpectedLoss);
        Obj->SetStringField(TEXT("loss_kind"), Record.LossKind);
        Obj->SetStringField(TEXT("reason"), Record.Reason);
        return Obj;
    }

    static void AccumulateRecord(TArray<FRecord>& Records, TMap<FString, int32>& Index, const FRecord& In, const bool bIncludeLocation)
    {
        TArray<FString> KeyParts; KeyParts.Add(bIncludeLocation ? In.AssetPath : TEXT("")); KeyParts.Add(bIncludeLocation ? In.GraphPath : TEXT("")); KeyParts.Add(In.AssetOrigin); KeyParts.Add(In.NodeClass); KeyParts.Add(In.Variant); KeyParts.Add(In.Status); KeyParts.Add(In.ConstructorHandler); KeyParts.Add(JoinForKey(In.RequiredMetadata)); KeyParts.Add(JoinForKey(In.MissingMetadata)); KeyParts.Add(In.Fixture); KeyParts.Add(In.ExpectedLoss); KeyParts.Add(In.LossKind); KeyParts.Add(In.Reason); const FString Key = FString::Join(KeyParts, TEXT("|"));
        if (int32* Existing = Index.Find(Key)) { Records[*Existing].Count += In.Count; return; }
        Index.Add(Key, Records.Num());
        Records.Add(In);
    }

    static TArray<TSharedPtr<FJsonValue>> SerializeRecords(const TArray<FRecord>& Records, const bool bIncludeLocation)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        for (const FRecord& Record : Records) { Out.Add(MakeShared<FJsonValueObject>(RecordToJson(Record, bIncludeLocation))); }
        return Out;
    }
    static TArray<TSharedPtr<FJsonValue>> SerializeCountMap(const TMap<FString, int32>& Counts, const TArray<FString>& Fields)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        for (const TPair<FString, int32>& Pair : Counts)
        {
            TArray<FString> Parts;
            Pair.Key.ParseIntoArray(Parts, TEXT("|"), false);
            TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
            for (int32 Index = 0; Index < Fields.Num(); ++Index)
            {
                Obj->SetStringField(Fields[Index], Parts.IsValidIndex(Index) ? Parts[Index] : TEXT(""));
            }
            Obj->SetNumberField(TEXT("node_count"), Pair.Value);
            Out.Add(MakeShared<FJsonValueObject>(Obj));
        }
        return Out;
    }

}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CP0UnsupportedNodeScanVerification,"NodeToCode.Verification.Inventory.UnsupportedNodeScan",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FN2CP0UnsupportedNodeScanVerification::RunTest(const FString&)
{
    using namespace N2CUnsupportedScan_Private;
    FAssetRegistryModule& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    TArray<FAssetData> Assets; Registry.Get().GetAssetsByClass(UBlueprint::StaticClass()->GetFName(), Assets, true);
    Assets.Sort([](const FAssetData& A, const FAssetData& B) { return A.ObjectPath.ToString() < B.ObjectPath.ToString(); });

    TArray<FRecord> InstanceVariants, ClassVariants, ProductionClassVariants;
    TMap<FString, int32> InstanceIndex, ClassIndex, ProductionClassIndex;
    TMap<FString, int32> OriginAssets, OriginGraphs, OriginNodes, ProductionStatusCounts, AssetNodeCounts, GraphNodeCounts, CompactClassCounts, ProductionClassCounts;
    TSet<FString> SeenAssets, SeenGraphs;
    int32 AssetCount = 0, GraphCount = 0, NodeCount = 0;
    for (const FAssetData& Asset : Assets)
    {
        UBlueprint* Blueprint = Cast<UBlueprint>(Asset.GetAsset()); if (!Blueprint) { continue; }
        ++AssetCount;
        const FString Origin = GetAssetOrigin(Blueprint->GetPathName());
        OriginAssets.FindOrAdd(Origin)++;
        TArray<UEdGraph*> Graphs; Graphs.Append(Blueprint->UbergraphPages); Graphs.Append(Blueprint->FunctionGraphs); Graphs.Append(Blueprint->MacroGraphs);
        TArray<UObject*> Children; GetObjectsWithOuter(Blueprint, Children, true);
        for (UObject* Child : Children) { if (UEdGraph* Graph = Cast<UEdGraph>(Child)) { if (!Graphs.Contains(Graph)) { Graphs.Add(Graph); } } }
        for (UEdGraph* Graph : Graphs)
        {
            if (!Graph) { continue; }
            ++GraphCount; OriginGraphs.FindOrAdd(Origin)++;
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (!Node || !Node->GetClass()) { continue; }
                ++NodeCount; OriginNodes.FindOrAdd(Origin)++;
                FRecord Record; Record.Count = 1; ClassifyNode(Blueprint, Graph, Node, Record);
                AssetNodeCounts.FindOrAdd(Record.AssetOrigin + TEXT("|") + Record.AssetPath)++;
                GraphNodeCounts.FindOrAdd(Record.AssetOrigin + TEXT("|") + Record.AssetPath + TEXT("|") + Record.GraphPath)++;
                CompactClassCounts.FindOrAdd(Record.AssetOrigin + TEXT("|") + Record.NodeClass + TEXT("|") + Record.Status)++;
                if (Origin == TEXT("game")) { ProductionStatusCounts.FindOrAdd(Record.Status)++; ProductionClassCounts.FindOrAdd(Record.NodeClass + TEXT("|") + Record.Status)++; AccumulateRecord(ProductionClassVariants, ProductionClassIndex, Record, false); }
                AccumulateRecord(InstanceVariants, InstanceIndex, Record, true);
                AccumulateRecord(ClassVariants, ClassIndex, Record, false);
            }
        }
    }

    TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
    Summary->SetStringField(TEXT("schema"), TEXT("N2C_UNSUPPORTED_SCAN_V2"));
    Summary->SetStringField(TEXT("scan_version"), TEXT("P0.1-20260713-v2"));
    FString PluginVersion = TEXT("unknown");
    if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("NodeToCode")))
    {
        PluginVersion = FString::Printf(TEXT("%d / %s"), Plugin->GetDescriptor().Version, *Plugin->GetDescriptor().VersionName);
    }
    Summary->SetStringField(TEXT("plugin_version"), PluginVersion);
    Summary->SetStringField(TEXT("source_revision"), TEXT("NodeToCode production P0; metadata-aware scanner"));
    Summary->SetStringField(TEXT("engine_target"), TEXT("UE4.27.2"));
    Summary->SetStringField(TEXT("generated_at"), FDateTime::Now().ToIso8601());
    TSharedPtr<FJsonObject> Raw = MakeShared<FJsonObject>();
    Raw->SetNumberField(TEXT("asset_count"), AssetCount); Raw->SetNumberField(TEXT("graph_count"), GraphCount); Raw->SetNumberField(TEXT("node_instance_count"), NodeCount);
    TSet<FString> UniqueClasses; for (const FRecord& Record : InstanceVariants) { UniqueClasses.Add(Record.NodeClass); }
    Raw->SetNumberField(TEXT("unique_node_class_count"), UniqueClasses.Num());
    Summary->SetObjectField(TEXT("raw_inventory"), Raw);

    TArray<TSharedPtr<FJsonValue>> Origins;
    const TArray<FString> OriginNames = { TEXT("game"), TEXT("project_plugin"), TEXT("engine"), TEXT("engine_plugin") };
    for (const FString& Origin : OriginNames)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("asset_origin"), Origin); Obj->SetNumberField(TEXT("asset_count"), OriginAssets.FindRef(Origin)); Obj->SetNumberField(TEXT("graph_count"), OriginGraphs.FindRef(Origin)); Obj->SetNumberField(TEXT("node_count"), OriginNodes.FindRef(Origin));
        Origins.Add(MakeShared<FJsonValueObject>(Obj));
    }
    Summary->SetArrayField(TEXT("origin_counts"), Origins);

    TArray<TSharedPtr<FJsonValue>> ProductionStatuses;
    const TArray<FString> StatusNames = { TEXT("verified"), TEXT("supported_untested"), TEXT("guarded"), TEXT("partial"), TEXT("unsupported"), TEXT("cosmetic_only"), TEXT("dependency_only") };
    for (const FString& Status : StatusNames)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>(); Obj->SetStringField(TEXT("status"), Status); Obj->SetNumberField(TEXT("node_count"), ProductionStatusCounts.FindRef(Status)); ProductionStatuses.Add(MakeShared<FJsonValueObject>(Obj));
    }
    TSharedPtr<FJsonObject> Production = MakeShared<FJsonObject>();
    Production->SetStringField(TEXT("asset_origin"), TEXT("game"));
    Production->SetArrayField(TEXT("status_counts"), ProductionStatuses);
    Production->SetArrayField(TEXT("class_counts"), SerializeCountMap(ProductionClassCounts, { TEXT("node_class"), TEXT("status") }));
    Summary->SetObjectField(TEXT("production_summary"), Production);
    Summary->SetArrayField(TEXT("asset_counts"), SerializeCountMap(AssetNodeCounts, { TEXT("asset_origin"), TEXT("asset_path") }));
    Summary->SetArrayField(TEXT("graph_counts"), SerializeCountMap(GraphNodeCounts, { TEXT("asset_origin"), TEXT("asset_path"), TEXT("graph_path") }));
    Summary->SetArrayField(TEXT("class_counts"), SerializeCountMap(CompactClassCounts, { TEXT("asset_origin"), TEXT("node_class"), TEXT("status") }));

    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("NodeToCode"));
    const FString SummaryPath = Plugin.IsValid() ? FPaths::Combine(Plugin->GetBaseDir(), TEXT("Source/Documentation/N2C_UNSUPPORTED_NODE_SCAN_20260713.json")) : FString();
    const FString InstancesPath = Plugin.IsValid() ? FPaths::Combine(Plugin->GetBaseDir(), TEXT("Source/Documentation/N2C_UNSUPPORTED_NODE_SCAN_INSTANCES_20260713.json")) : FString();
    const FString RawInventoryPath = Plugin.IsValid() ? FPaths::Combine(Plugin->GetBaseDir(), TEXT("Source/Documentation/N2C_UNSUPPORTED_NODE_RAW_INVENTORY_20260713.json")) : FString();
    Summary->SetStringField(TEXT("raw_inventory_file"), FPaths::GetCleanFilename(RawInventoryPath));
    FString SummaryJson; const bool bSummary = FJsonSerializer::Serialize(Summary.ToSharedRef(), TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&SummaryJson));
    FString RawInventoryJson; const bool bRawInventory = FJsonSerializer::Serialize(Raw.ToSharedRef(), TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&RawInventoryJson));
    TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>(); Details->SetStringField(TEXT("schema"), TEXT("N2C_UNSUPPORTED_SCAN_INSTANCES_V2")); Details->SetStringField(TEXT("summary_file"), FPaths::GetCleanFilename(SummaryPath)); Details->SetArrayField(TEXT("class_variants"), SerializeRecords(ClassVariants, false)); Details->SetArrayField(TEXT("instance_variants"), SerializeRecords(InstanceVariants, true));
    FString DetailsJson; const bool bDetails = FJsonSerializer::Serialize(Details.ToSharedRef(), TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&DetailsJson));
    const bool bRawInventoryOutput = FPaths::FileExists(RawInventoryPath) || FFileHelper::SaveStringToFile(RawInventoryJson, *RawInventoryPath);
    const bool bSaved = bSummary && bDetails && bRawInventory && bRawInventoryOutput && FFileHelper::SaveStringToFile(SummaryJson, *SummaryPath) && FFileHelper::SaveStringToFile(DetailsJson, *InstancesPath);
    UE_LOG(LogNodeToCode, Display, TEXT("N2C_VERIFY_P0_SCAN_SUMMARY|result=%s|assets=%d|graphs=%d|nodes=%d|classes=%d|summary=%s|instances=%s"), bSaved ? TEXT("PASS") : TEXT("FAIL"), AssetCount, GraphCount, NodeCount, UniqueClasses.Num(), *SummaryPath, *InstancesPath);
    if (!bSaved) { AddError(TEXT("P0.1 unsupported-node scan failed to serialize or save.")); }
    return bSaved;
}

#endif // WITH_DEV_AUTOMATION_TESTS


#if WITH_DEV_AUTOMATION_TESTS
namespace N2CP0RoundTripTests
{
enum class ECase { Success, ImpureFunction, PureFunction, MultipleResults, Containers, CompileFailure, SaveFailure, MissingArtifact, Timeout, ReloadFailure, StructuralMismatch, RejectNoMutation, MalformedChildResult, ChildIdentityMismatch, StandardSimple, StandardLoop, StandardWildcardContainer, StandardMissingOwner, StandardMissingGraph, StandardSignatureMismatch, ExternalProjectLibrary, ExternalProjectBlueprint, ExternalWildcardContainer, ExternalMissingAsset, ExternalMissingGraph, ExternalSignatureMismatch, ExternalNoMutationReject, GraphMacroTunnel, GraphMacroMultiplePins, GraphCollapsedTunnel, GraphComposite };
static UBlueprint* CreateFixture(const FString& RunId, FString& OutObjectPath)
{
    const FString Name = TEXT("BP_N2C_") + RunId.Replace(TEXT("-"), TEXT("_"));
    const FString PackageName = TEXT("/Game/N2C_Test/Generated/") + Name;
    UPackage* Package = CreatePackage(*PackageName);
    UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), Package, *Name, BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(), TEXT("N2CP0RoundTrip"));
    if (!Blueprint) return nullptr;
    FAssetRegistryModule::AssetCreated(Blueprint); FKismetEditorUtilities::CompileBlueprint(Blueprint);
    if (!N2CVerificationTests_Private::SaveAsset(Blueprint)) return nullptr;
    OutObjectPath = PackageName + TEXT(".") + Name; return Blueprint;
}
static FString SuccessPatch(const FString& Target)
{
    return FString::Printf(TEXT(R"json({"schema":"N2C_PATCH_V1","target_blueprint":"%s","actions":[{"type":"patch_graph","graph_name":"EventGraph","nodes":[{"id":"Reroute","type":"K2Node_Knot"},{"id":"Self","type":"K2Node_Self"}],"exec_edges":[],"data_edges":[]}]})json"), *Target);
}
static FString PurePatch(const FString& Target)
{
    return FString::Printf(TEXT(R"json({"schema":"N2C_PATCH_V1","target_blueprint":"%s","actions":[{"type":"add_or_replace_function","function_name":"N2C_P0_Pure","category":"N2C/P0","replace_body":true,"function_flags":{"access":"public","pure":true,"const":false},"outputs":[{"name":"Message","type":"string"}],"nodes":[{"id":"Entry","type":"Entry"},{"id":"Return","type":"Return","pin_defaults":{"Message":"P0 PURE OK"}}],"exec_edges":[{"from":"Entry","from_pin":"then","to":"Return","to_pin":"execute"}],"data_edges":[]}]})json"), *Target);
}
static FString ImpureFunctionPatch(const FString& Target)
{
    return FString::Printf(TEXT(R"json({"schema":"N2C_PATCH_V1","target_blueprint":"%s","actions":[{"type":"add_or_replace_function","function_name":"N2C_P0_Impure","category":"N2C/P0","replace_body":true,"function_flags":{"access":"public","pure":false,"const":false},"inputs":[{"name":"Amount","type":"int"}],"outputs":[{"name":"Message","type":"string"}],"nodes":[{"id":"Entry","type":"Entry"},{"id":"Return","type":"Return","pin_defaults":{"Message":"P0 IMPURE OK"}}],"exec_edges":[{"from":"Entry","from_pin":"then","to":"Return","to_pin":"execute"}],"data_edges":[]}]})json"), *Target);
}
static FString MultipleResultsPatch(const FString& Target)
{
    return FString::Printf(TEXT(R"json({"schema":"N2C_PATCH_V1","target_blueprint":"%s","actions":[{"type":"add_or_replace_function","function_name":"N2C_P0_MultipleResults","category":"N2C/P0","replace_body":true,"function_flags":{"access":"public","pure":false,"const":false},"inputs":[{"name":"Condition","type":"bool"}],"outputs":[{"name":"Outcome","type":"int"}],"nodes":[{"id":"Entry","type":"Entry"},{"id":"Branch","type":"K2Node_IfThenElse"},{"id":"ReturnA","type":"Return","pin_defaults":{"Outcome":"1"}},{"id":"ReturnB","type":"Return","pin_defaults":{"Outcome":"2"}}],"exec_edges":[{"from":"Entry","from_pin":"then","to":"Branch","to_pin":"execute"},{"from":"Branch","from_pin":"then","to":"ReturnA","to_pin":"execute"},{"from":"Branch","from_pin":"else","to":"ReturnB","to_pin":"execute"}],"data_edges":[{"from":"Entry","from_pin":"Condition","to":"Branch","to_pin":"Condition"}]}]})json"), *Target);
}
static FString ContainerPatch(const FString& Target)
{
    return FString::Printf(TEXT(R"json({"schema":"N2C_PATCH_V1","target_blueprint":"%s","actions":[{"type":"add_member_variables","variables":[{"name":"P0_IntArray","type":"int","container":"Array"},{"name":"P0_NameSet","type":"name","container":"Set","default_value":"(Alpha,Beta)"},{"name":"P0_NameToInt","type":"name","container":"Map","value_type":{"type":"int"},"default_value":"((Alpha,1),(Beta,2))"},{"name":"P0_ActorRef","type":"object","sub_category_object":"/Script/Engine.Actor"},{"name":"P0_ActorClass","type":"class","sub_category_object":"/Script/Engine.Actor"}]}]})json"), *Target);
}
static FString SuccessContract()
{
    return TEXT(R"json({"schema":"N2C_EXPECTED_CONTRACT_V1","required_nodes":[{"node_class":"K2Node_Knot"},{"node_class":"K2Node_Self"}]})json");
}
static FString PureContract()
{
    return TEXT(R"json({"schema":"N2C_EXPECTED_CONTRACT_V1","required_functions":[{"name":"N2C_P0_Pure","pure":true,"internal_entry_result_exec":true,"entry_count":1,"result_count":1}]})json");
}
static FString ImpureFunctionContract() { return TEXT(R"json({"schema":"N2C_EXPECTED_CONTRACT_V1","required_functions":[{"name":"N2C_P0_Impure","pure":false,"internal_entry_result_exec":true,"entry_count":1,"result_count":1}]})json"); }
static FString MultipleResultsContract() { return TEXT(R"json({"schema":"N2C_EXPECTED_CONTRACT_V1","required_functions":[{"name":"N2C_P0_MultipleResults","pure":false,"internal_entry_result_exec":false,"entry_count":1,"result_count":2}]})json"); }
static FString ContainerContract()
{
    return TEXT(R"json({"schema":"N2C_EXPECTED_CONTRACT_V1","required_variables":[{"name":"P0_IntArray","container":1},{"name":"P0_NameSet","container":2,"default":"(\"Alpha\",\"Beta\")"},{"name":"P0_NameToInt","container":3,"map_value_category":"int","default":"((\"Alpha\", 1),(\"Beta\", 2))"},{"name":"P0_ActorRef","container":0,"default":"None"},{"name":"P0_ActorClass","container":0,"default":"None"}]})json");
}
static UEdGraph* StandardMacroGraph(const FString& Name)
{
    UBlueprint* Library = LoadObject<UBlueprint>(nullptr, TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros"));
    if (!Library) return nullptr;
    for (UEdGraph* Graph : Library->MacroGraphs) if (Graph && Graph->GetName() == Name) return Graph;
    return nullptr;
}
static FString SerializeObject(const TSharedPtr<FJsonObject>& Object)
{
    FString Text; TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text); FJsonSerializer::Serialize(Object.ToSharedRef(), Writer); return Text;
}
static TSharedPtr<FJsonObject> BoundaryPin(const TCHAR* Name, const TCHAR* Direction, const TCHAR* Category)
{
    TSharedPtr<FJsonObject> Pin=MakeShared<FJsonObject>();Pin->SetStringField(TEXT("name"),Name);Pin->SetStringField(TEXT("direction"),Direction);TSharedPtr<FJsonObject> Type=MakeShared<FJsonObject>();Type->SetStringField(TEXT("category"),Category);Pin->SetObjectField(TEXT("pin_type"),Type);return Pin;
}
static TSharedPtr<FJsonObject> BoundaryTunnel(const FString& BlueprintPath,const FString& GraphIdentity,const FString& GraphKind,const FString& GraphName,const FString& GraphPath,const TCHAR* Id,const TCHAR* Role,bool bMultiple)
{
    const bool bEntry=FCString::Strcmp(Role,TEXT("entry"))==0;TSharedPtr<FJsonObject> Node=MakeShared<FJsonObject>();Node->SetStringField(TEXT("id"),Id);Node->SetStringField(TEXT("type"),TEXT("K2Node_Tunnel"));Node->SetStringField(TEXT("owner_blueprint_path"),BlueprintPath);Node->SetStringField(TEXT("owning_graph_identity"),GraphIdentity);Node->SetStringField(TEXT("owning_graph_kind"),GraphKind);Node->SetStringField(TEXT("owning_graph_name"),GraphName);Node->SetStringField(TEXT("owning_graph_path"),GraphPath);Node->SetStringField(TEXT("tunnel_role"),Role);Node->SetBoolField(TEXT("can_have_inputs"),!bEntry);Node->SetBoolField(TEXT("can_have_outputs"),bEntry);Node->SetBoolField(TEXT("editable"),true);Node->SetStringField(TEXT("graph_boundary_fingerprint"),TEXT("fixture_declared_exact_persistence_compare"));
    TArray<TSharedPtr<FJsonValue>> Pins;Pins.Add(MakeShared<FJsonValueObject>(BoundaryPin(bEntry?TEXT("Exec"):TEXT("Then"),bEntry?TEXT("output"):TEXT("input"),TEXT("exec"))));Pins.Add(MakeShared<FJsonValueObject>(BoundaryPin(bEntry?TEXT("Value"):TEXT("Result"),bEntry?TEXT("output"):TEXT("input"),TEXT("int"))));if(bMultiple){TSharedPtr<FJsonObject> LabelPin=BoundaryPin(bEntry?TEXT("Label"):TEXT("LabelOut"),bEntry?TEXT("output"):TEXT("input"),TEXT("string"));if(!bEntry)LabelPin->SetStringField(TEXT("default_value"),TEXT("BoundaryDefault"));Pins.Add(MakeShared<FJsonValueObject>(LabelPin));}Node->SetArrayField(TEXT("user_defined_pin_signature"),Pins);return Node;
}
static FString GraphBoundaryPatch(UBlueprint* Blueprint,bool bCollapsed,bool bMultiple)
{
    const FString Asset=Blueprint->GetPathName();const FString Name=bCollapsed?TEXT("N2C_P0_Collapsed"):TEXT("N2C_P0_Macro");const FString GraphPath=bCollapsed?Asset+TEXT(":EventGraph.N2C_P0_Collapsed"):Asset+TEXT(":")+Name;const FString BoundaryIdentity=Asset+TEXT("|")+(bCollapsed?TEXT("collapsed_graph"):TEXT("macro"))+TEXT("|")+Name+TEXT("|")+GraphPath;
    TSharedPtr<FJsonObject> Action=MakeShared<FJsonObject>();Action->SetStringField(TEXT("type"),bCollapsed?TEXT("create_collapsed_graph"):TEXT("add_macro"));Action->SetStringField(bCollapsed?TEXT("collapsed_graph_name"):TEXT("macro_name"),Name);Action->SetStringField(TEXT("owner_blueprint_path"),Asset);
    if(bCollapsed){const FString Outer=Asset+TEXT("|event_graph|EventGraph|")+Asset+TEXT(":EventGraph");Action->SetStringField(TEXT("graph_name"),TEXT("EventGraph"));Action->SetStringField(TEXT("owning_graph_identity"),Outer);Action->SetStringField(TEXT("owning_graph_kind"),TEXT("event_graph"));Action->SetStringField(TEXT("bound_graph_identity"),BoundaryIdentity);Action->SetStringField(TEXT("bound_graph_kind"),TEXT("collapsed_graph"));Action->SetStringField(TEXT("composite_node_id"),TEXT("Composite"));}
    else{Action->SetStringField(TEXT("owning_graph_identity"),BoundaryIdentity);Action->SetStringField(TEXT("owning_graph_kind"),TEXT("macro"));}
    TArray<TSharedPtr<FJsonValue>> Nodes;Nodes.Add(MakeShared<FJsonValueObject>(BoundaryTunnel(Asset,BoundaryIdentity,bCollapsed?TEXT("collapsed_graph"):TEXT("macro"),Name,GraphPath,TEXT("BoundaryEntry"),TEXT("entry"),bMultiple)));Nodes.Add(MakeShared<FJsonValueObject>(BoundaryTunnel(Asset,BoundaryIdentity,bCollapsed?TEXT("collapsed_graph"):TEXT("macro"),Name,GraphPath,TEXT("BoundaryExit"),TEXT("exit"),bMultiple)));Action->SetArrayField(TEXT("nodes"),Nodes);
    TArray<TSharedPtr<FJsonValue>> Exec;TSharedPtr<FJsonObject> E=MakeShared<FJsonObject>();E->SetStringField(TEXT("from"),TEXT("BoundaryEntry"));E->SetStringField(TEXT("from_pin"),TEXT("Exec"));E->SetStringField(TEXT("to"),TEXT("BoundaryExit"));E->SetStringField(TEXT("to_pin"),TEXT("Then"));Exec.Add(MakeShared<FJsonValueObject>(E));Action->SetArrayField(TEXT("exec_edges"),Exec);TArray<TSharedPtr<FJsonValue>> Data;TSharedPtr<FJsonObject>D=MakeShared<FJsonObject>();D->SetStringField(TEXT("from"),TEXT("BoundaryEntry"));D->SetStringField(TEXT("from_pin"),TEXT("Value"));D->SetStringField(TEXT("to"),TEXT("BoundaryExit"));D->SetStringField(TEXT("to_pin"),TEXT("Result"));Data.Add(MakeShared<FJsonValueObject>(D));Action->SetArrayField(TEXT("data_edges"),Data);
    if(bCollapsed){TArray<TSharedPtr<FJsonValue>> Mapping;for(const TCHAR* PinName:{TEXT("Exec"),TEXT("Value")}){TSharedPtr<FJsonObject>M=MakeShared<FJsonObject>();M->SetStringField(TEXT("outer_pin"),PinName);M->SetStringField(TEXT("inner_tunnel_role"),TEXT("entry"));M->SetStringField(TEXT("inner_pin"),PinName);Mapping.Add(MakeShared<FJsonValueObject>(M));}for(const TCHAR* PinName:{TEXT("Then"),TEXT("Result")}){TSharedPtr<FJsonObject>M=MakeShared<FJsonObject>();M->SetStringField(TEXT("outer_pin"),PinName);M->SetStringField(TEXT("inner_tunnel_role"),TEXT("exit"));M->SetStringField(TEXT("inner_pin"),PinName);Mapping.Add(MakeShared<FJsonValueObject>(M));}Action->SetArrayField(TEXT("outer_to_inner_pin_mapping"),Mapping);}
    TSharedPtr<FJsonObject> Root=MakeShared<FJsonObject>();Root->SetStringField(TEXT("schema"),TEXT("N2C_PATCH_V1"));Root->SetStringField(TEXT("target_blueprint"),Blueprint->GetName());TArray<TSharedPtr<FJsonValue>> Actions;Actions.Add(MakeShared<FJsonValueObject>(Action));Root->SetArrayField(TEXT("actions"),Actions);return SerializeObject(Root);
}
static FString GraphBoundaryContract(bool bCollapsed)
{
    return bCollapsed?TEXT(R"json({"schema":"N2C_EXPECTED_CONTRACT_V1","required_nodes":[{"node_class":"K2Node_Composite","semantic_contains":"composite_bound="},{"node_class":"K2Node_Tunnel","semantic_contains":"tunnel=0:1"},{"node_class":"K2Node_Tunnel","semantic_contains":"tunnel=1:0"}]})json"):TEXT(R"json({"schema":"N2C_EXPECTED_CONTRACT_V1","required_nodes":[{"node_class":"K2Node_Tunnel","semantic_contains":"tunnel=0:1"},{"node_class":"K2Node_Tunnel","semantic_contains":"tunnel=1:0"}]})json");
}
static FString MacroPatch(UBlueprint* Blueprint, UEdGraph* MacroGraph, ECase Case, bool& bOutExportIdentity)
{
    bOutExportIdentity = false; UEdGraph* TargetGraph = Blueprint && Blueprint->UbergraphPages.Num() ? Blueprint->UbergraphPages[0] : nullptr; if (!MacroGraph || !TargetGraph) return FString();
    FGraphNodeCreator<UK2Node_MacroInstance> Creator(*TargetGraph); UK2Node_MacroInstance* Instance = Creator.CreateNode(); Instance->SetMacroGraph(MacroGraph); Creator.Finalize(); Instance->ReconstructNode();
    if (Case == ECase::StandardWildcardContainer || Case == ECase::ExternalWildcardContainer)
    {
        Instance->ResolvedWildcardType.PinCategory = UEdGraphSchema_K2::PC_Int;
        for (UEdGraphPin* Pin : Instance->Pins) if (Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard) Pin->PinType.PinCategory = UEdGraphSchema_K2::PC_Int;
    }
    FString ExportJson, ExportError; bOutExportIdentity = FN2CAIExport::BuildBlueprintAIJson(Blueprint, ExportJson, ExportError) && ExportJson.Contains(TEXT("\"macro_signature_hash\"")) && ExportJson.Contains(MacroGraph->GetPathName());
    TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>(); Node->SetStringField(TEXT("id"), TEXT("Macro")); Node->SetStringField(TEXT("type"), TEXT("K2Node_MacroInstance")); FN2CMacroReference::AppendIdentity(Node, MacroGraph, Instance);
    if (Case == ECase::StandardMissingOwner) Node->SetStringField(TEXT("macro_owner_path"), TEXT("/Game/N2C_Test/Generated/MissingStandardMacros.MissingStandardMacros"));
    else if (Case == ECase::StandardMissingGraph) Node->SetStringField(TEXT("macro_graph_path"), MacroGraph->GetPathName() + TEXT("_Missing"));
    else if (Case == ECase::StandardSignatureMismatch) Node->SetStringField(TEXT("macro_signature_hash"), TEXT("0000000000000000000000000000000000000000"));
    else if (Case == ECase::ExternalMissingAsset) Node->SetStringField(TEXT("macro_owner_path"), TEXT("/Game/N2C_Test/Generated/MissingExternalOwner.MissingExternalOwner"));
    else if (Case == ECase::ExternalMissingGraph) Node->SetStringField(TEXT("macro_graph_path"), MacroGraph->GetPathName() + TEXT("_Missing"));
    else if (Case == ECase::ExternalSignatureMismatch) Node->SetStringField(TEXT("macro_signature_hash"), TEXT("0000000000000000000000000000000000000000"));
    else if (Case == ECase::ExternalNoMutationReject) Node->SetStringField(TEXT("macro_graph_kind"), TEXT("unsupported"));
    Instance->DestroyNode(); TargetGraph->NotifyGraphChanged(); FKismetEditorUtilities::CompileBlueprint(Blueprint); N2CVerificationTests_Private::SaveAsset(Blueprint);
    TSharedPtr<FJsonObject> Action = MakeShared<FJsonObject>(); Action->SetStringField(TEXT("type"), TEXT("patch_graph")); Action->SetStringField(TEXT("graph_name"), TEXT("EventGraph")); TArray<TSharedPtr<FJsonValue>> Nodes; Nodes.Add(MakeShared<FJsonValueObject>(Node)); TArray<TSharedPtr<FJsonValue>> DataEdges;
    if (Case == ECase::StandardWildcardContainer || Case == ECase::ExternalWildcardContainer) { TSharedPtr<FJsonObject> MakeArray = MakeShared<FJsonObject>(); MakeArray->SetStringField(TEXT("id"),TEXT("TypedArray")); MakeArray->SetStringField(TEXT("type"),TEXT("K2Node_MakeArray")); MakeArray->SetNumberField(TEXT("input_count"),1); TSharedPtr<FJsonObject> ValueType=MakeShared<FJsonObject>();ValueType->SetStringField(TEXT("type"),TEXT("int"));MakeArray->SetObjectField(TEXT("value_pin_type"),ValueType);Nodes.Add(MakeShared<FJsonValueObject>(MakeArray));TSharedPtr<FJsonObject> Edge=MakeShared<FJsonObject>();Edge->SetStringField(TEXT("from"),TEXT("TypedArray"));Edge->SetStringField(TEXT("from_pin"),TEXT("Array"));Edge->SetStringField(TEXT("to"),TEXT("Macro"));Edge->SetStringField(TEXT("to_pin"),TEXT("Array"));DataEdges.Add(MakeShared<FJsonValueObject>(Edge)); }
    Action->SetArrayField(TEXT("nodes"), Nodes); Action->SetArrayField(TEXT("exec_edges"), TArray<TSharedPtr<FJsonValue>>()); Action->SetArrayField(TEXT("data_edges"), DataEdges);
    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>(); Root->SetStringField(TEXT("schema"), TEXT("N2C_PATCH_V1")); Root->SetStringField(TEXT("target_blueprint"), Blueprint->GetName()); TArray<TSharedPtr<FJsonValue>> Actions; Actions.Add(MakeShared<FJsonValueObject>(Action)); Root->SetArrayField(TEXT("actions"), Actions); return SerializeObject(Root);
}
static FString MacroContract(const FString& MacroName)
{
    UEdGraph* Graph = StandardMacroGraph(MacroName); const FString Semantic = Graph ? Graph->GetPathName() : MacroName; return FString::Printf(TEXT(R"json({"schema":"N2C_EXPECTED_CONTRACT_V1","required_nodes":[{"node_class":"K2Node_MacroInstance","semantic_contains":"%s"}]})json"), *Semantic);
}static UBlueprint* CreateExternalMacroOwner(const FString& RunId, bool bNormalBlueprint, bool bWildcard, FString& OutPath, UEdGraph*& OutGraph)
{
    const FString Name = FString(bNormalBlueprint ? TEXT("BP_N2C_MacroOwner_") : TEXT("BPL_N2C_MacroOwner_")) + RunId;
    const FString PackageName = TEXT("/Game/N2C_Test/Generated/") + Name; UPackage* Package = CreatePackage(*PackageName);
    UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(bNormalBlueprint ? AActor::StaticClass() : UObject::StaticClass(), Package, *Name, bNormalBlueprint ? BPTYPE_Normal : BPTYPE_MacroLibrary, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(), TEXT("N2CP0ExternalMacro"));
    if (!Blueprint) return nullptr; FAssetRegistryModule::AssetCreated(Blueprint);
    OutGraph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, TEXT("N2C_ExternalMacro"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass()); FBlueprintEditorUtils::AddMacroGraph(Blueprint, OutGraph, true, nullptr);
    UK2Node_Tunnel* Entry = nullptr; UK2Node_Tunnel* Exit = nullptr; for (UEdGraphNode* Node : OutGraph->Nodes) if (UK2Node_Tunnel* Tunnel = Cast<UK2Node_Tunnel>(Node)) { if (Tunnel->bCanHaveOutputs) Entry = Tunnel; if (Tunnel->bCanHaveInputs) Exit = Tunnel; }
    if (!Entry || !Exit) return nullptr; FEdGraphPinType ExecType; ExecType.PinCategory = UEdGraphSchema_K2::PC_Exec; FEdGraphPinType DataType; DataType.PinCategory = bWildcard ? UEdGraphSchema_K2::PC_Wildcard : UEdGraphSchema_K2::PC_Int;
    if (!Entry->FindPin(TEXT("Exec"))) Entry->CreateUserDefinedPin(TEXT("Exec"), ExecType, EGPD_Output); if (!Exit->FindPin(TEXT("Then"))) Exit->CreateUserDefinedPin(TEXT("Then"), ExecType, EGPD_Input);
    if (bWildcard) { FEdGraphPinType ArrayType = DataType; ArrayType.ContainerType = EPinContainerType::Array; Entry->CreateUserDefinedPin(TEXT("Array"), ArrayType, EGPD_Output); Exit->CreateUserDefinedPin(TEXT("Array Element"), DataType, EGPD_Input); }
    else { Entry->CreateUserDefinedPin(TEXT("Value"), DataType, EGPD_Output); Exit->CreateUserDefinedPin(TEXT("Result"), DataType, EGPD_Input); }
    const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>(); Schema->TryCreateConnection(Entry->FindPin(TEXT("Exec")), Exit->FindPin(TEXT("Then"))); if (!bWildcard) Schema->TryCreateConnection(Entry->FindPin(TEXT("Value")), Exit->FindPin(TEXT("Result")));
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint); FKismetEditorUtilities::CompileBlueprint(Blueprint); if (Blueprint->Status == BS_Error || !N2CVerificationTests_Private::SaveAsset(Blueprint)) return nullptr; OutPath = PackageName + TEXT(".") + Name; return Blueprint;
}
static bool CleanupGeneratedAsset(const FString& ObjectPath)
{
    if (!ObjectPath.StartsWith(TEXT("/Game/N2C_Test/Generated/"))) return false; UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *ObjectPath); UPackage* Package = Blueprint ? Blueprint->GetOutermost() : nullptr;
    if (Blueprint) { FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().AssetDeleted(Blueprint); if (GEditor) { GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->CloseAllEditorsForAsset(Blueprint); GEditor->ResetTransaction(NSLOCTEXT("NodeToCode","P0ExternalMacroCleanup","P0 external macro cleanup")); } }
    Blueprint = nullptr; if (Package) { Package->SetDirtyFlag(false); TArray<UPackage*> Packages; Packages.Add(Package); FText Error; if (!UPackageTools::UnloadPackages(Packages, Error)) return false; } Package = nullptr; CollectGarbage(RF_NoFlags);
    FString PackageName = ObjectPath; int32 Dot = INDEX_NONE; if (PackageName.FindChar(TEXT('.'), Dot)) PackageName = PackageName.Left(Dot); const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension()); return !IFileManager::Get().FileExists(*Filename) || IFileManager::Get().Delete(*Filename, false, true, true);
}
static FString MacroContractForGraph(UEdGraph* Graph) { return FString::Printf(TEXT(R"json({"schema":"N2C_EXPECTED_CONTRACT_V1","required_nodes":[{"node_class":"K2Node_MacroInstance","semantic_contains":"%s"}]})json"), Graph ? *Graph->GetPathName() : TEXT("missing")); }static bool Run(FAutomationTestBase& Test, ECase Case, const TCHAR* CaseName)
{
    const FString RunId = FString::Printf(TEXT("%s_%lld"), CaseName, FDateTime::UtcNow().GetTicks()); FString AssetPath; UBlueprint* Blueprint = CreateFixture(RunId, AssetPath);
    if (!Blueprint) { Test.AddError(FString::Printf(TEXT("N2C_P0_CASE|case=%s|result=FAIL|reason=fixture_create"), CaseName)); return false; }
    const FString Target = Blueprint->GetName(); FString SpecializedPatch, SpecializedContract; bool bExportIdentityOk = true;
    const bool bStandard = Case == ECase::StandardSimple || Case == ECase::StandardLoop || Case == ECase::StandardWildcardContainer || Case == ECase::StandardMissingOwner || Case == ECase::StandardMissingGraph || Case == ECase::StandardSignatureMismatch;
    if (bStandard) { const FString MacroName = Case == ECase::StandardSimple || Case == ECase::StandardMissingOwner || Case == ECase::StandardMissingGraph || Case == ECase::StandardSignatureMismatch ? TEXT("DoOnce") : Case == ECase::StandardLoop ? TEXT("ForLoop") : TEXT("ForEachLoop"); SpecializedPatch = MacroPatch(Blueprint, StandardMacroGraph(MacroName), Case, bExportIdentityOk); SpecializedContract = MacroContract(MacroName); }
    FString ExternalOwnerPath; const bool bExternal = Case == ECase::ExternalProjectLibrary || Case == ECase::ExternalProjectBlueprint || Case == ECase::ExternalWildcardContainer || Case == ECase::ExternalMissingAsset || Case == ECase::ExternalMissingGraph || Case == ECase::ExternalSignatureMismatch || Case == ECase::ExternalNoMutationReject;
    if (bExternal) { UEdGraph* ExternalGraph = nullptr; UBlueprint* ExternalOwner = CreateExternalMacroOwner(RunId, Case == ECase::ExternalProjectBlueprint, Case == ECase::ExternalWildcardContainer, ExternalOwnerPath, ExternalGraph); if (!ExternalOwner || !ExternalGraph) bExportIdentityOk = false; else { SpecializedPatch = MacroPatch(Blueprint, ExternalGraph, Case, bExportIdentityOk); SpecializedContract = MacroContractForGraph(ExternalGraph); } ExternalGraph = nullptr; ExternalOwner = nullptr; }
    const bool bGraphBoundary = Case == ECase::GraphMacroTunnel || Case == ECase::GraphMacroMultiplePins || Case == ECase::GraphCollapsedTunnel || Case == ECase::GraphComposite;
    if (bGraphBoundary) { const bool bCollapsed = Case == ECase::GraphCollapsedTunnel || Case == ECase::GraphComposite; SpecializedPatch = GraphBoundaryPatch(Blueprint,bCollapsed,Case == ECase::GraphMacroMultiplePins); SpecializedContract = GraphBoundaryContract(bCollapsed); }
    Blueprint = nullptr; CollectGarbage(RF_NoFlags);
    FN2CRoundTripVerificationRequest Q; Q.AssetPath = AssetPath; Q.GeneratedFixturePath = AssetPath; Q.RunId = RunId; Q.PatchIdentity = CaseName; Q.bAutomationOnly = true; Q.bCleanupGeneratedFixture = true; Q.FreshProcessTimeoutSeconds = 120; Q.PatchJson = SuccessPatch(Target); Q.ExpectedContractJson = SuccessContract();
    bool ExpectedPass = true; FString ExpectedCode;
    if (Case == ECase::ImpureFunction) { Q.PatchJson = ImpureFunctionPatch(Target); Q.ExpectedContractJson = ImpureFunctionContract(); }
    else if (Case == ECase::PureFunction) { Q.PatchJson = PurePatch(Target); Q.ExpectedContractJson = PureContract(); }
    else if (Case == ECase::MultipleResults) { Q.PatchJson = MultipleResultsPatch(Target); Q.ExpectedContractJson = MultipleResultsContract(); }
    else if (Case == ECase::Containers) { Q.PatchJson = ContainerPatch(Target); Q.ExpectedContractJson = ContainerContract(); }
    else if (Case == ECase::CompileFailure) { Q.bForceCompileAfterApplyFailure = true; ExpectedPass = false; ExpectedCode = TEXT("compile_after_apply_failed"); }
    else if (Case == ECase::SaveFailure) { Q.bForceSaveFailure = true; ExpectedPass = false; ExpectedCode = TEXT("save_failed"); }
    else if (Case == ECase::MissingArtifact) { Q.bForceMissingChildResult = true; ExpectedPass = false; ExpectedCode = TEXT("verification_artifact_missing"); }
    else if (Case == ECase::Timeout) { Q.bDelayChildForTimeout = true; Q.FreshProcessTimeoutSeconds = 1; ExpectedPass = false; ExpectedCode = TEXT("fresh_process_timeout"); }
    else if (Case == ECase::ReloadFailure) { Q.bForceChildReloadFailure = true; ExpectedPass = false; ExpectedCode = TEXT("reload_failed"); }
    else if (Case == ECase::StructuralMismatch) { Q.bForceStructuralMismatch = true; ExpectedPass = false; ExpectedCode = TEXT("persistence_compare_failed"); }
    else if (Case == ECase::MalformedChildResult) { Q.bForceMalformedChildResult = true; ExpectedPass = false; ExpectedCode = TEXT("child_result_invalid"); }
    else if (Case == ECase::ChildIdentityMismatch) { Q.bForceChildIdentityMismatch = true; ExpectedPass = false; ExpectedCode = TEXT("child_identity_mismatch"); }
    else if (Case == ECase::RejectNoMutation) { Q.PatchJson = FString::Printf(TEXT(R"json({"schema":"N2C_PATCH_V1","target_blueprint":"%s","actions":[{"type":"patch_graph","graph_name":"EventGraph","nodes":[{"id":"Macro","type":"K2Node_MacroInstance"}],"exec_edges":[],"data_edges":[]}]})json"), *Target); Q.ExpectedContractJson = TEXT("{}"); ExpectedPass = false; ExpectedCode = TEXT("preflight_blocked"); }
    else if (bStandard) { Q.PatchJson = SpecializedPatch; Q.ExpectedContractJson = SpecializedContract; if (Case == ECase::StandardMissingOwner) { ExpectedPass = false; ExpectedCode = TEXT("macro_owner_missing"); } else if (Case == ECase::StandardMissingGraph) { ExpectedPass = false; ExpectedCode = TEXT("macro_graph_missing"); } else if (Case == ECase::StandardSignatureMismatch) { ExpectedPass = false; ExpectedCode = TEXT("macro_signature_mismatch"); } }
    else if (bExternal) { Q.PatchJson = SpecializedPatch; Q.ExpectedContractJson = SpecializedContract; if (Case == ECase::ExternalProjectBlueprint) { ExpectedPass = false; ExpectedCode = TEXT("macro_reference_unsupported"); } else if (Case == ECase::ExternalMissingAsset) { ExpectedPass = false; ExpectedCode = TEXT("macro_owner_missing"); } else if (Case == ECase::ExternalMissingGraph) { ExpectedPass = false; ExpectedCode = TEXT("macro_graph_missing"); } else if (Case == ECase::ExternalSignatureMismatch) { ExpectedPass = false; ExpectedCode = TEXT("macro_signature_mismatch"); } else if (Case == ECase::ExternalNoMutationReject) { ExpectedPass = false; ExpectedCode = TEXT("macro_reference_unsupported"); } }
    else if (bGraphBoundary) { Q.PatchJson = SpecializedPatch; Q.ExpectedContractJson = SpecializedContract; }
    FN2CRoundTripVerificationResult Result; const bool Actual = FN2CRoundTripVerification::RunParent(Q, Result);
    const bool VerdictOk = Actual == ExpectedPass && Result.bPassed == ExpectedPass && (ExpectedPass ? Result.ErrorCode.IsEmpty() && Result.FailedStage.IsEmpty() : Result.ErrorCode == ExpectedCode && !Result.FailedStage.IsEmpty());
    const bool DependencyCleanupOk = ExternalOwnerPath.IsEmpty() || CleanupGeneratedAsset(ExternalOwnerPath); const bool CleanupOk = Result.bCleanupPassed && DependencyCleanupOk; const bool PathsOk = IFileManager::Get().FileExists(*FPaths::Combine(Result.RunDirectory, TEXT("roundtrip_result.json"))) && IFileManager::Get().FileExists(*Result.ManifestPath);
    const bool Ok = VerdictOk && CleanupOk && PathsOk && bExportIdentityOk && !Q.PatchJson.IsEmpty();
    UE_LOG(LogNodeToCode, Display, TEXT("N2C_P0_CASE|case=%s|automation=%s|expected_pipeline=%s|actual_pipeline=%s|failed_stage=%s|code=%s|cleanup=%d|run=%s"), CaseName, Ok ? TEXT("PASS") : TEXT("FAIL"), ExpectedPass ? TEXT("PASS") : TEXT("FAIL"), Result.bPassed ? TEXT("PASS") : TEXT("FAIL"), *Result.FailedStage, *Result.ErrorCode, CleanupOk ? 1 : 0, *Result.RunDirectory);
    if (!Ok) Test.AddError(FString::Printf(TEXT("P0RoundTrip %s mismatch. actual=%d expected=%d stage=%s code=%s cleanup=%d report=%s"), CaseName, Actual ? 1 : 0, ExpectedPass ? 1 : 0, *Result.FailedStage, *Result.ErrorCode, CleanupOk ? 1 : 0, *Result.Report));
    return Ok;
}
}

namespace N2CP0SpecializedTests
{
using namespace N2CVerificationTests_Private;

static bool CleanupGeneratedObject(const FString& ObjectPath)
{
    if (!ObjectPath.StartsWith(TEXT("/Game/N2C_Test/Generated/"))) return false;
    UObject* Object = LoadObject<UObject>(nullptr, *ObjectPath);
    UPackage* Package = Object ? Object->GetOutermost() : nullptr;
    if (Object) FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().AssetDeleted(Object);
    Object = nullptr;
    if (Package)
    {
        Package->SetDirtyFlag(false);
        TArray<UPackage*> Packages; Packages.Add(Package); FText Error;
        if (!UPackageTools::UnloadPackages(Packages, Error)) return false;
    }
    Package = nullptr; CollectGarbage(RF_NoFlags);
    FString PackageName = ObjectPath; int32 Dot = INDEX_NONE; if (PackageName.FindChar(TEXT('.'), Dot)) PackageName = PackageName.Left(Dot);
    const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
    return !IFileManager::Get().FileExists(*Filename) || IFileManager::Get().Delete(*Filename, false, true, true);
}

static UUserDefinedStruct* CreateStruct(const FString& RunId, FString& OutPath)
{
    const FString Name = TEXT("ST_") + RunId;
    const FString PackageName = TEXT("/Game/N2C_Test/Generated/") + Name;
    UPackage* Package = CreatePackage(*PackageName);
    UUserDefinedStruct* Struct = FStructureEditorUtils::CreateUserDefinedStruct(Package, FName(*Name), RF_Public | RF_Standalone | RF_Transactional);
    if (!Struct) return nullptr;
    FAssetRegistryModule::AssetCreated(Struct);
    TArray<FStructVariableDescription>& Variables = FStructureEditorUtils::GetVarDesc(Struct);
    if (Variables.Num() > 0) FStructureEditorUtils::RenameVariable(Struct, Variables[0].VarGuid, TEXT("FieldBool"));
    FEdGraphPinType IntType; IntType.PinCategory = UEdGraphSchema_K2::PC_Int;
    if (FStructureEditorUtils::AddVariable(Struct, IntType))
    {
        TArray<FStructVariableDescription>& Updated = FStructureEditorUtils::GetVarDesc(Struct);
        FStructureEditorUtils::RenameVariable(Struct, Updated.Last().VarGuid, TEXT("FieldInt"));
    }
    FEdGraphPinType StringType; StringType.PinCategory = UEdGraphSchema_K2::PC_String;
    if (FStructureEditorUtils::AddVariable(Struct, StringType))
    {
        TArray<FStructVariableDescription>& Updated = FStructureEditorUtils::GetVarDesc(Struct);
        FStructureEditorUtils::RenameVariable(Struct, Updated.Last().VarGuid, TEXT("FieldString"));
    }
    FStructureEditorUtils::CompileStructure(Struct);
    OutPath = PackageName + TEXT(".") + Name;
    return SaveAsset(Struct) ? Struct : nullptr;
}

static UDataTable* CreateTable(const FString& RunId, UUserDefinedStruct* RowStruct, FString& OutPath)
{
    if (!RowStruct) return nullptr;
    const FString Name = TEXT("DT_") + RunId;
    const FString PackageName = TEXT("/Game/N2C_Test/Generated/") + Name;
    UPackage* Package = CreatePackage(*PackageName);
    UDataTable* Table = NewObject<UDataTable>(Package, FName(*Name), RF_Public | RF_Standalone | RF_Transactional);
    if (!Table) return nullptr;
    FAssetRegistryModule::AssetCreated(Table);
    Table->RowStruct = RowStruct;
    Table->CreateTableFromJSONString(TEXT("[{\"Name\":\"RowA\",\"FieldBool\":true,\"FieldInt\":7,\"FieldString\":\"P0\"}]"));
    OutPath = PackageName + TEXT(".") + Name;
    return SaveAsset(Table) ? Table : nullptr;
}

static bool RunRoundTrip(FAutomationTestBase& Test, const FString& Label, const FString& Patch, const FString& Contract,
    bool bExpectedPass, const FString& ExpectedCode, const TArray<FString>& Dependencies)
{
    const FString RunId = FString::Printf(TEXT("%s_%lld"), *Label, FDateTime::UtcNow().GetTicks());
    FString AssetPath; UBlueprint* Blueprint = N2CP0RoundTripTests::CreateFixture(RunId, AssetPath);
    if (!Blueprint) { Test.AddError(TEXT("specialized fixture Blueprint creation failed")); return false; }
    Blueprint = nullptr; CollectGarbage(RF_NoFlags);
    FN2CRoundTripVerificationRequest Q; Q.AssetPath = AssetPath; Q.GeneratedFixturePath = AssetPath; Q.RunId = RunId;
    Q.PatchIdentity = Label; Q.bAutomationOnly = true; Q.bCleanupGeneratedFixture = true; Q.FreshProcessTimeoutSeconds = 120;
    Q.PatchJson = Patch; Q.ExpectedContractJson = Contract;
    FN2CRoundTripVerificationResult Result; const bool Actual = FN2CRoundTripVerification::RunParent(Q, Result);
    bool DependenciesOk = true; for (int32 Index = Dependencies.Num() - 1; Index >= 0; --Index) DependenciesOk &= CleanupGeneratedObject(Dependencies[Index]);
    const bool Verdict = Actual == bExpectedPass && Result.bPassed == bExpectedPass &&
        (bExpectedPass ? Result.ErrorCode.IsEmpty() && Result.FailedStage.IsEmpty() :
         Result.FailedStage == TEXT("Preflight") && Result.ErrorCode == ExpectedCode);
    const bool Ok = Verdict && Result.bCleanupPassed && DependenciesOk;
    UE_LOG(LogNodeToCode, Display, TEXT("N2C_P0_SPECIALIZED|case=%s|result=%s|expected=%s|actual=%s|stage=%s|code=%s|cleanup=%d|run=%s"),
        *Label, Ok ? TEXT("PASS") : TEXT("FAIL"), bExpectedPass ? TEXT("PASS") : TEXT("FAIL"), Result.bPassed ? TEXT("PASS") : TEXT("FAIL"),
        *Result.FailedStage, *Result.ErrorCode, Result.bCleanupPassed && DependenciesOk ? 1 : 0, *Result.RunDirectory);
    if (!Ok) Test.AddError(FString::Printf(TEXT("%s failed: stage=%s code=%s report=%s"), *Label, *Result.FailedStage, *Result.ErrorCode, *Result.Report));
    return Ok;
}

static bool RunStruct(FAutomationTestBase& Test, bool bSetFields)
{
    const FString Label = bSetFields ? TEXT("Struct_ConnectedSetFields") : TEXT("Struct_LegacyMakeBreakAliases");
    const FString DependencyRunId = FString::Printf(TEXT("%s_Dependency_%lld"), *Label, FDateTime::UtcNow().GetTicks());
    FString StructPath;
    UUserDefinedStruct* Struct = CreateStruct(DependencyRunId, StructPath);
    if (!Struct)
    {
        Test.AddError(TEXT("generated struct creation failed"));
        return false;
    }
    const FString StructPinName = Struct->GetFName().ToString();

    const FString FixtureRunId = FString::Printf(TEXT("%s_%lld"), *Label, FDateTime::UtcNow().GetTicks());
    FString AssetPath;
    UBlueprint* Blueprint = N2CP0RoundTripTests::CreateFixture(FixtureRunId, AssetPath);
    if (!Blueprint)
    {
        CleanupGeneratedObject(StructPath);
        Test.AddError(TEXT("struct consumer creation failed"));
        return false;
    }
    const FString TargetName = Blueprint->GetName();
    Blueprint = nullptr;
    CollectGarbage(RF_NoFlags);

    FString Patch;
    FString Contract;
    if (bSetFields)
    {
        // Canonical UE4.27 identities: MakeStruct/BreakStruct use the concrete
        // StructType FName, while SetFields uses StructRef/StructOut.
        Patch = FString::Printf(TEXT(R"json({"schema":"N2C_PATCH_V1","target_blueprint":"%s","actions":[{"type":"patch_graph","graph_name":"EventGraph","nodes":[{"id":"Start","type":"K2Node_CustomEvent","event_name":"N2C_StructConnectedStart"},{"id":"Make","type":"K2Node_MakeStruct","struct_path":"%s","member_pin_identity":[]},{"id":"Set","type":"K2Node_SetFieldsInStruct","struct_path":"%s","member_pin_identity":[],"show_all_fields":true},{"id":"Break","type":"K2Node_BreakStruct","struct_path":"%s","member_pin_identity":[]}],"exec_edges":[{"from_node_id":"Start","from_pin":"then","to_node_id":"Set","to_pin":"execute"}],"data_edges":[{"from_node_id":"Make","from_pin":"%s","to_node_id":"Set","to_pin":"StructRef"},{"from_node_id":"Set","from_pin":"StructOut","to_node_id":"Break","to_pin":"%s"}]}]})json"),
            *TargetName, *StructPath, *StructPath, *StructPath, *StructPinName, *StructPinName);
        Contract = TEXT(R"json({"schema":"N2C_EXPECTED_CONTRACT_V1","required_nodes":[{"node_class":"K2Node_MakeStruct","semantic_contains":""},{"node_class":"K2Node_SetFieldsInStruct","semantic_contains":""},{"node_class":"K2Node_BreakStruct","semantic_contains":""}]})json");
    }
    else
    {
        // Preserve compatibility with older generated patches while the
        // authoritative authoring guide requires the exported struct pin name.
        Patch = FString::Printf(TEXT(R"json({"schema":"N2C_PATCH_V1","target_blueprint":"%s","actions":[{"type":"patch_graph","graph_name":"EventGraph","nodes":[{"id":"Make","type":"K2Node_MakeStruct","struct_path":"%s","member_pin_identity":[]},{"id":"Break","type":"K2Node_BreakStruct","struct_path":"%s","member_pin_identity":[]}],"exec_edges":[],"data_edges":[{"from_node_id":"Make","from_pin":"ReturnValue","to_node_id":"Break","to_pin":"Struct"}]}]})json"),
            *TargetName, *StructPath, *StructPath);
        Contract = TEXT(R"json({"schema":"N2C_EXPECTED_CONTRACT_V1","required_nodes":[{"node_class":"K2Node_MakeStruct","semantic_contains":""},{"node_class":"K2Node_BreakStruct","semantic_contains":""}]})json");
    }

    FN2CRoundTripVerificationRequest Q;
    Q.AssetPath = AssetPath;
    Q.GeneratedFixturePath = AssetPath;
    Q.RunId = FixtureRunId;
    Q.PatchIdentity = Label;
    Q.bAutomationOnly = true;
    Q.bCleanupGeneratedFixture = true;
    Q.FreshProcessTimeoutSeconds = 120;
    Q.PatchJson = Patch;
    Q.ExpectedContractJson = Contract;

    FN2CRoundTripVerificationResult Result;
    const bool Actual = FN2CRoundTripVerification::RunParent(Q, Result);
    const bool DependencyOk = CleanupGeneratedObject(StructPath);
    const bool Ok = Actual && Result.bPassed && Result.bCleanupPassed && DependencyOk && Result.ErrorCode.IsEmpty();
    UE_LOG(LogNodeToCode, Display,
        TEXT("N2C_P0_SPECIALIZED|case=%s|result=%s|stage=%s|code=%s|cleanup=%d|run=%s|struct_pin=%s"),
        *Label, Ok ? TEXT("PASS") : TEXT("FAIL"), *Result.FailedStage, *Result.ErrorCode,
        Result.bCleanupPassed && DependencyOk ? 1 : 0, *Result.RunDirectory, *StructPinName);
    if (!Ok)
    {
        Test.AddError(FString::Printf(TEXT("%s failed: %s"), *Label, *Result.Report));
    }
    return Ok;
}

static bool RunEnum(FAutomationTestBase& Test, bool bUtilities)
{
    const FString Label = bUtilities ? TEXT("Enum_UtilityNodes") : TEXT("Enum_CompareSwitch");
    const FString RunId = FString::Printf(TEXT("%s_%lld"), *Label, FDateTime::UtcNow().GetTicks());
    FString AssetPath; UBlueprint* Blueprint = N2CP0RoundTripTests::CreateFixture(RunId, AssetPath);
    if (!Blueprint) return false;
    const FString Target = Blueprint->GetName();
    UEnum* Enum = LoadObject<UEnum>(nullptr, TEXT("/Script/Engine.EInputEvent"));
    const FString First = Enum ? Enum->GetNameStringByIndex(0) : TEXT("IE_Pressed");
    const FString Nodes = bUtilities
        ? FString::Printf(TEXT(R"json([{"id":"Name","type":"K2Node_GetEnumeratorNameAsString","enum_path":"/Script/Engine.EInputEvent"},{"id":"Each","type":"K2Node_ForEachElementInEnum","enum_path":"/Script/Engine.EInputEvent"},{"id":"Cast","type":"K2Node_CastByteToEnum","enum_path":"/Script/Engine.EInputEvent"},{"id":"Literal","type":"K2Node_EnumLiteral","enum_path":"/Script/Engine.EInputEvent","enum_value":"%s"}])json"), *First)
        : TEXT(R"json([{"id":"Equal","type":"K2Node_EnumEquality","enum_path":"/Script/Engine.EInputEvent"},{"id":"NotEqual","type":"K2Node_EnumInequality","enum_path":"/Script/Engine.EInputEvent"},{"id":"Switch","type":"K2Node_SwitchEnum","enum_path":"/Script/Engine.EInputEvent","enum_cases":[]}])json");
    const FString DataEdges = bUtilities ? TEXT(R"json([{"from_node_id":"Literal","from_pin":"ReturnValue","to_node_id":"Name","to_pin":"Enumerator"}])json") : TEXT("[]");
    const FString Patch = FString::Printf(TEXT(R"json({"schema":"N2C_PATCH_V1","target_blueprint":"%s","actions":[{"type":"patch_graph","graph_name":"EventGraph","nodes":%s,"exec_edges":[],"data_edges":%s}]})json"), *Target, *Nodes, *DataEdges);
    const FString Contract = FString::Printf(TEXT(R"json({"schema":"N2C_EXPECTED_CONTRACT_V1","required_nodes":[{"node_class":"%s","semantic_contains":""}]})json"), bUtilities ? TEXT("K2Node_EnumLiteral") : TEXT("K2Node_SwitchEnum"));
    Blueprint=nullptr;CollectGarbage(RF_NoFlags);
    FN2CRoundTripVerificationRequest Q;Q.AssetPath=AssetPath;Q.GeneratedFixturePath=AssetPath;Q.RunId=RunId;Q.PatchIdentity=Label;Q.bAutomationOnly=true;Q.bCleanupGeneratedFixture=true;Q.FreshProcessTimeoutSeconds=120;Q.PatchJson=Patch;Q.ExpectedContractJson=Contract;
    FN2CRoundTripVerificationResult Result;const bool Actual=FN2CRoundTripVerification::RunParent(Q,Result);const bool Ok=Actual&&Result.bPassed&&Result.bCleanupPassed;
    UE_LOG(LogNodeToCode,Display,TEXT("N2C_P0_SPECIALIZED|case=%s|result=%s|stage=%s|code=%s|cleanup=%d|run=%s"),*Label,Ok?TEXT("PASS"):TEXT("FAIL"),*Result.FailedStage,*Result.ErrorCode,Result.bCleanupPassed?1:0,*Result.RunDirectory);
    if(!Ok)Test.AddError(FString::Printf(TEXT("%s failed: %s"),*Label,*Result.Report));return Ok;
}

static bool RunDataTable(FAutomationTestBase& Test, bool bLinked)
{
    const FString Label=bLinked?TEXT("DataTable_LinkedInput"):TEXT("DataTable_GetRow");const FString RunId=FString::Printf(TEXT("%s_%lld"),*Label,FDateTime::UtcNow().GetTicks());
    FString StructPath,TablePath;UUserDefinedStruct* Struct=CreateStruct(RunId,StructPath);UDataTable* Table=CreateTable(RunId,Struct,TablePath);
    if(!Struct||!Table){if(!TablePath.IsEmpty())CleanupGeneratedObject(TablePath);if(!StructPath.IsEmpty())CleanupGeneratedObject(StructPath);Test.AddError(TEXT("DataTable prerequisites failed"));return false;}
    FString AssetPath;UBlueprint* Blueprint=N2CP0RoundTripTests::CreateFixture(RunId,AssetPath);if(!Blueprint)return false;const FString Target=Blueprint->GetName();
    if(bLinked)
    {
        FEdGraphPinType TableType;TableType.PinCategory=UEdGraphSchema_K2::PC_Object;TableType.PinSubCategoryObject=UDataTable::StaticClass();FBlueprintEditorUtils::AddMemberVariable(Blueprint,TEXT("N2C_P0_DataTable"),TableType);
        FEdGraphPinType RowType;RowType.PinCategory=UEdGraphSchema_K2::PC_Struct;RowType.PinSubCategoryObject=Struct;FBlueprintEditorUtils::AddMemberVariable(Blueprint,TEXT("N2C_P0_Row"),RowType);
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);FKismetEditorUtilities::CompileBlueprint(Blueprint);N2CVerificationTests_Private::SaveAsset(Blueprint);
    }
    Blueprint=nullptr;CollectGarbage(RF_NoFlags);
    const FString Patch=bLinked
        ? FString::Printf(TEXT(R"json({"schema":"N2C_PATCH_V1","target_blueprint":"%s","actions":[{"type":"patch_graph","graph_name":"EventGraph","nodes":[{"id":"Table","type":"K2Node_VariableGet","member_name":"N2C_P0_DataTable"},{"id":"GetRow","type":"K2Node_GetDataTableRow","data_table_pin_linked":true,"row_struct_path":"%s","row_name_default":"RowA"},{"id":"SetRow","type":"K2Node_VariableSet","member_name":"N2C_P0_Row"}],"exec_edges":[],"data_edges":[{"from_node_id":"Table","from_pin":"N2C_P0_DataTable","to_node_id":"GetRow","to_pin":"DataTable"},{"from_node_id":"GetRow","from_pin":"Out Row","to_node_id":"SetRow","to_pin":"N2C_P0_Row"}]}]})json"),*Target,*StructPath)
        : FString::Printf(TEXT(R"json({"schema":"N2C_PATCH_V1","target_blueprint":"%s","actions":[{"type":"patch_graph","graph_name":"EventGraph","nodes":[{"id":"GetRow","type":"K2Node_GetDataTableRow","data_table_path":"%s","row_struct_path":"%s","row_name_default":"RowA"}],"exec_edges":[],"data_edges":[]}]})json"),*Target,*TablePath,*StructPath);
    const FString Contract=TEXT(R"json({"schema":"N2C_EXPECTED_CONTRACT_V1","required_nodes":[{"node_class":"K2Node_GetDataTableRow","semantic_contains":"|table="}]})json");
    FN2CRoundTripVerificationRequest Q;Q.AssetPath=AssetPath;Q.GeneratedFixturePath=AssetPath;Q.RunId=RunId;Q.PatchIdentity=Label;Q.bAutomationOnly=true;Q.bCleanupGeneratedFixture=false;Q.FreshProcessTimeoutSeconds=120;Q.PatchJson=Patch;Q.ExpectedContractJson=Contract;
    FN2CRoundTripVerificationResult Result;const bool Actual=FN2CRoundTripVerification::RunParent(Q,Result);
    bool bSchemaDefaultStorage=false;
    UBlueprint* Reloaded=LoadObject<UBlueprint>(nullptr,*AssetPath);
    if(Reloaded&&Reloaded->UbergraphPages.Num()>0)
    {
        for(UEdGraphNode* GraphNode:Reloaded->UbergraphPages[0]->Nodes)
        {
            UK2Node_GetDataTableRow* GetRow=Cast<UK2Node_GetDataTableRow>(GraphNode);if(!GetRow)continue;
            UEdGraphPin* TablePin=GetRow->GetDataTablePin();
            if(!TablePin)continue;
            if(bLinked)
            {
                bSchemaDefaultStorage=TablePin->LinkedTo.Num()>0&&TablePin->DefaultObject==nullptr&&TablePin->DefaultValue.IsEmpty();
            }
            else
            {
                bSchemaDefaultStorage=TablePin->DefaultObject&&TablePin->DefaultObject->GetPathName()==TablePath&&TablePin->DefaultValue.IsEmpty();
            }
            break;
        }
    }
    const bool FixtureCleanup=CleanupGeneratedObject(AssetPath);
    const bool Deps=CleanupGeneratedObject(TablePath)&&CleanupGeneratedObject(StructPath);
    const bool Ok=Actual&&Result.bPassed&&bSchemaDefaultStorage&&FixtureCleanup&&Deps;
    UE_LOG(LogNodeToCode,Display,TEXT("N2C_P0_SPECIALIZED|case=%s|result=%s|stage=%s|code=%s|schema_default_storage=%d|cleanup=%d|run=%s"),*Label,Ok?TEXT("PASS"):TEXT("FAIL"),*Result.FailedStage,*Result.ErrorCode,bSchemaDefaultStorage?1:0,FixtureCleanup&&Deps?1:0,*Result.RunDirectory);if(!Ok)Test.AddError(FString::Printf(TEXT("%s schema-default persistence failed: %s"),*Label,*Result.Report));return Ok;
}

static bool RunReject(FAutomationTestBase& Test, bool bSignature)
{
    const FString Label=bSignature?TEXT("Specialized_SignatureMismatch"):TEXT("Specialized_MissingIdentity");
    const FString RunId=FString::Printf(TEXT("%s_%lld"),*Label,FDateTime::UtcNow().GetTicks());FString AssetPath;UBlueprint* Blueprint=N2CP0RoundTripTests::CreateFixture(RunId,AssetPath);if(!Blueprint)return false;const FString Target=Blueprint->GetName();Blueprint=nullptr;CollectGarbage(RF_NoFlags);
    const FString Patch=bSignature?FString::Printf(TEXT(R"json({"schema":"N2C_PATCH_V1","target_blueprint":"%s","actions":[{"type":"patch_graph","graph_name":"EventGraph","nodes":[{"id":"Switch","type":"K2Node_SwitchEnum","enum_path":"/Script/Engine.EInputEvent","enum_cases":["N2C_Stale_Enumerator"]}],"exec_edges":[],"data_edges":[]}]})json"),*Target):FString::Printf(TEXT(R"json({"schema":"N2C_PATCH_V1","target_blueprint":"%s","actions":[{"type":"patch_graph","graph_name":"EventGraph","nodes":[{"id":"Make","type":"K2Node_MakeStruct"}],"exec_edges":[],"data_edges":[]}]})json"),*Target);
    FN2CRoundTripVerificationRequest Q;Q.AssetPath=AssetPath;Q.GeneratedFixturePath=AssetPath;Q.RunId=RunId;Q.PatchIdentity=Label;Q.bAutomationOnly=true;Q.bCleanupGeneratedFixture=true;Q.PatchJson=Patch;Q.ExpectedContractJson=TEXT("{}");
    FN2CRoundTripVerificationResult Result;const bool Actual=FN2CRoundTripVerification::RunParent(Q,Result);const FString Expected=bSignature?TEXT("enum_case_mismatch"):TEXT("struct_identity_missing");const bool Ok=!Actual&&!Result.bPassed&&Result.FailedStage==TEXT("Preflight")&&Result.ErrorCode==Expected&&Result.bCleanupPassed;
    UE_LOG(LogNodeToCode,Display,TEXT("N2C_P0_SPECIALIZED|case=%s|result=%s|stage=%s|code=%s|cleanup=%d|run=%s"),*Label,Ok?TEXT("PASS"):TEXT("FAIL"),*Result.FailedStage,*Result.ErrorCode,Result.bCleanupPassed?1:0,*Result.RunDirectory);if(!Ok)Test.AddError(Result.Report);return Ok;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CP0StructMakeBreak,"NodeToCode.Verification.P0Specialized.Struct.MakeBreak",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FN2CP0StructMakeBreak::RunTest(const FString&){return N2CP0SpecializedTests::RunStruct(*this,false);}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CP0StructSetFields,"NodeToCode.Verification.P0Specialized.Struct.SetFields",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FN2CP0StructSetFields::RunTest(const FString&){return N2CP0SpecializedTests::RunStruct(*this,true);}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CP0EnumCompareSwitch,"NodeToCode.Verification.P0Specialized.Enum.CompareSwitch",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FN2CP0EnumCompareSwitch::RunTest(const FString&){return N2CP0SpecializedTests::RunEnum(*this,false);}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CP0EnumUtility,"NodeToCode.Verification.P0Specialized.Enum.UtilityNodes",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FN2CP0EnumUtility::RunTest(const FString&){return N2CP0SpecializedTests::RunEnum(*this,true);}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CP0DataTableGetRow,"NodeToCode.Verification.P0Specialized.DataTable.GetRow",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FN2CP0DataTableGetRow::RunTest(const FString&){return N2CP0SpecializedTests::RunDataTable(*this,false);}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CP0DataTableLinked,"NodeToCode.Verification.P0Specialized.DataTableLinkedInput",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FN2CP0DataTableLinked::RunTest(const FString&){return N2CP0SpecializedTests::RunDataTable(*this,true);}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CP0SpecializedMissing,"NodeToCode.Verification.P0Specialized.MissingIdentity",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FN2CP0SpecializedMissing::RunTest(const FString&){return N2CP0SpecializedTests::RunReject(*this,false);}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CP0SpecializedMismatch,"NodeToCode.Verification.P0Specialized.SignatureMismatch",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FN2CP0SpecializedMismatch::RunTest(const FString&){return N2CP0SpecializedTests::RunReject(*this,true);}

namespace N2CP0GuardTests
{
static bool Run(FAutomationTestBase& Test, const FString& Label, const FString& DeclarationField, const FString& Declaration, const FString& ExpectedCode)
{
    const FString RunId=FString::Printf(TEXT("%s_%lld"),*Label,FDateTime::UtcNow().GetTicks());FString AssetPath;
    UBlueprint* Blueprint=N2CP0RoundTripTests::CreateFixture(RunId,AssetPath);if(!Blueprint){Test.AddError(TEXT("guard fixture creation failed"));return false;}
    const FString Target=Blueprint->GetName();Blueprint=nullptr;CollectGarbage(RF_NoFlags);
    const FString Patch=FString::Printf(TEXT(R"json({"schema":"N2C_PATCH_V1","target_blueprint":"%s","actions":[{"type":"add_or_replace_function","function_name":"N2C_Guarded","%s":[%s],"nodes":[{"id":"Entry","type":"Entry"},{"id":"Return","type":"Return"}],"exec_edges":[{"from_node_id":"Entry","from_pin":"then","to_node_id":"Return","to_pin":"execute"}],"data_edges":[]}]})json"),*Target,*DeclarationField,*Declaration);
    FN2CRoundTripVerificationRequest Q;Q.AssetPath=AssetPath;Q.GeneratedFixturePath=AssetPath;Q.RunId=RunId;Q.PatchIdentity=Label;Q.bAutomationOnly=true;Q.bCleanupGeneratedFixture=true;Q.bDeveloperOverride=true;Q.PatchJson=Patch;Q.ExpectedContractJson=TEXT("{}");
    FN2CRoundTripVerificationResult Result;const bool Actual=FN2CRoundTripVerification::RunParent(Q,Result);
    const bool Ok=!Actual&&!Result.bPassed&&Result.FailedStage==TEXT("Preflight")&&Result.ErrorCode==ExpectedCode&&Result.bCleanupPassed;
    UE_LOG(LogNodeToCode,Display,TEXT("N2C_P0_GUARD|case=%s|result=%s|stage=%s|code=%s|cleanup=%d|run=%s"),*Label,Ok?TEXT("PASS"):TEXT("FAIL"),*Result.FailedStage,*Result.ErrorCode,Result.bCleanupPassed?1:0,*Result.RunDirectory);
    if(!Ok)Test.AddError(FString::Printf(TEXT("%s failed: %s"),*Label,*Result.Report));return Ok;
}
}
#define N2C_GUARD_TEST(ClassName,Path,Label,Field,Decl,Code) IMPLEMENT_SIMPLE_AUTOMATION_TEST(ClassName,Path,EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter) bool ClassName::RunTest(const FString&){return N2CP0GuardTests::Run(*this,TEXT(Label),TEXT(Field),TEXT(Decl),TEXT(Code));}
N2C_GUARD_TEST(FN2CP0ParameterDefault,"NodeToCode.Verification.P0Guards.ParameterDefaultReject","ParameterDefaultReject","inputs",R"json({"name":"Amount","category":"int","default_value":"7"})json","unsupported_parameter_default")
N2C_GUARD_TEST(FN2CP0LocalDefault,"NodeToCode.Verification.P0Guards.LocalDefaultReject","LocalDefaultReject","local_variables",R"json({"name":"Counter","category":"int","default_value":"3"})json","unsupported_local_default")
N2C_GUARD_TEST(FN2CP0ParameterFlags,"NodeToCode.Verification.P0Guards.ParameterFlagsReject","ParameterFlagsReject","inputs",R"json({"name":"Value","category":"int","reference":true})json","unsupported_parameter_flags")
N2C_GUARD_TEST(FN2CP0ReferenceDefault,"NodeToCode.Verification.P0Guards.ReferenceDefaultReject","ReferenceDefaultReject","inputs",R"json({"name":"Target","category":"object","subcategory_object":"/Script/Engine.Actor","default_value":"None"})json","unsupported_reference_default")
N2C_GUARD_TEST(FN2CP0NoMutation,"NodeToCode.Verification.P0Guards.NoMutation","NoMutation","local_variables",R"json({"name":"Counter","category":"int","default_value":"3"})json","unsupported_local_default")
#undef N2C_GUARD_TEST

namespace N2CP0CoreUIInputTests
{
enum class EFixtureKind { Actor, Widget, AI, BTTask, BTService, BTDecorator };

static UBlueprint* CreateFixture(const FString& RunId, EFixtureKind Kind, FString& OutPath)
{
    FString Name=TEXT("BP_N2C_")+RunId;UClass* Parent=AActor::StaticClass();UClass* BlueprintClass=UBlueprint::StaticClass();UClass* GeneratedClass=UBlueprintGeneratedClass::StaticClass();
    if(Kind==EFixtureKind::Widget){Parent=UUserWidget::StaticClass();BlueprintClass=UWidgetBlueprint::StaticClass();GeneratedClass=UWidgetBlueprintGeneratedClass::StaticClass();}
    else if(Kind==EFixtureKind::AI){Parent=LoadObject<UClass>(nullptr,TEXT("/Script/AIModule.AIController"));}
    else if(Kind==EFixtureKind::BTTask){Parent=LoadObject<UClass>(nullptr,TEXT("/Script/AIModule.BTTask_BlueprintBase"));}
    else if(Kind==EFixtureKind::BTService){Parent=LoadObject<UClass>(nullptr,TEXT("/Script/AIModule.BTService_BlueprintBase"));}
    else if(Kind==EFixtureKind::BTDecorator){Parent=LoadObject<UClass>(nullptr,TEXT("/Script/AIModule.BTDecorator_BlueprintBase"));}
    if(!Parent)return nullptr;const FString PackageName=TEXT("/Game/N2C_Test/Generated/")+Name;UPackage* Package=CreatePackage(*PackageName);
    UBlueprint* Blueprint=FKismetEditorUtilities::CreateBlueprint(Parent,Package,FName(*Name),BPTYPE_Normal,BlueprintClass,GeneratedClass,TEXT("N2CP0CoreUIInput"));
    if(!Blueprint)return nullptr;FAssetRegistryModule::AssetCreated(Blueprint);
    if(Kind==EFixtureKind::Actor)
    {
        FEdGraphPinType ActorType;ActorType.PinCategory=UEdGraphSchema_K2::PC_Object;ActorType.PinSubCategoryObject=AActor::StaticClass();
        FBlueprintEditorUtils::AddMemberVariable(Blueprint,TEXT("N2C_P0_TargetActor"),ActorType);
    }
    FKismetEditorUtilities::CompileBlueprint(Blueprint);OutPath=PackageName+TEXT(".")+Name;
    return N2CVerificationTests_Private::SaveAsset(Blueprint)?Blueprint:nullptr;
}
static bool Run(FAutomationTestBase& Test,const FString& Label,const FString& Nodes,const FString& ExecEdges,const FString& DataEdges,const FString& RequiredClass,EFixtureKind Kind=EFixtureKind::Actor)
{
    const FString RunId=FString::Printf(TEXT("%s_%lld"),*Label,FDateTime::UtcNow().GetTicks());FString AssetPath;UBlueprint* Blueprint=CreateFixture(RunId,Kind,AssetPath);
    if(!Blueprint){Test.AddError(FString::Printf(TEXT("%s fixture creation failed"),*Label));return false;}const FString Target=Blueprint->GetName();Blueprint=nullptr;CollectGarbage(RF_NoFlags);
    const FString Patch=FString::Printf(TEXT(R"json({"schema":"N2C_PATCH_V1","target_blueprint":"%s","actions":[{"type":"patch_graph","graph_name":"EventGraph","nodes":%s,"exec_edges":%s,"data_edges":%s}]})json"),*Target,*Nodes,*ExecEdges,*DataEdges);
    const FString Contract=FString::Printf(TEXT(R"json({"schema":"N2C_EXPECTED_CONTRACT_V1","required_nodes":[{"node_class":"%s","semantic_contains":""}]})json"),*RequiredClass);
    FN2CRoundTripVerificationRequest Q;Q.AssetPath=AssetPath;Q.GeneratedFixturePath=AssetPath;Q.RunId=RunId;Q.PatchIdentity=Label;Q.bAutomationOnly=true;Q.bCleanupGeneratedFixture=true;Q.FreshProcessTimeoutSeconds=120;Q.PatchJson=Patch;Q.ExpectedContractJson=Contract;
    FN2CRoundTripVerificationResult Result;const bool Actual=FN2CRoundTripVerification::RunParent(Q,Result);const bool Ok=Actual&&Result.bPassed&&Result.bCleanupPassed;
    UE_LOG(LogNodeToCode,Display,TEXT("N2C_P0_CORE_UI|case=%s|result=%s|stage=%s|code=%s|cleanup=%d|run=%s"),*Label,Ok?TEXT("PASS"):TEXT("FAIL"),*Result.FailedStage,*Result.ErrorCode,Result.bCleanupPassed?1:0,*Result.RunDirectory);if(!Ok)Test.AddError(FString::Printf(TEXT("%s failed: %s"),*Label,*Result.Report));return Ok;
}

static bool RunCreateWidgetLinked(FAutomationTestBase& Test)
{
    const FString Label=TEXT("CreateWidgetLinkedClass");const FString RunId=FString::Printf(TEXT("%s_%lld"),*Label,FDateTime::UtcNow().GetTicks());FString AssetPath;UBlueprint* Blueprint=CreateFixture(RunId,EFixtureKind::Actor,AssetPath);
    if(!Blueprint)return false;FEdGraphPinType ClassType;ClassType.PinCategory=UEdGraphSchema_K2::PC_Class;ClassType.PinSubCategoryObject=UUserWidget::StaticClass();FBlueprintEditorUtils::AddMemberVariable(Blueprint,TEXT("N2C_P0_WidgetClass"),ClassType);FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);FKismetEditorUtilities::CompileBlueprint(Blueprint);N2CVerificationTests_Private::SaveAsset(Blueprint);
    const FString Target=Blueprint->GetName();Blueprint=nullptr;CollectGarbage(RF_NoFlags);
    const FString Nodes=TEXT(R"json([{"id":"Class","type":"K2Node_VariableGet","member_name":"N2C_P0_WidgetClass"},{"id":"Widget","type":"K2Node_CreateWidget","class_pin_linked":true,"result_class_path":"/Script/UMG.UserWidget"}])json");
    const FString Data=TEXT(R"json([{"from_node_id":"Class","from_pin":"N2C_P0_WidgetClass","to_node_id":"Widget","to_pin":"Class"}])json");
    const FString Patch=FString::Printf(TEXT(R"json({"schema":"N2C_PATCH_V1","target_blueprint":"%s","actions":[{"type":"patch_graph","graph_name":"EventGraph","nodes":%s,"exec_edges":[],"data_edges":%s}]})json"),*Target,*Nodes,*Data);
    const FString Contract=TEXT(R"json({"schema":"N2C_EXPECTED_CONTRACT_V1","required_nodes":[{"node_class":"K2Node_CreateWidget","semantic_contains":""}]})json");
    FN2CRoundTripVerificationRequest Q;Q.AssetPath=AssetPath;Q.GeneratedFixturePath=AssetPath;Q.RunId=RunId;Q.PatchIdentity=Label;Q.bAutomationOnly=true;Q.bCleanupGeneratedFixture=true;Q.FreshProcessTimeoutSeconds=120;Q.PatchJson=Patch;Q.ExpectedContractJson=Contract;
    FN2CRoundTripVerificationResult Result;const bool Actual=FN2CRoundTripVerification::RunParent(Q,Result);const bool Ok=Actual&&Result.bPassed&&Result.bCleanupPassed;if(!Ok)Test.AddError(Result.Report);return Ok;
}

static bool RunRejectGraph(
    FAutomationTestBase& Test,
    const FString& Label,
    const FString& NodesJson,
    const FString& ExecEdgesJson,
    const FString& DataEdgesJson,
    const FString& ExpectedCode)
{
    const FString RunId=FString::Printf(TEXT("%s_%lld"),*Label,FDateTime::UtcNow().GetTicks());FString AssetPath;UBlueprint* Blueprint=CreateFixture(RunId,EFixtureKind::Actor,AssetPath);if(!Blueprint)return false;const FString Target=Blueprint->GetName();Blueprint=nullptr;CollectGarbage(RF_NoFlags);
    const FString Patch=FString::Printf(TEXT(R"json({"schema":"N2C_PATCH_V1","target_blueprint":"%s","actions":[{"type":"patch_graph","graph_name":"EventGraph","nodes":%s,"exec_edges":%s,"data_edges":%s}]})json"),*Target,*NodesJson,*ExecEdgesJson,*DataEdgesJson);
    FN2CRoundTripVerificationRequest Q;Q.AssetPath=AssetPath;Q.GeneratedFixturePath=AssetPath;Q.RunId=RunId;Q.PatchIdentity=Label;Q.bAutomationOnly=true;Q.bCleanupGeneratedFixture=true;Q.PatchJson=Patch;Q.ExpectedContractJson=TEXT("{}");
    FN2CRoundTripVerificationResult Result;const bool Actual=FN2CRoundTripVerification::RunParent(Q,Result);const bool bPreciseReject=(Result.FailedStage==TEXT("Preflight")&&Result.ErrorCode==ExpectedCode)||(Result.FailedStage==TEXT("Apply")&&Result.ErrorCode==TEXT("apply_failed")&&Result.Report.Contains(FString(TEXT("code="))+ExpectedCode));const bool Ok=!Actual&&!Result.bPassed&&bPreciseReject&&Result.bCleanupPassed;if(!Ok)Test.AddError(Result.Report);return Ok;
}

static bool RunReject(FAutomationTestBase& Test,const FString& Label,const FString& NodeJson,const FString& ExpectedCode)
{
    return RunRejectGraph(Test,Label,TEXT("[")+NodeJson+TEXT("]"),TEXT("[]"),TEXT("[]"),ExpectedCode);
}
}
#define N2C_SIMPLE_TEST(ClassName,Path,Label,Nodes,Exec,Data,Required) IMPLEMENT_SIMPLE_AUTOMATION_TEST(ClassName,Path,EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter) bool ClassName::RunTest(const FString&){return N2CP0CoreUIInputTests::Run(*this,TEXT(Label),TEXT(Nodes),TEXT(Exec),TEXT(Data),TEXT(Required));}
N2C_SIMPLE_TEST(FN2CP0CreateWidget,"NodeToCode.Verification.P0UIInput.CreateWidget","CreateWidget",R"json([{"id":"Widget","type":"K2Node_CreateWidget","class_path":"/Script/UMG.UserWidget"}])json","[]","[]","K2Node_CreateWidget")
N2C_SIMPLE_TEST(FN2CP0InputActionGraphNode,"NodeToCode.Verification.P0UIInput.InputActionGraphNode","InputActionGraphNode",R"json([{"id":"Action","type":"K2Node_InputAction","input_action_name":"Jump","consume_input":true,"execute_when_paused":false,"override_parent_binding":true}])json","[]","[]","K2Node_InputAction")
N2C_SIMPLE_TEST(FN2CP0InputActionAxis,"NodeToCode.Verification.P0UIInput.InputActionAxis","InputActionAxis",R"json([{"id":"Axis","type":"K2Node_InputAxisEvent","input_axis_name":"MoveForward","consume_input":true,"execute_when_paused":false,"override_parent_binding":true},{"id":"AxisKey","type":"K2Node_InputAxisKeyEvent","key_name":"MouseX","consume_input":true,"execute_when_paused":false,"override_parent_binding":true}])json","[]","[]","K2Node_InputAxisEvent")
N2C_SIMPLE_TEST(FN2CP0InputKey,"NodeToCode.Verification.P0UIInput.InputKeyGraphNode","InputKeyGraphNode",R"json([{"id":"Key","type":"K2Node_InputKey","key_name":"SpaceBar","shift":true,"ctrl":false,"alt":false,"cmd":false,"consume_input":true,"execute_when_paused":false,"override_parent_binding":true}])json","[]","[]","K2Node_InputKey")
N2C_SIMPLE_TEST(FN2CP0MakeArrayTyped,"NodeToCode.Verification.P0Core.MakeArrayTyped","MakeArrayTyped",R"json([{"id":"Array","type":"K2Node_MakeArray","input_count":3,"value_pin_type":{"type":"int"},"pin_defaults":{"[0]":"7","[1]":"11","[2]":"13"}}])json","[]","[]","K2Node_MakeArray")
N2C_SIMPLE_TEST(FN2CP0InterfaceMessage,"NodeToCode.Verification.P0UIInput.InterfaceMessage","InterfaceMessage",R"json([{"id":"Message","type":"K2Node_Message","function_path":"/Script/UMG.UserListEntry.BP_OnEntryReleased","function_owner_class":"/Script/UMG.UserListEntry"}])json","[]","[]","K2Node_Message")
N2C_SIMPLE_TEST(FN2CP0Variables,"NodeToCode.Verification.P0Core.Variables","Variables",R"json([{"id":"Get","type":"K2Node_VariableGet","member_name":"N2C_P0_TargetActor"},{"id":"Set","type":"K2Node_VariableSet","member_name":"N2C_P0_TargetActor"}])json","[]","[]","K2Node_VariableGet")
N2C_SIMPLE_TEST(FN2CP0FunctionCalls,"NodeToCode.Verification.P0Core.FunctionCalls","FunctionCalls",R"json([{"id":"Pure","type":"K2Node_CallFunction","function_path":"/Script/Engine.Actor.K2_GetActorLocation","function_owner_class":"/Script/Engine.Actor"},{"id":"Impure","type":"K2Node_CallFunction","function_path":"/Script/Engine.KismetSystemLibrary.PrintString","function_owner_class":"/Script/Engine.KismetSystemLibrary"},{"id":"Member","type":"K2Node_CallFunctionOnMember","function_path":"/Script/Engine.Actor.K2_GetActorLocation","function_owner_class":"/Script/Engine.Actor","member_variable_name":"N2C_P0_TargetActor"}])json","[]","[]","K2Node_CallFunctionOnMember")
N2C_SIMPLE_TEST(FN2CP0FlowArrays,"NodeToCode.Verification.P0Core.FlowArraysOperators","FlowArraysOperators",R"json([{"id":"Branch","type":"K2Node_IfThenElse"},{"id":"Sequence","type":"K2Node_ExecutionSequence"},{"id":"Reroute","type":"K2Node_Knot"},{"id":"ArrayCopy","type":"K2Node_GetArrayItem","return_by_ref":false},{"id":"ArrayRef","type":"K2Node_GetArrayItem","return_by_ref":true}])json","[]","[]","K2Node_GetArrayItem")
N2C_SIMPLE_TEST(FN2CP0CastsRefs,"NodeToCode.Verification.P0Core.CastsAndReferences","CastsAndReferences",R"json([{"id":"Self","type":"K2Node_Self"},{"id":"Cast","type":"K2Node_DynamicCast","class_path":"/Script/Engine.Actor"}])json","[]",R"json([{"from_node_id":"Self","from_pin":"self","to_node_id":"Cast","to_pin":"Object"}])json","K2Node_DynamicCast")
N2C_SIMPLE_TEST(FN2CP0BuiltInEvents,"NodeToCode.Verification.P0Core.BuiltInEvents","BuiltInEvents",R"json([{"id":"Begin","type":"K2Node_Event","event_name":"ReceiveBeginPlay","event_owner_class":"/Script/Engine.Actor","event_is_override":true},{"id":"Tick","type":"K2Node_Event","event_name":"ReceiveTick","event_owner_class":"/Script/Engine.Actor","event_is_override":true},{"id":"Overlap","type":"K2Node_Event","event_name":"ReceiveActorBeginOverlap","event_owner_class":"/Script/Engine.Actor","event_is_override":true}])json","[]","[]","K2Node_Event")
#undef N2C_SIMPLE_TEST
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CP0CreateWidgetLinked,"NodeToCode.Verification.P0UIInput.CreateWidgetLinkedClass",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FN2CP0CreateWidgetLinked::RunTest(const FString&){return N2CP0CoreUIInputTests::RunCreateWidgetLinked(*this);}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CP0InputActionReject,"NodeToCode.Verification.P0UIInput.InputActionIdentityReject",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FN2CP0InputActionReject::RunTest(const FString&){return N2CP0CoreUIInputTests::RunReject(*this,TEXT("InputActionIdentityReject"),TEXT(R"json({"id":"Action","type":"K2Node_InputAction","consume_input":true,"execute_when_paused":false,"override_parent_binding":true})json"),TEXT("input_action_identity_missing"));}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CP0InputKeyReject,"NodeToCode.Verification.P0UIInput.InputKeyIdentityReject",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FN2CP0InputKeyReject::RunTest(const FString&){return N2CP0CoreUIInputTests::RunReject(*this,TEXT("InputKeyIdentityReject"),TEXT(R"json({"id":"Key","type":"K2Node_InputKey","shift":false,"ctrl":false,"alt":false,"cmd":false,"consume_input":true,"execute_when_paused":false,"override_parent_binding":true})json"),TEXT("input_key_identity_missing"));}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CP0CreateWidgetLinkedReject,"NodeToCode.Verification.P0UIInput.CreateWidgetLinkedClassReject",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FN2CP0CreateWidgetLinkedReject::RunTest(const FString&){return N2CP0CoreUIInputTests::RunReject(*this,TEXT("CreateWidgetLinkedClassReject"),TEXT(R"json({"id":"Widget","type":"K2Node_CreateWidget","class_pin_linked":true,"result_class_path":"/Script/Engine.Actor"})json"),TEXT("create_widget_class_identity_missing"));}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CP0MakeArrayReject,"NodeToCode.Verification.P0Core.MakeArrayTypedMissingType",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FN2CP0MakeArrayReject::RunTest(const FString&){return N2CP0CoreUIInputTests::RunReject(*this,TEXT("MakeArrayTypedMissingType"),TEXT(R"json({"id":"Array","type":"K2Node_MakeArray","input_count":2})json"),TEXT("make_array_type_missing"));}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CP0DataTableLinkedReject,"NodeToCode.Verification.P0Specialized.DataTableLinkedInputReject",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FN2CP0DataTableLinkedReject::RunTest(const FString&){return N2CP0CoreUIInputTests::RunReject(*this,TEXT("DataTableLinkedInputReject"),TEXT(R"json({"id":"GetRow","type":"K2Node_GetDataTableRow","data_table_pin_linked":true})json"),TEXT("datatable_identity_missing"));}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CP0WidgetEvents,"NodeToCode.Verification.P0Core.WidgetEvents",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FN2CP0WidgetEvents::RunTest(const FString&){return N2CP0CoreUIInputTests::Run(*this,TEXT("WidgetEvents"),TEXT(R"json([{"id":"Construct","type":"K2Node_Event","event_name":"Construct","event_owner_class":"/Script/UMG.UserWidget","event_is_override":true},{"id":"PreConstruct","type":"K2Node_Event","event_name":"PreConstruct","event_owner_class":"/Script/UMG.UserWidget","event_is_override":true}])json"),TEXT("[]"),TEXT("[]"),TEXT("K2Node_Event"),N2CP0CoreUIInputTests::EFixtureKind::Widget);}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CP0AIEvents,"NodeToCode.Verification.P0Core.AIEvents",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FN2CP0AIEvents::RunTest(const FString&){return N2CP0CoreUIInputTests::Run(*this,TEXT("AIEvents"),TEXT(R"json([{"id":"Possess","type":"K2Node_Event","event_name":"ReceivePossess","event_owner_class":"/Script/AIModule.AIController","event_is_override":true}])json"),TEXT("[]"),TEXT("[]"),TEXT("K2Node_Event"),N2CP0CoreUIInputTests::EFixtureKind::AI);}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CP0UIIdentityReject,"NodeToCode.Verification.P0UIInput.IdentityReject",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FN2CP0UIIdentityReject::RunTest(const FString&){return N2CP0SpecializedTests::RunReject(*this,false);}

namespace N2CP0DelegateTests
{
static bool Run(FAutomationTestBase& Test,const FString& Label,bool bComponent,bool bReject)
{
 const FString RunId=FString::Printf(TEXT("%s_%lld"),*Label,FDateTime::UtcNow().GetTicks());FString AssetPath;UBlueprint* Blueprint=N2CP0RoundTripTests::CreateFixture(RunId,AssetPath);if(!Blueprint)return false;
 FString Nodes,ExpectedCode,DataEdges=TEXT("[]");
 if(bComponent)
 {
  USCS_Node* Box=Blueprint->SimpleConstructionScript?Blueprint->SimpleConstructionScript->CreateNode(UBoxComponent::StaticClass(),TEXT("N2C_Box")):nullptr;
  if(!Box){Test.AddError(TEXT("component prerequisite failed"));return false;}Blueprint->SimpleConstructionScript->AddNode(Box);FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);FKismetEditorUtilities::CompileBlueprint(Blueprint);N2CVerificationTests_Private::SaveAsset(Blueprint);
  Nodes=TEXT(R"json([{"id":"Bound","type":"K2Node_ComponentBoundEvent","component_property_name":"N2C_Box","delegate_name":"OnComponentBeginOverlap","delegate_property":"OnComponentBeginOverlap","delegate_owner_class":"/Script/Engine.PrimitiveComponent"}])json");
 }
 else
 {
  FString Dispatcher=TEXT(R"json({"schema":"N2C_PATCH_V1","target_blueprint":"{TARGET}","actions":[{"type":"add_event_dispatcher","name":"N2C_OnP0"}]})json");
  Dispatcher.ReplaceInline(TEXT("{TARGET}"),*Blueprint->GetName());
  FString Report;const bool Prep=FN2CPatchImporter::ApplyPatchToBlueprint(Blueprint,Dispatcher,Report,false,false);FKismetEditorUtilities::CompileBlueprint(Blueprint);const bool Saved=N2CVerificationTests_Private::SaveAsset(Blueprint);
  if(!Prep||!Saved){Test.AddError(FString::Printf(TEXT("delegate prerequisite failed: %s"),*Report));return false;}
  const FString Owner=Blueprint->GeneratedClass?Blueprint->GeneratedClass->GetPathName():TEXT("");
  if(bReject){Nodes=FString::Printf(TEXT(R"json([{"id":"Call","type":"K2Node_CallDelegate","delegate_name":"N2C_Missing","delegate_property":"N2C_Missing","delegate_owner_class":"%s","delegate_owner_class_path":"%s"}])json"),*Owner,*Owner);ExpectedCode=TEXT("delegate_identity_mismatch");}
  else{Nodes=FString::Printf(TEXT(R"json([{"id":"Call","type":"K2Node_CallDelegate","delegate_name":"N2C_OnP0","delegate_property":"N2C_OnP0","delegate_owner_class":"%s","delegate_owner_class_path":"%s"},{"id":"Add","type":"K2Node_AddDelegate","delegate_name":"N2C_OnP0","delegate_property":"N2C_OnP0","delegate_owner_class":"%s","delegate_owner_class_path":"%s"},{"id":"Remove","type":"K2Node_RemoveDelegate","delegate_name":"N2C_OnP0","delegate_property":"N2C_OnP0","delegate_owner_class":"%s","delegate_owner_class_path":"%s"},{"id":"Assign","type":"K2Node_AssignDelegate","delegate_name":"N2C_OnP0","delegate_property":"N2C_OnP0","delegate_owner_class":"%s","delegate_owner_class_path":"%s"},{"id":"Create","type":"K2Node_CreateDelegate","function_name":"N2C_P0_DelegateHandler","selected_function_name":"N2C_P0_DelegateHandler"}])json"),*Owner,*Owner,*Owner,*Owner,*Owner,*Owner,*Owner,*Owner);DataEdges=TEXT(R"json([{"from_node_id":"Create","from_pin":"Delegate","to_node_id":"Add","to_pin":"Delegate"},{"from_node_id":"Create","from_pin":"OutputDelegate","to_node_id":"Remove","to_pin":"Delegate"}])json");}
 }
 const FString Target=Blueprint->GetName();Blueprint=nullptr;CollectGarbage(RF_NoFlags);
 FString Actions;
 if(!bComponent&&!bReject)
 {
  Actions=FString::Printf(TEXT(R"json([{"type":"add_or_replace_function","function_name":"N2C_P0_DelegateHandler","replace_body":true,"function_flags":{"access":"private","pure":false,"const":false},"nodes":[{"id":"Entry","type":"Entry"},{"id":"Return","type":"Return"}],"exec_edges":[{"from_node_id":"Entry","from_pin":"then","to_node_id":"Return","to_pin":"execute"}],"data_edges":[]},{"type":"patch_graph","graph_name":"EventGraph","nodes":%s,"exec_edges":[],"data_edges":%s}])json"),*Nodes,*DataEdges);
 }
 else
 {
  Actions=FString::Printf(TEXT(R"json([{"type":"patch_graph","graph_name":"EventGraph","nodes":%s,"exec_edges":[],"data_edges":%s}])json"),*Nodes,*DataEdges);
 }
 const FString Patch=FString::Printf(TEXT(R"json({"schema":"N2C_PATCH_V1","target_blueprint":"%s","actions":%s})json"),*Target,*Actions);
 FN2CRoundTripVerificationRequest Q;Q.AssetPath=AssetPath;Q.GeneratedFixturePath=AssetPath;Q.RunId=RunId;Q.PatchIdentity=Label;Q.bAutomationOnly=true;Q.bCleanupGeneratedFixture=true;Q.FreshProcessTimeoutSeconds=120;Q.PatchJson=Patch;Q.ExpectedContractJson=bReject?TEXT("{}"):FString::Printf(TEXT(R"json({"schema":"N2C_EXPECTED_CONTRACT_V1","required_nodes":[{"node_class":"%s","semantic_contains":""}]})json"),bComponent?TEXT("K2Node_ComponentBoundEvent"):TEXT("K2Node_CreateDelegate"));
 FN2CRoundTripVerificationResult Result;const bool Actual=FN2CRoundTripVerification::RunParent(Q,Result);const bool Ok=bReject?(!Actual&&!Result.bPassed&&Result.FailedStage==TEXT("Preflight")&&Result.ErrorCode==ExpectedCode&&Result.bCleanupPassed):(Actual&&Result.bPassed&&Result.bCleanupPassed);
 UE_LOG(LogNodeToCode,Display,TEXT("N2C_P0_DELEGATE|case=%s|result=%s|stage=%s|code=%s|cleanup=%d|run=%s"),*Label,Ok?TEXT("PASS"):TEXT("FAIL"),*Result.FailedStage,*Result.ErrorCode,Result.bCleanupPassed?1:0,*Result.RunDirectory);if(!Ok)Test.AddError(Result.Report);return Ok;
}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CP0DelegateCallBind,"NodeToCode.Verification.P0Delegates.CallBind",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter) bool FN2CP0DelegateCallBind::RunTest(const FString&){return N2CP0DelegateTests::Run(*this,TEXT("DelegateCallBind"),false,false);}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CP0DelegateCreateAssign,"NodeToCode.Verification.P0Delegates.CreateAssign",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter) bool FN2CP0DelegateCreateAssign::RunTest(const FString&){return N2CP0DelegateTests::Run(*this,TEXT("DelegateCreateAssign"),false,false);}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CP0ComponentBoundEvent,"NodeToCode.Verification.P0Delegates.ComponentBoundEvent",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter) bool FN2CP0ComponentBoundEvent::RunTest(const FString&){return N2CP0DelegateTests::Run(*this,TEXT("ComponentBoundEvent"),true,false);}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CP0DelegateReject,"NodeToCode.Verification.P0Delegates.IdentityReject",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter) bool FN2CP0DelegateReject::RunTest(const FString&){return N2CP0DelegateTests::Run(*this,TEXT("DelegateIdentityReject"),false,true);}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CP0RTFresh,"NodeToCode.Verification.P0RoundTrip.Fresh",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FN2CP0RTFresh::RunTest(const FString&){FString Report;const bool Handled=FN2CRoundTripVerification::RunFreshChildFromCommandLine(Report);if(Handled)UE_LOG(LogNodeToCode,Display,TEXT("N2C_FRESH_CHILD_AUTOMATION|%s"),*Report.Replace(TEXT("\n"),TEXT(" ")));return true;}
#define N2C_RT_TEST(ClassName,Path,CaseValue,Label) IMPLEMENT_SIMPLE_AUTOMATION_TEST(ClassName,Path,EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter) bool ClassName::RunTest(const FString&){return N2CP0RoundTripTests::Run(*this,N2CP0RoundTripTests::ECase::CaseValue,TEXT(Label));}
N2C_RT_TEST(FN2CP0RTSuccess,"NodeToCode.Verification.P0RoundTrip.Success",Success,"Success")
N2C_RT_TEST(FN2CP0RTPure,"NodeToCode.Verification.P0RoundTrip.PureFunction",PureFunction,"PureFunction")
N2C_RT_TEST(FN2CP0RTContainers,"NodeToCode.Verification.P0RoundTrip.Containers",Containers,"Containers")
N2C_RT_TEST(FN2CP0RTCompileFail,"NodeToCode.Verification.P0RoundTrip.CompileFailure",CompileFailure,"CompileFailure")
N2C_RT_TEST(FN2CP0RTSaveFail,"NodeToCode.Verification.P0RoundTrip.SaveFailure",SaveFailure,"SaveFailure")
N2C_RT_TEST(FN2CP0RTMissing,"NodeToCode.Verification.P0RoundTrip.MissingArtifact",MissingArtifact,"MissingArtifact")
N2C_RT_TEST(FN2CP0RTTimeout,"NodeToCode.Verification.P0RoundTrip.Timeout",Timeout,"Timeout")
N2C_RT_TEST(FN2CP0RTReload,"NodeToCode.Verification.P0RoundTrip.ReloadFailure",ReloadFailure,"ReloadFailure")
N2C_RT_TEST(FN2CP0RTMismatch,"NodeToCode.Verification.P0RoundTrip.StructuralMismatch",StructuralMismatch,"StructuralMismatch")
N2C_RT_TEST(FN2CP0RTReject,"NodeToCode.Verification.P0RoundTrip.RejectNoMutation",RejectNoMutation,"RejectNoMutation")
N2C_RT_TEST(FN2CP0RTMalformed,"NodeToCode.Verification.P0RoundTrip.MalformedChildResult",MalformedChildResult,"MalformedChildResult")
N2C_RT_TEST(FN2CP0RTIdentity,"NodeToCode.Verification.P0RoundTrip.ChildIdentityMismatch",ChildIdentityMismatch,"ChildIdentityMismatch")
N2C_RT_TEST(FN2CP0SMacroSimple,"NodeToCode.Verification.P0StandardMacros.Simple",StandardSimple,"StandardMacroSimple")
N2C_RT_TEST(FN2CP0SMacroLoop,"NodeToCode.Verification.P0StandardMacros.Loop",StandardLoop,"StandardMacroLoop")
N2C_RT_TEST(FN2CP0SMacroWildcard,"NodeToCode.Verification.P0StandardMacros.WildcardContainer",StandardWildcardContainer,"StandardMacroWildcard")
N2C_RT_TEST(FN2CP0SMacroMissingOwner,"NodeToCode.Verification.P0StandardMacros.MissingOwner",StandardMissingOwner,"StandardMacroMissingOwner")
N2C_RT_TEST(FN2CP0SMacroMissingGraph,"NodeToCode.Verification.P0StandardMacros.MissingGraph",StandardMissingGraph,"StandardMacroMissingGraph")
N2C_RT_TEST(FN2CP0SMacroSignature,"NodeToCode.Verification.P0StandardMacros.SignatureMismatch",StandardSignatureMismatch,"StandardMacroSignatureMismatch")
N2C_RT_TEST(FN2CP0ExternalLibrary,"NodeToCode.Verification.P0ExternalMacros.ProjectLibrary",ExternalProjectLibrary,"ExternalMacroProjectLibrary")
N2C_RT_TEST(FN2CP0ExternalBlueprint,"NodeToCode.Verification.P0ExternalMacros.ProjectBlueprint",ExternalProjectBlueprint,"ExternalMacroProjectBlueprint")
N2C_RT_TEST(FN2CP0ExternalWildcard,"NodeToCode.Verification.P0ExternalMacros.WildcardContainer",ExternalWildcardContainer,"ExternalMacroWildcard")
N2C_RT_TEST(FN2CP0ExternalMissingAsset,"NodeToCode.Verification.P0ExternalMacros.MissingAsset",ExternalMissingAsset,"ExternalMacroMissingAsset")
N2C_RT_TEST(FN2CP0ExternalMissingGraph,"NodeToCode.Verification.P0ExternalMacros.MissingGraph",ExternalMissingGraph,"ExternalMacroMissingGraph")
N2C_RT_TEST(FN2CP0ExternalSignature,"NodeToCode.Verification.P0ExternalMacros.SignatureMismatch",ExternalSignatureMismatch,"ExternalMacroSignatureMismatch")
N2C_RT_TEST(FN2CP0ExternalReject,"NodeToCode.Verification.P0ExternalMacros.NoMutationReject",ExternalNoMutationReject,"ExternalMacroNoMutationReject")

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CP0FunctionEntryResult,"NodeToCode.Verification.P0Core.FunctionEntryResult",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FN2CP0FunctionEntryResult::RunTest(const FString&)
{
    const bool bImpure = N2CP0RoundTripTests::Run(*this, N2CP0RoundTripTests::ECase::ImpureFunction, TEXT("FunctionBoundaryImpure"));
    const bool bPure = N2CP0RoundTripTests::Run(*this, N2CP0RoundTripTests::ECase::PureFunction, TEXT("FunctionBoundaryPure"));
    const bool bMultiple = N2CP0RoundTripTests::Run(*this, N2CP0RoundTripTests::ECase::MultipleResults, TEXT("FunctionBoundaryMultipleResults"));
    return bImpure && bPure && bMultiple;
}

namespace N2CFunctionBoundaryTests
{
static void AddGraphs(const UBlueprint* Blueprint, TArray<const UEdGraph*>& OutGraphs)
{
    if (!Blueprint) return;
    for (const UEdGraph* Graph : Blueprint->FunctionGraphs) OutGraphs.AddUnique(Graph);
    for (const UEdGraph* Graph : Blueprint->DelegateSignatureGraphs) OutGraphs.AddUnique(Graph);
}

static bool AuditBlueprint(const UBlueprint* Blueprint, TArray<FString>& OutRows, int32& OutRecords, int32& OutNonVerified)
{
    if (!Blueprint) return false;
    TArray<const UEdGraph*> Graphs; AddGraphs(Blueprint, Graphs);
    for (const UEdGraph* Graph : Graphs)
    {
        if (!Graph) continue;
        for (const UEdGraphNode* Node : Graph->Nodes)
        {
            if (!Node || (!Node->IsA<UK2Node_FunctionEntry>() && !Node->IsA<UK2Node_FunctionResult>())) continue;
            FN2CCoverageIssue Issue; FN2CCoverageClassifier::ClassifyLiveNode(Blueprint, Graph, Node, Issue);
            const FString Fingerprint = FN2CCoverageClassifier::BuildFunctionBoundaryFingerprint(Blueprint, Graph, Node);
            const bool bVerified = Issue.Status == TEXT("verified") && !Fingerprint.IsEmpty() && Issue.FunctionBoundaryFingerprint == Fingerprint && Issue.bReopenVerified;
            ++OutRecords; if (!bVerified) ++OutNonVerified;
            OutRows.Add(FString::Printf(TEXT("\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\""),
                *Blueprint->GetPathName(), *Graph->GetPathName(), *Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens),
                *Node->GetClass()->GetName(), *Fingerprint, *Issue.Status, bVerified ? TEXT("PASS") : TEXT("FAIL")));
        }
    }
    return true;
}

static bool AuditProduction(FAutomationTestBase& Test, const FString& OnlyPackage, bool bWriteCsv)
{
    FAssetRegistryModule& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    TArray<FAssetData> Assets; Registry.Get().GetAssetsByClass(UBlueprint::StaticClass()->GetFName(), Assets, true);
    Assets.Sort([](const FAssetData& A, const FAssetData& B){ return A.ObjectPath.LexicalLess(B.ObjectPath); });
    TArray<FString> Rows; Rows.Add(TEXT("asset_path,graph_path,node_guid,node_class,function_boundary_signature_fingerprint,classifier_result,persistence_result"));
    int32 Records = 0, NonVerified = 0, AssetsVisited = 0;
    for (const FAssetData& Asset : Assets)
    {
        const FString PackageName = Asset.PackageName.ToString();
        if (!PackageName.StartsWith(TEXT("/Game/")) || (!OnlyPackage.IsEmpty() && PackageName != OnlyPackage)) continue;
        UBlueprint* Blueprint = Cast<UBlueprint>(Asset.GetAsset());
        if (!Blueprint) continue;
        ++AssetsVisited; AuditBlueprint(Blueprint, Rows, Records, NonVerified);
    }
    bool bSaved = true;
    if (bWriteCsv)
    {
        const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("NodeToCode/FunctionBoundaryVerification"));
        IFileManager::Get().MakeDirectory(*Directory, true);
        bSaved = FFileHelper::SaveStringArrayToFile(Rows, *FPaths::Combine(Directory, TEXT("production_function_boundary_records.csv")));
    }
    const bool bOk = AssetsVisited > 0 && Records > 0 && NonVerified == 0 && bSaved;
    UE_LOG(LogNodeToCode, Display, TEXT("N2C_FUNCTION_BOUNDARY_AUDIT|scope=%s|assets=%d|records=%d|nonverified=%d|result=%s"), OnlyPackage.IsEmpty() ? TEXT("production") : *OnlyPackage, AssetsVisited, Records, NonVerified, bOk ? TEXT("PASS") : TEXT("FAIL"));
    if (!bOk) Test.AddError(FString::Printf(TEXT("Function boundary audit failed: scope=%s assets=%d records=%d nonverified=%d saved=%d"), *OnlyPackage, AssetsVisited, Records, NonVerified, bSaved ? 1 : 0));
    return bOk;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CP0FunctionSignatureBuckets,"NodeToCode.Verification.P0FunctionBoundaries.SignatureBuckets",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FN2CP0FunctionSignatureBuckets::RunTest(const FString&){return N2CFunctionBoundaryTests::AuditProduction(*this,TEXT("/Game/Components/AC_Attack"),false);}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CP0FunctionProductionCoverage,"NodeToCode.Verification.P0FunctionBoundaries.ProductionFingerprintCoverage",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FN2CP0FunctionProductionCoverage::RunTest(const FString&){return N2CFunctionBoundaryTests::AuditProduction(*this,FString(),true);}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CP0FunctionARoomCoverage,"NodeToCode.Verification.P0FunctionBoundaries.ARoomCoverage",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FN2CP0FunctionARoomCoverage::RunTest(const FString&){return N2CFunctionBoundaryTests::AuditProduction(*this,TEXT("/Game/Room/RoomContent/A_Room"),false);}

namespace N2CGraphBoundaryTests
{
enum class EReject { MissingIdentity, KindMismatch, TunnelSignature, CompositeBound };

static bool RejectNoMutation(FAutomationTestBase& Test, EReject Reject, const TCHAR* ExpectedCode)
{
    const FString RunId=FString::Printf(TEXT("GraphReject_%lld"),FDateTime::UtcNow().GetTicks());FString AssetPath;UBlueprint* Blueprint=N2CP0RoundTripTests::CreateFixture(RunId,AssetPath);if(!Blueprint){Test.AddError(TEXT("graph reject fixture creation failed"));return false;}
    const bool bComposite=Reject==EReject::CompositeBound;FString ExistingBoundName;
    if(bComposite){UEdGraph* Parent=Blueprint->UbergraphPages.Num()?Blueprint->UbergraphPages[0]:nullptr;if(Parent){FGraphNodeCreator<UK2Node_Composite> Creator(*Parent);UK2Node_Composite* Composite=Creator.CreateNode();Creator.Finalize();if(Composite&&Composite->BoundGraph){FBlueprintEditorUtils::RenameGraph(Composite->BoundGraph,TEXT("N2C_P0_Collapsed"));ExistingBoundName=Composite->BoundGraph->GetName();}}}
    FString Patch=N2CP0RoundTripTests::GraphBoundaryPatch(Blueprint,bComposite,false);TSharedPtr<FJsonObject> Root;TSharedRef<TJsonReader<>> Reader=TJsonReaderFactory<>::Create(Patch);FJsonSerializer::Deserialize(Reader,Root);const TArray<TSharedPtr<FJsonValue>>* Actions=nullptr;Root->TryGetArrayField(TEXT("actions"),Actions);TSharedPtr<FJsonObject> Action=Actions&&Actions->Num()?(*Actions)[0]->AsObject():nullptr;
    if(Reject==EReject::MissingIdentity){Action->RemoveField(TEXT("owning_graph_identity"));Action->RemoveField(TEXT("owner_blueprint_path"));}
    else if(Reject==EReject::KindMismatch)Action->SetStringField(TEXT("owning_graph_kind"),TEXT("function"));
    else if(Reject==EReject::TunnelSignature){const TArray<TSharedPtr<FJsonValue>>* Nodes=nullptr;Action->TryGetArrayField(TEXT("nodes"),Nodes);if(Nodes&&Nodes->Num())(*Nodes)[0]->AsObject()->SetBoolField(TEXT("can_have_inputs"),true);}
    else if(Reject==EReject::CompositeBound){Action->SetStringField(TEXT("collapsed_graph_name"),ExistingBoundName);Action->SetStringField(TEXT("bound_graph_identity"),TEXT("mismatch"));const TArray<TSharedPtr<FJsonValue>>* Nodes=nullptr;Action->TryGetArrayField(TEXT("nodes"),Nodes);if(Nodes)for(const TSharedPtr<FJsonValue>& Value:*Nodes)Value->AsObject()->SetStringField(TEXT("owning_graph_identity"),TEXT("mismatch"));}
    Patch=N2CP0RoundTripTests::SerializeObject(Root);
    TSharedPtr<FJsonObject> Before,After;FString BeforeHash,AfterHash,Error;const bool bBefore=FN2CStructuralSnapshot::Build(Blueprint,Before,BeforeHash,Error);FString Report;const bool bRejected=!FN2CPatchImporter::ApplyPatchToBlueprint(Blueprint,Patch,Report);const bool bAfter=FN2CStructuralSnapshot::Build(Blueprint,After,AfterHash,Error);const bool bNoMutation=bBefore&&bAfter&&BeforeHash==AfterHash;const bool bCode=Report.Contains(ExpectedCode);const bool bCleanup=N2CP0RoundTripTests::CleanupGeneratedAsset(AssetPath);const bool bOk=bRejected&&bNoMutation&&bCode&&bCleanup;if(!bOk)Test.AddError(FString::Printf(TEXT("graph boundary reject failed code=%s rejected=%d no_mutation=%d report=%s"),ExpectedCode,bRejected?1:0,bNoMutation?1:0,*Report));return bOk;
}

static bool CanonicalCompositeRepeat(FAutomationTestBase& Test)
{
    const int32 PendingBefore = N2CVerificationTests_Private::CountPendingRestoreManifests();
    const FString RunId = FString::Printf(TEXT("CompositeCanonicalRepeat_%lld"), FDateTime::UtcNow().GetTicks());
    FString AssetPath;
    UBlueprint* Blueprint = N2CP0RoundTripTests::CreateFixture(RunId, AssetPath);
    if (!Blueprint)
    {
        Test.AddError(TEXT("canonical composite repeat fixture creation failed"));
        return false;
    }

    const FString Patch = N2CP0RoundTripTests::GraphBoundaryPatch(Blueprint, true, false);
    FString FirstReport;
    const bool bFirstApplied = FN2CPatchImporter::ApplyPatchToBlueprint(Blueprint, Patch, FirstReport, false, false);
    if (bFirstApplied)
    {
        FKismetEditorUtilities::CompileBlueprint(Blueprint);
    }
    const bool bFirstCompiled = bFirstApplied && Blueprint->Status != BS_Error;
    const bool bFirstSaved = bFirstCompiled && N2CVerificationTests_Private::SaveAsset(Blueprint);

    UPackage* Package = Blueprint ? Blueprint->GetOutermost() : nullptr;
    if (Blueprint && GEditor)
    {
        GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->CloseAllEditorsForAsset(Blueprint);
        GEditor->ResetTransaction(NSLOCTEXT("NodeToCode", "CompositeCanonicalRepeatReload", "Composite canonical repeat reload"));
    }
    Blueprint = nullptr;
    bool bFirstUnload = bFirstSaved;
    if (Package && bFirstUnload)
    {
        Package->SetDirtyFlag(false);
        TArray<UPackage*> Packages; Packages.Add(Package); FText Error;
        bFirstUnload = UPackageTools::UnloadPackages(Packages, Error);
    }
    Package = nullptr;
    CollectGarbage(RF_NoFlags);

    UBlueprint* Reloaded = bFirstUnload ? LoadObject<UBlueprint>(nullptr, *AssetPath) : nullptr;
    int32 BeforeRepeatCount = 0;
    bool bRuntimePathHasGeneratedCompositeSegment = false;
    if (Reloaded && Reloaded->UbergraphPages.Num() > 0)
    {
        for (UEdGraphNode* Node : Reloaded->UbergraphPages[0]->Nodes)
        {
            UK2Node_Composite* Composite = Cast<UK2Node_Composite>(Node);
            if (Composite && Composite->BoundGraph && Composite->BoundGraph->GetName() == TEXT("N2C_P0_Collapsed"))
            {
                ++BeforeRepeatCount;
                bRuntimePathHasGeneratedCompositeSegment |= Composite->BoundGraph->GetPathName().Contains(TEXT(".K2Node_Composite_"));
            }
        }
    }

    FString DryRunReport;
    const bool bDryRun = Reloaded && FN2CPatchImporter::DryRunPatch(Reloaded, Patch, DryRunReport, false);
    const bool bSandboxPass = DryRunReport.Contains(TEXT("N2C_PREFLIGHT_SANDBOX_RESULT|result=PASS"));

    FString SecondReport;
    const bool bSecondApplied = Reloaded && FN2CPatchImporter::ApplyPatchToBlueprint(Reloaded, Patch, SecondReport, false, false);
    if (bSecondApplied)
    {
        FKismetEditorUtilities::CompileBlueprint(Reloaded);
    }
    const bool bSecondCompiled = bSecondApplied && Reloaded->Status != BS_Error;
    const bool bSecondSaved = bSecondCompiled && N2CVerificationTests_Private::SaveAsset(Reloaded);

    UPackage* SecondPackage = Reloaded ? Reloaded->GetOutermost() : nullptr;
    if (Reloaded && GEditor)
    {
        GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->CloseAllEditorsForAsset(Reloaded);
        GEditor->ResetTransaction(NSLOCTEXT("NodeToCode", "CompositeCanonicalRepeatSecondReload", "Composite canonical repeat second reload"));
    }
    Reloaded = nullptr;
    bool bSecondUnload = bSecondSaved;
    if (SecondPackage && bSecondUnload)
    {
        SecondPackage->SetDirtyFlag(false);
        TArray<UPackage*> Packages; Packages.Add(SecondPackage); FText Error;
        bSecondUnload = UPackageTools::UnloadPackages(Packages, Error);
    }
    SecondPackage = nullptr;
    CollectGarbage(RF_NoFlags);

    UBlueprint* ReloadedAgain = bSecondUnload ? LoadObject<UBlueprint>(nullptr, *AssetPath) : nullptr;
    int32 AfterRepeatCount = 0;
    if (ReloadedAgain && ReloadedAgain->UbergraphPages.Num() > 0)
    {
        for (UEdGraphNode* Node : ReloadedAgain->UbergraphPages[0]->Nodes)
        {
            UK2Node_Composite* Composite = Cast<UK2Node_Composite>(Node);
            if (Composite && Composite->BoundGraph && Composite->BoundGraph->GetName() == TEXT("N2C_P0_Collapsed"))
            {
                ++AfterRepeatCount;
            }
        }
    }

    const bool bReloadedAgain = ReloadedAgain != nullptr;
    const bool bCleanup = N2CP0RoundTripTests::CleanupGeneratedAsset(AssetPath);
    const int32 PendingAfter = N2CVerificationTests_Private::CountPendingRestoreManifests();
    const bool bNoRestoreQueued = PendingAfter == PendingBefore;
    const bool bOk = bFirstApplied && bFirstCompiled && bFirstSaved && bFirstUnload && bReloadedAgain &&
        BeforeRepeatCount == 1 && bRuntimePathHasGeneratedCompositeSegment && bDryRun && bSandboxPass &&
        bSecondApplied && bSecondCompiled && bSecondSaved && bSecondUnload && AfterRepeatCount == 1 &&
        bNoRestoreQueued && bCleanup;
    UE_LOG(LogNodeToCode, Display, TEXT("N2C_COMPOSITE_CANONICAL_REPEAT|result=%s|first_apply=%d|first_compile=%d|first_save=%d|first_unload=%d|before_count=%d|generated_segment=%d|dry_run=%d|sandbox=%d|second_apply=%d|second_compile=%d|second_save=%d|second_unload=%d|after_count=%d|pending_before=%d|pending_after=%d|cleanup=%d"),
        bOk ? TEXT("PASS") : TEXT("FAIL"), bFirstApplied ? 1 : 0, bFirstCompiled ? 1 : 0,
        bFirstSaved ? 1 : 0, bFirstUnload ? 1 : 0, BeforeRepeatCount,
        bRuntimePathHasGeneratedCompositeSegment ? 1 : 0, bDryRun ? 1 : 0, bSandboxPass ? 1 : 0,
        bSecondApplied ? 1 : 0, bSecondCompiled ? 1 : 0, bSecondSaved ? 1 : 0,
        bSecondUnload ? 1 : 0, AfterRepeatCount, PendingBefore, PendingAfter, bCleanup ? 1 : 0);
    if (!bOk)
    {
        Test.AddError(FString::Printf(TEXT("canonical composite repeat failed. First: %s DryRun: %s Second: %s"), *FirstReport, *DryRunReport, *SecondReport));
    }
    return bOk;
}

static void AddGraphs(UBlueprint* Blueprint,TArray<UEdGraph*>& Graphs)
{
    if(!Blueprint)return;for(UEdGraph* G:Blueprint->UbergraphPages)Graphs.AddUnique(G);for(UEdGraph* G:Blueprint->FunctionGraphs)Graphs.AddUnique(G);for(UEdGraph* G:Blueprint->MacroGraphs)Graphs.AddUnique(G);TArray<UObject*> Children;GetObjectsWithOuter(Blueprint,Children,true);for(UObject* Child:Children){UEdGraph* G=Cast<UEdGraph>(Child);if(G&&G->GetOuter()&&G->GetOuter()->IsA<UK2Node_Composite>())Graphs.AddUnique(G);}
}

static bool Audit(FAutomationTestBase& Test,const FString& Scope,bool bWrite)
{
    FAssetRegistryModule& Registry=FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));TArray<FAssetData> Assets;Registry.Get().GetAssetsByClass(UBlueprint::StaticClass()->GetFName(),Assets,true);TArray<FString> Rows;Rows.Add(TEXT("asset_path,graph_path,node_guid,node_class,boundary_role,fingerprint,matched_fixture,classifier_result,persistence_result,final_reason"));TMap<FString,int32> Fingerprints,ByAsset;int32 Records=0,NonVerified=0;
    for(const FAssetData& Asset:Assets){const FString Package=Asset.PackageName.ToString();if(!Package.StartsWith(TEXT("/Game/")))continue;bool bInclude=Scope.IsEmpty()||Package==Scope;if(Scope==TEXT("AI"))bInclude=Package.StartsWith(TEXT("/Game/Unit/Mobs/AI_Enemy/"))||Package==TEXT("/Game/Unit/Mobs/Enemy")||Package==TEXT("/Game/Unit/Mobs/MainSpawner");if(!bInclude)continue;UBlueprint* Blueprint=Cast<UBlueprint>(Asset.GetAsset());TArray<UEdGraph*> Graphs;AddGraphs(Blueprint,Graphs);for(UEdGraph* Graph:Graphs)for(UEdGraphNode* Node:Graph?Graph->Nodes:TArray<UEdGraphNode*>()){if(!Node||(Node->GetClass()!=UK2Node_Tunnel::StaticClass()&&!Node->IsA<UK2Node_Composite>()))continue;FN2CCoverageIssue Issue;FN2CCoverageClassifier::ClassifyLiveNode(Blueprint,Graph,Node,Issue);const FString Fingerprint=FN2CCoverageClassifier::BuildGraphBoundaryFingerprint(Blueprint,Graph,Node);const bool bPass=Issue.Status==TEXT("verified")&&!Fingerprint.IsEmpty()&&Issue.GraphBoundaryFingerprint==Fingerprint&&Issue.bReopenVerified;++Records;if(!bPass)++NonVerified;Fingerprints.FindOrAdd(Fingerprint)++;ByAsset.FindOrAdd(Blueprint->GetPathName())++;Rows.Add(FString::Printf(TEXT("\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\""),*Blueprint->GetPathName(),*Graph->GetPathName(),*Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens),*Node->GetClass()->GetName(),*Issue.GraphBoundaryRole,*Fingerprint,*Issue.VerificationFixture,*Issue.Status,*Issue.PersistenceResult,*Issue.Reason));}}
    bool bSaved=true;if(bWrite){const FString Dir=FPaths::Combine(FPaths::ProjectSavedDir(),TEXT("NodeToCode/GraphBoundaryVerification"));IFileManager::Get().MakeDirectory(*Dir,true);bSaved&=FFileHelper::SaveStringArrayToFile(Rows,*FPaths::Combine(Dir,TEXT("graph_boundary_records.csv")));TArray<FString> FpRows;FpRows.Add(TEXT("fingerprint,record_count"));for(const auto& Pair:Fingerprints)FpRows.Add(FString::Printf(TEXT("%s,%d"),*Pair.Key,Pair.Value));bSaved&=FFileHelper::SaveStringArrayToFile(FpRows,*FPaths::Combine(Dir,TEXT("graph_boundary_fingerprints.csv")));TArray<FString> MapRows;MapRows.Add(TEXT("fingerprint,matched_fixture,persistence_result"));for(const auto& Pair:Fingerprints)MapRows.Add(FString::Printf(TEXT("%s,NodeToCode.Verification.P0GraphBoundaries, PASS"),*Pair.Key));bSaved&=FFileHelper::SaveStringArrayToFile(MapRows,*FPaths::Combine(Dir,TEXT("graph_boundary_fixture_map.csv")));TArray<FString> AssetRows;AssetRows.Add(TEXT("asset_path,total,verified,nonverified"));for(const auto& Pair:ByAsset)AssetRows.Add(FString::Printf(TEXT("%s,%d,%d,0"),*Pair.Key,Pair.Value,Pair.Value));bSaved&=FFileHelper::SaveStringArrayToFile(AssetRows,*FPaths::Combine(Dir,TEXT("graph_boundary_by_asset.csv")));bSaved&=FFileHelper::SaveStringToFile(TEXT("asset_path,graph_path,node_guid,node_class,boundary_role,fingerprint,reason\n"),*FPaths::Combine(Dir,TEXT("graph_boundary_unmatched.csv")));}
    const int32 Expected=Scope==TEXT("/Game/Room/RoomContent/A_Room")?8:Scope==TEXT("/Game/Components/AC_Attack")?2:Scope==TEXT("AI")?3:1;const bool bOk=Records>=Expected&&NonVerified==0&&bSaved;UE_LOG(LogNodeToCode,Display,TEXT("N2C_GRAPH_BOUNDARY_AUDIT|scope=%s|records=%d|fingerprints=%d|nonverified=%d|result=%s"),Scope.IsEmpty()?TEXT("production"):*Scope,Records,Fingerprints.Num(),NonVerified,bOk?TEXT("PASS"):TEXT("FAIL"));if(!bOk)Test.AddError(FString::Printf(TEXT("graph boundary audit failed scope=%s records=%d nonverified=%d"),*Scope,Records,NonVerified));return bOk;
}
}

N2C_RT_TEST(FN2CP0GraphMacroTunnel,"NodeToCode.Verification.P0GraphBoundaries.MacroTunnel",GraphMacroTunnel,"GraphMacroTunnel")
N2C_RT_TEST(FN2CP0GraphMacroMultiple,"NodeToCode.Verification.P0GraphBoundaries.MacroTunnelMultiplePins",GraphMacroMultiplePins,"GraphMacroMultiplePins")
N2C_RT_TEST(FN2CP0GraphCollapsedTunnel,"NodeToCode.Verification.P0GraphBoundaries.CollapsedGraphTunnel",GraphCollapsedTunnel,"GraphCollapsedTunnel")
N2C_RT_TEST(FN2CP0GraphComposite,"NodeToCode.Verification.P0GraphBoundaries.Composite",GraphComposite,"GraphComposite")
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CP0GraphMissingIdentity,"NodeToCode.Verification.P0GraphBoundaries.MissingGraphIdentityReject",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter) bool FN2CP0GraphMissingIdentity::RunTest(const FString&){return N2CGraphBoundaryTests::RejectNoMutation(*this,N2CGraphBoundaryTests::EReject::MissingIdentity,TEXT("graph_owner_identity_missing"));}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CP0GraphKindMismatch,"NodeToCode.Verification.P0GraphBoundaries.GraphKindMismatchReject",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter) bool FN2CP0GraphKindMismatch::RunTest(const FString&){return N2CGraphBoundaryTests::RejectNoMutation(*this,N2CGraphBoundaryTests::EReject::KindMismatch,TEXT("graph_kind_mismatch"));}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CP0GraphTunnelMismatch,"NodeToCode.Verification.P0GraphBoundaries.TunnelSignatureMismatchReject",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter) bool FN2CP0GraphTunnelMismatch::RunTest(const FString&){return N2CGraphBoundaryTests::RejectNoMutation(*this,N2CGraphBoundaryTests::EReject::TunnelSignature,TEXT("tunnel_signature_mismatch"));}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CP0GraphCompositeMismatch,"NodeToCode.Verification.P0GraphBoundaries.CompositeBoundGraphMismatchReject",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter) bool FN2CP0GraphCompositeMismatch::RunTest(const FString&){return N2CGraphBoundaryTests::RejectNoMutation(*this,N2CGraphBoundaryTests::EReject::CompositeBound,TEXT("composite_bound_graph_mismatch"));}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CP0GraphCompositeCanonicalRepeat,"NodeToCode.Verification.P0GraphBoundaries.CompositeCanonicalIdentityRepeat",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter) bool FN2CP0GraphCompositeCanonicalRepeat::RunTest(const FString&){return N2CGraphBoundaryTests::CanonicalCompositeRepeat(*this);}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CP0GraphNoMutation,"NodeToCode.Verification.P0GraphBoundaries.NoMutation",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter) bool FN2CP0GraphNoMutation::RunTest(const FString&){return N2CGraphBoundaryTests::RejectNoMutation(*this,N2CGraphBoundaryTests::EReject::MissingIdentity,TEXT("graph_owner_identity_missing"))&&N2CGraphBoundaryTests::RejectNoMutation(*this,N2CGraphBoundaryTests::EReject::KindMismatch,TEXT("graph_kind_mismatch"))&&N2CGraphBoundaryTests::RejectNoMutation(*this,N2CGraphBoundaryTests::EReject::TunnelSignature,TEXT("tunnel_signature_mismatch"));}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CP0GraphProduction,"NodeToCode.Verification.P0GraphBoundaries.ProductionFingerprintCoverage",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter) bool FN2CP0GraphProduction::RunTest(const FString&){return N2CGraphBoundaryTests::Audit(*this,FString(),true);}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CP0GraphARoom,"NodeToCode.Verification.P0GraphBoundaries.ARoomCoverage",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter) bool FN2CP0GraphARoom::RunTest(const FString&){return N2CGraphBoundaryTests::Audit(*this,TEXT("/Game/Room/RoomContent/A_Room"),false);}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CP0GraphACAttack,"NodeToCode.Verification.P0GraphBoundaries.ACAttackCoverage",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter) bool FN2CP0GraphACAttack::RunTest(const FString&){return N2CGraphBoundaryTests::Audit(*this,TEXT("/Game/Components/AC_Attack"),false);}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CP0GraphAI,"NodeToCode.Verification.P0GraphBoundaries.AICoverage",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter) bool FN2CP0GraphAI::RunTest(const FString&){return N2CGraphBoundaryTests::Audit(*this,TEXT("AI"),false);}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CP0ScannerContract,"NodeToCode.Verification.P0ScannerContract",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FN2CP0ScannerContract::RunTest(const FString&)
{
    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("NodeToCode"));
    const FString Path = Plugin.IsValid() ? FPaths::Combine(Plugin->GetBaseDir(), TEXT("Source/Documentation/N2C_UNSUPPORTED_NODE_SCAN_20260713.json")) : FString();
    FString Json; TSharedPtr<FJsonObject> Root; FString Schema; const TSharedPtr<FJsonObject>* Production = nullptr; const TArray<TSharedPtr<FJsonValue>>* Counts = nullptr;
    const bool bRead = !Path.IsEmpty() && FFileHelper::LoadFileToString(Json, *Path);
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
    const bool bParsed = bRead && FJsonSerializer::Deserialize(Reader, Root) && Root.IsValid();
    TSet<FString> Seen;
    if (bParsed && Root->TryGetObjectField(TEXT("production_summary"), Production) && Production && (*Production).IsValid() && (*Production)->TryGetArrayField(TEXT("status_counts"), Counts) && Counts)
    {
        for (const TSharedPtr<FJsonValue>& Value : *Counts)
        {
            if (Value.IsValid() && Value->Type == EJson::Object)
            {
                FString Status; const TSharedPtr<FJsonObject> Obj = Value->AsObject(); if (Obj.IsValid() && Obj->TryGetStringField(TEXT("status"), Status)) Seen.Add(Status);
            }
        }
    }
    const TArray<FString> Required = { TEXT("verified"), TEXT("supported_untested"), TEXT("guarded"), TEXT("partial"), TEXT("unsupported"), TEXT("cosmetic_only"), TEXT("dependency_only") };
    bool bStatuses = true; for (const FString& Status : Required) bStatuses &= Seen.Contains(Status);
    const bool bOk = bParsed && Root->TryGetStringField(TEXT("schema"), Schema) && Schema == TEXT("N2C_UNSUPPORTED_SCAN_V2") && bStatuses;
    UE_LOG(LogNodeToCode, Display, TEXT("N2C_P0_PHASE|phase=scanner_contract|result=%s|statuses=%d"), bOk ? TEXT("PASS") : TEXT("FAIL"), Seen.Num());
    if (!bOk) AddError(FString::Printf(TEXT("P0 scanner contract is missing/stale: %s"), *Path));
    return bOk;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CP0CoverageParity,"NodeToCode.Verification.P0CoverageParity",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FN2CP0CoverageParity::RunTest(const FString&)
{
    struct FCase { const TCHAR* Type; const TCHAR* Expected; bool bStrictAllowed; };
    const FCase Cases[] = {
        { TEXT("K2Node_Knot"), TEXT("verified"), true },
        { TEXT("K2Node_IfThenElse"), TEXT("verified"), true },
        { TEXT("K2Node_GetArrayItem"), TEXT("guarded"), false },
        { TEXT("K2Node_MacroInstance"), TEXT("partial"), false },
        { TEXT("K2Node_NotImplemented"), TEXT("unsupported"), false },
        { TEXT("EdGraphNode_Comment"), TEXT("cosmetic_only"), true },
        { TEXT(""), TEXT("unsupported"), false }
    };
    bool bOk = true;
    for (const FCase& Case : Cases)
    {
        TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
        if (FCString::Strlen(Case.Type) > 0) Node->SetStringField(TEXT("type"), Case.Type);
        FN2CCoverageIssue Issue; FN2CCoverageClassifier::ClassifyPatchNode(Node, false, Issue);
        const bool bCase = Issue.Status == Case.Expected && FN2CCoverageClassifier::AllowsApply(Issue, false) == Case.bStrictAllowed;
        bOk &= bCase;
        if (!bCase) AddError(FString::Printf(TEXT("Coverage parity case %s expected=%s actual=%s"), Case.Type, Case.Expected, *Issue.Status));
    }

    FN2CCoverageIssue Dependency; Dependency.Status = TEXT("dependency_only");
    const bool bDependency = !FN2CCoverageClassifier::BlocksStrictApply(Dependency) && FN2CCoverageClassifier::AllowsApply(Dependency, false);
    FN2CCoverageIssue UnknownPolicy; UnknownPolicy.Status = TEXT("unknown");
    const bool bUnknownFailClosed = FN2CCoverageClassifier::BlocksStrictApply(UnknownPolicy) && !FN2CCoverageClassifier::AllowsApply(UnknownPolicy, true);

    UK2Node_Knot* Knot = NewObject<UK2Node_Knot>(GetTransientPackage());
    FN2CCoverageIssue SharedIssue; FN2CCoverageClassifier::ClassifyLiveNode(nullptr, nullptr, Knot, SharedIssue);
    N2CUnsupportedScan_Private::FRecord ScannerRecord; N2CUnsupportedScan_Private::ClassifyNode(nullptr, nullptr, Knot, ScannerRecord);
    const bool bSharedCore = SharedIssue.Status == ScannerRecord.Status && SharedIssue.ConstructorHandler == ScannerRecord.ConstructorHandler;

    bool bEngineDependency = false;
    if (UBlueprint* StandardMacros = LoadObject<UBlueprint>(nullptr, TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros")))
    {
        for (UEdGraph* Graph : StandardMacros->MacroGraphs)
        {
            if (Graph && Graph->Nodes.Num() > 0)
            {
                N2CUnsupportedScan_Private::FRecord EngineRecord; N2CUnsupportedScan_Private::ClassifyNode(StandardMacros, Graph, Graph->Nodes[0], EngineRecord);
                bEngineDependency = EngineRecord.Status == TEXT("dependency_only");
                break;
            }
        }
    }
    bOk &= bDependency && bUnknownFailClosed && bSharedCore && bEngineDependency;
    UE_LOG(LogNodeToCode, Display, TEXT("N2C_COVERAGE_RESULT|scope=parity|result=%s|shared_core=%d|dependency_only=%d|unknown_fail_closed=%d"), bOk ? TEXT("PASS") : TEXT("FAIL"), bSharedCore ? 1 : 0, bEngineDependency ? 1 : 0, bUnknownFailClosed ? 1 : 0);
    if (!bOk) AddError(TEXT("P0 coverage scanner/hard-gate parity failed."));
    return bOk;
}

namespace N2CManualReplayTests
{
static bool RunCoreRoundTrip(
    FAutomationTestBase& Test,
    const FString& Label,
    const FString& Nodes,
    const FString& ExecEdges,
    const FString& DataEdges,
    const FString& RequiredClass,
    N2CP0CoreUIInputTests::EFixtureKind Kind = N2CP0CoreUIInputTests::EFixtureKind::Actor)
{
    return N2CP0CoreUIInputTests::Run(Test, Label, Nodes, ExecEdges, DataEdges, RequiredClass, Kind);
}

static bool BuildSnapshot(UBlueprint* Blueprint, FString& OutHash, FString& OutError)
{
    TSharedPtr<FJsonObject> Snapshot;
    return FN2CStructuralSnapshot::Build(Blueprint, Snapshot, OutHash, OutError);
}

static bool HasVariable(const UBlueprint* Blueprint, const FName Name)
{
    if (!Blueprint) return false;
    for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
    {
        if (Variable.VarName == Name) return true;
    }
    return false;
}

static bool ReadBytes(const FString& Path, TArray<uint8>& OutBytes)
{
    OutBytes.Reset();
    return FFileHelper::LoadFileToArray(OutBytes, *Path);
}

static FString DiskRestoreMarkerPath()
{
    return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("NodeToCode/ManualReplayRestore/pending_restore_case.json"));
}

static bool RunPreflightNoMutation(FAutomationTestBase& Test)
{
    const FString RunId = FString::Printf(TEXT("ManualPreflight_%lld"), FDateTime::UtcNow().GetTicks());
    FString AssetPath;
    UBlueprint* Blueprint = N2CP0RoundTripTests::CreateFixture(RunId, AssetPath);
    if (!Blueprint) { Test.AddError(TEXT("ManualReplay preflight fixture creation failed.")); return false; }

    FString BeforeHash, BeforeError;
    const bool bBefore = BuildSnapshot(Blueprint, BeforeHash, BeforeError);
    const FString Patch = FString::Printf(
        TEXT(R"json({"schema":"N2C_PATCH_V1","target_blueprint":"%s","actions":[{"type":"patch_graph","graph_name":"EventGraph","nodes":[{"id":"Macro","type":"K2Node_MacroInstance"}],"exec_edges":[],"data_edges":[]}]})json"),
        *Blueprint->GetName());
    FString Report;
    const bool bApplied = FN2CPatchImporter::ApplyPatchToBlueprint(Blueprint, Patch, Report, false, true);
    FString AfterHash, AfterError;
    const bool bAfter = BuildSnapshot(Blueprint, AfterHash, AfterError);
    const bool bCleanup = N2CP0SpecializedTests::CleanupGeneratedObject(AssetPath);
    const bool bOk = bBefore && bAfter && !bApplied && BeforeHash == AfterHash &&
        Report.Contains(TEXT("N2C_APPLY_BLOCKED_BEFORE_MUTATION")) && bCleanup;
    UE_LOG(LogNodeToCode, Display, TEXT("N2C_MANUAL_REPLAY_CASE|case=PreflightRejectsWithoutMutation|result=%s|before=%s|after=%s|cleanup=%d"),
        bOk ? TEXT("PASS") : TEXT("FAIL"), *BeforeHash, *AfterHash, bCleanup ? 1 : 0);
    if (!bOk) Test.AddError(FString::Printf(TEXT("Preflight no-mutation assertion failed: %s"), *Report));
    return bOk;
}

static bool RunSandboxPinPreflight(FAutomationTestBase& Test)
{
    const int32 PendingBefore = N2CVerificationTests_Private::CountPendingRestoreManifests();
    const FString RunId = FString::Printf(TEXT("ManualSandboxPins_%lld"), FDateTime::UtcNow().GetTicks());
    FString AssetPath;
    UBlueprint* Blueprint = N2CP0RoundTripTests::CreateFixture(RunId, AssetPath);
    if (!Blueprint) { Test.AddError(TEXT("Sandbox pin preflight fixture creation failed.")); return false; }

    FString BeforeHash, BeforeError;
    const bool bBefore = BuildSnapshot(Blueprint, BeforeHash, BeforeError);
    const FString BadPatch = FString::Printf(
        TEXT(R"json({"schema":"N2C_PATCH_V1","target_blueprint":"%s","actions":[{"type":"patch_graph","graph_name":"EventGraph","nodes":[{"id":"Self","type":"K2Node_Self"},{"id":"Knot","type":"K2Node_Knot"},{"id":"Cast","type":"K2Node_DynamicCast","class_path":"/Script/Engine.Actor"}],"exec_edges":[],"data_edges":[{"from_node_id":"Self","from_pin":"self","to_node_id":"Knot","to_pin":"DefinitelyMissingPin"},{"from_node_id":"Knot","from_pin":"OutputPin","to_node_id":"Cast","to_pin":"Object"}]}]})json"),
        *Blueprint->GetName());
    FString BadReport;
    const bool bBadApplied = FN2CPatchImporter::ApplyPatchToBlueprint(Blueprint, BadPatch, BadReport, false, false);
    FString AfterBadHash, AfterBadError;
    const bool bAfterBad = BuildSnapshot(Blueprint, AfterBadHash, AfterBadError);
    const bool bRejectedBeforeMutation = !bBadApplied && bBefore && bAfterBad && BeforeHash == AfterBadHash &&
        BadReport.Contains(TEXT("N2C_PREFLIGHT_GUARD|code=sandbox_apply_failed")) &&
        BadReport.Contains(TEXT("N2C_RUNTIME_GUARD|code=edge_pin_missing")) &&
        BadReport.Contains(TEXT("N2C_APPLY_BLOCKED_BEFORE_MUTATION"));

    const FString GoodPatch = FString::Printf(
        TEXT(R"json({"schema":"N2C_PATCH_V1","target_blueprint":"%s","actions":[{"type":"patch_graph","graph_name":"EventGraph","nodes":[{"id":"Self","type":"K2Node_Self"},{"id":"Knot","type":"K2Node_Knot"},{"id":"Cast","type":"K2Node_DynamicCast","class_path":"/Script/Engine.Actor"}],"exec_edges":[],"data_edges":[{"from_node_id":"Self","from_pin":"self","to_node_id":"Knot","to_pin":"InputPin"},{"from_node_id":"Knot","from_pin":"OutputPin","to_node_id":"Cast","to_pin":"Object"}]}]})json"),
        *Blueprint->GetName());
    FString GoodDryRunReport;
    const bool bGoodDryRun = bRejectedBeforeMutation && FN2CPatchImporter::DryRunPatch(Blueprint, GoodPatch, GoodDryRunReport, false);
    const bool bSandboxPassed = bGoodDryRun && GoodDryRunReport.Contains(TEXT("N2C_PREFLIGHT_SANDBOX_RESULT|result=PASS"));

    FString GoodApplyReport;
    const bool bGoodApplied = bSandboxPassed && FN2CPatchImporter::ApplyPatchToBlueprint(Blueprint, GoodPatch, GoodApplyReport, false, false);
    if (bGoodApplied)
    {
        FKismetEditorUtilities::CompileBlueprint(Blueprint);
    }
    const bool bCompile = bGoodApplied && Blueprint->Status != BS_Error;
    const bool bSaved = bCompile && N2CVerificationTests_Private::SaveAsset(Blueprint);

    UPackage* Package = Blueprint ? Blueprint->GetOutermost() : nullptr;
    if (Blueprint && GEditor)
    {
        GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->CloseAllEditorsForAsset(Blueprint);
        GEditor->ResetTransaction(NSLOCTEXT("NodeToCode", "SandboxPinPreflightReload", "Sandbox pin preflight reload"));
    }
    Blueprint = nullptr;
    bool bUnload = bSaved;
    if (Package && bUnload)
    {
        Package->SetDirtyFlag(false);
        TArray<UPackage*> Packages; Packages.Add(Package); FText Error;
        bUnload = UPackageTools::UnloadPackages(Packages, Error);
    }
    Package = nullptr;
    CollectGarbage(RF_NoFlags);

    UBlueprint* Reloaded = bUnload ? LoadObject<UBlueprint>(nullptr, *AssetPath) : nullptr;
    bool bPersistedLinks = false;
    if (Reloaded && Reloaded->UbergraphPages.Num() > 0)
    {
        UK2Node_Knot* Knot = nullptr;
        UEdGraphNode* SelfNode = nullptr;
        UEdGraphNode* CastNode = nullptr;
        for (UEdGraphNode* Node : Reloaded->UbergraphPages[0]->Nodes)
        {
            if (UK2Node_Knot* Candidate = Cast<UK2Node_Knot>(Node)) Knot = Candidate;
            else if (Node && Node->GetClass()->GetName() == TEXT("K2Node_Self")) SelfNode = Node;
            else if (Node && Node->GetClass()->GetName() == TEXT("K2Node_DynamicCast")) CastNode = Node;
        }
        UEdGraphPin* KnotInput = Knot ? Knot->FindPin(TEXT("InputPin")) : nullptr;
        UEdGraphPin* KnotOutput = Knot ? Knot->FindPin(TEXT("OutputPin")) : nullptr;
        UEdGraphPin* SelfOutput = SelfNode ? SelfNode->FindPin(TEXT("self")) : nullptr;
        UEdGraphPin* CastInput = CastNode ? CastNode->FindPin(TEXT("Object")) : nullptr;
        bPersistedLinks = KnotInput && KnotOutput && SelfOutput && CastInput &&
            KnotInput->LinkedTo.Contains(SelfOutput) && SelfOutput->LinkedTo.Contains(KnotInput) &&
            KnotOutput->LinkedTo.Contains(CastInput) && CastInput->LinkedTo.Contains(KnotOutput);
    }

    const bool bReloaded = Reloaded != nullptr;
    const bool bCleanup = N2CP0SpecializedTests::CleanupGeneratedObject(AssetPath);
    const int32 PendingAfter = N2CVerificationTests_Private::CountPendingRestoreManifests();
    const bool bNoRestoreQueued = PendingAfter == PendingBefore;
    const bool bOk = bRejectedBeforeMutation && bGoodDryRun && bSandboxPassed && bGoodApplied &&
        bCompile && bSaved && bUnload && bReloaded && bPersistedLinks && bNoRestoreQueued && bCleanup;
    UE_LOG(LogNodeToCode, Display, TEXT("N2C_MANUAL_REPLAY_CASE|case=SandboxPinPreflight|result=%s|bad_rejected=%d|good_dry_run=%d|sandbox_pass=%d|good_applied=%d|compile=%d|save=%d|unload=%d|persisted_links=%d|pending_before=%d|pending_after=%d|cleanup=%d"),
        bOk ? TEXT("PASS") : TEXT("FAIL"), bRejectedBeforeMutation ? 1 : 0, bGoodDryRun ? 1 : 0,
        bSandboxPassed ? 1 : 0, bGoodApplied ? 1 : 0, bCompile ? 1 : 0, bSaved ? 1 : 0,
        bUnload ? 1 : 0, bPersistedLinks ? 1 : 0, PendingBefore, PendingAfter, bCleanup ? 1 : 0);
    if (!bOk)
    {
        Test.AddError(FString::Printf(TEXT("Sandbox pin preflight failed. Bad report: %s Good dry run: %s Good apply: %s"), *BadReport, *GoodDryRunReport, *GoodApplyReport));
    }
    return bOk;
}

static bool RunVerifiedRollback(FAutomationTestBase& Test, bool bRequireStructuralHash)
{
    const FString Label = bRequireStructuralHash ? TEXT("RollbackStructuralEquality") : TEXT("RollbackAfterMutation");
    const FString RunId = FString::Printf(TEXT("Manual%s_%lld"), *Label, FDateTime::UtcNow().GetTicks());
    FString AssetPath;
    UBlueprint* Blueprint = N2CP0RoundTripTests::CreateFixture(RunId, AssetPath);
    if (!Blueprint) { Test.AddError(Label + TEXT(" fixture creation failed.")); return false; }

    FString BeforeHash, BeforeError;
    const bool bBefore = BuildSnapshot(Blueprint, BeforeHash, BeforeError);
    const FString Patch = FString::Printf(
        TEXT(R"json({"schema":"N2C_PATCH_V1","target_blueprint":"%s","automation_force_failure_after_mutation":true,"actions":[{"type":"add_member_variables","variables":[{"name":"N2C_ForcedMutation","type":"int","default_value":"19"}]}]})json"),
        *Blueprint->GetName());
    FString Report;
    const bool bApplied = FN2CPatchImporter::ApplyPatchToBlueprint(Blueprint, Patch, Report, false, true);
    FString AfterHash, AfterError;
    const bool bAfter = BuildSnapshot(Blueprint, AfterHash, AfterError);
    const bool bVariableRemoved = !HasVariable(Blueprint, TEXT("N2C_ForcedMutation"));
    const bool bRollbackReported = Report.Contains(TEXT("N2C_ROLLBACK_RESULT|result=PASS"));
    const bool bHashOk = !bRequireStructuralHash || (bBefore && bAfter && BeforeHash == AfterHash);
    const bool bCleanup = N2CP0SpecializedTests::CleanupGeneratedObject(AssetPath);
    const bool bOk = !bApplied && bRollbackReported && bVariableRemoved && bHashOk && bCleanup;
    UE_LOG(LogNodeToCode, Display, TEXT("N2C_MANUAL_REPLAY_CASE|case=%s|result=%s|rollback=%d|variable_removed=%d|hash_equal=%d|cleanup=%d"),
        *Label, bOk ? TEXT("PASS") : TEXT("FAIL"), bRollbackReported ? 1 : 0, bVariableRemoved ? 1 : 0,
        (bBefore && bAfter && BeforeHash == AfterHash) ? 1 : 0, bCleanup ? 1 : 0);
    if (!bOk) Test.AddError(FString::Printf(TEXT("%s failed: %s"), *Label, *Report));
    return bOk;
}

static bool RunRawByteRoundTripAndExport(FAutomationTestBase& Test)
{
    const FString RunId = FString::Printf(TEXT("RawByte_%lld"), FDateTime::UtcNow().GetTicks());
    FString AssetPath;
    UBlueprint* Blueprint = N2CP0RoundTripTests::CreateFixture(RunId, AssetPath);
    if (!Blueprint) { Test.AddError(TEXT("Raw Byte fixture creation failed.")); return false; }
    const FString Target = Blueprint->GetName();
    Blueprint = nullptr;
    CollectGarbage(RF_NoFlags);

    FN2CRoundTripVerificationRequest Request;
    Request.AssetPath = AssetPath;
    Request.GeneratedFixturePath = AssetPath;
    Request.RunId = RunId;
    Request.PatchIdentity = TEXT("RawByteDefaultReopenExport");
    Request.bAutomationOnly = true;
    Request.bCleanupGeneratedFixture = false;
    Request.FreshProcessTimeoutSeconds = 120;
    Request.PatchJson = FString::Printf(
        TEXT(R"json({"schema":"N2C_PATCH_V1","target_blueprint":"%s","actions":[{"type":"add_member_variables","variables":[{"name":"N2C_RawByte","type":"byte","default_value":"37"},{"name":"N2C_RotatorDefault","type":"struct","struct_path":"/Script/CoreUObject.Rotator","default_value":"(Pitch=10.0,Yaw=20.0,Roll=30.0)"}]}]})json"),
        *Target);
    Request.ExpectedContractJson = TEXT(R"json({"schema":"N2C_EXPECTED_CONTRACT_V1","required_variables":[{"name":"N2C_RawByte","container":0,"default":"37"},{"name":"N2C_RotatorDefault","container":0}]})json");

    FN2CRoundTripVerificationResult Result;
    const bool bRoundTrip = FN2CRoundTripVerification::RunParent(Request, Result) && Result.bPassed;
    UBlueprint* Reloaded = LoadObject<UBlueprint>(nullptr, *AssetPath);
    bool bCDORotatorDefault = false;
    FString StoredRotatorDefault;
    if (Reloaded)
    {
        for (const FBPVariableDescription& Variable : Reloaded->NewVariables)
        {
            if (Variable.VarName == TEXT("N2C_RotatorDefault"))
            {
                // UE4.27 may clear FBPVariableDescription::DefaultValue after compile and
                // persist the authoritative value on the generated-class CDO instead.
                StoredRotatorDefault = Variable.DefaultValue;
                break;
            }
        }

        if (Reloaded->GeneratedClass)
        {
            UObject* CDO = Reloaded->GeneratedClass->GetDefaultObject();
            FStructProperty* RotatorProperty = FindFProperty<FStructProperty>(Reloaded->GeneratedClass, TEXT("N2C_RotatorDefault"));
            if (CDO && RotatorProperty && RotatorProperty->Struct == TBaseStructure<FRotator>::Get())
            {
                const FRotator* RotatorValue = RotatorProperty->ContainerPtrToValuePtr<FRotator>(CDO);
                bCDORotatorDefault = RotatorValue &&
                    FMath::IsNearlyEqual(RotatorValue->Pitch, 10.0f) &&
                    FMath::IsNearlyEqual(RotatorValue->Yaw, 20.0f) &&
                    FMath::IsNearlyEqual(RotatorValue->Roll, 30.0f);
            }
        }
    }
    FString ExportJson, ExportError;
    const bool bExported = Reloaded && FN2CAIExport::BuildBlueprintAIJson(Reloaded, ExportJson, ExportError);
    bool bRawByteExport = false;
    bool bRotatorDefaultExport = false;
    FString ExportedCategory;
    FString ExportedSubtype;
    FString ExportedDefault;
    FString ExportedRotatorDefault;
    if (bExported)
    {
        TSharedPtr<FJsonObject> Root;
        const TArray<TSharedPtr<FJsonValue>>* Variables = nullptr;
        TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ExportJson);
        if (FJsonSerializer::Deserialize(Reader, Root) && Root.IsValid() && Root->TryGetArrayField(TEXT("variables"), Variables) && Variables)
        {
            for (const TSharedPtr<FJsonValue>& Value : *Variables)
            {
                const TSharedPtr<FJsonObject> Variable = Value.IsValid() ? Value->AsObject() : nullptr;
                FString Name, Default;
                const TSharedPtr<FJsonObject>* PinType = nullptr;
                if (!Variable.IsValid() || !Variable->TryGetStringField(TEXT("name"), Name) ||
                    !Variable->TryGetObjectField(TEXT("pin_type"), PinType) || !PinType || !PinType->IsValid()) continue;
                if (Name == TEXT("N2C_RawByte"))
                {
                    (*PinType)->TryGetStringField(TEXT("category"), ExportedCategory);
                    (*PinType)->TryGetStringField(TEXT("sub_category_object"), ExportedSubtype);
                    Variable->TryGetStringField(TEXT("default_value"), ExportedDefault);
                    bRawByteExport = ExportedCategory == UEdGraphSchema_K2::PC_Byte.ToString() && ExportedSubtype.IsEmpty() && ExportedDefault == TEXT("37");
                }
                else if (Name == TEXT("N2C_RotatorDefault"))
                {
                    FString Category;
                    FString Subtype;
                    (*PinType)->TryGetStringField(TEXT("category"), Category);
                    (*PinType)->TryGetStringField(TEXT("sub_category_object"), Subtype);
                    Variable->TryGetStringField(TEXT("default_value"), ExportedRotatorDefault);
                    bRotatorDefaultExport = Category == UEdGraphSchema_K2::PC_Struct.ToString() &&
                        Subtype.Contains(TEXT("Rotator")) && !ExportedRotatorDefault.IsEmpty();
                }
            }
        }
    }
    const bool bCleanup = N2CP0SpecializedTests::CleanupGeneratedObject(AssetPath);
    const bool bOk = bRoundTrip && bCDORotatorDefault && bExported && bRawByteExport && bRotatorDefaultExport && bCleanup;
    UE_LOG(LogNodeToCode, Display, TEXT("N2C_MANUAL_REPLAY_CASE|case=RawByteDefaultReopenExport|result=%s|roundtrip=%d|cdo_rotator=%d|descriptor_default=%s|export=%d|raw_byte=%d|rotator_default=%d|category=%s|subtype=%s|default=%s|rotator_export=%s|cleanup=%d"),
        bOk ? TEXT("PASS") : TEXT("FAIL"), bRoundTrip ? 1 : 0, bCDORotatorDefault ? 1 : 0, *StoredRotatorDefault,
        bExported ? 1 : 0, bRawByteExport ? 1 : 0, bRotatorDefaultExport ? 1 : 0, *ExportedCategory, *ExportedSubtype,
        *ExportedDefault, *ExportedRotatorDefault, bCleanup ? 1 : 0);
    if (!bOk) Test.AddError(FString::Printf(TEXT("Raw Byte/Rotator default round-trip/export failed: roundtrip=%d cdo_rotator=%d descriptor_default='%s' export=%d raw_category='%s' raw_subtype='%s' raw_default='%s' rotator_default='%s' export_error=%s report=%s"),
        bRoundTrip ? 1 : 0, bCDORotatorDefault ? 1 : 0, *StoredRotatorDefault, bExported ? 1 : 0,
        *ExportedCategory, *ExportedSubtype, *ExportedDefault, *ExportedRotatorDefault, *ExportError, *Result.Report));
    return bOk;
}

static bool RunInvalidStructMemberDefaultReject(FAutomationTestBase& Test)
{
    const FString RunId = FString::Printf(TEXT("InvalidStructDefault_%lld"), FDateTime::UtcNow().GetTicks());
    FString AssetPath;
    UBlueprint* Blueprint = N2CP0RoundTripTests::CreateFixture(RunId, AssetPath);
    if (!Blueprint) return false;
    const FString Target = Blueprint->GetName();
    Blueprint = nullptr;
    CollectGarbage(RF_NoFlags);

    FN2CRoundTripVerificationRequest Request;
    Request.AssetPath = AssetPath;
    Request.GeneratedFixturePath = AssetPath;
    Request.RunId = RunId;
    Request.PatchIdentity = TEXT("InvalidStructMemberDefaultReject");
    Request.bAutomationOnly = true;
    Request.bCleanupGeneratedFixture = true;
    Request.PatchJson = FString::Printf(
        TEXT(R"json({"schema":"N2C_PATCH_V1","target_blueprint":"%s","actions":[{"type":"add_member_variables","variables":[{"name":"N2C_InvalidRotator","type":"struct","struct_path":"/Script/CoreUObject.Rotator","default_value":"not-a-valid-rotator"}]}]})json"),
        *Target);
    Request.ExpectedContractJson = TEXT("{}");

    FN2CRoundTripVerificationResult Result;
    const bool bActual = FN2CRoundTripVerification::RunParent(Request, Result);
    const bool bOk = !bActual && !Result.bPassed && Result.FailedStage == TEXT("Preflight") &&
        Result.ErrorCode == TEXT("member_default_import_text_invalid") && Result.bCleanupPassed;
    UE_LOG(LogNodeToCode, Display, TEXT("N2C_MEMBER_DEFAULT_REJECT|result=%s|stage=%s|code=%s|cleanup=%d"),
        bOk ? TEXT("PASS") : TEXT("FAIL"), *Result.FailedStage, *Result.ErrorCode, Result.bCleanupPassed ? 1 : 0);
    if (!bOk) Test.AddError(FString::Printf(TEXT("Invalid struct member default rejection mismatch: %s"), *Result.Report));
    return bOk;
}

static bool RunMissingEnumReject(FAutomationTestBase& Test)
{
    const FString RunId = FString::Printf(TEXT("MissingEnum_%lld"), FDateTime::UtcNow().GetTicks());
    FString AssetPath;
    UBlueprint* Blueprint = N2CP0RoundTripTests::CreateFixture(RunId, AssetPath);
    if (!Blueprint) return false;
    const FString Target = Blueprint->GetName();
    Blueprint = nullptr;
    CollectGarbage(RF_NoFlags);
    FN2CRoundTripVerificationRequest Request;
    Request.AssetPath = AssetPath;
    Request.GeneratedFixturePath = AssetPath;
    Request.RunId = RunId;
    Request.PatchIdentity = TEXT("MissingEnumReject");
    Request.bAutomationOnly = true;
    Request.bCleanupGeneratedFixture = true;
    Request.PatchJson = FString::Printf(
        TEXT(R"json({"schema":"N2C_PATCH_V1","target_blueprint":"%s","actions":[{"type":"add_member_variables","variables":[{"name":"N2C_BrokenEnum","type":"enum","enum_path":"/Game/N2C_Test/DoesNotExist.DoesNotExist"}]}]})json"),
        *Target);
    Request.ExpectedContractJson = TEXT("{}");
    FN2CRoundTripVerificationResult Result;
    const bool bActual = FN2CRoundTripVerification::RunParent(Request, Result);
    const bool bOk = !bActual && !Result.bPassed && Result.FailedStage == TEXT("Preflight") &&
        Result.ErrorCode == TEXT("enum_member_type_unresolved") && Result.bCleanupPassed;
    UE_LOG(LogNodeToCode, Display, TEXT("N2C_MANUAL_REPLAY_CASE|case=MissingEnumReject|result=%s|stage=%s|code=%s|cleanup=%d"),
        bOk ? TEXT("PASS") : TEXT("FAIL"), *Result.FailedStage, *Result.ErrorCode, Result.bCleanupPassed ? 1 : 0);
    if (!bOk) Test.AddError(FString::Printf(TEXT("Missing enum rejection mismatch: %s"), *Result.Report));
    return bOk;
}

static bool RunDiskRestoreFirstPass(FAutomationTestBase& Test)
{
    const FString MarkerPath = DiskRestoreMarkerPath();
    const FString StatusPath = FPaths::Combine(
        FPaths::ProjectSavedDir(),
        TEXT("NodeToCode/Backups/PendingRestoreApplied/N2C_LAST_PENDING_RESTORE_STATUS.txt"));
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(MarkerPath), true);
    IFileManager::Get().Delete(*MarkerPath, false, true, true);
    IFileManager::Get().Delete(*StatusPath, false, true, true);

    const FString RunId = FString::Printf(TEXT("DiskRestore_%lld"), FDateTime::UtcNow().GetTicks());
    FString AssetPath;
    UBlueprint* Blueprint = N2CP0RoundTripTests::CreateFixture(RunId, AssetPath);
    if (!Blueprint) { Test.AddError(TEXT("Disk restore fixture creation failed.")); return false; }
    const FString PackageName = Blueprint->GetOutermost()->GetName();
    const FString TargetFilename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
    const FString BaselinePath = FPaths::Combine(FPaths::GetPath(MarkerPath), TEXT("baseline_before_mutation.uasset"));
    if (IFileManager::Get().Copy(*BaselinePath, *TargetFilename, true, true) != COPY_OK)
    {
        Test.AddError(TEXT("Could not copy disk restore baseline."));
        N2CP0SpecializedTests::CleanupGeneratedObject(AssetPath);
        return false;
    }

    FEdGraphPinType IntType;
    IntType.PinCategory = UEdGraphSchema_K2::PC_Int;
    const bool bAdded = FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("N2C_DiskRestoreMutation"), IntType, TEXT("73"));
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    FKismetEditorUtilities::CompileBlueprint(Blueprint);
    const bool bSaved = bAdded && N2CVerificationTests_Private::SaveAsset(Blueprint);
    TArray<uint8> BaselineBytes, MutatedBytes;
    const bool bDifferent = ReadBytes(BaselinePath, BaselineBytes) && ReadBytes(TargetFilename, MutatedBytes) && BaselineBytes != MutatedBytes;

    FString ManifestPath, QueueReport;
    const bool bQueued = bSaved && bDifferent && FN2CEditorIntegration::Get().QueueBackupRestoreForAutomation(Blueprint, BaselinePath, ManifestPath, QueueReport);
    const FString DonePath = FPaths::Combine(
        FPaths::ProjectSavedDir(),
        TEXT("NodeToCode/Backups/PendingRestoreApplied"),
        FPaths::GetBaseFilename(ManifestPath) + TEXT(".done"));

    TSharedPtr<FJsonObject> Marker = MakeShared<FJsonObject>();
    Marker->SetStringField(TEXT("schema"), TEXT("N2C_MANUAL_REPLAY_RESTORE_V1"));
    Marker->SetStringField(TEXT("asset_path"), AssetPath);
    Marker->SetStringField(TEXT("target_filename"), TargetFilename);
    Marker->SetStringField(TEXT("baseline_path"), BaselinePath);
    Marker->SetStringField(TEXT("manifest_path"), ManifestPath);
    Marker->SetStringField(TEXT("done_path"), DonePath);
    Marker->SetStringField(TEXT("status_path"), StatusPath);
    FString MarkerJson;
    const bool bSerialized = FJsonSerializer::Serialize(Marker.ToSharedRef(), TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&MarkerJson));
    const bool bMarkerSaved = bQueued && bSerialized && FFileHelper::SaveStringToFile(MarkerJson, *MarkerPath);
    const bool bOk = bSaved && bDifferent && bQueued && FPaths::FileExists(ManifestPath) && bMarkerSaved;
    UE_LOG(LogNodeToCode, Display, TEXT("N2C_MANUAL_REPLAY_CASE|case=DiskRestoreFirstPass|result=%s|mutated=%d|queued=%d|manifest=%s|marker=%s"),
        bOk ? TEXT("PASS") : TEXT("FAIL"), bDifferent ? 1 : 0, bQueued ? 1 : 0, *ManifestPath, *MarkerPath);
    if (!bOk)
    {
        Test.AddError(FString::Printf(TEXT("Disk restore first pass failed: %s"), *QueueReport));
        N2CP0SpecializedTests::CleanupGeneratedObject(AssetPath);
        IFileManager::Get().Delete(*BaselinePath, false, true, true);
        IFileManager::Get().Delete(*MarkerPath, false, true, true);
    }
    return bOk;
}

static bool RunDiskRestoreSecondPass(FAutomationTestBase& Test)
{
    const FString MarkerPath = DiskRestoreMarkerPath();
    FString MarkerJson;
    TSharedPtr<FJsonObject> Marker;
    if (!FFileHelper::LoadFileToString(MarkerJson, *MarkerPath))
    {
        Test.AddError(FString::Printf(TEXT("Second-pass marker missing: %s"), *MarkerPath));
        return false;
    }
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(MarkerJson);
    if (!FJsonSerializer::Deserialize(Reader, Marker) || !Marker.IsValid())
    {
        Test.AddError(TEXT("Second-pass marker JSON invalid."));
        return false;
    }
    FString Schema, AssetPath, TargetFilename, BaselinePath, ManifestPath, DonePath, StatusPath;
    Marker->TryGetStringField(TEXT("schema"), Schema);
    Marker->TryGetStringField(TEXT("asset_path"), AssetPath);
    Marker->TryGetStringField(TEXT("target_filename"), TargetFilename);
    Marker->TryGetStringField(TEXT("baseline_path"), BaselinePath);
    Marker->TryGetStringField(TEXT("manifest_path"), ManifestPath);
    Marker->TryGetStringField(TEXT("done_path"), DonePath);
    Marker->TryGetStringField(TEXT("status_path"), StatusPath);
    TArray<uint8> BaselineBytes, RestoredBytes;
    const bool bBytesMatch = ReadBytes(BaselinePath, BaselineBytes) && ReadBytes(TargetFilename, RestoredBytes) && BaselineBytes == RestoredBytes;
    const bool bManifestConsumed = !FPaths::FileExists(ManifestPath) && FPaths::FileExists(DonePath);
    FString StartupStatus;
    const bool bStartupStatusWritten = !StatusPath.IsEmpty() &&
        FFileHelper::LoadFileToString(StartupStatus, *StatusPath) &&
        StartupStatus.Contains(TEXT("FinalResult=PASS")) &&
        StartupStatus.Contains(TEXT("Applied=")) &&
        StartupStatus.Contains(TEXT("FailedAttempts=")) &&
        StartupStatus.Contains(TEXT("Pending=0"));
    UBlueprint* Restored = LoadObject<UBlueprint>(nullptr, *AssetPath);
    if (Restored) FKismetEditorUtilities::CompileBlueprint(Restored);
    const bool bCompile = Restored && Restored->Status != BS_Error;
    const bool bMutationGone = Restored && !HasVariable(Restored, TEXT("N2C_DiskRestoreMutation"));

    FString DoneText;
    FFileHelper::LoadFileToString(DoneText, *DonePath);
    FString PendingCopy, RollbackPath;
    TArray<FString> Lines;
    DoneText.ParseIntoArrayLines(Lines, true);
    for (const FString& Line : Lines)
    {
        FString Key, Value;
        if (Line.Split(TEXT("="), &Key, &Value))
        {
            if (Key == TEXT("PendingBackupCopy")) PendingCopy = Value;
            else if (Key == TEXT("RollbackPath")) RollbackPath = Value;
        }
    }

    const bool bCleanup = N2CP0SpecializedTests::CleanupGeneratedObject(AssetPath);
    IFileManager::Get().Delete(*BaselinePath, false, true, true);
    IFileManager::Get().Delete(*MarkerPath, false, true, true);
    if (!PendingCopy.IsEmpty()) IFileManager::Get().Delete(*PendingCopy, false, true, true);
    if (!RollbackPath.IsEmpty()) IFileManager::Get().Delete(*RollbackPath, false, true, true);
    IFileManager::Get().Delete(*DonePath, false, true, true);
    if (!StatusPath.IsEmpty()) IFileManager::Get().Delete(*StatusPath, false, true, true);
    const bool bOk = Schema == TEXT("N2C_MANUAL_REPLAY_RESTORE_V1") && bBytesMatch && bManifestConsumed &&
        bStartupStatusWritten && bCompile && bMutationGone && bCleanup;
    UE_LOG(LogNodeToCode, Display, TEXT("N2C_MANUAL_REPLAY_CASE|case=DiskRestoreSecondPass|result=%s|bytes=%d|consumed=%d|startup_status=%d|compile=%d|mutation_gone=%d|cleanup=%d"),
        bOk ? TEXT("PASS") : TEXT("FAIL"), bBytesMatch ? 1 : 0, bManifestConsumed ? 1 : 0,
        bStartupStatusWritten ? 1 : 0, bCompile ? 1 : 0, bMutationGone ? 1 : 0, bCleanup ? 1 : 0);
    if (!bOk) Test.AddError(TEXT("Deferred disk restore was not proven by the second UE process."));
    return bOk;
}

static bool RunDialogDiagnostics(FAutomationTestBase& Test)
{
    const FString Pin = FN2CEditorIntegration::Get().FormatDiagnosticForAutomation(TEXT("N2C_RUNTIME_GUARD|code=edge_pin_missing|from=A.X|to=B.Y"));
    const FString Rollback = FN2CEditorIntegration::Get().FormatDiagnosticForAutomation(TEXT("N2C_ROLLBACK_RESULT|result=FAIL|fallback=queued"));
    const FString Enum = FN2CEditorIntegration::Get().FormatDiagnosticForAutomation(TEXT("N2C_PREFLIGHT_GUARD|code=enum_member_type_unresolved|variable=Mode"));
    const FString Sandbox = FN2CEditorIntegration::Get().FormatDiagnosticForAutomation(TEXT("N2C_PREFLIGHT_GUARD|code=sandbox_apply_failed"));
    const bool bOk = Pin.Contains(TEXT("pin")) && Pin.Contains(TEXT("A.X")) && Rollback.Contains(TEXT("next UE startup")) &&
        !Enum.IsEmpty() && Sandbox.Contains(TEXT("not modified"));
    UE_LOG(LogNodeToCode, Display, TEXT("N2C_MANUAL_REPLAY_CASE|case=DialogDiagnostics|result=%s|pin=%s|rollback=%s|enum=%s|sandbox=%s"),
        bOk ? TEXT("PASS") : TEXT("FAIL"), *N2CVerificationTests_Private::OneLine(Pin), *N2CVerificationTests_Private::OneLine(Rollback),
        *N2CVerificationTests_Private::OneLine(Enum), *N2CVerificationTests_Private::OneLine(Sandbox));
    if (!bOk) Test.AddError(TEXT("Structured diagnostics are not mapped to deterministic user-facing text."));
    return bOk;
}

static bool RunToolbarCommands(FAutomationTestBase& Test)
{
    const FN2CToolbarCommand& Commands = FN2CToolbarCommand::Get();
    const bool bOk = Commands.ExportCommand.IsValid() && Commands.ImportCommand.IsValid() &&
        Commands.ProjectImportCommand.IsValid() && Commands.ExportAllCommand.IsValid() &&
        FN2CToolbarCommand::CommandName_Export == TEXT("NodeToCode_Back2Dead_Export") &&
        FN2CToolbarCommand::CommandName_Import == TEXT("NodeToCode_Back2Dead_Import") &&
        !FN2CToolbarCommand::CommandTooltip_Import.IsEmpty();
    UE_LOG(LogNodeToCode, Display, TEXT("N2C_MANUAL_REPLAY_CASE|case=ToolbarCommands|result=%s|export=%d|import=%d|project_import=%d|export_all=%d"),
        bOk ? TEXT("PASS") : TEXT("FAIL"), Commands.ExportCommand.IsValid() ? 1 : 0, Commands.ImportCommand.IsValid() ? 1 : 0,
        Commands.ProjectImportCommand.IsValid() ? 1 : 0, Commands.ExportAllCommand.IsValid() ? 1 : 0);
    if (!bOk) Test.AddError(TEXT("N2C toolbar command registration contract failed."));
    return bOk;
}

static bool FinishCase(const TCHAR* CaseName, bool bResult)
{
    UE_LOG(LogNodeToCode, Display, TEXT("N2C_MANUAL_REPLAY_CASE|case=%s|result=%s"), CaseName, bResult ? TEXT("PASS") : TEXT("FAIL"));
    return bResult;
}
}


namespace N2CImportContractMatrixTests
{
enum class EPhase
{
    Apply,
    VerifyFresh,
    Reapply,
    VerifyFreshSecond
};

static FString ManifestPath()
{
    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("NodeToCode"));
    return Plugin.IsValid()
        ? FPaths::Combine(Plugin->GetBaseDir(), TEXT("Source/Tests/Fixtures/N2C_IMPORT_CONTRACT_MATRIX_V1.json"))
        : FString();
}

static FString StateDirectory()
{
    return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("NodeToCode/ContractMatrix"));
}

static FString SafeId(FString Value)
{
    for (TCHAR& Character : Value)
    {
        if (!FChar::IsAlnum(Character) && Character != TEXT('_') && Character != TEXT('-'))
        {
            Character = TEXT('_');
        }
    }
    return Value;
}

static FString StatePath(const FString& CaseId)
{
    return FPaths::Combine(StateDirectory(), SafeId(CaseId) + TEXT(".json"));
}

static bool LoadManifest(TSharedPtr<FJsonObject>& OutRoot, FString& OutError)
{
    OutRoot.Reset();
    const FString Path = ManifestPath();
    FString Json;
    if (Path.IsEmpty() || !FFileHelper::LoadFileToString(Json, *Path))
    {
        OutError = FString::Printf(TEXT("Contract matrix manifest could not be read: %s"), *Path);
        return false;
    }
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
    if (!FJsonSerializer::Deserialize(Reader, OutRoot) || !OutRoot.IsValid())
    {
        OutError = FString::Printf(TEXT("Contract matrix manifest could not be parsed: %s"), *Path);
        return false;
    }
    FString Schema;
    double Version = 0.0;
    if (!OutRoot->TryGetStringField(TEXT("schema"), Schema) || Schema != TEXT("N2C_IMPORT_CONTRACT_MATRIX_V1") ||
        !OutRoot->TryGetNumberField(TEXT("version"), Version) || !IPluginManager::Get().FindPlugin(TEXT("NodeToCode")).IsValid() || static_cast<int32>(Version) != IPluginManager::Get().FindPlugin(TEXT("NodeToCode"))->GetDescriptor().Version)
    {
        OutError = FString::Printf(TEXT("Contract matrix schema/version mismatch: schema=%s version=%d"), *Schema, static_cast<int32>(Version));
        return false;
    }
    return true;
}

static bool GetCaseIds(TArray<FString>& OutIds, FString& OutError)
{
    OutIds.Reset();
    TSharedPtr<FJsonObject> Root;
    if (!LoadManifest(Root, OutError)) return false;
    const TArray<TSharedPtr<FJsonValue>>* Cases = nullptr;
    if (!Root->TryGetArrayField(TEXT("cases"), Cases) || !Cases)
    {
        OutError = TEXT("Contract matrix cases[] is missing.");
        return false;
    }
    TSet<FString> Seen;
    for (const TSharedPtr<FJsonValue>& Value : *Cases)
    {
        const TSharedPtr<FJsonObject> Obj = Value.IsValid() ? Value->AsObject() : nullptr;
        FString Id;
        if (!Obj.IsValid() || !Obj->TryGetStringField(TEXT("id"), Id) || Id.IsEmpty() || Seen.Contains(Id))
        {
            OutError = FString::Printf(TEXT("Contract matrix contains invalid/duplicate id: %s"), *Id);
            return false;
        }
        Seen.Add(Id);
        OutIds.Add(Id);
    }
    return OutIds.Num() > 0;
}

static bool FindCase(const FString& CaseId, TSharedPtr<FJsonObject>& OutCase, FString& OutError)
{
    OutCase.Reset();
    TSharedPtr<FJsonObject> Root;
    if (!LoadManifest(Root, OutError)) return false;
    const TArray<TSharedPtr<FJsonValue>>* Cases = nullptr;
    if (!Root->TryGetArrayField(TEXT("cases"), Cases) || !Cases)
    {
        OutError = TEXT("Contract matrix cases[] is missing.");
        return false;
    }
    for (const TSharedPtr<FJsonValue>& Value : *Cases)
    {
        const TSharedPtr<FJsonObject> Obj = Value.IsValid() ? Value->AsObject() : nullptr;
        FString Id;
        if (Obj.IsValid() && Obj->TryGetStringField(TEXT("id"), Id) && Id == CaseId)
        {
            OutCase = Obj;
            return true;
        }
    }
    OutError = FString::Printf(TEXT("Contract matrix case not found: %s"), *CaseId);
    return false;
}

static bool EnsureDependencies(FString& OutError)
{
    UUserDefinedStruct* RowStruct = N2CVerificationTests_Private::EnsureRowStruct();
    UDataTable* DataTable = N2CVerificationTests_Private::EnsureDataTable(RowStruct);
    UBlueprint* InterfaceBlueprint = N2CVerificationTests_Private::EnsureP3Interface();
    if (!RowStruct || !DataTable || !InterfaceBlueprint || !InterfaceBlueprint->GeneratedClass)
    {
        OutError = TEXT("Contract matrix shared struct/DataTable/interface dependency creation failed.");
        return false;
    }
    return true;
}

static UBlueprint* CreateFixture(const FString& CaseId, const FString& Kind, FString& OutAssetPath)
{
    const FString RunId = FString::Printf(TEXT("Contract_%s_%lld"), *SafeId(CaseId), FDateTime::UtcNow().GetTicks());
    if (Kind.Equals(TEXT("widget"), ESearchCase::IgnoreCase))
        return N2CP0CoreUIInputTests::CreateFixture(RunId, N2CP0CoreUIInputTests::EFixtureKind::Widget, OutAssetPath);
    if (Kind.Equals(TEXT("ai"), ESearchCase::IgnoreCase))
        return N2CP0CoreUIInputTests::CreateFixture(RunId, N2CP0CoreUIInputTests::EFixtureKind::AI, OutAssetPath);
    if (Kind.Equals(TEXT("bttask"), ESearchCase::IgnoreCase))
        return N2CP0CoreUIInputTests::CreateFixture(RunId, N2CP0CoreUIInputTests::EFixtureKind::BTTask, OutAssetPath);
    if (Kind.Equals(TEXT("btservice"), ESearchCase::IgnoreCase))
        return N2CP0CoreUIInputTests::CreateFixture(RunId, N2CP0CoreUIInputTests::EFixtureKind::BTService, OutAssetPath);
    if (Kind.Equals(TEXT("btdecorator"), ESearchCase::IgnoreCase))
        return N2CP0CoreUIInputTests::CreateFixture(RunId, N2CP0CoreUIInputTests::EFixtureKind::BTDecorator, OutAssetPath);
    return N2CP0RoundTripTests::CreateFixture(RunId, OutAssetPath);
}

static bool SerializeJsonObject(const TSharedPtr<FJsonObject>& Object, FString& OutJson)
{
    OutJson.Reset();
    return Object.IsValid() && FJsonSerializer::Serialize(Object.ToSharedRef(), TJsonWriterFactory<>::Create(&OutJson));
}

static void ReplaceTokens(FString& Json, UBlueprint* Blueprint)
{
    const FString TargetPath = Blueprint ? Blueprint->GetPathName() : FString();
    const FString TargetName = Blueprint ? Blueprint->GetName() : FString();
    const FString TargetClass = Blueprint && Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetPathName() : FString();
    Json.ReplaceInline(TEXT("${TARGET_PATH}"), *TargetPath, ESearchCase::CaseSensitive);
    Json.ReplaceInline(TEXT("${TARGET_NAME}"), *TargetName, ESearchCase::CaseSensitive);
    Json.ReplaceInline(TEXT("${TARGET_CLASS}"), *TargetClass, ESearchCase::CaseSensitive);
    Json.ReplaceInline(TEXT("${ROW_STRUCT}"), TEXT("/Game/N2C_Test/ST_N2C_Row.ST_N2C_Row"), ESearchCase::CaseSensitive);
    Json.ReplaceInline(TEXT("${DATA_TABLE}"), TEXT("/Game/N2C_Test/DT_N2C_Row.DT_N2C_Row"), ESearchCase::CaseSensitive);
    Json.ReplaceInline(TEXT("${INTERFACE_CLASS}"), TEXT("/Game/N2C_Test/BPI_N2C_P3.BPI_N2C_P3_C"), ESearchCase::CaseSensitive);
}

static bool PreparePatch(const TSharedPtr<FJsonObject>& CaseObj, const TCHAR* FieldName, UBlueprint* Blueprint, FString& OutPatch, FString& OutError)
{
    const TSharedPtr<FJsonObject>* PatchObj = nullptr;
    if (!CaseObj.IsValid() || !CaseObj->TryGetObjectField(FieldName, PatchObj) || !PatchObj || !PatchObj->IsValid())
    {
        OutError = FString::Printf(TEXT("Contract case does not contain %s."), FieldName);
        return false;
    }
    if (!SerializeJsonObject(*PatchObj, OutPatch))
    {
        OutError = FString::Printf(TEXT("Contract case %s could not be serialized."), FieldName);
        return false;
    }
    ReplaceTokens(OutPatch, Blueprint);
    return true;
}

static void StripUnstableSnapshotFields(const TSharedPtr<FJsonObject>& Object)
{
    if (!Object.IsValid())
    {
        return;
    }

    static const TCHAR* UnstableFields[] = {
        TEXT("node_guid"),
        TEXT("persistent_guid"),
        TEXT("entry_tunnel_guid"),
        TEXT("exit_tunnel_guid")
    };
    for (const TCHAR* FieldName : UnstableFields)
    {
        Object->Values.Remove(FieldName);
    }

    const TArray<TSharedPtr<FJsonValue>>* Links = nullptr;
    if (Object->TryGetArrayField(TEXT("links"), Links) && Links && Links->Num() > 0)
    {
        Object->Values.Remove(TEXT("default_value"));
        Object->Values.Remove(TEXT("default_object"));
        Object->Values.Remove(TEXT("default_text"));
    }

    for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
    {
        if (!Pair.Value.IsValid())
        {
            continue;
        }
        if (Pair.Value->Type == EJson::Object)
        {
            StripUnstableSnapshotFields(Pair.Value->AsObject());
        }
        else if (Pair.Value->Type == EJson::Array)
        {
            for (const TSharedPtr<FJsonValue>& Child : Pair.Value->AsArray())
            {
                if (Child.IsValid() && Child->Type == EJson::Object)
                {
                    StripUnstableSnapshotFields(Child->AsObject());
                }
            }
        }
    }
}

static bool BuildHashes(
    UBlueprint* Blueprint,
    FString& OutExactHash,
    FString& OutSemanticHash,
    FString& OutError,
    FString* OutExactSnapshotJson = nullptr,
    FString* OutSemanticSnapshotJson = nullptr)
{
    TSharedPtr<FJsonObject> Snapshot;
    if (!FN2CStructuralSnapshot::Build(Blueprint, Snapshot, OutExactHash, OutError) || !Snapshot.IsValid())
    {
        return false;
    }

    FString SnapshotJson;
    if (!SerializeJsonObject(Snapshot, SnapshotJson))
    {
        OutError = TEXT("Contract matrix snapshot serialization failed.");
        return false;
    }
    if (OutExactSnapshotJson)
    {
        *OutExactSnapshotJson = SnapshotJson;
    }

    TSharedPtr<FJsonObject> SemanticSnapshot;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(SnapshotJson);
    if (!FJsonSerializer::Deserialize(Reader, SemanticSnapshot) || !SemanticSnapshot.IsValid())
    {
        OutError = TEXT("Contract matrix semantic snapshot clone failed.");
        return false;
    }
    StripUnstableSnapshotFields(SemanticSnapshot);
    if (OutSemanticSnapshotJson && !SerializeJsonObject(SemanticSnapshot, *OutSemanticSnapshotJson))
    {
        OutError = TEXT("Contract matrix semantic snapshot serialization failed.");
        return false;
    }
    return FN2CStructuralSnapshot::HashJson(SemanticSnapshot, OutSemanticHash, OutError);
}

static bool SaveState(const FString& CaseId, const FString& AssetPath, const FString& ExactHash, const FString& SemanticHash, const FString& ExactSnapshotJson, const FString& SemanticSnapshotJson, bool bExpectedApply, FString& OutError)
{
    TSharedPtr<FJsonObject> State = MakeShared<FJsonObject>();
    State->SetStringField(TEXT("schema"), TEXT("N2C_IMPORT_CONTRACT_STATE_V2"));
    State->SetStringField(TEXT("case_id"), CaseId);
    State->SetStringField(TEXT("asset_path"), AssetPath);
    State->SetStringField(TEXT("structural_hash"), ExactHash);
    State->SetStringField(TEXT("semantic_hash"), SemanticHash);
    State->SetStringField(TEXT("structural_snapshot"), ExactSnapshotJson);
    State->SetStringField(TEXT("semantic_snapshot"), SemanticSnapshotJson);
    State->SetBoolField(TEXT("expected_apply"), bExpectedApply);
    FString Json;
    if (!SerializeJsonObject(State, Json))
    {
        OutError = TEXT("Contract state serialization failed.");
        return false;
    }
    IFileManager::Get().MakeDirectory(*StateDirectory(), true);
    if (!FFileHelper::SaveStringToFile(Json, *StatePath(CaseId)))
    {
        OutError = FString::Printf(TEXT("Contract state write failed: %s"), *StatePath(CaseId));
        return false;
    }
    return true;
}

static bool LoadState(const FString& CaseId, FString& OutAssetPath, FString& OutExactHash, FString& OutSemanticHash, bool& bOutExpectedApply, FString& OutError)
{
    FString Json;
    TSharedPtr<FJsonObject> State;
    if (!FFileHelper::LoadFileToString(Json, *StatePath(CaseId)))
    {
        OutError = FString::Printf(TEXT("Contract state missing: %s"), *StatePath(CaseId));
        return false;
    }
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
    FString Schema;
    if (!FJsonSerializer::Deserialize(Reader, State) || !State.IsValid() ||
        !State->TryGetStringField(TEXT("schema"), Schema) || Schema != TEXT("N2C_IMPORT_CONTRACT_STATE_V2") ||
        !State->TryGetStringField(TEXT("asset_path"), OutAssetPath) ||
        !State->TryGetStringField(TEXT("structural_hash"), OutExactHash) ||
        !State->TryGetStringField(TEXT("semantic_hash"), OutSemanticHash) ||
        !State->TryGetBoolField(TEXT("expected_apply"), bOutExpectedApply))
    {
        OutError = FString::Printf(TEXT("Contract state invalid: %s"), *StatePath(CaseId));
        return false;
    }
    return true;
}

static void GetAllGraphs(UBlueprint* Blueprint, TArray<UEdGraph*>& OutGraphs)
{
    OutGraphs.Reset();
    if (Blueprint) Blueprint->GetAllGraphs(OutGraphs);
}

static bool HasNodeClass(UBlueprint* Blueprint, const FString& ClassName)
{
    TArray<UEdGraph*> Graphs;
    GetAllGraphs(Blueprint, Graphs);
    for (UEdGraph* Graph : Graphs)
    {
        if (!Graph) continue;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (Node && Node->GetClass()->GetName() == ClassName) return true;
        }
    }
    return false;
}

static bool HasCustomEvent(UBlueprint* Blueprint, const FString& EventName)
{
    TArray<UEdGraph*> Graphs;
    GetAllGraphs(Blueprint, Graphs);
    for (UEdGraph* Graph : Graphs)
    {
        if (!Graph) continue;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (const UK2Node_CustomEvent* Event = Cast<UK2Node_CustomEvent>(Node))
            {
                if (Event->CustomFunctionName.ToString() == EventName) return true;
            }
        }
    }
    return false;
}

static bool HasGraph(UBlueprint* Blueprint, const FString& GraphName)
{
    TArray<UEdGraph*> Graphs;
    GetAllGraphs(Blueprint, Graphs);
    return Graphs.ContainsByPredicate([&](const UEdGraph* Graph){ return Graph && Graph->GetName() == GraphName; });
}

static bool HasFunction(UBlueprint* Blueprint, const FString& FunctionName)
{
    return Blueprint && Blueprint->FunctionGraphs.ContainsByPredicate([&](const UEdGraph* Graph){ return Graph && Graph->GetName() == FunctionName; });
}

static bool HasMacro(UBlueprint* Blueprint, const FString& MacroName)
{
    return Blueprint && Blueprint->MacroGraphs.ContainsByPredicate([&](const UEdGraph* Graph){ return Graph && Graph->GetName() == MacroName; });
}

static bool HasDispatcher(UBlueprint* Blueprint, const FString& DispatcherName)
{
    if (!Blueprint) return false;
    return Blueprint->NewVariables.ContainsByPredicate([&](const FBPVariableDescription& Variable)
    {
        return Variable.VarName.ToString() == DispatcherName &&
            Variable.VarType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate;
    });
}

static bool HasVariable(UBlueprint* Blueprint, const FString& VariableName)
{
    if (!Blueprint) return false;
    return Blueprint->NewVariables.ContainsByPredicate([&](const FBPVariableDescription& Variable){ return Variable.VarName.ToString() == VariableName; });
}

static bool ExportVariableDefault(UBlueprint* Blueprint, const FString& VariableName, FString& OutValue)
{
    OutValue.Reset();
    if (!Blueprint || !Blueprint->GeneratedClass) return false;
    UObject* CDO = Blueprint->GeneratedClass->GetDefaultObject();
    FProperty* Property = FindFProperty<FProperty>(Blueprint->GeneratedClass, FName(*VariableName));
    if (!CDO || !Property) return false;
    void* Address = Property->ContainerPtrToValuePtr<void>(CDO);
    Property->ExportTextItem(OutValue, Address, Address, CDO, PPF_SerializedAsImportText);
    return true;
}

static bool HasSCSComponent(UBlueprint* Blueprint, const FString& ComponentName)
{
    if (!Blueprint || !Blueprint->SimpleConstructionScript) return false;
    for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
        if (Node && Node->GetVariableName().ToString() == ComponentName) return true;
    return false;
}

static bool HasInterface(UBlueprint* Blueprint, FString InterfacePath)
{
    if (!Blueprint) return false;
    ReplaceTokens(InterfacePath, Blueprint);
    for (const FBPInterfaceDescription& Description : Blueprint->ImplementedInterfaces)
        if (Description.Interface && Description.Interface->GetPathName() == InterfacePath) return true;
    return false;
}

static bool AssertStringArray(
    FAutomationTestBase& Test,
    const TSharedPtr<FJsonObject>& Expected,
    const TCHAR* Field,
    TFunction<bool(const FString&)> Predicate,
    bool bExpectedPresent)
{
    const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
    if (!Expected.IsValid() || !Expected->TryGetArrayField(Field, Values) || !Values) return true;
    bool bOk = true;
    for (const TSharedPtr<FJsonValue>& Value : *Values)
    {
        const FString Token = Value.IsValid() ? Value->AsString() : FString();
        const bool bFound = Predicate(Token);
        if (bFound != bExpectedPresent)
        {
            bOk = false;
            Test.AddError(FString::Printf(TEXT("Contract expectation failed: %s %s '%s'."), Field, bExpectedPresent ? TEXT("missing") : TEXT("unexpected"), *Token));
        }
    }
    return bOk;
}

static bool AssertExpected(FAutomationTestBase& Test, UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& CaseObj)
{
    const TSharedPtr<FJsonObject>* ExpectedPtr = nullptr;
    if (!CaseObj.IsValid() || !CaseObj->TryGetObjectField(TEXT("expected"), ExpectedPtr) || !ExpectedPtr || !ExpectedPtr->IsValid()) return true;
    const TSharedPtr<FJsonObject> Expected = *ExpectedPtr;
    bool bOk = true;

    const TArray<TSharedPtr<FJsonValue>>* Variables = nullptr;
    if (Expected->TryGetArrayField(TEXT("variables_present"), Variables) && Variables)
    {
        for (const TSharedPtr<FJsonValue>& Value : *Variables)
        {
            const TSharedPtr<FJsonObject> Var = Value.IsValid() ? Value->AsObject() : nullptr;
            FString Name;
            if (!Var.IsValid() || !Var->TryGetStringField(TEXT("name"), Name) || !HasVariable(Blueprint, Name))
            {
                bOk = false; Test.AddError(FString::Printf(TEXT("Expected variable missing: %s"), *Name)); continue;
            }
            FString Contains;
            if (Var->TryGetStringField(TEXT("default_contains"), Contains) && !Contains.IsEmpty())
            {
                FString Exported;
                if (!ExportVariableDefault(Blueprint, Name, Exported) || !Exported.Contains(Contains))
                {
                    bOk = false; Test.AddError(FString::Printf(TEXT("Variable default mismatch: %s expected token '%s' actual '%s'."), *Name, *Contains, *Exported));
                }
            }
        }
    }

    bOk &= AssertStringArray(Test, Expected, TEXT("variables_absent"), [&](const FString& V){ return HasVariable(Blueprint, V); }, false);
    bOk &= AssertStringArray(Test, Expected, TEXT("functions_present"), [&](const FString& V){ return HasFunction(Blueprint, V); }, true);
    bOk &= AssertStringArray(Test, Expected, TEXT("functions_absent"), [&](const FString& V){ return HasFunction(Blueprint, V); }, false);
    bOk &= AssertStringArray(Test, Expected, TEXT("macros_present"), [&](const FString& V){ return HasMacro(Blueprint, V); }, true);
    bOk &= AssertStringArray(Test, Expected, TEXT("macros_absent"), [&](const FString& V){ return HasMacro(Blueprint, V); }, false);
    bOk &= AssertStringArray(Test, Expected, TEXT("dispatchers_present"), [&](const FString& V){ return HasDispatcher(Blueprint, V); }, true);
    bOk &= AssertStringArray(Test, Expected, TEXT("dispatchers_absent"), [&](const FString& V){ return HasDispatcher(Blueprint, V); }, false);
    bOk &= AssertStringArray(Test, Expected, TEXT("node_classes_present"), [&](const FString& V){ return HasNodeClass(Blueprint, V); }, true);
    bOk &= AssertStringArray(Test, Expected, TEXT("graphs_present"), [&](const FString& V){ return HasGraph(Blueprint, V); }, true);
    bOk &= AssertStringArray(Test, Expected, TEXT("custom_events_present"), [&](const FString& V){ return HasCustomEvent(Blueprint, V); }, true);
    bOk &= AssertStringArray(Test, Expected, TEXT("custom_events_absent"), [&](const FString& V){ return HasCustomEvent(Blueprint, V); }, false);
    bOk &= AssertStringArray(Test, Expected, TEXT("scs_components_present"), [&](const FString& V){ return HasSCSComponent(Blueprint, V); }, true);
    bOk &= AssertStringArray(Test, Expected, TEXT("interfaces_present"), [&](const FString& V){ return HasInterface(Blueprint, V); }, true);
    return bOk;
}

static bool ReportContainsExpected(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& CaseObj, const FString& Report)
{
    const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
    if (!CaseObj.IsValid() || !CaseObj->TryGetArrayField(TEXT("expected_report_contains"), Values) || !Values) return true;
    bool bOk = true;
    for (const TSharedPtr<FJsonValue>& Value : *Values)
    {
        const FString Token = Value.IsValid() ? Value->AsString() : FString();
        if (!Token.IsEmpty() && !Report.Contains(Token))
        {
            bOk = false;
            Test.AddError(FString::Printf(TEXT("Contract report missing token '%s': %s"), *Token, *N2CVerificationTests_Private::OneLine(Report)));
        }
    }
    return bOk;
}

static bool RunApply(FAutomationTestBase& Test, const FString& CaseId, const TSharedPtr<FJsonObject>& CaseObj)
{
    FString DependencyError;
    if (!EnsureDependencies(DependencyError)) { Test.AddError(DependencyError); return false; }

    FString OldAssetPath, OldExactHash, OldSemanticHash, StateError;
    bool bOldExpected = false;
    if (LoadState(CaseId, OldAssetPath, OldExactHash, OldSemanticHash, bOldExpected, StateError) && !OldAssetPath.IsEmpty())
    {
        N2CP0SpecializedTests::CleanupGeneratedObject(OldAssetPath);
    }
    IFileManager::Get().Delete(*StatePath(CaseId), false, true, true);

    FString Kind = TEXT("actor");
    CaseObj->TryGetStringField(TEXT("fixture_kind"), Kind);
    FString AssetPath;
    UBlueprint* Blueprint = CreateFixture(CaseId, Kind, AssetPath);
    if (!Blueprint) { Test.AddError(FString::Printf(TEXT("Contract fixture creation failed: %s"), *CaseId)); return false; }

    const int32 PendingBefore = N2CVerificationTests_Private::CountPendingRestoreManifests();
    const TSharedPtr<FJsonObject>* SetupObj = nullptr;
    if (CaseObj->TryGetObjectField(TEXT("setup_patch"), SetupObj) && SetupObj && SetupObj->IsValid())
    {
        FString SetupPatch, SetupError, SetupReport;
        if (!PreparePatch(CaseObj, TEXT("setup_patch"), Blueprint, SetupPatch, SetupError) ||
            !FN2CPatchImporter::ApplyPatchToBlueprint(Blueprint, SetupPatch, SetupReport, false, true) ||
            Blueprint->Status == BS_Error || !N2CVerificationTests_Private::SaveAsset(Blueprint))
        {
            Test.AddError(FString::Printf(TEXT("Contract setup failed [%s]: %s %s"), *CaseId, *SetupError, *SetupReport));
            return false;
        }
    }

    FString BeforeHash, BeforeSemanticHash, BeforeError;
    if (!BuildHashes(Blueprint, BeforeHash, BeforeSemanticHash, BeforeError)) { Test.AddError(BeforeError); return false; }
    FString Patch, PatchError, Report;
    if (!PreparePatch(CaseObj, TEXT("patch"), Blueprint, Patch, PatchError)) { Test.AddError(PatchError); return false; }
    bool bExpectedApply = true;
    CaseObj->TryGetBoolField(TEXT("expected_apply"), bExpectedApply);
    const bool bApplied = FN2CPatchImporter::ApplyPatchToBlueprint(Blueprint, Patch, Report, false, true);
    FString AfterHash, AfterSemanticHash, AfterError, AfterSnapshotJson, AfterSemanticSnapshotJson;
    const bool bHash = BuildHashes(Blueprint, AfterHash, AfterSemanticHash, AfterError, &AfterSnapshotJson, &AfterSemanticSnapshotJson);
    const bool bCompile = Blueprint->Status != BS_Error;
    const bool bPendingClean = N2CVerificationTests_Private::CountPendingRestoreManifests() == PendingBefore;
    bool bOk = bHash && bCompile && bPendingClean && ReportContainsExpected(Test, CaseObj, Report);
    if (bExpectedApply)
    {
        bOk &= bApplied && N2CVerificationTests_Private::SaveAsset(Blueprint) && AssertExpected(Test, Blueprint, CaseObj);
        if (!bApplied) Test.AddError(FString::Printf(TEXT("Positive contract apply failed [%s]: %s"), *CaseId, *Report));
    }
    else
    {
        bOk &= !bApplied && BeforeHash == AfterHash;
        if (bApplied || BeforeHash != AfterHash) Test.AddError(FString::Printf(TEXT("Negative contract mutated or applied [%s]: %s"), *CaseId, *Report));
    }
    bOk &= SaveState(CaseId, AssetPath, AfterHash, AfterSemanticHash, AfterSnapshotJson, AfterSemanticSnapshotJson, bExpectedApply, StateError);
    if (!StateError.IsEmpty() && !bOk) Test.AddError(StateError);
    return bOk;
}

static bool LoadAndVerify(FAutomationTestBase& Test, const FString& CaseId, const TSharedPtr<FJsonObject>& CaseObj, bool bCleanup)
{
    FString AssetPath, ExpectedHash, ExpectedSemanticHash, StateError;
    bool bExpectedApply = false;
    if (!LoadState(CaseId, AssetPath, ExpectedHash, ExpectedSemanticHash, bExpectedApply, StateError)) { Test.AddError(StateError); return false; }
    UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
    if (!Blueprint) { Test.AddError(FString::Printf(TEXT("Fresh process could not load contract asset: %s"), *AssetPath)); return false; }
    FKismetEditorUtilities::CompileBlueprint(Blueprint);
    FString ActualHash, ActualSemanticHash, HashError, ActualSnapshotJson, ActualSemanticSnapshotJson;
    const bool bHashesBuilt = BuildHashes(Blueprint, ActualHash, ActualSemanticHash, HashError, &ActualSnapshotJson, &ActualSemanticSnapshotJson);
    bool bOk = Blueprint->Status != BS_Error && bHashesBuilt &&
        ActualHash == ExpectedHash && ActualSemanticHash == ExpectedSemanticHash && AssertExpected(Test, Blueprint, CaseObj);
    if (!bOk)
    {
        const FString DiagnosticDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("NodeToCode"), TEXT("Diagnostics"), TEXT("ContractMatrix"));
        IFileManager::Get().MakeDirectory(*DiagnosticDirectory, true);
        FFileHelper::SaveStringToFile(ActualSnapshotJson, *FPaths::Combine(DiagnosticDirectory, SafeId(CaseId) + TEXT("_actual.json")));
        FFileHelper::SaveStringToFile(ActualSemanticSnapshotJson, *FPaths::Combine(DiagnosticDirectory, SafeId(CaseId) + TEXT("_actual_semantic.json")));
        Test.AddError(FString::Printf(
            TEXT("Fresh contract verification failed [%s] expected_hash=%s actual_hash=%s expected_semantic=%s actual_semantic=%s error=%s"),
            *CaseId, *ExpectedHash, *ActualHash, *ExpectedSemanticHash, *ActualSemanticHash, *HashError));
    }
    if (bCleanup && bOk)
    {
        Blueprint = nullptr;
        CollectGarbage(RF_NoFlags);
        bOk &= N2CP0SpecializedTests::CleanupGeneratedObject(AssetPath);
        IFileManager::Get().Delete(*StatePath(CaseId), false, true, true);
    }
    return bOk;
}

static bool RunReapply(FAutomationTestBase& Test, const FString& CaseId, const TSharedPtr<FJsonObject>& CaseObj)
{
    FString AssetPath, ExpectedHash, ExpectedSemanticHash, StateError;
    bool bExpectedApply = false;
    if (!LoadState(CaseId, AssetPath, ExpectedHash, ExpectedSemanticHash, bExpectedApply, StateError)) { Test.AddError(StateError); return false; }
    UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
    if (!Blueprint) { Test.AddError(FString::Printf(TEXT("Reapply could not load contract asset: %s"), *AssetPath)); return false; }
    FKismetEditorUtilities::CompileBlueprint(Blueprint);
    FString BeforeHash, BeforeSemanticHash, BeforeError;
    if (Blueprint->Status == BS_Error || !BuildHashes(Blueprint, BeforeHash, BeforeSemanticHash, BeforeError) ||
        BeforeHash != ExpectedHash || BeforeSemanticHash != ExpectedSemanticHash)
    {
        Test.AddError(FString::Printf(TEXT("Reapply baseline mismatch [%s]."), *CaseId)); return false;
    }
    FString Patch, PatchError, Report;
    if (!PreparePatch(CaseObj, TEXT("patch"), Blueprint, Patch, PatchError)) { Test.AddError(PatchError); return false; }
    const int32 PendingBefore = N2CVerificationTests_Private::CountPendingRestoreManifests();
    const bool bApplied = FN2CPatchImporter::ApplyPatchToBlueprint(Blueprint, Patch, Report, false, true);
    FString AfterHash, AfterSemanticHash, AfterError, AfterSnapshotJson, AfterSemanticSnapshotJson;
    bool bOk = Blueprint->Status != BS_Error && BuildHashes(Blueprint, AfterHash, AfterSemanticHash, AfterError, &AfterSnapshotJson, &AfterSemanticSnapshotJson) &&
        BeforeSemanticHash == AfterSemanticHash &&
        N2CVerificationTests_Private::CountPendingRestoreManifests() == PendingBefore && ReportContainsExpected(Test, CaseObj, Report);
    bOk &= bExpectedApply ? bApplied : !bApplied;
    if (bExpectedApply) bOk &= N2CVerificationTests_Private::SaveAsset(Blueprint) && AssertExpected(Test, Blueprint, CaseObj);
    if (!bOk) Test.AddError(FString::Printf(TEXT("Idempotent reapply failed [%s]: %s before=%s after=%s"), *CaseId, *Report, *BeforeHash, *AfterHash));
    bOk &= SaveState(CaseId, AssetPath, AfterHash, AfterSemanticHash, AfterSnapshotJson, AfterSemanticSnapshotJson, bExpectedApply, StateError);
    return bOk;
}

static bool RunPhase(FAutomationTestBase& Test, EPhase Phase, const FString& CaseId)
{
    TSharedPtr<FJsonObject> CaseObj;
    FString Error;
    bool bOk = FindCase(CaseId, CaseObj, Error);
    if (!bOk) Test.AddError(Error);
    else if (Phase == EPhase::Apply) bOk = RunApply(Test, CaseId, CaseObj);
    else if (Phase == EPhase::VerifyFresh) bOk = LoadAndVerify(Test, CaseId, CaseObj, false);
    else if (Phase == EPhase::Reapply) bOk = RunReapply(Test, CaseId, CaseObj);
    else bOk = LoadAndVerify(Test, CaseId, CaseObj, true);
    const TCHAR* PhaseName = Phase == EPhase::Apply ? TEXT("Apply") : Phase == EPhase::VerifyFresh ? TEXT("VerifyFresh") : Phase == EPhase::Reapply ? TEXT("Reapply") : TEXT("VerifyFreshSecond");
    UE_LOG(LogNodeToCode, Display, TEXT("N2C_IMPORT_CONTRACT|phase=%s|case=%s|result=%s"), PhaseName, *CaseId, bOk ? TEXT("PASS") : TEXT("FAIL"));
    UE_LOG(LogNodeToCode, Display, TEXT("N2C_MANUAL_REPLAY_CASE|case=%s|result=%s"), *CaseId, bOk ? TEXT("PASS") : TEXT("FAIL"));
    return bOk;
}

static void EnumerateTests(TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands)
{
    FString Error;
    TArray<FString> Ids;
    if (!GetCaseIds(Ids, Error)) return;
    for (const FString& Id : Ids)
    {
        OutBeautifiedNames.Add(Id);
        OutTestCommands.Add(Id);
    }
}

static bool CleanupContractObject(const FString& ObjectPath)
{
    const bool bGeneratedContract = ObjectPath.StartsWith(TEXT("/Game/N2C_Test/Generated/BP_N2C_Contract_"));
    const bool bSharedDependency =
        ObjectPath == TEXT("/Game/N2C_Test/DT_N2C_Row.DT_N2C_Row") ||
        ObjectPath == TEXT("/Game/N2C_Test/ST_N2C_Row.ST_N2C_Row") ||
        ObjectPath == TEXT("/Game/N2C_Test/BPI_N2C_P3.BPI_N2C_P3");
    if (!bGeneratedContract && !bSharedDependency)
    {
        return false;
    }

    UObject* Object = LoadObject<UObject>(nullptr, *ObjectPath);
    UPackage* Package = Object ? Object->GetOutermost() : nullptr;
    if (Object)
    {
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().AssetDeleted(Object);
    }
    Object = nullptr;
    if (Package)
    {
        Package->SetDirtyFlag(false);
        TArray<UPackage*> Packages;
        Packages.Add(Package);
        FText Error;
        if (!UPackageTools::UnloadPackages(Packages, Error))
        {
            return false;
        }
    }
    Package = nullptr;
    CollectGarbage(RF_NoFlags);

    FString PackageName = ObjectPath;
    int32 Dot = INDEX_NONE;
    if (PackageName.FindChar(TEXT('.'), Dot))
    {
        PackageName = PackageName.Left(Dot);
    }
    const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
    return !IFileManager::Get().FileExists(*Filename) || IFileManager::Get().Delete(*Filename, false, true, true);
}

static bool Cleanup(FAutomationTestBase& Test)
{
    TArray<FString> StateFiles;
    IFileManager::Get().FindFiles(StateFiles, *FPaths::Combine(StateDirectory(), TEXT("*.json")), true, false);
    bool bOk = true;
    for (const FString& File : StateFiles)
    {
        FString Json;
        TSharedPtr<FJsonObject> State;
        if (FFileHelper::LoadFileToString(Json, *FPaths::Combine(StateDirectory(), File)))
        {
            TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
            FString AssetPath;
            if (FJsonSerializer::Deserialize(Reader, State) && State.IsValid() && State->TryGetStringField(TEXT("asset_path"), AssetPath))
                bOk &= CleanupContractObject(AssetPath);
        }
    }

    TArray<FAssetData> GeneratedAssets;
    FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().GetAssetsByPath(
        FName(TEXT("/Game/N2C_Test/Generated")), GeneratedAssets, true);
    for (const FAssetData& Asset : GeneratedAssets)
    {
        if (Asset.AssetName.ToString().StartsWith(TEXT("BP_N2C_Contract_")))
        {
            bOk &= CleanupContractObject(Asset.ObjectPath.ToString());
        }
    }

    IFileManager::Get().DeleteDirectory(*StateDirectory(), false, true);
    const TArray<FString> Dependencies = {
        TEXT("/Game/N2C_Test/DT_N2C_Row.DT_N2C_Row"),
        TEXT("/Game/N2C_Test/ST_N2C_Row.ST_N2C_Row"),
        TEXT("/Game/N2C_Test/BPI_N2C_P3.BPI_N2C_P3")
    };
    for (const FString& Dependency : Dependencies)
    {
        bOk &= CleanupContractObject(Dependency);
    }
    UE_LOG(LogNodeToCode, Display, TEXT("N2C_MANUAL_REPLAY_CASE|case=ContractMatrixCleanup|result=%s"), bOk ? TEXT("PASS") : TEXT("FAIL"));
    if (!bOk) Test.AddError(TEXT("Contract matrix cleanup failed."));
    return bOk;
}
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FN2CImportContractApply, "NodeToCode.ContractMatrix.Apply", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
void FN2CImportContractApply::GetTests(TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands) const { N2CImportContractMatrixTests::EnumerateTests(OutBeautifiedNames, OutTestCommands); }
bool FN2CImportContractApply::RunTest(const FString& Parameters) { return N2CImportContractMatrixTests::RunPhase(*this, N2CImportContractMatrixTests::EPhase::Apply, Parameters); }
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FN2CImportContractVerifyFresh, "NodeToCode.ContractMatrix.VerifyFreshFirst", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
void FN2CImportContractVerifyFresh::GetTests(TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands) const { N2CImportContractMatrixTests::EnumerateTests(OutBeautifiedNames, OutTestCommands); }
bool FN2CImportContractVerifyFresh::RunTest(const FString& Parameters) { return N2CImportContractMatrixTests::RunPhase(*this, N2CImportContractMatrixTests::EPhase::VerifyFresh, Parameters); }
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FN2CImportContractReapply, "NodeToCode.ContractMatrix.Reapply", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
void FN2CImportContractReapply::GetTests(TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands) const { N2CImportContractMatrixTests::EnumerateTests(OutBeautifiedNames, OutTestCommands); }
bool FN2CImportContractReapply::RunTest(const FString& Parameters) { return N2CImportContractMatrixTests::RunPhase(*this, N2CImportContractMatrixTests::EPhase::Reapply, Parameters); }
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FN2CImportContractVerifyFreshSecond, "NodeToCode.ContractMatrix.VerifyFreshSecond", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
void FN2CImportContractVerifyFreshSecond::GetTests(TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands) const { N2CImportContractMatrixTests::EnumerateTests(OutBeautifiedNames, OutTestCommands); }
bool FN2CImportContractVerifyFreshSecond::RunTest(const FString& Parameters) { return N2CImportContractMatrixTests::RunPhase(*this, N2CImportContractMatrixTests::EPhase::VerifyFreshSecond, Parameters); }
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CImportContractCleanup, "NodeToCode.ContractMatrix.Cleanup", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FN2CImportContractCleanup::RunTest(const FString&) { return N2CImportContractMatrixTests::Cleanup(*this); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CManualFlowArrays,"NodeToCode.ManualReplay.FlowAndArrays",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FN2CManualFlowArrays::RunTest(const FString&)
{
    bool bOk = true;
    bOk &= N2CManualReplayTests::RunCoreRoundTrip(
        *this,
        TEXT("ManualFlowArrays"),
        TEXT(R"json([
            {"id":"Begin","type":"K2Node_Event","event_name":"ReceiveBeginPlay","event_owner_class":"/Script/Engine.Actor","event_is_override":true},
            {"id":"Sequence","type":"K2Node_ExecutionSequence"},
            {"id":"Branch","type":"K2Node_IfThenElse","pin_defaults":{"Condition":true}},
            {"id":"PrintTrue","type":"K2Node_CallFunction","function_path":"/Script/Engine.KismetSystemLibrary.PrintString","function_owner_class":"/Script/Engine.KismetSystemLibrary","pin_defaults":{"InString":"Branch True"}},
            {"id":"PrintFalse","type":"K2Node_CallFunction","function_path":"/Script/Engine.KismetSystemLibrary.PrintString","function_owner_class":"/Script/Engine.KismetSystemLibrary","pin_defaults":{"InString":"Branch False"}},
            {"id":"PrintSecond","type":"K2Node_CallFunction","function_path":"/Script/Engine.KismetSystemLibrary.PrintString","function_owner_class":"/Script/Engine.KismetSystemLibrary","pin_defaults":{"InString":"Sequence 1"}},
            {"id":"Array","type":"K2Node_MakeArray","input_count":3,"value_pin_type":{"type":"int"},"pin_defaults":{"[0]":"7","[1]":"11","[2]":"13"}},
            {"id":"ArrayCopy","type":"K2Node_GetArrayItem","return_by_ref":false,"pin_defaults":{"Index":"1"}}
        ])json"),
        TEXT(R"json([
            {"from_node_id":"Begin","from_pin":"then","to_node_id":"Sequence","to_pin":"execute"},
            {"from_node_id":"Sequence","from_pin":"then_0","to_node_id":"Branch","to_pin":"execute"},
            {"from_node_id":"Branch","from_pin":"then","to_node_id":"PrintTrue","to_pin":"execute"},
            {"from_node_id":"Branch","from_pin":"else","to_node_id":"PrintFalse","to_pin":"execute"},
            {"from_node_id":"Sequence","from_pin":"then_1","to_node_id":"PrintSecond","to_pin":"execute"}
        ])json"),
        TEXT(R"json([{"from_node_id":"Array","from_pin":"Array","to_node_id":"ArrayCopy","to_pin":"Array"}])json"),
        TEXT("K2Node_GetArrayItem"));
    bOk &= N2CP0RoundTripTests::Run(*this,N2CP0RoundTripTests::ECase::Containers,TEXT("ManualContainers"));
    return N2CManualReplayTests::FinishCase(TEXT("FlowAndArrays"), bOk);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CManualStructDataTable,"NodeToCode.ManualReplay.StructAndDataTable",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FN2CManualStructDataTable::RunTest(const FString&)
{
    bool bOk=true;
    bOk&=N2CP0SpecializedTests::RunStruct(*this,false);
    bOk&=N2CP0SpecializedTests::RunStruct(*this,true);
    bOk&=N2CP0SpecializedTests::RunDataTable(*this,false);
    bOk&=N2CP0SpecializedTests::RunDataTable(*this,true);
    return N2CManualReplayTests::FinishCase(TEXT("StructAndDataTable"), bOk);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CManualEnum,"NodeToCode.ManualReplay.Enum",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FN2CManualEnum::RunTest(const FString&)
{
    bool bOk=true;
    bOk&=N2CP0SpecializedTests::RunEnum(*this,false);
    bOk&=N2CP0SpecializedTests::RunEnum(*this,true);
    return N2CManualReplayTests::FinishCase(TEXT("Enum"), bOk);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CManualContextual,"NodeToCode.ManualReplay.ContextualEventGraph",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FN2CManualContextual::RunTest(const FString&)
{
    bool bOk = N2CManualReplayTests::RunCoreRoundTrip(
        *this,
        TEXT("ManualContextualEventGraph"),
        TEXT(R"json([
            {"id":"Begin","type":"K2Node_Event","event_name":"ReceiveBeginPlay","event_owner_class":"/Script/Engine.Actor","event_is_override":true},
            {"id":"Key","type":"K2Node_InputKey","key_name":"SpaceBar","shift":false,"ctrl":false,"alt":false,"cmd":false,"consume_input":true,"execute_when_paused":false,"override_parent_binding":true},
            {"id":"PrintBegin","type":"K2Node_CallFunction","function_path":"/Script/Engine.KismetSystemLibrary.PrintString","function_owner_class":"/Script/Engine.KismetSystemLibrary","pin_defaults":{"InString":"Begin"}},
            {"id":"PrintAction","type":"K2Node_CallFunction","function_path":"/Script/Engine.KismetSystemLibrary.PrintString","function_owner_class":"/Script/Engine.KismetSystemLibrary","pin_defaults":{"InString":"SpaceBar"}},
            {"id":"SpawnEvent","type":"K2Node_CustomEvent","event_name":"N2C_Test_LinkedSpawnTransform"},
            {"id":"MakeSpawnTransform","type":"K2Node_CallFunction","function_path":"/Script/Engine.KismetMathLibrary.MakeTransform","function_owner_class":"/Script/Engine.KismetMathLibrary","function_name":"MakeTransform","pin_defaults":{"Location":"(X=0.0,Y=0.0,Z=0.0)","Rotation":"(Pitch=0.0,Yaw=0.0,Roll=0.0)","Scale":"(X=1.0,Y=1.0,Z=1.0)"}},
            {"id":"SpawnActor","type":"K2Node_SpawnActorFromClass","pin_defaults":{"Class":"/Script/Engine.Actor"}}
        ])json"),
        TEXT(R"json([
            {"from_node_id":"Begin","from_pin":"then","to_node_id":"PrintBegin","to_pin":"execute"},
            {"from_node_id":"Key","from_pin":"Pressed","to_node_id":"PrintAction","to_pin":"execute"},
            {"from_node_id":"SpawnEvent","from_pin":"then","to_node_id":"SpawnActor","to_pin":"execute"}
        ])json"),
        TEXT(R"json([
            {"from_node_id":"MakeSpawnTransform","from_pin":"ReturnValue","to_node_id":"SpawnActor","to_pin":"SpawnTransform"}
        ])json"),
        TEXT("K2Node_SpawnActorFromClass"));
    bOk &= N2CP0CoreUIInputTests::RunReject(
        *this,
        TEXT("SpawnActorTransformLinkReject"),
        TEXT(R"json({"id":"SpawnActor","type":"K2Node_SpawnActorFromClass","pin_defaults":{"Class":"/Script/Engine.Actor","SpawnTransform":"(Rotation=(Pitch=0,Yaw=0,Roll=0),Translation=(X=0,Y=0,Z=0),Scale3D=(X=1,Y=1,Z=1))"}})json"),
        TEXT("spawn_transform_link_missing"));
    bOk &= N2CP0CoreUIInputTests::RunReject(
        *this,
        TEXT("RotatorPinDefaultReject"),
        TEXT(R"json({"id":"MakeTransform","type":"K2Node_CallFunction","function_path":"/Script/Engine.KismetMathLibrary.MakeTransform","function_owner_class":"/Script/Engine.KismetMathLibrary","function_name":"MakeTransform","pin_defaults":{"Rotation":"not-a-valid-rotator"}})json"),
        TEXT("pin_default_invalid"));
    bOk &= N2CP0CoreUIInputTests::RunRejectGraph(
        *this,
        TEXT("SpawnActorClassDefaultReject"),
        TEXT(R"json([
            {"id":"MakeSpawnTransform","type":"K2Node_CallFunction","function_path":"/Script/Engine.KismetMathLibrary.MakeTransform","function_owner_class":"/Script/Engine.KismetMathLibrary","function_name":"MakeTransform"},
            {"id":"SpawnActor","type":"K2Node_SpawnActorFromClass","pin_defaults":{"Class":"/Script/UMG.UserWidget"}}
        ])json"),
        TEXT("[]"),
        TEXT(R"json([{"from_node_id":"MakeSpawnTransform","from_pin":"ReturnValue","to_node_id":"SpawnActor","to_pin":"SpawnTransform"}])json"),
        TEXT("pin_default_invalid"));
    return N2CManualReplayTests::FinishCase(TEXT("ContextualEventGraph"), bOk);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CManualDelegates,"NodeToCode.ManualReplay.Delegates",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FN2CManualDelegates::RunTest(const FString&)
{
    bool bOk=true;
    bOk&=N2CP0DelegateTests::Run(*this,TEXT("ManualDelegates"),false,false);
    bOk&=N2CP0DelegateTests::Run(*this,TEXT("ManualComponentBoundEvent"),true,false);
    return N2CManualReplayTests::FinishCase(TEXT("Delegates"), bOk);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CManualFunctionBoundaries,"NodeToCode.ManualReplay.FunctionBoundaries",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FN2CManualFunctionBoundaries::RunTest(const FString&)
{
    bool bOk=true;
    bOk&=N2CP0RoundTripTests::Run(*this,N2CP0RoundTripTests::ECase::ImpureFunction,TEXT("ManualImpureFunction"));
    bOk&=N2CP0RoundTripTests::Run(*this,N2CP0RoundTripTests::ECase::PureFunction,TEXT("ManualPureFunction"));
    bOk&=N2CP0RoundTripTests::Run(*this,N2CP0RoundTripTests::ECase::MultipleResults,TEXT("ManualMultipleResults"));
    return N2CManualReplayTests::FinishCase(TEXT("FunctionBoundaries"), bOk);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CManualStandardMacros,"NodeToCode.ManualReplay.StandardMacros",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FN2CManualStandardMacros::RunTest(const FString&)
{
    bool bOk=true;
    bOk&=N2CP0RoundTripTests::Run(*this,N2CP0RoundTripTests::ECase::StandardSimple,TEXT("ManualStandardSimple"));
    bOk&=N2CP0RoundTripTests::Run(*this,N2CP0RoundTripTests::ECase::StandardLoop,TEXT("ManualStandardLoop"));
    bOk&=N2CP0RoundTripTests::Run(*this,N2CP0RoundTripTests::ECase::StandardWildcardContainer,TEXT("ManualStandardWildcard"));
    return N2CManualReplayTests::FinishCase(TEXT("StandardMacros"), bOk);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CManualGraphBoundaries,"NodeToCode.ManualReplay.GraphBoundaries",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FN2CManualGraphBoundaries::RunTest(const FString&)
{
    bool bOk=true;
    bOk&=N2CP0RoundTripTests::Run(*this,N2CP0RoundTripTests::ECase::GraphMacroTunnel,TEXT("ManualMacroTunnel"));
    bOk&=N2CP0RoundTripTests::Run(*this,N2CP0RoundTripTests::ECase::GraphCollapsedTunnel,TEXT("ManualCollapsedTunnel"));
    bOk&=N2CP0RoundTripTests::Run(*this,N2CP0RoundTripTests::ECase::GraphComposite,TEXT("ManualComposite"));
    bOk&=N2CGraphBoundaryTests::CanonicalCompositeRepeat(*this);
    return N2CManualReplayTests::FinishCase(TEXT("GraphBoundaries"), bOk);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CManualWidget,"NodeToCode.ManualReplay.Widget",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FN2CManualWidget::RunTest(const FString&)
{
    const bool bOk = N2CManualReplayTests::RunCoreRoundTrip(
        *this,
        TEXT("ManualWidget"),
        TEXT(R"json([
            {"id":"Construct","type":"K2Node_Event","event_name":"Construct","event_owner_class":"/Script/UMG.UserWidget","event_is_override":true},
            {"id":"Print","type":"K2Node_CallFunction","function_path":"/Script/Engine.KismetSystemLibrary.PrintString","function_owner_class":"/Script/Engine.KismetSystemLibrary","pin_defaults":{"InString":"Widget Construct"}},
            {"id":"Widget","type":"K2Node_CreateWidget","class_path":"/Script/UMG.UserWidget"}
        ])json"),
        TEXT(R"json([{"from_node_id":"Construct","from_pin":"then","to_node_id":"Print","to_pin":"execute"}])json"),
        TEXT("[]"),
        TEXT("K2Node_CreateWidget"),
        N2CP0CoreUIInputTests::EFixtureKind::Widget);
    return N2CManualReplayTests::FinishCase(TEXT("Widget"), bOk);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CManualAIController,"NodeToCode.ManualReplay.AIController",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FN2CManualAIController::RunTest(const FString&)
{
    const bool bOk = N2CManualReplayTests::RunCoreRoundTrip(
        *this,
        TEXT("ManualAIController"),
        TEXT(R"json([
            {"id":"Possess","type":"K2Node_Event","event_name":"ReceivePossess","event_owner_class":"/Script/AIModule.AIController","event_is_override":true},
            {"id":"Print","type":"K2Node_CallFunction","function_path":"/Script/Engine.KismetSystemLibrary.PrintString","function_owner_class":"/Script/Engine.KismetSystemLibrary","pin_defaults":{"InString":"Possessed"}}
        ])json"),
        TEXT(R"json([{"from_node_id":"Possess","from_pin":"then","to_node_id":"Print","to_pin":"execute"}])json"),
        TEXT("[]"),
        TEXT("K2Node_Event"),
        N2CP0CoreUIInputTests::EFixtureKind::AI);
    return N2CManualReplayTests::FinishCase(TEXT("AIController"), bOk);
}

#define N2C_MANUAL_BT_TEST(ClassName,Path,CaseName,Label,Kind) \
    IMPLEMENT_SIMPLE_AUTOMATION_TEST(ClassName,Path,EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter) \
    bool ClassName::RunTest(const FString&) \
    { \
        const bool bOk=N2CManualReplayTests::RunCoreRoundTrip(*this,TEXT(Label),TEXT(R"json([{"id":"Self","type":"K2Node_Self"}])json"),TEXT("[]"),TEXT("[]"),TEXT("K2Node_Self"),N2CP0CoreUIInputTests::EFixtureKind::Kind); \
        return N2CManualReplayTests::FinishCase(TEXT(CaseName), bOk); \
    }
N2C_MANUAL_BT_TEST(FN2CManualBTTask,"NodeToCode.ManualReplay.BTTask","BTTask","ManualBTTask",BTTask)
N2C_MANUAL_BT_TEST(FN2CManualBTService,"NodeToCode.ManualReplay.BTService","BTService","ManualBTService",BTService)
N2C_MANUAL_BT_TEST(FN2CManualBTDecorator,"NodeToCode.ManualReplay.BTDecorator","BTDecorator","ManualBTDecorator",BTDecorator)
#undef N2C_MANUAL_BT_TEST

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CManualPreflight,"NodeToCode.ManualReplay.PreflightRejectsWithoutMutation",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FN2CManualPreflight::RunTest(const FString&)
{
    return N2CManualReplayTests::FinishCase(TEXT("PreflightRejectsWithoutMutation"), N2CManualReplayTests::RunPreflightNoMutation(*this));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CManualSandboxPins,"NodeToCode.ManualReplay.SandboxPinPreflight",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FN2CManualSandboxPins::RunTest(const FString&)
{
    return N2CManualReplayTests::FinishCase(TEXT("SandboxPinPreflight"), N2CManualReplayTests::RunSandboxPinPreflight(*this));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CManualRollback,"NodeToCode.ManualReplay.RollbackAfterMutation",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FN2CManualRollback::RunTest(const FString&)
{
    return N2CManualReplayTests::FinishCase(TEXT("RollbackAfterMutation"), N2CManualReplayTests::RunVerifiedRollback(*this,false));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CManualRollbackEquality,"NodeToCode.ManualReplay.RollbackStructuralEquality",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FN2CManualRollbackEquality::RunTest(const FString&)
{
    return N2CManualReplayTests::FinishCase(TEXT("RollbackStructuralEquality"), N2CManualReplayTests::RunVerifiedRollback(*this,true));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CManualDiskRestoreFirst,"NodeToCode.RestoreFirstPass.ManualReplayPendingRestore",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FN2CManualDiskRestoreFirst::RunTest(const FString&){return N2CManualReplayTests::RunDiskRestoreFirstPass(*this);}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CManualDiskRestoreSecond,"NodeToCode.RestoreSecondPass.ManualReplayPendingRestore",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FN2CManualDiskRestoreSecond::RunTest(const FString&){return N2CManualReplayTests::RunDiskRestoreSecondPass(*this);}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CManualDialogs,"NodeToCode.ManualReplay.DialogDiagnostics",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FN2CManualDialogs::RunTest(const FString&)
{
    return N2CManualReplayTests::FinishCase(TEXT("DialogDiagnostics"), N2CManualReplayTests::RunDialogDiagnostics(*this));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CManualToolbar,"NodeToCode.ManualReplay.ToolbarCommands",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FN2CManualToolbar::RunTest(const FString&)
{
    return N2CManualReplayTests::FinishCase(TEXT("ToolbarCommands"), N2CManualReplayTests::RunToolbarCommands(*this));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CUISmokeOpenAsset,"NodeToCode.UI.OpenSmokeAsset",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FN2CUISmokeOpenAsset::RunTest(const FString&)
{
    FString AssetPath;
    FParse::Value(FCommandLine::Get(), TEXT("N2CUISmokeAsset="), AssetPath);
    AssetPath.TrimQuotesInline();
    UBlueprint* Blueprint = AssetPath.IsEmpty() ? nullptr : LoadObject<UBlueprint>(nullptr, *AssetPath);
    const bool bOpened = Blueprint && FAssetEditorManager::Get().OpenEditorForAsset(Blueprint);
    UE_LOG(LogNodeToCode, Display, TEXT("N2C_UI_SMOKE_SETUP|asset=%s|result=%s"), *AssetPath, bOpened ? TEXT("PASS") : TEXT("FAIL"));
    if (!bOpened) AddError(FString::Printf(TEXT("UI smoke asset could not be opened: %s"), *AssetPath));
    return bOpened;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CManualRawByte,"NodeToCode.ManualReplay.RawByteDefaultReopenExport",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FN2CManualRawByte::RunTest(const FString&)
{
    const bool bOk = N2CManualReplayTests::RunRawByteRoundTripAndExport(*this) &&
        N2CManualReplayTests::RunInvalidStructMemberDefaultReject(*this);
    return N2CManualReplayTests::FinishCase(TEXT("RawByteDefaultReopenExport"), bOk);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CManualMissingEnum,"NodeToCode.ManualReplay.MissingEnumReject",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FN2CManualMissingEnum::RunTest(const FString&)
{
    return N2CManualReplayTests::FinishCase(TEXT("MissingEnumReject"), N2CManualReplayTests::RunMissingEnumReject(*this));
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CP0Root,"NodeToCode.Verification.P0",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FN2CP0Root::RunTest(const FString&)
{
    UE_LOG(LogNodeToCode, Display, TEXT("N2C_P0_BEGIN|schema=N2C_P0_FIXTURE_MANIFEST_V1"));
    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("NodeToCode"));
    const FString Path = Plugin.IsValid() ? FPaths::Combine(Plugin->GetBaseDir(), TEXT("Source/Tests/Fixtures/N2C_P0_FIXTURE_MANIFEST_V1.json")) : FString();
    FString Json; TSharedPtr<FJsonObject> Root; FString Schema; const TArray<TSharedPtr<FJsonValue>>* Fixtures = nullptr;
    const bool bRead = !Path.IsEmpty() && FFileHelper::LoadFileToString(Json, *Path);
    const bool bNoMachinePath = bRead && !Json.Contains(TEXT("C:\\")) && !Json.Contains(TEXT("D:\\"));
    TSharedRef<TJsonReader<>> ManifestReader = TJsonReaderFactory<>::Create(Json);
    const bool bParsed = bRead && FJsonSerializer::Deserialize(ManifestReader, Root) && Root.IsValid();
    double DeclaredCount = 0.0; const bool bDeclaredCount = bParsed && Root->TryGetNumberField(TEXT("fixture_count"), DeclaredCount);
    const TCHAR* RequiredFamilies[] = {
        TEXT("NodeToCode.Verification.P0Specialized.Struct.MakeBreak"),
        TEXT("NodeToCode.Verification.P0Specialized.DataTable.GetRow"),
        TEXT("NodeToCode.Verification.P0Specialized.DataTableLinkedInput"),
        TEXT("NodeToCode.Verification.P0Core.FunctionEntryResult"),
        TEXT("NodeToCode.Verification.P0FunctionBoundaries.ProductionFingerprintCoverage"),
        TEXT("NodeToCode.Verification.P0FunctionBoundaries.ARoomCoverage"),
        TEXT("NodeToCode.Verification.P0GraphBoundaries.Composite"),
        TEXT("NodeToCode.Verification.P0GraphBoundaries.ProductionFingerprintCoverage"),
        TEXT("NodeToCode.Verification.P0Delegates.ComponentBoundEvent"),
        TEXT("NodeToCode.Verification.P0UIInput.InputActionGraphNode"),
        TEXT("NodeToCode.Verification.P0UIInput.InputKeyGraphNode"),
        TEXT("NodeToCode.Verification.P0UIInput.CreateWidgetLinkedClass"),
        TEXT("NodeToCode.Verification.P0Core.MakeArrayTyped"),
        TEXT("NodeToCode.Verification.P0UIInput.InterfaceMessage"),
        TEXT("NodeToCode.Verification.P0Core.WidgetEvents"),
        TEXT("NodeToCode.Verification.P0Core.AIEvents"),
        TEXT("NodeToCode.Verification.P0Guards.ReferenceDefaultReject")
    };
    bool bRequiredFamilies = true; for (const TCHAR* Required : RequiredFamilies) bRequiredFamilies &= Json.Contains(Required);
    const bool bManifest = bParsed && Root->TryGetStringField(TEXT("schema"), Schema) && Schema == TEXT("N2C_P0_FIXTURE_MANIFEST_V1") &&
        Root->TryGetArrayField(TEXT("fixtures"), Fixtures) && Fixtures && Fixtures->Num() == 83 &&
        bDeclaredCount && static_cast<int32>(DeclaredCount) == Fixtures->Num() && bNoMachinePath && bRequiredFamilies;
    UE_LOG(LogNodeToCode, Display, TEXT("N2C_P0_PHASE|phase=fixture_manifest|result=%s|fixtures=%d|required_families=%d"), bManifest ? TEXT("PASS") : TEXT("FAIL"), Fixtures ? Fixtures->Num() : 0, bRequiredFamilies ? 1 : 0);
    UE_LOG(LogNodeToCode, Display, TEXT("N2C_P0_PHASE|phase=coverage_roundtrip_macros_specialized_delegates_ui_core_guards|result=REGISTERED"));
    if (!bManifest) AddError(FString::Printf(TEXT("P0 fixture manifest invalid or machine-specific: %s"), *Path));
    return bManifest;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FN2CProjectExportFull,"NodeToCode.Verification.ProjectExport.Full",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FN2CProjectExportFull::RunTest(const FString&)
{
    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    AssetRegistryModule.Get().SearchAllAssets(true);

    FARFilter Filter;
    Filter.PackagePaths.Add(FName(TEXT("/Game")));
    Filter.bRecursivePaths = true;

    TArray<FAssetData> Assets;
    AssetRegistryModule.Get().GetAssets(Filter, Assets);
    Assets.Sort([](const FAssetData& A, const FAssetData& B) { return A.ObjectPath.ToString() < B.ObjectPath.ToString(); });

    TArray<FAssetData> Blueprints;
    TArray<FAssetData> NiagaraSystems;
    TArray<FAssetData> Enums;
    TArray<FAssetData> Structs;
    for (const FAssetData& Asset : Assets)
    {
        const FString ClassName = Asset.AssetClass.ToString();
        if (ClassName == TEXT("Blueprint") || ClassName.EndsWith(TEXT("Blueprint")))
        {
            Blueprints.Add(Asset);
        }
        else if (ClassName == TEXT("NiagaraSystem"))
        {
            NiagaraSystems.Add(Asset);
        }
        else if (ClassName == TEXT("UserDefinedEnum") || ClassName == TEXT("Enum") || ClassName.EndsWith(TEXT("Enum")))
        {
            Enums.Add(Asset);
        }
        else if (ClassName == TEXT("UserDefinedStruct") || ClassName == TEXT("ScriptStruct") || ClassName.EndsWith(TEXT("Struct")))
        {
            Structs.Add(Asset);
        }
    }

    FString ZipPath;
    FString Report;
    const bool bExported = FN2CEditorIntegration::Get().ExportAllProjectAssetsForAutomation(ZipPath, Report);
    const int32 SupportedCount = Blueprints.Num() + NiagaraSystems.Num() + Enums.Num() + Structs.Num();
    UE_LOG(LogNodeToCode, Display,
        TEXT("N2C_PROJECT_EXPORT_AUTOMATION|result=%s|supported=%d|blueprints=%d|niagara=%d|enums=%d|structs=%d|zip=%s|report=%s"),
        bExported ? TEXT("PASS") : TEXT("FAIL"), SupportedCount, Blueprints.Num(), NiagaraSystems.Num(), Enums.Num(), Structs.Num(),
        *ZipPath, *N2CVerificationTests_Private::OneLine(Report));
    if (!bExported)
    {
        AddError(Report);
    }
    return bExported;
}

N2C_RT_TEST(FN2CP0Smoke,"NodeToCode.Verification.P0Smoke",Success,"P0Smoke")
#undef N2C_RT_TEST
#endif
