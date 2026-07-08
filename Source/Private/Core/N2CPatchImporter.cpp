// Copyright (c) 2025 Nick McClure (Protospatial). All Rights Reserved.
// Back2Dead custom extension: safe Blueprint patch importer.

#include "Core/N2CPatchImporter.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphNodeUtils.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "HAL/FileManager.h"
#include "K2Node_CallFunction.h"
#include "K2Node_EditablePinBase.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonReader.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Utils/N2CLogger.h"
#include "UObject/UnrealType.h"

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
            AppendLine(OutReport, FString::Printf(TEXT("ERROR: unsupported schema '%s'. Expected N2C_PATCH_V1."), *Schema));
            return false;
        }

        if (!GetArrayFieldSafe(OutRoot, TEXT("actions")))
        {
            AppendLine(OutReport, TEXT("ERROR: patch does not contain actions[]."));
            return false;
        }

        return true;
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
        const FString Timestamp = FString::Printf(TEXT("%lld"), FDateTime::Now().GetTicks());
        OutBackupPath = BackupDir / FString::Printf(TEXT("%s_%s.uasset"), *SafeAssetName, *Timestamp);

        const int32 CopyResult = IFileManager::Get().Copy(*OutBackupPath, *SourceFilename, true, true);
        if (CopyResult == COPY_OK)
        {
            AppendLine(OutReport, FString::Printf(TEXT("Backup created: %s"), *OutBackupPath));
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


    static bool IsSupportedActionType(const FString& ActionType)
    {
        return ActionType == TEXT("add_or_replace_function") ||
               ActionType == TEXT("replace_function_body") ||
               ActionType == TEXT("add_member_variables") ||
               ActionType == TEXT("add_variables") ||
               ActionType == TEXT("rename_function") ||
               ActionType == TEXT("set_function_category") ||
               ActionType == TEXT("move_function_to_category");
    }

    static bool IsSupportedNodeType(const FString& Type)
    {
        const FString Lower = Type.ToLower();
        return Lower == TEXT("entry") ||
               Lower == TEXT("functionentry") ||
               Lower.Contains(TEXT("functionentry")) ||
               Lower == TEXT("return") ||
               Lower == TEXT("functionresult") ||
               Lower.Contains(TEXT("functionresult")) ||
               Lower == TEXT("branch") ||
               Lower.Contains(TEXT("ifthenelse")) ||
               Lower == TEXT("callfunction") ||
               Lower == TEXT("call_function") ||
               Lower.Contains(TEXT("callfunction")) ||
               Lower == TEXT("variableget") ||
               Lower == TEXT("variable_get") ||
               Lower.Contains(TEXT("variableget")) ||
               Lower == TEXT("variableset") ||
               Lower == TEXT("variable_set") ||
               Lower.Contains(TEXT("variableset"));
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
            if (Pin->PinName.ToString() == Name || Pin->GetDisplayName().ToString() == Name)
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
        }
        return bOk;
    }

    static bool ValidateActionShape(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& ActionObj, FString& Report)
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

        return bOk;
    }

    static FEdGraphPinType MakePinTypeFromJson(const TSharedPtr<FJsonObject>& Obj)
    {
        FEdGraphPinType PinType;
        PinType.PinCategory = UEdGraphSchema_K2::PC_Wildcard;

        FString Type = GetStringFieldSafe(Obj, TEXT("type"));
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
        else if (Lower == TEXT("byte"))
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
        else if (Lower == TEXT("exec"))
        {
            PinType.PinCategory = UEdGraphSchema_K2::PC_Exec;
        }

        if (Obj.IsValid())
        {
            FString SubCategoryObjectPath = GetStringFieldSafe(Obj, TEXT("sub_category_object"));
            if (SubCategoryObjectPath.IsEmpty())
            {
                const TSharedPtr<FJsonObject>* PinTypeObj = nullptr;
                if (Obj->TryGetObjectField(TEXT("pin_type"), PinTypeObj) && PinTypeObj)
                {
                    SubCategoryObjectPath = GetStringFieldSafe(*PinTypeObj, TEXT("sub_category_object"));
                }
            }
            if (!SubCategoryObjectPath.IsEmpty())
            {
                UObject* TypeObject = FindObject<UObject>(ANY_PACKAGE, *SubCategoryObjectPath);
                if (!TypeObject)
                {
                    TypeObject = LoadObject<UObject>(nullptr, *SubCategoryObjectPath);
                }
                if (TypeObject)
                {
                    PinType.PinSubCategoryObject = TypeObject;
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
            const FString DefaultValue = NormalizeMemberVariableDefaultValue(PinType, GetVariableDefaultValue(VarObj));

            if (!IsSafeBlueprintIdentifier(Name))
            {
                AppendLine(Report, FString::Printf(TEXT("ERROR: invalid Blueprint member variable name '%s'."), *Name));
                bOk = false;
                continue;
            }

            if (BlueprintHasMemberVariable(Blueprint, Name))
            {
                AppendLine(Report, FString::Printf(TEXT("Variable already exists, skipped duplicate: %s"), *Name));
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

    static bool RenameFunctionGraph(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& ActionObj, bool bDryRun, FString& Report)
    {
        const FString OldName = GetStringFieldSafe(ActionObj, TEXT("old_function_name"), GetStringFieldSafe(ActionObj, TEXT("from")));
        const FString NewName = GetStringFieldSafe(ActionObj, TEXT("new_function_name"), GetStringFieldSafe(ActionObj, TEXT("to")));
        const FString Category = GetStringFieldSafe(ActionObj, TEXT("category"), GetStringFieldSafe(ActionObj, TEXT("folder")));

        UEdGraph* Graph = FindFunctionGraph(Blueprint, OldName);
        if (!Graph)
        {
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

            if (Pin->PinName.ToString() == Name || Pin->GetDisplayName().ToString() == Name)
            {
                return Pin;
            }
        }

        // Friendly aliases used by generated patches.
        const FString Lower = Name.ToLower();
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin || Pin->Direction != Direction)
            {
                continue;
            }
            const FString PinLower = Pin->PinName.ToString().ToLower();
            const FString DisplayLower = Pin->GetDisplayName().ToString().ToLower();
            if ((Lower == TEXT("then") && PinLower == TEXT("then")) ||
                (Lower == TEXT("exec") && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) ||
                (Lower == TEXT("condition") && (PinLower == TEXT("condition") || DisplayLower == TEXT("condition"))) ||
                (Lower == TEXT("true") && (PinLower == TEXT("then") || DisplayLower == TEXT("true"))) ||
                (Lower == TEXT("false") && (PinLower == TEXT("else") || DisplayLower == TEXT("false"))))
            {
                return Pin;
            }
        }

        return nullptr;
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
               FieldName == TEXT("pin_defaults");
    }

    static void ApplyNodePinDefaults(UEdGraphNode* Node, const TSharedPtr<FJsonObject>& NodeObj)
    {
        if (!Node || !NodeObj.IsValid())
        {
            return;
        }

        const TSharedPtr<FJsonObject>* DefaultsObj = nullptr;
        if (NodeObj->TryGetObjectField(TEXT("pin_defaults"), DefaultsObj) && DefaultsObj && DefaultsObj->IsValid())
        {
            for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*DefaultsObj)->Values)
            {
                if (UEdGraphPin* Pin = FindPinByLooseName(Node, Pair.Key, EGPD_Input))
                {
                    Pin->DefaultValue = JsonValueToPinDefaultString(Pair.Value);
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
                Pin->DefaultValue = JsonValueToPinDefaultString(Pair.Value);
            }
        }
    }

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

        if (Lower.Contains(TEXT("functionentry")) || Lower == TEXT("entry"))
        {
            return FindOrCreateEntryNode(Graph, Graph->GetName());
        }
        if (Lower.Contains(TEXT("functionresult")) || Lower == TEXT("return"))
        {
            return FindOrCreateResultNode(Graph, Graph->GetName());
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
        if (Lower == TEXT("callfunction") || Lower == TEXT("call_function") || Lower.Contains(TEXT("callfunction")))
        {
            FGraphNodeCreator<UK2Node_CallFunction> Creator(*Graph);
            UK2Node_CallFunction* Call = Creator.CreateNode();
            UFunction* Function = ResolveFunction(NodeObj, Blueprint);
            const FString FunctionName = GetStringFieldSafe(NodeObj, TEXT("function_name"), GetStringFieldSafe(NodeObj, TEXT("member_name"), GetStringFieldSafe(NodeObj, TEXT("name"))));
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

        AppendLine(Report, FString::Printf(TEXT("WARNING: unsupported node type '%s'. Node was not created."), *Type));
        return nullptr;
    }

    static void ConnectEdges(UEdGraph* Graph, const TMap<FString, UEdGraphNode*>& NodesById, const TSharedPtr<FJsonObject>& ActionObj, FString& Report)
    {
        if (!Graph || !ActionObj.IsValid())
        {
            return;
        }

        const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(Graph->GetSchema());
        if (!Schema)
        {
            AppendLine(Report, TEXT("WARNING: graph schema is not K2; links were not connected."));
            return;
        }

        auto ConnectArray = [&](const FString& FieldName)
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
                    AppendLine(Report, FString::Printf(TEXT("WARNING: edge skipped because node id was not found: %s -> %s"), *FromId, *ToId));
                    continue;
                }

                UEdGraphPin* FromPin = FindPinByLooseName(FromNode, FromPinName, EGPD_Output);
                UEdGraphPin* ToPin = FindPinByLooseName(ToNode, ToPinName, EGPD_Input);
                if (!FromPin || !ToPin)
                {
                    AppendLine(Report, FString::Printf(TEXT("WARNING: edge skipped because pin was not found: %s.%s -> %s.%s"), *FromId, *FromPinName, *ToId, *ToPinName));
                    continue;
                }

                if (!Schema->TryCreateConnection(FromPin, ToPin))
                {
                    AppendLine(Report, FString::Printf(TEXT("WARNING: TryCreateConnection failed: %s.%s -> %s.%s"), *FromId, *FromPinName, *ToId, *ToPinName));
                }
            }
        };

        ConnectArray(TEXT("exec_edges"));
        ConnectArray(TEXT("data_edges"));
        ConnectArray(TEXT("edges"));
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
            }
        }

        AddSignaturePins(Graph, ActionObj, FunctionName, Report);
        AddLocalVariables(Blueprint, Graph, ActionObj, FunctionName, Report);

        const FString FunctionCategory = GetStringFieldSafe(ActionObj, TEXT("category"), GetStringFieldSafe(ActionObj, TEXT("folder")));
        if (!FunctionCategory.IsEmpty())
        {
            FBlueprintEditorUtils::SetBlueprintFunctionOrMacroCategory(Graph, FText::FromString(FunctionCategory), true);
            AppendLine(Report, FString::Printf(TEXT("CHANGE: function category: %s -> %s"), *FunctionName, *FunctionCategory));
        }

        ApplyFunctionOptions(Blueprint, Graph, ActionObj, FunctionName, bDryRun, Report);

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
        if (Nodes)
        {
            int32 Index = 0;
            for (const TSharedPtr<FJsonValue>& Value : *Nodes)
            {
                TSharedPtr<FJsonObject> NodeObj = Value.IsValid() ? Value->AsObject() : nullptr;
                if (!NodeObj.IsValid())
                {
                    continue;
                }
                const FString NodeId = GetStringFieldSafe(NodeObj, TEXT("id"), FString::Printf(TEXT("PatchNode_%d"), Index));
                UEdGraphNode* CreatedNode = CreatePatchNode(Graph, Blueprint, NodeObj, Index, Report);
                if (CreatedNode)
                {
                    ApplyNodePinDefaults(CreatedNode, NodeObj);
                    NodesById.Add(NodeId, CreatedNode);
                }
                ++Index;
            }
        }

        ConnectEdges(Graph, NodesById, ActionObj, Report);

        Graph->NotifyGraphChanged();
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
        return true;
    }

    static bool ApplyPatchInternal(UBlueprint* Blueprint, const FString& PatchJson, bool bDryRun, FString& OutReport)
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

        const TArray<TSharedPtr<FJsonValue>>* Actions = GetArrayFieldSafe(Root, TEXT("actions"));
        if (!Actions || Actions->Num() == 0)
        {
            AppendLine(OutReport, TEXT("ERROR: actions[] is empty."));
            return false;
        }

        AppendLine(OutReport, FString::Printf(TEXT("Patch target Blueprint: %s"), *Blueprint->GetName()));
        AppendLine(OutReport, FString::Printf(TEXT("Actions: %d"), Actions->Num()));

        const FString DeclaredTarget = GetStringFieldSafe(Root, TEXT("target_blueprint"));
        if (!DeclaredTarget.IsEmpty() && DeclaredTarget != Blueprint->GetName() && DeclaredTarget != Blueprint->GetPathName())
        {
            AppendLine(OutReport, FString::Printf(TEXT("ERROR: target_blueprint mismatch. Patch='%s', current='%s' (%s)."), *DeclaredTarget, *Blueprint->GetName(), *Blueprint->GetPathName()));
            return false;
        }

        bool bValidationOk = true;
        if (GetMemberVariablesArray(Root))
        {
            bValidationOk &= ValidateMemberVariablesShape(Root, TEXT("root variables"), OutReport);
        }
        for (const TSharedPtr<FJsonValue>& Value : *Actions)
        {
            TSharedPtr<FJsonObject> ActionObj = Value.IsValid() ? Value->AsObject() : nullptr;
            bValidationOk &= ValidateActionShape(Blueprint, ActionObj, OutReport);
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
        else
        {
            FString BackupPath;
            if (!BackupBlueprintAsset(Blueprint, BackupPath, OutReport))
            {
                AppendLine(OutReport, TEXT("Patch aborted before mutation."));
                return false;
            }
        }

        bool bAllOk = true;
        if (!bDryRun)
        {
            Blueprint->Modify();
        }

        // Optional root-level member variables support:
        // { "schema":"N2C_PATCH_V1", "variables":[...], "actions":[...] }
        if (GetMemberVariablesArray(Root))
        {
            bAllOk &= AddMemberVariables(Blueprint, Root, bDryRun, OutReport);
        }

        auto ApplyAction = [&](const TSharedPtr<FJsonObject>& ActionObj) -> bool
        {
            const FString ActionType = GetStringFieldSafe(ActionObj, TEXT("type"));
            if (IsMemberVariablesAction(ActionType))
            {
                return AddMemberVariables(Blueprint, ActionObj, bDryRun, OutReport);
            }
            if (ActionType == TEXT("rename_function"))
            {
                return RenameFunctionGraph(Blueprint, ActionObj, bDryRun, OutReport);
            }
            if (ActionType == TEXT("set_function_category") || ActionType == TEXT("move_function_to_category"))
            {
                return SetFunctionCategory(Blueprint, ActionObj, bDryRun, OutReport);
            }
            return CreateOrReplaceFunction(Blueprint, ActionObj, bDryRun, OutReport);
        };

        for (const TSharedPtr<FJsonValue>& Value : *Actions)
        {
            TSharedPtr<FJsonObject> ActionObj = Value.IsValid() ? Value->AsObject() : nullptr;
            bAllOk &= ApplyAction(ActionObj);
        }

        if (!bDryRun)
        {
            FKismetEditorUtilities::CompileBlueprint(Blueprint);
            AppendLine(OutReport, TEXT("Compile requested after patch apply. Check Unreal compiler output for final graph errors."));
        }

        AppendLine(OutReport, bAllOk ? TEXT("Patch processing finished.") : TEXT("Patch processing finished with errors/warnings."));
        return bAllOk;
    }
}

bool FN2CPatchImporter::ApplyPatchToBlueprint(UBlueprint* Blueprint, const FString& PatchJson, FString& OutReport)
{
    using namespace N2CPatchImporter_Private;
    const FScopedTransaction Transaction(NSLOCTEXT("NodeToCode", "ApplyN2CPatch", "Apply N2C Blueprint Patch"));
    return ApplyPatchInternal(Blueprint, PatchJson, false, OutReport);
}

bool FN2CPatchImporter::DryRunPatch(UBlueprint* Blueprint, const FString& PatchJson, FString& OutReport)
{
    using namespace N2CPatchImporter_Private;
    return ApplyPatchInternal(Blueprint, PatchJson, true, OutReport);
}
