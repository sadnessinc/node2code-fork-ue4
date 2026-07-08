// Copyright (c) 2025 Nick McClure (Protospatial). All Rights Reserved.
// Back2Dead custom extension: AI-friendly Blueprint/Niagara export.

#include "Core/N2CAIExport.h"

#include "Utils/N2CLogger.h"

#include "AssetData.h"
#include "Editor.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "HAL/FileManager.h"
#include "Internationalization/Internationalization.h"
#include "K2Node.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_EditablePinBase.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Variable.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Policies/PrettyJsonPrintPolicy.h"
#include "UObject/UnrealType.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsPlatformApplicationMisc.h"
#endif
#if PLATFORM_MAC
#include "Mac/MacPlatformApplicationMisc.h"
#endif
#if PLATFORM_LINUX
#include "Linux/LinuxPlatformApplicationMisc.h"
#endif

namespace N2CAIExport_Private
{
    static FString SafeName(const FString& In)
    {
        FString Out = In;
        Out.ReplaceInline(TEXT("/"), TEXT("_"));
        Out.ReplaceInline(TEXT("\\"), TEXT("_"));
        Out.ReplaceInline(TEXT(":"), TEXT("_"));
        Out.ReplaceInline(TEXT(" "), TEXT("_"));
        return Out;
    }

    static FString DirectionToString(EEdGraphPinDirection Direction)
    {
        switch (Direction)
        {
        case EGPD_Input:
            return TEXT("input");
        case EGPD_Output:
            return TEXT("output");
        default:
            return TEXT("unknown");
        }
    }

    static FString ContainerToString(EPinContainerType ContainerType)
    {
        switch (ContainerType)
        {
        case EPinContainerType::Array:
            return TEXT("Array");
        case EPinContainerType::Set:
            return TEXT("Set");
        case EPinContainerType::Map:
            return TEXT("Map");
        default:
            return TEXT("None");
        }
    }

    static bool IsExecPin(const UEdGraphPin* Pin)
    {
        return Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
    }

    static FString PinReadableName(const UEdGraphPin* Pin)
    {
        if (!Pin)
        {
            return TEXT("None");
        }

        FString Display = Pin->GetDisplayName().ToString();
        if (Display.IsEmpty())
        {
            Display = Pin->PinName.ToString();
        }
        return Display;
    }

    static FString NodeReadableName(const UEdGraphNode* Node)
    {
        if (!Node)
        {
            return TEXT("None");
        }

        FString Title = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
        if (Title.IsEmpty())
        {
            Title = Node->GetName();
        }
        return Title.Replace(TEXT("\n"), TEXT(" "));
    }

    static FString GetGraphKind(const UEdGraph* Graph, const UBlueprint* Blueprint)
    {
        if (!Graph || !Blueprint)
        {
            return TEXT("unknown");
        }

        if (Blueprint->UbergraphPages.Contains(const_cast<UEdGraph*>(Graph)))
        {
            return TEXT("event_graph");
        }
        if (Blueprint->FunctionGraphs.Contains(const_cast<UEdGraph*>(Graph)))
        {
            return TEXT("function");
        }
        if (Blueprint->MacroGraphs.Contains(const_cast<UEdGraph*>(Graph)))
        {
            return TEXT("macro");
        }
        if (Blueprint->DelegateSignatureGraphs.Contains(const_cast<UEdGraph*>(Graph)))
        {
            return TEXT("delegate_signature");
        }
        return TEXT("unknown");
    }

    static FString GetSpecialNodeName(const UEdGraphNode* Node)
    {
        if (!Node)
        {
            return TEXT("");
        }

        if (const UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
        {
            return Entry->FunctionReference.GetMemberName().ToString();
        }
        if (const UK2Node_CustomEvent* CustomEvent = Cast<UK2Node_CustomEvent>(Node))
        {
            return CustomEvent->CustomFunctionName.ToString();
        }
        if (const UK2Node_Event* Event = Cast<UK2Node_Event>(Node))
        {
            return Event->EventReference.GetMemberName().ToString();
        }
        if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
        {
            return Call->FunctionReference.GetMemberName().ToString();
        }
        if (const UK2Node_Variable* Var = Cast<UK2Node_Variable>(Node))
        {
            return Var->VariableReference.GetMemberName().ToString();
        }
        if (const UK2Node_MacroInstance* Macro = Cast<UK2Node_MacroInstance>(Node))
        {
            return Macro->GetMacroGraph() ? Macro->GetMacroGraph()->GetName() : NodeReadableName(Node);
        }
        return NodeReadableName(Node);
    }

    static TSharedPtr<FJsonObject> PinTypeToJson(const FEdGraphPinType& PinType)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("category"), PinType.PinCategory.ToString());
        Obj->SetStringField(TEXT("sub_category"), PinType.PinSubCategory.ToString());
        Obj->SetStringField(TEXT("container"), ContainerToString(PinType.ContainerType));
        Obj->SetBoolField(TEXT("is_reference"), PinType.bIsReference);
        Obj->SetBoolField(TEXT("is_const"), PinType.bIsConst);
        Obj->SetBoolField(TEXT("is_weak_pointer"), PinType.bIsWeakPointer);

        if (PinType.PinSubCategoryObject.IsValid())
        {
            UObject* TypeObj = PinType.PinSubCategoryObject.Get();
            Obj->SetStringField(TEXT("sub_category_object"), TypeObj->GetPathName());
            Obj->SetStringField(TEXT("sub_category_object_class"), TypeObj->GetClass()->GetName());

            if (const UEnum* Enum = Cast<UEnum>(TypeObj))
            {
                TArray<TSharedPtr<FJsonValue>> Values;
                for (int32 Index = 0; Index < Enum->NumEnums(); ++Index)
                {
                    if (Enum->HasMetaData(TEXT("Hidden"), Index))
                    {
                        continue;
                    }
                    TSharedPtr<FJsonObject> ValueObj = MakeShared<FJsonObject>();
                    ValueObj->SetNumberField(TEXT("index"), Index);
                    ValueObj->SetStringField(TEXT("internal_name"), Enum->GetNameStringByIndex(Index));
                    ValueObj->SetStringField(TEXT("display_name"), Enum->GetDisplayNameTextByIndex(Index).ToString());
                    Values.Add(MakeShared<FJsonValueObject>(ValueObj));
                }
                Obj->SetArrayField(TEXT("enum_values"), Values);
            }
        }

        return Obj;
    }

    static TSharedPtr<FJsonObject> PinToJson(const UEdGraphPin* Pin, const TMap<const UEdGraphNode*, FString>& NodeIds)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        if (!Pin)
        {
            return Obj;
        }

        const FString PinId = Pin->PersistentGuid.IsValid() ? Pin->PersistentGuid.ToString(EGuidFormats::DigitsWithHyphens) : Pin->PinId.ToString(EGuidFormats::DigitsWithHyphens);
        Obj->SetStringField(TEXT("id"), PinId);
        Obj->SetStringField(TEXT("name"), Pin->PinName.ToString());
        Obj->SetStringField(TEXT("display_name"), PinReadableName(Pin));
        Obj->SetStringField(TEXT("direction"), DirectionToString(Pin->Direction));
        Obj->SetObjectField(TEXT("pin_type"), PinTypeToJson(Pin->PinType));
        Obj->SetBoolField(TEXT("is_exec"), IsExecPin(Pin));
        Obj->SetBoolField(TEXT("connected"), Pin->LinkedTo.Num() > 0);

        if (!Pin->DefaultValue.IsEmpty())
        {
            Obj->SetStringField(TEXT("default_value"), Pin->DefaultValue);
        }
        if (Pin->DefaultObject)
        {
            Obj->SetStringField(TEXT("default_object"), Pin->DefaultObject->GetPathName());
        }
        if (!Pin->DefaultTextValue.IsEmpty())
        {
            Obj->SetStringField(TEXT("default_text"), Pin->DefaultTextValue.ToString());
        }

        TArray<TSharedPtr<FJsonValue>> Links;
        for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
        {
            if (!LinkedPin || !LinkedPin->GetOwningNode())
            {
                continue;
            }
            TSharedPtr<FJsonObject> LinkObj = MakeShared<FJsonObject>();
            const UEdGraphNode* LinkedNode = LinkedPin->GetOwningNode();
            const FString* NodeId = NodeIds.Find(LinkedNode);
            LinkObj->SetStringField(TEXT("node_id"), NodeId ? *NodeId : LinkedNode->GetName());
            LinkObj->SetStringField(TEXT("node_name"), NodeReadableName(LinkedNode));
            LinkObj->SetStringField(TEXT("pin_name"), LinkedPin->PinName.ToString());
            LinkObj->SetStringField(TEXT("pin_display_name"), PinReadableName(LinkedPin));
            Links.Add(MakeShared<FJsonValueObject>(LinkObj));
        }
        Obj->SetArrayField(TEXT("links"), Links);

        return Obj;
    }

    static TSharedPtr<FJsonObject> NodeToJson(const UEdGraphNode* Node, const FString& NodeId, const TMap<const UEdGraphNode*, FString>& NodeIds)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        if (!Node)
        {
            return Obj;
        }

        Obj->SetStringField(TEXT("id"), NodeId);
        Obj->SetStringField(TEXT("class"), Node->GetClass()->GetName());
        Obj->SetStringField(TEXT("name"), GetSpecialNodeName(Node));
        Obj->SetStringField(TEXT("title"), NodeReadableName(Node));
        Obj->SetNumberField(TEXT("pos_x"), Node->NodePosX);
        Obj->SetNumberField(TEXT("pos_y"), Node->NodePosY);

        if (const UK2Node* K2Node = Cast<UK2Node>(Node))
        {
            Obj->SetStringField(TEXT("node_guid"), K2Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
        }

        if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
        {
            Obj->SetStringField(TEXT("member_name"), Call->FunctionReference.GetMemberName().ToString());
            if (UFunction* Function = Call->GetTargetFunction())
            {
                Obj->SetStringField(TEXT("function_path"), Function->GetPathName());
                Obj->SetStringField(TEXT("function_owner_class"), Function->GetOwnerClass() ? Function->GetOwnerClass()->GetPathName() : TEXT(""));
            }
        }
        else if (const UK2Node_Variable* Var = Cast<UK2Node_Variable>(Node))
        {
            Obj->SetStringField(TEXT("member_name"), Var->VariableReference.GetMemberName().ToString());
        }

        TArray<TSharedPtr<FJsonValue>> InputPins;
        TArray<TSharedPtr<FJsonValue>> OutputPins;
        for (const UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin)
            {
                continue;
            }
            if (Pin->Direction == EGPD_Input)
            {
                InputPins.Add(MakeShared<FJsonValueObject>(PinToJson(Pin, NodeIds)));
            }
            else if (Pin->Direction == EGPD_Output)
            {
                OutputPins.Add(MakeShared<FJsonValueObject>(PinToJson(Pin, NodeIds)));
            }
        }
        Obj->SetArrayField(TEXT("input_pins"), InputPins);
        Obj->SetArrayField(TEXT("output_pins"), OutputPins);

        return Obj;
    }

    static void AddReadableLink(TArray<TSharedPtr<FJsonValue>>& OutLinks, const UEdGraphNode* FromNode, const UEdGraphPin* FromPin, const UEdGraphNode* ToNode, const UEdGraphPin* ToPin, const TMap<const UEdGraphNode*, FString>& NodeIds)
    {
        if (!FromNode || !FromPin || !ToNode || !ToPin)
        {
            return;
        }

        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("from_node_id"), NodeIds.FindRef(FromNode));
        Obj->SetStringField(TEXT("from_node"), NodeReadableName(FromNode));
        Obj->SetStringField(TEXT("from_pin"), PinReadableName(FromPin));
        Obj->SetStringField(TEXT("to_node_id"), NodeIds.FindRef(ToNode));
        Obj->SetStringField(TEXT("to_node"), NodeReadableName(ToNode));
        Obj->SetStringField(TEXT("to_pin"), PinReadableName(ToPin));
        Obj->SetStringField(TEXT("readable"), FString::Printf(TEXT("%s.%s -> %s.%s"), *NodeReadableName(FromNode), *PinReadableName(FromPin), *NodeReadableName(ToNode), *PinReadableName(ToPin)));
        OutLinks.Add(MakeShared<FJsonValueObject>(Obj));
    }

    static void BuildEdges(const UEdGraph* Graph, const TMap<const UEdGraphNode*, FString>& NodeIds, TArray<TSharedPtr<FJsonValue>>& OutExecEdges, TArray<TSharedPtr<FJsonValue>>& OutDataEdges)
    {
        if (!Graph)
        {
            return;
        }

        for (const UEdGraphNode* Node : Graph->Nodes)
        {
            if (!Node)
            {
                continue;
            }
            for (const UEdGraphPin* Pin : Node->Pins)
            {
                if (!Pin || Pin->Direction != EGPD_Output)
                {
                    continue;
                }
                for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
                {
                    if (!LinkedPin || !LinkedPin->GetOwningNode())
                    {
                        continue;
                    }
                    if (IsExecPin(Pin) || IsExecPin(LinkedPin))
                    {
                        AddReadableLink(OutExecEdges, Node, Pin, LinkedPin->GetOwningNode(), LinkedPin, NodeIds);
                    }
                    else
                    {
                        AddReadableLink(OutDataEdges, Node, Pin, LinkedPin->GetOwningNode(), LinkedPin, NodeIds);
                    }
                }
            }
        }
    }

    static void TraverseExec(const UEdGraphNode* Node, const TMap<const UEdGraphNode*, FString>& NodeIds, TSet<const UEdGraphNode*>& Visited, TArray<TSharedPtr<FJsonValue>>& OutFlow, int32 Depth)
    {
        if (!Node || Visited.Contains(Node) || Depth > 256)
        {
            return;
        }

        Visited.Add(Node);
        OutFlow.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%s: %s"), *NodeIds.FindRef(Node), *NodeReadableName(Node))));

        for (const UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin || Pin->Direction != EGPD_Output || !IsExecPin(Pin))
            {
                continue;
            }
            for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
            {
                if (!LinkedPin || !LinkedPin->GetOwningNode())
                {
                    continue;
                }
                OutFlow.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("  %s -> %s"), *PinReadableName(Pin), *NodeIds.FindRef(LinkedPin->GetOwningNode()))));
                TraverseExec(LinkedPin->GetOwningNode(), NodeIds, Visited, OutFlow, Depth + 1);
            }
        }
    }

    static void BuildLinearFlow(const UEdGraph* Graph, const TMap<const UEdGraphNode*, FString>& NodeIds, TArray<TSharedPtr<FJsonValue>>& OutFlow)
    {
        if (!Graph)
        {
            return;
        }

        TArray<const UEdGraphNode*> EntryNodes;
        for (const UEdGraphNode* Node : Graph->Nodes)
        {
            if (!Node)
            {
                continue;
            }
            if (Node->IsA<UK2Node_FunctionEntry>() || Node->IsA<UK2Node_CustomEvent>() || Node->IsA<UK2Node_Event>())
            {
                EntryNodes.Add(Node);
            }
        }

        if (EntryNodes.Num() == 0 && Graph->Nodes.Num() > 0)
        {
            EntryNodes.Add(Graph->Nodes[0]);
        }

        TSet<const UEdGraphNode*> Visited;
        for (const UEdGraphNode* Entry : EntryNodes)
        {
            TraverseExec(Entry, NodeIds, Visited, OutFlow, 0);
        }
    }

    static TSharedPtr<FJsonObject> BlueprintVariableToJson(const FBPVariableDescription& Var);
    static TArray<TSharedPtr<FJsonValue>> LocalVariablesToJson(const UEdGraph* Graph);
    static void AddFunctionMetadataToJson(TSharedPtr<FJsonObject> Obj, const UEdGraph* Graph);

    static TSharedPtr<FJsonObject> GraphToFunctionJson(const UEdGraph* Graph, const UBlueprint* Blueprint)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        if (!Graph || !Blueprint)
        {
            return Obj;
        }

        Obj->SetStringField(TEXT("name"), Graph->GetName());
        Obj->SetStringField(TEXT("graph_type"), GetGraphKind(Graph, Blueprint));
        Obj->SetStringField(TEXT("graph_class"), Graph->GetClass()->GetName());
        AddFunctionMetadataToJson(Obj, Graph);
        Obj->SetArrayField(TEXT("local_variables"), LocalVariablesToJson(Graph));

        TMap<const UEdGraphNode*, FString> NodeIds;
        int32 Counter = 1;
        for (const UEdGraphNode* Node : Graph->Nodes)
        {
            if (Node)
            {
                NodeIds.Add(Node, FString::Printf(TEXT("N%d"), Counter++));
            }
        }

        TArray<TSharedPtr<FJsonValue>> Nodes;
        for (const UEdGraphNode* Node : Graph->Nodes)
        {
            if (Node)
            {
                Nodes.Add(MakeShared<FJsonValueObject>(NodeToJson(Node, NodeIds.FindRef(Node), NodeIds)));
            }
        }
        Obj->SetArrayField(TEXT("nodes"), Nodes);

        TArray<TSharedPtr<FJsonValue>> ExecEdges;
        TArray<TSharedPtr<FJsonValue>> DataEdges;
        BuildEdges(Graph, NodeIds, ExecEdges, DataEdges);
        Obj->SetArrayField(TEXT("exec_edges"), ExecEdges);
        Obj->SetArrayField(TEXT("data_edges"), DataEdges);

        TArray<TSharedPtr<FJsonValue>> LinearFlow;
        BuildLinearFlow(Graph, NodeIds, LinearFlow);
        Obj->SetArrayField(TEXT("linear_flow"), LinearFlow);

        return Obj;
    }

    static FString GetBlueprintVariableMetaDataSafe(const FBPVariableDescription& Var, const FName& Key)
    {
        // FBPVariableDescription::GetMetaData() asserts in UE4.27 when the key is missing.
        // Export must never crash because a Blueprint variable has no tooltip/category metadata.
        for (const FBPVariableMetaDataEntry& Entry : Var.MetaDataArray)
        {
            if (Entry.DataKey == Key)
            {
                return Entry.DataValue;
            }
        }
        return TEXT("");
    }

    static TSharedPtr<FJsonObject> BlueprintVariableFlagsToJson(const FBPVariableDescription& Var);

    static TSharedPtr<FJsonObject> BlueprintVariableToJson(const FBPVariableDescription& Var)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), Var.VarName.ToString());
        Obj->SetStringField(TEXT("category"), Var.Category.ToString());
        Obj->SetObjectField(TEXT("pin_type"), PinTypeToJson(Var.VarType));
        Obj->SetStringField(TEXT("default_value"), Var.DefaultValue);

        FString Tooltip = GetBlueprintVariableMetaDataSafe(Var, TEXT("tooltip"));
        if (Tooltip.IsEmpty())
        {
            Tooltip = GetBlueprintVariableMetaDataSafe(Var, TEXT("ToolTip"));
        }
        Obj->SetStringField(TEXT("tooltip"), Tooltip);
        Obj->SetObjectField(TEXT("flags"), BlueprintVariableFlagsToJson(Var));
        return Obj;
    }


    static UK2Node_FunctionEntry* FindFunctionEntryNode(const UEdGraph* Graph)
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
        return nullptr;
    }

    static FString TextToStringSafe(const FText& Text)
    {
        return Text.IsEmpty() ? TEXT("") : Text.ToString();
    }

    static bool StringLooksTrue(const FString& Value)
    {
        const FString Lower = Value.ToLower();
        return Lower == TEXT("true") || Lower == TEXT("1") || Lower == TEXT("yes");
    }

    static bool BlueprintVariableHasMetaData(const FBPVariableDescription& Var, const FName& Key)
    {
        return StringLooksTrue(GetBlueprintVariableMetaDataSafe(Var, Key));
    }

    static TSharedPtr<FJsonObject> BlueprintVariableFlagsToJson(const FBPVariableDescription& Var)
    {
        TSharedPtr<FJsonObject> Flags = MakeShared<FJsonObject>();
        Flags->SetBoolField(TEXT("instance_editable"), (Var.PropertyFlags & CPF_Edit) != 0);
        Flags->SetBoolField(TEXT("blueprint_read_only"), (Var.PropertyFlags & CPF_BlueprintReadOnly) != 0);
        Flags->SetBoolField(TEXT("save_game"), (Var.PropertyFlags & CPF_SaveGame) != 0);
        Flags->SetBoolField(TEXT("transient"), (Var.PropertyFlags & CPF_Transient) != 0);
        Flags->SetBoolField(TEXT("expose_on_spawn"), BlueprintVariableHasMetaData(Var, TEXT("ExposeOnSpawn")));
        Flags->SetBoolField(TEXT("private"), BlueprintVariableHasMetaData(Var, TEXT("BlueprintPrivate")));
        Flags->SetNumberField(TEXT("property_flags_raw"), static_cast<double>(Var.PropertyFlags));
        return Flags;
    }

    static bool GetFunctionEntryFlags(const UK2Node_FunctionEntry* Entry, uint64& OutFlags)
    {
        OutFlags = 0;
        if (!Entry)
        {
            return false;
        }

        if (const FUInt32Property* UInt32Prop = FindFProperty<FUInt32Property>(Entry->GetClass(), TEXT("FunctionFlags")))
        {
            OutFlags = UInt32Prop->GetPropertyValue_InContainer(Entry);
            return true;
        }
        if (const FIntProperty* IntProp = FindFProperty<FIntProperty>(Entry->GetClass(), TEXT("FunctionFlags")))
        {
            OutFlags = static_cast<uint64>(IntProp->GetPropertyValue_InContainer(Entry));
            return true;
        }
        if (const FUInt64Property* UInt64Prop = FindFProperty<FUInt64Property>(Entry->GetClass(), TEXT("FunctionFlags")))
        {
            OutFlags = UInt64Prop->GetPropertyValue_InContainer(Entry);
            return true;
        }

        // UE4.27 Blueprint function entry nodes store custom function flags on
        // ExtraFlags instead of a reflected FunctionFlags property. Read it by
        // reflection so the code stays safe across minor engine versions.
        if (const FUInt32Property* ExtraUInt32Prop = FindFProperty<FUInt32Property>(Entry->GetClass(), TEXT("ExtraFlags")))
        {
            OutFlags = ExtraUInt32Prop->GetPropertyValue_InContainer(Entry);
            return true;
        }
        if (const FIntProperty* ExtraIntProp = FindFProperty<FIntProperty>(Entry->GetClass(), TEXT("ExtraFlags")))
        {
            OutFlags = static_cast<uint64>(ExtraIntProp->GetPropertyValue_InContainer(Entry));
            return true;
        }
        if (const FUInt64Property* ExtraUInt64Prop = FindFProperty<FUInt64Property>(Entry->GetClass(), TEXT("ExtraFlags")))
        {
            OutFlags = ExtraUInt64Prop->GetPropertyValue_InContainer(Entry);
            return true;
        }
        return false;
    }

    static FString FunctionAccessFromFlags(uint64 Flags)
    {
        if ((Flags & FUNC_Private) != 0)
        {
            return TEXT("private");
        }
        if ((Flags & FUNC_Protected) != 0)
        {
            return TEXT("protected");
        }
        return TEXT("public");
    }

    static TSharedPtr<FJsonObject> FunctionFlagsToJson(const UEdGraph* Graph)
    {
        TSharedPtr<FJsonObject> FlagsObj = MakeShared<FJsonObject>();
        const UK2Node_FunctionEntry* Entry = FindFunctionEntryNode(Graph);

        uint64 Flags = 0;
        const bool bHasFlags = GetFunctionEntryFlags(Entry, Flags);
        FlagsObj->SetBoolField(TEXT("has_function_flags"), bHasFlags);
        FlagsObj->SetStringField(TEXT("access"), FunctionAccessFromFlags(Flags));
        FlagsObj->SetBoolField(TEXT("private"), (Flags & FUNC_Private) != 0);
        FlagsObj->SetBoolField(TEXT("protected"), (Flags & FUNC_Protected) != 0);
        FlagsObj->SetBoolField(TEXT("public"), (Flags & FUNC_Public) != 0 || ((Flags & (FUNC_Private | FUNC_Protected)) == 0));
        FlagsObj->SetBoolField(TEXT("pure"), (Flags & FUNC_BlueprintPure) != 0);
        FlagsObj->SetBoolField(TEXT("const"), (Flags & FUNC_Const) != 0);
        FlagsObj->SetBoolField(TEXT("blueprint_callable"), (Flags & FUNC_BlueprintCallable) != 0);
        FlagsObj->SetNumberField(TEXT("function_flags_raw"), static_cast<double>(Flags));
        return FlagsObj;
    }

    static void AddFunctionMetadataToJson(TSharedPtr<FJsonObject> Obj, const UEdGraph* Graph)
    {
        if (!Obj.IsValid() || !Graph)
        {
            return;
        }

        const FKismetUserDeclaredFunctionMetadata* MetaData = FBlueprintEditorUtils::GetGraphFunctionMetaData(Graph);
        if (!MetaData)
        {
            Obj->SetStringField(TEXT("category"), TEXT(""));
            Obj->SetStringField(TEXT("tooltip"), TEXT(""));
            Obj->SetStringField(TEXT("keywords"), TEXT(""));
            Obj->SetObjectField(TEXT("function_flags"), FunctionFlagsToJson(Graph));
            return;
        }

        Obj->SetStringField(TEXT("category"), TextToStringSafe(MetaData->Category));
        Obj->SetStringField(TEXT("tooltip"), TextToStringSafe(MetaData->ToolTip));
        Obj->SetStringField(TEXT("keywords"), TextToStringSafe(MetaData->Keywords));
        Obj->SetStringField(TEXT("compact_node_title"), TextToStringSafe(MetaData->CompactNodeTitle));
        Obj->SetBoolField(TEXT("call_in_editor"), MetaData->bCallInEditor);
        // UE4.27 FKismetUserDeclaredFunctionMetadata has no bThreadSafe field.
        // Keep the JSON key stable, but export a safe default instead of touching a missing API member.
        Obj->SetBoolField(TEXT("thread_safe"), false);
        Obj->SetBoolField(TEXT("deprecated"), MetaData->bIsDeprecated);
        Obj->SetStringField(TEXT("deprecation_message"), MetaData->DeprecationMessage);
        Obj->SetObjectField(TEXT("function_flags"), FunctionFlagsToJson(Graph));
    }

    static TArray<TSharedPtr<FJsonValue>> LocalVariablesToJson(const UEdGraph* Graph)
    {
        TArray<TSharedPtr<FJsonValue>> LocalVariables;

        const UK2Node_FunctionEntry* Entry = FindFunctionEntryNode(Graph);
        if (!Entry)
        {
            return LocalVariables;
        }

        for (const FBPVariableDescription& LocalVar : Entry->LocalVariables)
        {
            LocalVariables.Add(MakeShared<FJsonValueObject>(BlueprintVariableToJson(LocalVar)));
        }

        return LocalVariables;
    }

    static bool SerializeJsonObject(const TSharedPtr<FJsonObject>& Root, FString& OutJson)
    {
        TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&OutJson);
        return FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
    }

    static void AddObjectPropertyValue(TSharedPtr<FJsonObject> Obj, const FString& Key, UObject* Owner, FProperty* Property)
    {
        if (!Obj.IsValid() || !Owner || !Property)
        {
            return;
        }

        const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Owner);
        FString Value;
        Property->ExportTextItem(Value, ValuePtr, nullptr, Owner, PPF_None);
        Obj->SetStringField(Key, Value);
    }

    static TSharedPtr<FJsonObject> ExportNiagaraObject(UObject* Asset)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        if (!Asset)
        {
            return Obj;
        }

        Obj->SetStringField(TEXT("asset_name"), Asset->GetName());
        Obj->SetStringField(TEXT("asset_path"), Asset->GetPathName());
        Obj->SetStringField(TEXT("class"), Asset->GetClass()->GetName());
        Obj->SetStringField(TEXT("export_note"), TEXT("Safe reflection export. Niagara internal graph reconstruction is intentionally not modified by this tool."));

        TArray<TSharedPtr<FJsonValue>> Properties;
        for (TFieldIterator<FProperty> It(Asset->GetClass()); It; ++It)
        {
            FProperty* Property = *It;
            if (!Property || Property->HasAnyPropertyFlags(CPF_Transient))
            {
                continue;
            }

            TSharedPtr<FJsonObject> PropObj = MakeShared<FJsonObject>();
            PropObj->SetStringField(TEXT("name"), Property->GetName());
            PropObj->SetStringField(TEXT("type"), Property->GetClass()->GetName());
            AddObjectPropertyValue(PropObj, TEXT("value"), Asset, Property);
            Properties.Add(MakeShared<FJsonValueObject>(PropObj));
        }
        Obj->SetArrayField(TEXT("properties"), Properties);

        return Obj;
    }
}

bool FN2CAIExport::BuildBlueprintAIJson(UBlueprint* Blueprint, FString& OutJson, FString& OutError)
{
    using namespace N2CAIExport_Private;

    OutJson.Empty();
    OutError.Empty();

    if (!Blueprint)
    {
        OutError = TEXT("Invalid Blueprint");
        return false;
    }

    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("schema"), TEXT("N2C_AI_EXPORT_V2"));
    Root->SetStringField(TEXT("export_kind"), TEXT("Blueprint"));
    Root->SetStringField(TEXT("exported_at"), FDateTime::Now().ToIso8601());
    Root->SetStringField(TEXT("engine_target"), TEXT("UE4.27"));

    TSharedPtr<FJsonObject> Metadata = MakeShared<FJsonObject>();
    Metadata->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
    Metadata->SetStringField(TEXT("blueprint_path"), Blueprint->GetPathName());
    Metadata->SetStringField(TEXT("blueprint_class"), Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetPathName() : TEXT(""));
    Metadata->SetStringField(TEXT("parent_class"), Blueprint->ParentClass ? Blueprint->ParentClass->GetPathName() : TEXT(""));
    Metadata->SetBoolField(TEXT("is_blueprint_dirty"), Blueprint->GetOutermost() ? Blueprint->GetOutermost()->IsDirty() : false);
    Root->SetObjectField(TEXT("metadata"), Metadata);

    TArray<TSharedPtr<FJsonValue>> Variables;
    for (const FBPVariableDescription& Var : Blueprint->NewVariables)
    {
        Variables.Add(MakeShared<FJsonValueObject>(BlueprintVariableToJson(Var)));
    }
    Root->SetArrayField(TEXT("variables"), Variables);

    TArray<TSharedPtr<FJsonValue>> Functions;
    auto AddGraphs = [&Functions, Blueprint](const TArray<UEdGraph*>& Graphs)
    {
        for (const UEdGraph* Graph : Graphs)
        {
            if (Graph)
            {
                Functions.Add(MakeShared<FJsonValueObject>(GraphToFunctionJson(Graph, Blueprint)));
            }
        }
    };

    AddGraphs(Blueprint->FunctionGraphs);
    AddGraphs(Blueprint->UbergraphPages);
    AddGraphs(Blueprint->MacroGraphs);
    AddGraphs(Blueprint->DelegateSignatureGraphs);
    Root->SetArrayField(TEXT("functions"), Functions);

    TArray<TSharedPtr<FJsonValue>> Warnings;
    Warnings.Add(MakeShared<FJsonValueString>(TEXT("This export is optimized for AI/code review and patch generation, not a full binary-safe Blueprint clone.")));
    Warnings.Add(MakeShared<FJsonValueString>(TEXT("Use FunctionEntry nodes and graph_type to separate functions. Do not rely on one EventGraph as one function.")));
    Warnings.Add(MakeShared<FJsonValueString>(TEXT("Enum pins contain internal_name and display_name when the enum object is available.")));
    Root->SetArrayField(TEXT("warnings"), Warnings);

    return SerializeJsonObject(Root, OutJson);
}

bool FN2CAIExport::SaveJsonToFile(const FString& Json, const FString& TargetPath, FString& OutError)
{
    OutError.Empty();

    if (Json.IsEmpty())
    {
        OutError = TEXT("JSON is empty");
        return false;
    }

    if (TargetPath.IsEmpty())
    {
        OutError = TEXT("Target path is empty");
        return false;
    }

    IFileManager::Get().MakeDirectory(*FPaths::GetPath(TargetPath), true);
    const bool bSaved = FFileHelper::SaveStringToFile(Json, *TargetPath, FFileHelper::EEncodingOptions::ForceUTF8);
    if (!bSaved)
    {
        OutError = FString::Printf(TEXT("Failed to save JSON: %s"), *TargetPath);
        return false;
    }

    FPlatformApplicationMisc::ClipboardCopy(*Json);
    return true;
}

bool FN2CAIExport::BuildSelectedNiagaraAIJson(FString& OutJson, FString& OutError)
{
    using namespace N2CAIExport_Private;

    OutJson.Empty();
    OutError.Empty();

    TArray<FAssetData> SelectedAssets;
    if (GEditor)
    {
        GEditor->GetContentBrowserSelections(SelectedAssets);
    }

    if (SelectedAssets.Num() == 0)
    {
        OutError = TEXT("No assets selected in Content Browser");
        return false;
    }

    TArray<TSharedPtr<FJsonValue>> NiagaraAssets;
    for (const FAssetData& AssetData : SelectedAssets)
    {
        const FString ClassName = AssetData.AssetClass.ToString();
        if (!ClassName.Contains(TEXT("Niagara")))
        {
            continue;
        }

        UObject* Asset = AssetData.GetAsset();
        if (!Asset)
        {
            continue;
        }

        NiagaraAssets.Add(MakeShared<FJsonValueObject>(ExportNiagaraObject(Asset)));
    }

    if (NiagaraAssets.Num() == 0)
    {
        OutError = TEXT("No Niagara assets selected in Content Browser");
        return false;
    }

    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("schema"), TEXT("N2C_AI_EXPORT_V2"));
    Root->SetStringField(TEXT("export_kind"), TEXT("NiagaraReflection"));
    Root->SetStringField(TEXT("exported_at"), FDateTime::Now().ToIso8601());
    Root->SetStringField(TEXT("engine_target"), TEXT("UE4.27"));
    Root->SetArrayField(TEXT("niagara_assets"), NiagaraAssets);
    Root->SetStringField(TEXT("safety_note"), TEXT("Read-only reflection export. This plugin does not import or mutate Niagara graphs."));

    return SerializeJsonObject(Root, OutJson);
}
