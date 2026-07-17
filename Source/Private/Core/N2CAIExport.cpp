// Copyright (c) 2025 Nick McClure (Protospatial). All Rights Reserved.
// Back2Dead custom extension: AI-friendly Blueprint/Niagara export.

#include "Core/N2CAIExport.h"
#include "Core/N2CCoverage.h"
#include "Core/N2CMacroReference.h"

#include "Utils/N2CLogger.h"

#include "AssetData.h"
#include "Editor.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/DataTable.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Engine/TimelineTemplate.h"
#include "Engine/UserDefinedEnum.h"
#include "Engine/UserDefinedStruct.h"
#include "HAL/FileManager.h"
#include "Internationalization/Internationalization.h"
#include "K2Node.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallFunctionOnMember.h"
#include "K2Node_CallParentFunction.h"
#include "K2Node_BaseMCDelegate.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_CastByteToEnum.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_Composite.h"
#include "K2Node_CreateDelegate.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_EnumEquality.h"
#include "K2Node_EnumLiteral.h"
#include "K2Node_ForEachElementInEnum.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_EditablePinBase.h"
#include "K2Node_GetArrayItem.h"
#include "K2Node_GetDataTableRow.h"
#include "K2Node_GetEnumeratorName.h"
#include "K2Node_InputAction.h"
#include "K2Node_InputAxisEvent.h"
#include "K2Node_InputAxisKeyEvent.h"
#include "K2Node_InputKey.h"
#include "K2Node_Knot.h"
#include "K2Node_MakeArray.h"
#include "K2Node_SetFieldsInStruct.h"
#include "K2Node_Self.h"
#include "K2Node_StructOperation.h"
#include "K2Node_SwitchEnum.h"
#include "K2Node_Tunnel.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Variable.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Policies/PrettyJsonPrintPolicy.h"
#include "UObject/UnrealType.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"

#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraDataInterface.h"
#include "NiagaraParameterStore.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSourceBase.h"
#include "NiagaraScriptSource.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeOutput.h"
#include "EdGraphSchema_Niagara.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"
#include "NiagaraUserRedirectionParameterStore.h"

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

    static FString PinRawName(const UEdGraphPin* Pin)
    {
        if (!Pin)
        {
            return TEXT("None");
        }
        return Pin->PinName.ToString();
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
            Display = PinRawName(Pin);
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

    static bool GraphHasFunctionEntryName(const UEdGraph* Graph, const FName& EntryName)
    {
        if (!Graph || EntryName.IsNone())
        {
            return false;
        }
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (const UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
            {
                if (Entry->FunctionReference.GetMemberName() == EntryName)
                {
                    return true;
                }
            }
        }
        return false;
    }

    static FString GetGraphKind(const UEdGraph* Graph, const UBlueprint* Blueprint)
    {
        if (!Graph || !Blueprint)
        {
            return TEXT("unknown");
        }

        const FString GraphName = Graph->GetName();
        const FString GraphNameLower = GraphName.ToLower();
        UEdGraph* MutableGraph = const_cast<UEdGraph*>(Graph);

        // UE4.27 keeps UserConstructionScript in function-like containers in some
        // states. Classify it before the generic FunctionGraphs branch so export
        // users can reliably distinguish construction script patches from normal
        // functions.
        if (GraphName == TEXT("UserConstructionScript") ||
            GraphHasFunctionEntryName(Graph, FName(TEXT("UserConstructionScript"))) ||
            GraphNameLower.Contains(TEXT("construction")))
        {
            return TEXT("construction_script");
        }

        if (Blueprint->UbergraphPages.Contains(MutableGraph))
        {
            return TEXT("event_graph");
        }
        if (Blueprint->FunctionGraphs.Contains(MutableGraph))
        {
            return TEXT("function");
        }
        if (Blueprint->MacroGraphs.Contains(MutableGraph))
        {
            return TEXT("macro");
        }
        if (Blueprint->DelegateSignatureGraphs.Contains(MutableGraph))
        {
            return TEXT("delegate_signature");
        }

        for (const FBPInterfaceDescription& InterfaceDesc : Blueprint->ImplementedInterfaces)
        {
            if (InterfaceDesc.Graphs.Contains(MutableGraph))
            {
                return TEXT("interface_graph");
            }
        }

        const FString GraphClass = Graph->GetClass() ? Graph->GetClass()->GetName().ToLower() : TEXT("");
        UObject* Outer = Graph->GetOuter();
        const FString OuterClass = Outer && Outer->GetClass() ? Outer->GetClass()->GetName().ToLower() : TEXT("");
        if (GraphNameLower.Contains(TEXT("timeline")) || GraphClass.Contains(TEXT("timeline")) || OuterClass.Contains(TEXT("timeline")))
        {
            return TEXT("timeline_graph");
        }
        if (Outer && Outer->IsA<UEdGraphNode>())
        {
            return TEXT("collapsed_graph");
        }
        return TEXT("extra_graph");
    }

    static FString GetStableGraphIdentity(const UEdGraph* Graph, const UBlueprint* Blueprint)
    {
        return FString::Printf(TEXT("%s|%s|%s|%s"), Blueprint ? *Blueprint->GetPathName() : TEXT(""), Graph && Blueprint ? *GetGraphKind(Graph, Blueprint) : TEXT("unknown"), Graph ? *Graph->GetName() : TEXT(""), Graph ? *Graph->GetPathName() : TEXT(""));
    }

    static FString GetTunnelRole(const UK2Node_Tunnel* Tunnel)
    {
        if (!Tunnel) return FString();
        if (Tunnel->bCanHaveOutputs && !Tunnel->bCanHaveInputs) return TEXT("entry");
        if (Tunnel->bCanHaveInputs && !Tunnel->bCanHaveOutputs) return TEXT("exit");
        return TEXT("unsupported");
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

        TSharedPtr<FJsonObject> ValueType = MakeShared<FJsonObject>();
        ValueType->SetStringField(TEXT("category"), PinType.PinValueType.TerminalCategory.ToString());
        ValueType->SetStringField(TEXT("sub_category"), PinType.PinValueType.TerminalSubCategory.ToString());
        ValueType->SetStringField(TEXT("sub_category_object"), PinType.PinValueType.TerminalSubCategoryObject.IsValid() ? PinType.PinValueType.TerminalSubCategoryObject->GetPathName() : FString());
        Obj->SetObjectField(TEXT("value_type"), ValueType);

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

    static TSharedPtr<FJsonObject> NodeToJson(const UEdGraphNode* Node, const FString& NodeId, const TMap<const UEdGraphNode*, FString>& NodeIds, const UEdGraph* Graph, const UBlueprint* Blueprint)
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
        Obj->SetNumberField(TEXT("width"), Node->NodeWidth);
        Obj->SetNumberField(TEXT("height"), Node->NodeHeight);
        Obj->SetStringField(TEXT("node_guid"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));

        if (Node->IsA<UK2Node_FunctionEntry>() || Node->IsA<UK2Node_FunctionResult>())
        {
            Obj->SetStringField(TEXT("graph_name"), Graph ? Graph->GetName() : TEXT(""));
            Obj->SetStringField(TEXT("graph_type"), Graph && Blueprint ? GetGraphKind(Graph, Blueprint) : TEXT(""));
            Obj->SetStringField(TEXT("function_boundary_role"), Node->IsA<UK2Node_FunctionEntry>() ? TEXT("entry") : TEXT("result"));
            Obj->SetStringField(TEXT("function_boundary_signature_fingerprint"), FN2CCoverageClassifier::BuildFunctionBoundaryFingerprint(Blueprint, Graph, Node));
        }
        const UK2Node_Tunnel* BoundaryTunnel = Cast<UK2Node_Tunnel>(Node);
        if (BoundaryTunnel && (Node->GetClass() == UK2Node_Tunnel::StaticClass() || Node->IsA<UK2Node_Composite>()))
        {
            const UK2Node_Tunnel* Tunnel = BoundaryTunnel;
            const FString OwnerIdentity = GetStableGraphIdentity(Graph, Blueprint);
            Obj->SetStringField(TEXT("owner_blueprint_path"), Blueprint ? Blueprint->GetPathName() : TEXT(""));
            Obj->SetStringField(TEXT("owning_graph_identity"), OwnerIdentity);
            Obj->SetStringField(TEXT("owning_graph_kind"), Graph && Blueprint ? GetGraphKind(Graph, Blueprint) : TEXT("unknown"));
            Obj->SetStringField(TEXT("owning_graph_name"), Graph ? Graph->GetName() : TEXT(""));
            Obj->SetStringField(TEXT("owning_graph_path"), Graph ? Graph->GetPathName() : TEXT(""));
            Obj->SetStringField(TEXT("tunnel_role"), GetTunnelRole(Tunnel));
            Obj->SetBoolField(TEXT("can_have_inputs"), Tunnel->bCanHaveInputs);
            Obj->SetBoolField(TEXT("can_have_outputs"), Tunnel->bCanHaveOutputs);
            Obj->SetBoolField(TEXT("editable"), Tunnel->IsEditable());
            Obj->SetStringField(TEXT("graph_boundary_fingerprint"), FN2CCoverageClassifier::BuildGraphBoundaryFingerprint(Blueprint, Graph, Node));

            TArray<TSharedPtr<FJsonValue>> OrderedSignature;
            for (const UEdGraphPin* Pin : Node->Pins) if (Pin) OrderedSignature.Add(MakeShared<FJsonValueObject>(PinToJson(Pin, NodeIds)));
            Obj->SetArrayField(TEXT("user_defined_pin_signature"), OrderedSignature);
            Obj->SetArrayField(TEXT("pin_order"), OrderedSignature);

            if (const UK2Node_Composite* Composite = Cast<UK2Node_Composite>(Node))
            {
                const UEdGraph* Bound = Composite->BoundGraph;
                const FString BoundIdentity = GetStableGraphIdentity(Bound, Blueprint);
                Obj->SetStringField(TEXT("outer_graph_identity"), OwnerIdentity);
                Obj->SetStringField(TEXT("outer_graph_kind"), Graph && Blueprint ? GetGraphKind(Graph, Blueprint) : TEXT("unknown"));
                Obj->SetStringField(TEXT("composite_node_identity"), OwnerIdentity + TEXT("|composite|") + (Bound ? Bound->GetName() : TEXT("")));
                Obj->SetStringField(TEXT("bound_graph_identity"), BoundIdentity);
                Obj->SetStringField(TEXT("bound_graph_kind"), Bound && Blueprint ? GetGraphKind(Bound, Blueprint) : TEXT("unknown"));
                Obj->SetStringField(TEXT("bound_graph_name"), Bound ? Bound->GetName() : TEXT(""));
                Obj->SetStringField(TEXT("bound_graph_path"), Bound ? Bound->GetPathName() : TEXT(""));
                Obj->SetArrayField(TEXT("outer_pin_signature"), OrderedSignature);
                Obj->SetStringField(TEXT("entry_tunnel_identity"), Composite->GetEntryNode() ? BoundIdentity + TEXT("|entry") : TEXT(""));
                Obj->SetStringField(TEXT("exit_tunnel_identity"), Composite->GetExitNode() ? BoundIdentity + TEXT("|exit") : TEXT(""));
                TArray<TSharedPtr<FJsonValue>> Mapping;
                for (const UEdGraphPin* Pin : Composite->Pins) if (Pin) { TSharedPtr<FJsonObject> Map = MakeShared<FJsonObject>(); Map->SetStringField(TEXT("outer_pin"), Pin->PinName.ToString()); Map->SetStringField(TEXT("outer_direction"), DirectionToString(Pin->Direction)); Map->SetStringField(TEXT("inner_tunnel_role"), Pin->Direction == EGPD_Input ? TEXT("entry") : TEXT("exit")); Map->SetStringField(TEXT("inner_pin"), Pin->PinName.ToString()); Mapping.Add(MakeShared<FJsonValueObject>(Map)); }
                Obj->SetArrayField(TEXT("outer_to_inner_pin_mapping"), Mapping);
            }
        }
        Obj->SetStringField(TEXT("comment"), Node->NodeComment);
        Obj->SetBoolField(TEXT("comment_bubble_visible"), Node->bCommentBubbleVisible);
        Obj->SetBoolField(TEXT("comment_bubble_pinned"), Node->bCommentBubblePinned);
        Obj->SetBoolField(TEXT("advanced_view"), Node->AdvancedPinDisplay != ENodeAdvancedPins::NoPins);

        const FString NodeClassLower = Node->GetClass() ? Node->GetClass()->GetName().ToLower() : TEXT("");
        Obj->SetBoolField(TEXT("is_comment_node"), NodeClassLower.Contains(TEXT("comment")));
        Obj->SetBoolField(TEXT("is_reroute_node"), NodeClassLower.Contains(TEXT("knot")) || NodeClassLower.Contains(TEXT("reroute")));

        if (const UK2Node* K2Node = Cast<UK2Node>(Node))
        {
            Obj->SetStringField(TEXT("k2_node_guid"), K2Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
        }

        if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
        {
            Obj->SetStringField(TEXT("member_name"), Call->FunctionReference.GetMemberName().ToString());
            if (UFunction* Function = Call->GetTargetFunction())
            {
                Obj->SetStringField(TEXT("function_path"), Function->GetPathName());
                Obj->SetStringField(TEXT("function_owner_class"), Function->GetOwnerClass() ? Function->GetOwnerClass()->GetPathName() : TEXT(""));
            }
            if (const UK2Node_CallFunctionOnMember* MemberCall = Cast<UK2Node_CallFunctionOnMember>(Node))
            {
                Obj->SetStringField(TEXT("member_variable_name"), MemberCall->MemberVariableToCallOn.GetMemberName().ToString());
                Obj->SetStringField(TEXT("member_variable_owner_class"), MemberCall->MemberVariableToCallOn.GetMemberParentClass() ? MemberCall->MemberVariableToCallOn.GetMemberParentClass()->GetPathName() : TEXT(""));
            }
            if (Cast<UK2Node_CallParentFunction>(Node))
            {
                Obj->SetBoolField(TEXT("call_parent"), true);
            }
        }
        else if (const UK2Node_Event* Event = Cast<UK2Node_Event>(Node))
        {
            Obj->SetStringField(TEXT("event_name"), Event->EventReference.GetMemberName().ToString());
            UClass* OwnerClass = Event->EventReference.GetMemberParentClass();
            Obj->SetStringField(TEXT("event_owner_class"), OwnerClass ? OwnerClass->GetPathName() : TEXT(""));
            Obj->SetBoolField(TEXT("event_is_override"), Event->bOverrideFunction);
        }
        else if (const UK2Node_Variable* Var = Cast<UK2Node_Variable>(Node))
        {
            Obj->SetStringField(TEXT("member_name"), Var->VariableReference.GetMemberName().ToString());
        }

        if (const UK2Node_MacroInstance* Macro = Cast<UK2Node_MacroInstance>(Node))
        {
            FN2CMacroReference::AppendIdentity(Obj, Macro->GetMacroGraph(), Macro);
        }

        if (const UK2Node_GetArrayItem* ArrayItem = Cast<UK2Node_GetArrayItem>(Node))
        {
            const UEdGraphPin* ResultPin = ArrayItem->GetResultPin();
            Obj->SetBoolField(TEXT("return_by_ref"), ResultPin && ResultPin->PinType.bIsReference);
        }
        if (Cast<UK2Node_Knot>(Node))
        {
            Obj->SetStringField(TEXT("import_strategy"), TEXT("direct_knot"));
        }
        if (Cast<UK2Node_Self>(Node))
        {
            Obj->SetBoolField(TEXT("self_context"), true);
        }

        // Durable P0 constructor identity. Raw input/output pins below remain
        // authoritative for defaults and links.
        if (const UK2Node_StructOperation* StructNode = Cast<UK2Node_StructOperation>(Node))
        {
            Obj->SetStringField(TEXT("struct_path"), StructNode->StructType ? StructNode->StructType->GetPathName() : TEXT(""));
            TArray<TSharedPtr<FJsonValue>> Members;
            for (const UEdGraphPin* Pin : Node->Pins)
            {
                if (!Pin || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec ||
                    Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
                {
                    continue;
                }
                TSharedPtr<FJsonObject> Member = MakeShared<FJsonObject>();
                Member->SetStringField(TEXT("pin_name"), Pin->PinName.ToString());
                Member->SetStringField(TEXT("persistent_guid"), Pin->PersistentGuid.ToString(EGuidFormats::DigitsWithHyphens));
                Member->SetNumberField(TEXT("direction"), static_cast<int32>(Pin->Direction));
                Members.Add(MakeShared<FJsonValueObject>(Member));
            }
            Obj->SetArrayField(TEXT("member_pin_identity"), Members);
        }

        UEnum* EnumIdentity = nullptr;
        if (const UK2Node_SwitchEnum* Switch = Cast<UK2Node_SwitchEnum>(Node))
        {
            EnumIdentity = Switch->Enum;
            TArray<TSharedPtr<FJsonValue>> Cases;
            for (const FName Entry : Switch->EnumEntries)
            {
                Cases.Add(MakeShared<FJsonValueString>(Entry.ToString()));
            }
            Obj->SetArrayField(TEXT("enum_cases"), Cases);
            Obj->SetBoolField(TEXT("has_default_pin"), Node->FindPin(TEXT("Default")) != nullptr);
        }
        else if (const UK2Node_EnumLiteral* Literal = Cast<UK2Node_EnumLiteral>(Node)) EnumIdentity = Literal->Enum;
        else if (const UK2Node_ForEachElementInEnum* ForEach = Cast<UK2Node_ForEachElementInEnum>(Node)) EnumIdentity = ForEach->Enum;
        else if (const UK2Node_CastByteToEnum* CastNode = Cast<UK2Node_CastByteToEnum>(Node))
        {
            EnumIdentity = CastNode->Enum;
            Obj->SetBoolField(TEXT("enum_cast_safe"), CastNode->bSafe);
        }
        else if (Cast<UK2Node_GetEnumeratorName>(Node))
        {
            for (const UEdGraphPin* Pin : Node->Pins)
            {
                if (!Pin || Pin->PinName != TEXT("Enumerator")) continue;
                const UEdGraphPin* TypePin = Pin->LinkedTo.Num() > 0 ? Pin->LinkedTo[0] : Pin;
                EnumIdentity = TypePin ? Cast<UEnum>(TypePin->PinType.PinSubCategoryObject.Get()) : nullptr;
                break;
            }
        }
        else if (Cast<UK2Node_EnumEquality>(Node))
        {
            for (const UEdGraphPin* Pin : Node->Pins)
            {
                if (Pin && Pin->Direction == EGPD_Input && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Byte)
                {
                    EnumIdentity = Cast<UEnum>(Pin->PinType.PinSubCategoryObject.Get());
                    if (EnumIdentity) break;
                }
            }
        }
        if (EnumIdentity) Obj->SetStringField(TEXT("enum_path"), EnumIdentity->GetPathName());

        if (NodeClassLower.Contains(TEXT("createwidget")))
        {
            const UEdGraphPin* ClassPin = Node->FindPin(TEXT("Class"));
            if (!ClassPin) ClassPin = Node->FindPin(TEXT("WidgetType"));
            const UEdGraphPin* ResultPin = Node->FindPin(TEXT("ReturnValue"));
            Obj->SetStringField(TEXT("class_path"), ClassPin && ClassPin->DefaultObject ? ClassPin->DefaultObject->GetPathName() : TEXT(""));
            Obj->SetBoolField(TEXT("class_pin_linked"), ClassPin && ClassPin->LinkedTo.Num() > 0);
            Obj->SetStringField(TEXT("result_class_path"), ResultPin && ResultPin->PinType.PinSubCategoryObject.IsValid() ? ResultPin->PinType.PinSubCategoryObject->GetPathName() : TEXT(""));
        }

        if (const UK2Node_GetDataTableRow* GetRow = Cast<UK2Node_GetDataTableRow>(Node))
        {
            const UEdGraphPin* TablePin = GetRow->GetDataTablePin();
            const UDataTable* Table = TablePin ? Cast<UDataTable>(TablePin->DefaultObject) : nullptr;
            const UEdGraphPin* RowPin = GetRow->GetRowNamePin();
            Obj->SetStringField(TEXT("data_table_path"), Table ? Table->GetPathName() : TEXT(""));
            Obj->SetBoolField(TEXT("data_table_pin_linked"), TablePin && TablePin->LinkedTo.Num() > 0);
            Obj->SetStringField(TEXT("row_struct_path"), GetRow->GetDataTableRowStructType() ? GetRow->GetDataTableRowStructType()->GetPathName() : TEXT(""));
            Obj->SetStringField(TEXT("row_name_default"), RowPin ? RowPin->DefaultValue : TEXT(""));
            Obj->SetBoolField(TEXT("row_name_linked"), RowPin && RowPin->LinkedTo.Num() > 0);
        }

        if (const UK2Node_BaseMCDelegate* Delegate = Cast<UK2Node_BaseMCDelegate>(Node))
        {
            const FProperty* Property = Delegate->GetProperty();
            const FMulticastDelegateProperty* Multicast = CastField<FMulticastDelegateProperty>(Property);
            Obj->SetStringField(TEXT("delegate_property"), Delegate->GetPropertyName().ToString());
            Obj->SetStringField(TEXT("delegate_owner_class"), Property && Property->GetOwnerClass() ? Property->GetOwnerClass()->GetPathName() : TEXT(""));
            Obj->SetStringField(TEXT("delegate_signature"), Multicast && Multicast->SignatureFunction ? Multicast->SignatureFunction->GetPathName() : TEXT(""));
        }
        if (const UK2Node_CreateDelegate* CreateDelegate = Cast<UK2Node_CreateDelegate>(Node))
        {
            Obj->SetStringField(TEXT("selected_function_name"), CreateDelegate->SelectedFunctionName.ToString());
            Obj->SetStringField(TEXT("selected_function_guid"), CreateDelegate->SelectedFunctionGuid.ToString(EGuidFormats::DigitsWithHyphens));
        }
        if (const UK2Node_ComponentBoundEvent* Bound = Cast<UK2Node_ComponentBoundEvent>(Node))
        {
            Obj->SetStringField(TEXT("component_property_name"), Bound->ComponentPropertyName.ToString());
            Obj->SetStringField(TEXT("delegate_property"), Bound->DelegatePropertyName.ToString());
            Obj->SetStringField(TEXT("delegate_owner_class"), Bound->DelegateOwnerClass ? Bound->DelegateOwnerClass->GetPathName() : TEXT(""));
        }

        if (const UK2Node_InputAction* ActionInput = Cast<UK2Node_InputAction>(Node))
        {
            Obj->SetStringField(TEXT("input_action_name"), ActionInput->InputActionName.ToString());
            Obj->SetBoolField(TEXT("consume_input"), ActionInput->bConsumeInput);
            Obj->SetBoolField(TEXT("execute_when_paused"), ActionInput->bExecuteWhenPaused);
            Obj->SetBoolField(TEXT("override_parent_binding"), ActionInput->bOverrideParentBinding);
        }
        else if (const UK2Node_InputAxisEvent* AxisInput = Cast<UK2Node_InputAxisEvent>(Node))
        {
            Obj->SetStringField(TEXT("input_axis_name"), AxisInput->InputAxisName.ToString());
            Obj->SetBoolField(TEXT("consume_input"), AxisInput->bConsumeInput);
            Obj->SetBoolField(TEXT("execute_when_paused"), AxisInput->bExecuteWhenPaused);
            Obj->SetBoolField(TEXT("override_parent_binding"), AxisInput->bOverrideParentBinding);
        }
        else if (const UK2Node_InputAxisKeyEvent* AxisKeyInput = Cast<UK2Node_InputAxisKeyEvent>(Node))
        {
            Obj->SetStringField(TEXT("key_name"), AxisKeyInput->AxisKey.GetFName().ToString());
            Obj->SetBoolField(TEXT("consume_input"), AxisKeyInput->bConsumeInput);
            Obj->SetBoolField(TEXT("execute_when_paused"), AxisKeyInput->bExecuteWhenPaused);
            Obj->SetBoolField(TEXT("override_parent_binding"), AxisKeyInput->bOverrideParentBinding);
        }
        else if (const UK2Node_InputKey* KeyInput = Cast<UK2Node_InputKey>(Node))
        {
            Obj->SetStringField(TEXT("key_name"), KeyInput->InputKey.GetFName().ToString());
            Obj->SetBoolField(TEXT("shift"), KeyInput->bShift);
            Obj->SetBoolField(TEXT("ctrl"), KeyInput->bControl);
            Obj->SetBoolField(TEXT("alt"), KeyInput->bAlt);
            Obj->SetBoolField(TEXT("cmd"), KeyInput->bCommand);
            Obj->SetBoolField(TEXT("consume_input"), KeyInput->bConsumeInput);
            Obj->SetBoolField(TEXT("execute_when_paused"), KeyInput->bExecuteWhenPaused);
            Obj->SetBoolField(TEXT("override_parent_binding"), KeyInput->bOverrideParentBinding);
        }

        if (const UK2Node_MakeArray* MakeArray = Cast<UK2Node_MakeArray>(Node))
        {
            const UEdGraphPin* TypePin = nullptr;
            int32 InputCount = 0;
            for (const UEdGraphPin* Pin : Node->Pins)
            {
                if (Pin && Pin->Direction == EGPD_Input)
                {
                    ++InputCount;
                    if (!TypePin) TypePin = Pin;
                }
            }
            if (!TypePin) TypePin = MakeArray->GetOutputPin();
            if (TypePin)
            {
                FEdGraphPinType ElementType = TypePin->PinType;
                ElementType.ContainerType = EPinContainerType::None;
                Obj->SetObjectField(TEXT("value_pin_type"), PinTypeToJson(ElementType));
            }
            Obj->SetNumberField(TEXT("input_count"), InputCount);
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
        Obj->SetStringField(TEXT("from_pin"), PinRawName(FromPin));
        Obj->SetStringField(TEXT("from_pin_display_name"), PinReadableName(FromPin));
        Obj->SetStringField(TEXT("to_node_id"), NodeIds.FindRef(ToNode));
        Obj->SetStringField(TEXT("to_node"), NodeReadableName(ToNode));
        Obj->SetStringField(TEXT("to_pin"), PinRawName(ToPin));
        Obj->SetStringField(TEXT("to_pin_display_name"), PinReadableName(ToPin));
        Obj->SetStringField(TEXT("readable"), FString::Printf(TEXT("%s.%s -> %s.%s"), *NodeReadableName(FromNode), *PinRawName(FromPin), *NodeReadableName(ToNode), *PinRawName(ToPin)));
        const FString FromDisplay = PinReadableName(FromPin);
        const FString ToDisplay = PinReadableName(ToPin);
        if (FromDisplay != PinRawName(FromPin) || ToDisplay != PinRawName(ToPin))
        {
            Obj->SetStringField(TEXT("readable_display"), FString::Printf(TEXT("%s.%s -> %s.%s"), *NodeReadableName(FromNode), *FromDisplay, *NodeReadableName(ToNode), *ToDisplay));
        }
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

    static TSharedPtr<FJsonObject> BlueprintVariableToJson(const FBPVariableDescription& Var, const UBlueprint* Blueprint = nullptr);
    static TArray<TSharedPtr<FJsonValue>> LocalVariablesToJson(const UEdGraph* Graph);
    static void AddFunctionMetadataToJson(TSharedPtr<FJsonObject> Obj, const UEdGraph* Graph);

    static TSharedPtr<FJsonObject> GraphSignatureToJson(const UEdGraph* Graph, const TMap<const UEdGraphNode*, FString>& NodeIds)
    {
        TSharedPtr<FJsonObject> Signature = MakeShared<FJsonObject>();
        TArray<TSharedPtr<FJsonValue>> Inputs;
        TArray<TSharedPtr<FJsonValue>> Outputs;
        if (!Graph)
        {
            Signature->SetArrayField(TEXT("inputs"), Inputs);
            Signature->SetArrayField(TEXT("outputs"), Outputs);
            return Signature;
        }

        for (const UEdGraphNode* Node : Graph->Nodes)
        {
            if (!Node)
            {
                continue;
            }
            if (Node->IsA<UK2Node_FunctionEntry>())
            {
                for (const UEdGraphPin* Pin : Node->Pins)
                {
                    if (Pin && Pin->Direction == EGPD_Output && !IsExecPin(Pin))
                    {
                        Inputs.Add(MakeShared<FJsonValueObject>(PinToJson(Pin, NodeIds)));
                    }
                }
            }
            else if (Node->IsA<UK2Node_FunctionResult>())
            {
                for (const UEdGraphPin* Pin : Node->Pins)
                {
                    if (Pin && Pin->Direction == EGPD_Input && !IsExecPin(Pin))
                    {
                        Outputs.Add(MakeShared<FJsonValueObject>(PinToJson(Pin, NodeIds)));
                    }
                }
            }
        }

        Signature->SetArrayField(TEXT("inputs"), Inputs);
        Signature->SetArrayField(TEXT("outputs"), Outputs);
        return Signature;
    }

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
        Obj->SetStringField(TEXT("graph_path"), Graph->GetPathName());
        Obj->SetStringField(TEXT("owner_blueprint_path"), Blueprint->GetPathName());
        Obj->SetStringField(TEXT("owning_graph_identity"), GetStableGraphIdentity(Graph, Blueprint));
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

        Obj->SetObjectField(TEXT("signature"), GraphSignatureToJson(Graph, NodeIds));

        TArray<TSharedPtr<FJsonValue>> Nodes;
        for (const UEdGraphNode* Node : Graph->Nodes)
        {
            if (Node)
            {
                Nodes.Add(MakeShared<FJsonValueObject>(NodeToJson(Node, NodeIds.FindRef(Node), NodeIds, Graph, Blueprint)));
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

    static FString BlueprintVariableDefaultToString(const FBPVariableDescription& Var, const UBlueprint* Blueprint)
    {
        FString DefaultValue = Var.DefaultValue;
        if (!Blueprint || !Blueprint->GeneratedClass)
        {
            return DefaultValue;
        }

        UObject* CDO = Blueprint->GeneratedClass->GetDefaultObject(false);
        FProperty* Property = FindFProperty<FProperty>(Blueprint->GeneratedClass, Var.VarName);
        if (!CDO || !Property)
        {
            return DefaultValue;
        }

        const void* Address = Property->ContainerPtrToValuePtr<void>(CDO);
        if (!Address)
        {
            return DefaultValue;
        }

        DefaultValue.Reset();
        Property->ExportTextItem(DefaultValue, Address, Address, CDO, PPF_SerializedAsImportText);
        return DefaultValue;
    }

    static TSharedPtr<FJsonObject> BlueprintVariableToJson(const FBPVariableDescription& Var, const UBlueprint* Blueprint)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), Var.VarName.ToString());
        Obj->SetStringField(TEXT("category"), Var.Category.ToString());
        Obj->SetObjectField(TEXT("pin_type"), PinTypeToJson(Var.VarType));
        Obj->SetStringField(TEXT("default_value"), BlueprintVariableDefaultToString(Var, Blueprint));

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

    static void MinifyJsonString(const FString& InJson, FString& OutJson)
    {
        OutJson.Reset(InJson.Len());

        bool bInsideString = false;
        bool bEscaped = false;

        for (int32 Index = 0; Index < InJson.Len(); ++Index)
        {
            const TCHAR C = InJson[Index];

            if (bInsideString)
            {
                OutJson.AppendChar(C);

                if (bEscaped)
                {
                    bEscaped = false;
                }
                else if (C == TEXT('\\'))
                {
                    bEscaped = true;
                }
                else if (C == TEXT('\"'))
                {
                    bInsideString = false;
                }

                continue;
            }

            if (C == TEXT('\"'))
            {
                bInsideString = true;
                OutJson.AppendChar(C);
                continue;
            }

            if (C == TEXT(' ') || C == TEXT('\n') || C == TEXT('\r') || C == TEXT('\t'))
            {
                continue;
            }

            OutJson.AppendChar(C);
        }
    }

    static bool SerializeJsonObjectCompact(const TSharedPtr<FJsonObject>& Root, FString& OutJson)
    {
        // UE4.27 does not expose TCondensedJsonPrintPolicy consistently across all build setups.
        // Serialize with the stable pretty writer first, then safely strip whitespace outside strings.
        FString PrettyJson;
        if (!SerializeJsonObject(Root, PrettyJson))
        {
            OutJson.Empty();
            return false;
        }

        MinifyJsonString(PrettyJson, OutJson);
        return true;
    }

    static FString TrimJsonString(const FString& In, int32 MaxChars = 2048)
    {
        if (In.Len() <= MaxChars)
        {
            return In;
        }

        return In.Left(MaxChars) + FString::Printf(TEXT("... [N2C_TRUNCATED original_length=%d]"), In.Len());
    }

    static bool IsOpaqueOrHugePropertyName(const FString& PropertyName)
    {
        return PropertyName.Contains(TEXT("ByteCode"))
            || PropertyName.Contains(TEXT("Cached"))
            || PropertyName.Contains(TEXT("Hlsl"))
            || PropertyName.Contains(TEXT("Shader"))
            || PropertyName.Contains(TEXT("VMExecutable"))
            || PropertyName.Contains(TEXT("ScriptLiterals"))
            || PropertyName.Contains(TEXT("Generated"));
    }

    static FString SafeExportPropertyValue(FProperty* Property, const void* ValuePtr, UObject* Owner)
    {
        if (!Property || !ValuePtr)
        {
            return TEXT("");
        }

        const FString PropertyName = Property->GetName();
        if (IsOpaqueOrHugePropertyName(PropertyName))
        {
            return FString::Printf(TEXT("[N2C_OMITTED_OPAQUE_PROPERTY name=%s]"), *PropertyName);
        }

        FString Value;
        Property->ExportTextItem(Value, ValuePtr, nullptr, Owner, PPF_None);
        return TrimJsonString(Value);
    }

    static TArray<TSharedPtr<FJsonValue>> PropertyFlagsToJson(FProperty* Property)
    {
        TArray<TSharedPtr<FJsonValue>> Flags;
        if (!Property)
        {
            return Flags;
        }

        auto AddFlag = [&Flags](const TCHAR* FlagName)
        {
            Flags.Add(MakeShared<FJsonValueString>(FlagName));
        };

        if (Property->HasAnyPropertyFlags(CPF_Edit)) { AddFlag(TEXT("Edit")); }
        if (Property->HasAnyPropertyFlags(CPF_BlueprintVisible)) { AddFlag(TEXT("BlueprintVisible")); }
        if (Property->HasAnyPropertyFlags(CPF_BlueprintReadOnly)) { AddFlag(TEXT("BlueprintReadOnly")); }
        if (Property->HasAnyPropertyFlags(CPF_Transient)) { AddFlag(TEXT("Transient")); }
        if (Property->HasAnyPropertyFlags(CPF_Config)) { AddFlag(TEXT("Config")); }
        if (Property->HasAnyPropertyFlags(CPF_DisableEditOnInstance)) { AddFlag(TEXT("DisableEditOnInstance")); }
        if (Property->HasAnyPropertyFlags(CPF_EditConst)) { AddFlag(TEXT("EditConst")); }
        if (Property->HasAnyPropertyFlags(CPF_InstancedReference)) { AddFlag(TEXT("InstancedReference")); }
        if (Property->HasAnyPropertyFlags(CPF_ContainsInstancedReference)) { AddFlag(TEXT("ContainsInstancedReference")); }
        if (Property->HasAnyPropertyFlags(CPF_SaveGame)) { AddFlag(TEXT("SaveGame")); }
        if (Property->HasAnyPropertyFlags(CPF_AdvancedDisplay)) { AddFlag(TEXT("AdvancedDisplay")); }

        return Flags;
    }

    static FString SafePropertyCPPType(FProperty* Property)
    {
        if (!Property)
        {
            return TEXT("");
        }
        if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
        {
            return StructProperty->Struct ? StructProperty->GetCPPType(nullptr, 0) : TEXT("unresolved_struct");
        }
        if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
        {
            return (EnumProperty->GetEnum() && EnumProperty->GetUnderlyingProperty()) ? EnumProperty->GetCPPType(nullptr, 0) : TEXT("unresolved_enum");
        }
        if (const FClassProperty* ClassProperty = CastField<FClassProperty>(Property))
        {
            return ClassProperty->MetaClass ? ClassProperty->GetCPPType(nullptr, 0) : TEXT("unresolved_class");
        }
        if (const FSoftClassProperty* SoftClassProperty = CastField<FSoftClassProperty>(Property))
        {
            return SoftClassProperty->MetaClass ? SoftClassProperty->GetCPPType(nullptr, 0) : TEXT("unresolved_soft_class");
        }
        if (const FInterfaceProperty* InterfaceProperty = CastField<FInterfaceProperty>(Property))
        {
            return InterfaceProperty->InterfaceClass ? InterfaceProperty->GetCPPType(nullptr, 0) : TEXT("unresolved_interface");
        }
        if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
        {
            return ObjectProperty->PropertyClass ? ObjectProperty->GetCPPType() : TEXT("unresolved_object");
        }
        return Property->GetClass() ? Property->GetClass()->GetName() : TEXT("FProperty");
    }

    static void AddPropertyMetadata(TSharedPtr<FJsonObject> Obj, FProperty* Property)
    {
        if (!Obj.IsValid() || !Property)
        {
            return;
        }

        Obj->SetStringField(TEXT("name"), Property->GetName());
        Obj->SetStringField(TEXT("type"), Property->GetClass() ? Property->GetClass()->GetName() : TEXT(""));
        Obj->SetStringField(TEXT("cpp_type"), SafePropertyCPPType(Property));
        Obj->SetNumberField(TEXT("array_dim"), Property->ArrayDim);
        Obj->SetStringField(TEXT("flags_hex"), FString::Printf(TEXT("0x%016llx"), static_cast<uint64>(Property->PropertyFlags)));
        Obj->SetArrayField(TEXT("flags"), PropertyFlagsToJson(Property));

        if (Property->HasMetaData(TEXT("DisplayName")))
        {
            Obj->SetStringField(TEXT("display_name"), Property->GetMetaData(TEXT("DisplayName")));
        }
        if (Property->HasMetaData(TEXT("Category")))
        {
            Obj->SetStringField(TEXT("category"), Property->GetMetaData(TEXT("Category")));
        }
        if (Property->HasMetaData(TEXT("ToolTip")))
        {
            Obj->SetStringField(TEXT("tooltip"), Property->GetMetaData(TEXT("ToolTip")));
        }
    }

    static TSharedPtr<FJsonObject> ExportUObjectDeep(UObject* Object, UObject* RootAsset, int32 Depth, TSet<const UObject*>& Visited);
    static TSharedPtr<FJsonValue> ExportPropertyValueDeep(FProperty* Property, const void* ValuePtr, UObject* Owner, UObject* RootAsset, int32 Depth, TSet<const UObject*>& Visited);
    static bool IsLocalObjectPathForRoot(const UObject* Object, const UObject* RootAsset);
    static bool IsWorthDeepExportingLocalObject(const UObject* Object);

    static bool ShouldDeepExportReferencedObject(const UObject* Object, const UObject* RootAsset, int32 Depth)
    {
        if (!Object || !RootAsset)
        {
            return false;
        }

        if (Depth >= 18)
        {
            return false;
        }

        if (Object->IsA<UClass>() || Object->IsA<UPackage>())
        {
            return false;
        }

        if (Object == RootAsset)
        {
            return true;
        }

        // Niagara System subobjects can be nested under emitter instances rather than directly under the system.
        // Path-prefix checks catch objects like /Game/FX/NS.NS:Emitter.SpawnScript even when UObject::IsIn()
        // does not consider the system asset to be the direct outer chain owner.
        if ((Object->IsIn(RootAsset) || IsLocalObjectPathForRoot(Object, RootAsset)) && IsWorthDeepExportingLocalObject(Object))
        {
            return true;
        }

        if (Object->GetOutermost() && RootAsset->GetOutermost() && Object->GetOutermost() == RootAsset->GetOutermost() && IsWorthDeepExportingLocalObject(Object))
        {
            return true;
        }

        return false;
    }

    static TSharedPtr<FJsonObject> ExportObjectReference(UObject* Object, UObject* RootAsset, int32 Depth, TSet<const UObject*>& Visited, bool bIncludeDeepExport = false)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        if (!Object)
        {
            Obj->SetBoolField(TEXT("is_null"), true);
            return Obj;
        }

        Obj->SetBoolField(TEXT("is_null"), false);
        Obj->SetStringField(TEXT("name"), Object->GetName());
        Obj->SetStringField(TEXT("path"), Object->GetPathName());
        Obj->SetStringField(TEXT("class"), Object->GetClass() ? Object->GetClass()->GetName() : TEXT(""));
        Obj->SetStringField(TEXT("outer"), Object->GetOuter() ? Object->GetOuter()->GetPathName() : TEXT(""));
        Obj->SetStringField(TEXT("package"), Object->GetOutermost() ? Object->GetOutermost()->GetName() : TEXT(""));
        Obj->SetStringField(TEXT("object_flags_hex"), FString::Printf(TEXT("0x%08x"), static_cast<uint32>(Object->GetFlags())));
        const bool bCanDeepExport = ShouldDeepExportReferencedObject(Object, RootAsset, Depth);
        Obj->SetBoolField(TEXT("is_local_to_exported_asset"), bCanDeepExport);
        Obj->SetBoolField(TEXT("deep_export_inlined"), false);

        // Important: keep object references shallow by default.
        // The previous Niagara exporter inlined deep_export inside every object reference,
        // which duplicated SpawnScript/UpdateScript/Renderer/Graph objects hundreds of times
        // and produced 300+ MB JSON for a tiny system. Detailed object data is exported once
        // in typed summaries / local_subobjects instead.
        if (bIncludeDeepExport && bCanDeepExport)
        {
            Obj->SetBoolField(TEXT("deep_export_inlined"), true);
            Obj->SetObjectField(TEXT("deep_export"), ExportUObjectDeep(Object, RootAsset, Depth + 1, Visited));
        }

        return Obj;
    }

    static TSharedPtr<FJsonValue> ExportStructValueDeep(FStructProperty* StructProperty, const void* ValuePtr, UObject* Owner, UObject* RootAsset, int32 Depth, TSet<const UObject*>& Visited)
    {
        TSharedPtr<FJsonObject> StructObj = MakeShared<FJsonObject>();
        if (!StructProperty || !ValuePtr || !StructProperty->Struct)
        {
            return MakeShared<FJsonValueObject>(StructObj);
        }

        StructObj->SetStringField(TEXT("struct_name"), StructProperty->Struct->GetName());
        StructObj->SetStringField(TEXT("struct_path"), StructProperty->Struct->GetPathName());
        StructObj->SetStringField(TEXT("raw"), SafeExportPropertyValue(StructProperty, ValuePtr, Owner));

        TArray<TSharedPtr<FJsonValue>> Fields;
        if (Depth < 28)
        {
            for (TFieldIterator<FProperty> It(StructProperty->Struct); It; ++It)
            {
                FProperty* Field = *It;
                if (!Field)
                {
                    continue;
                }

                TSharedPtr<FJsonObject> FieldObj = MakeShared<FJsonObject>();
                AddPropertyMetadata(FieldObj, Field);

                if (Field->ArrayDim > 1)
                {
                    TArray<TSharedPtr<FJsonValue>> StaticArrayValues;
                    for (int32 StaticIndex = 0; StaticIndex < Field->ArrayDim; ++StaticIndex)
                    {
                        const void* FieldValuePtr = Field->ContainerPtrToValuePtr<void>(ValuePtr, StaticIndex);
                        StaticArrayValues.Add(ExportPropertyValueDeep(Field, FieldValuePtr, Owner, RootAsset, Depth + 1, Visited));
                    }
                    FieldObj->SetArrayField(TEXT("values"), StaticArrayValues);
                }
                else
                {
                    const void* FieldValuePtr = Field->ContainerPtrToValuePtr<void>(ValuePtr);
                    FieldObj->SetField(TEXT("value"), ExportPropertyValueDeep(Field, FieldValuePtr, Owner, RootAsset, Depth + 1, Visited));
                }

                Fields.Add(MakeShared<FJsonValueObject>(FieldObj));
            }
        }
        else
        {
            StructObj->SetBoolField(TEXT("truncated_by_depth_limit"), true);
        }

        StructObj->SetArrayField(TEXT("fields"), Fields);
        return MakeShared<FJsonValueObject>(StructObj);
    }

    static TSharedPtr<FJsonValue> ExportArrayValueDeep(FArrayProperty* ArrayProperty, const void* ValuePtr, UObject* Owner, UObject* RootAsset, int32 Depth, TSet<const UObject*>& Visited)
    {
        TSharedPtr<FJsonObject> ArrayObj = MakeShared<FJsonObject>();
        if (!ArrayProperty || !ValuePtr || !ArrayProperty->Inner)
        {
            return MakeShared<FJsonValueObject>(ArrayObj);
        }

        FScriptArrayHelper Helper(ArrayProperty, ValuePtr);
        ArrayObj->SetNumberField(TEXT("count"), Helper.Num());
        ArrayObj->SetStringField(TEXT("inner_type"), ArrayProperty->Inner->GetCPPType());
        ArrayObj->SetStringField(TEXT("raw"), SafeExportPropertyValue(ArrayProperty, ValuePtr, Owner));

        TArray<TSharedPtr<FJsonValue>> Items;
        if (Depth < 28)
        {
            for (int32 Index = 0; Index < Helper.Num(); ++Index)
            {
                TSharedPtr<FJsonObject> ItemObj = MakeShared<FJsonObject>();
                ItemObj->SetNumberField(TEXT("index"), Index);
                ItemObj->SetField(TEXT("value"), ExportPropertyValueDeep(ArrayProperty->Inner, Helper.GetRawPtr(Index), Owner, RootAsset, Depth + 1, Visited));
                Items.Add(MakeShared<FJsonValueObject>(ItemObj));
            }
        }
        else
        {
            ArrayObj->SetBoolField(TEXT("truncated_by_depth_limit"), true);
        }

        ArrayObj->SetArrayField(TEXT("items"), Items);
        return MakeShared<FJsonValueObject>(ArrayObj);
    }

    static TSharedPtr<FJsonValue> ExportSetValueDeep(FSetProperty* SetProperty, const void* ValuePtr, UObject* Owner, UObject* RootAsset, int32 Depth, TSet<const UObject*>& Visited)
    {
        TSharedPtr<FJsonObject> SetObj = MakeShared<FJsonObject>();
        if (!SetProperty || !ValuePtr || !SetProperty->ElementProp)
        {
            return MakeShared<FJsonValueObject>(SetObj);
        }

        FScriptSetHelper Helper(SetProperty, ValuePtr);
        SetObj->SetNumberField(TEXT("count"), Helper.Num());
        SetObj->SetStringField(TEXT("element_type"), SetProperty->ElementProp->GetCPPType());
        SetObj->SetStringField(TEXT("raw"), SafeExportPropertyValue(SetProperty, ValuePtr, Owner));

        TArray<TSharedPtr<FJsonValue>> Items;
        if (Depth < 28)
        {
            for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
            {
                if (!Helper.IsValidIndex(Index))
                {
                    continue;
                }

                TSharedPtr<FJsonObject> ItemObj = MakeShared<FJsonObject>();
                ItemObj->SetNumberField(TEXT("set_index"), Index);
                ItemObj->SetField(TEXT("value"), ExportPropertyValueDeep(SetProperty->ElementProp, Helper.GetElementPtr(Index), Owner, RootAsset, Depth + 1, Visited));
                Items.Add(MakeShared<FJsonValueObject>(ItemObj));
            }
        }
        else
        {
            SetObj->SetBoolField(TEXT("truncated_by_depth_limit"), true);
        }

        SetObj->SetArrayField(TEXT("items"), Items);
        return MakeShared<FJsonValueObject>(SetObj);
    }

    static TSharedPtr<FJsonValue> ExportMapValueDeep(FMapProperty* MapProperty, const void* ValuePtr, UObject* Owner, UObject* RootAsset, int32 Depth, TSet<const UObject*>& Visited)
    {
        TSharedPtr<FJsonObject> MapObj = MakeShared<FJsonObject>();
        if (!MapProperty || !ValuePtr || !MapProperty->KeyProp || !MapProperty->ValueProp)
        {
            return MakeShared<FJsonValueObject>(MapObj);
        }

        FScriptMapHelper Helper(MapProperty, ValuePtr);
        MapObj->SetNumberField(TEXT("count"), Helper.Num());
        MapObj->SetStringField(TEXT("key_type"), MapProperty->KeyProp->GetCPPType());
        MapObj->SetStringField(TEXT("value_type"), MapProperty->ValueProp->GetCPPType());
        MapObj->SetStringField(TEXT("raw"), SafeExportPropertyValue(MapProperty, ValuePtr, Owner));

        TArray<TSharedPtr<FJsonValue>> Pairs;
        if (Depth < 28)
        {
            for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
            {
                if (!Helper.IsValidIndex(Index))
                {
                    continue;
                }

                TSharedPtr<FJsonObject> PairObj = MakeShared<FJsonObject>();
                PairObj->SetNumberField(TEXT("map_index"), Index);
                PairObj->SetField(TEXT("key"), ExportPropertyValueDeep(MapProperty->KeyProp, Helper.GetKeyPtr(Index), Owner, RootAsset, Depth + 1, Visited));
                PairObj->SetField(TEXT("value"), ExportPropertyValueDeep(MapProperty->ValueProp, Helper.GetValuePtr(Index), Owner, RootAsset, Depth + 1, Visited));
                Pairs.Add(MakeShared<FJsonValueObject>(PairObj));
            }
        }
        else
        {
            MapObj->SetBoolField(TEXT("truncated_by_depth_limit"), true);
        }

        MapObj->SetArrayField(TEXT("pairs"), Pairs);
        return MakeShared<FJsonValueObject>(MapObj);
    }

    static TSharedPtr<FJsonValue> ExportEnumValueDeep(FEnumProperty* EnumProperty, const void* ValuePtr, UObject* Owner)
    {
        TSharedPtr<FJsonObject> EnumObj = MakeShared<FJsonObject>();
        if (!EnumProperty || !ValuePtr)
        {
            return MakeShared<FJsonValueObject>(EnumObj);
        }

        const UEnum* Enum = EnumProperty->GetEnum();
        const int64 NumericValue = EnumProperty->GetUnderlyingProperty()->GetSignedIntPropertyValue(ValuePtr);
        EnumObj->SetNumberField(TEXT("numeric_value"), static_cast<double>(NumericValue));
        EnumObj->SetStringField(TEXT("raw"), SafeExportPropertyValue(EnumProperty, ValuePtr, Owner));
        if (Enum)
        {
            EnumObj->SetStringField(TEXT("enum_name"), Enum->GetName());
            EnumObj->SetStringField(TEXT("enum_path"), Enum->GetPathName());
            EnumObj->SetStringField(TEXT("value_name"), Enum->GetNameStringByValue(NumericValue));
            EnumObj->SetStringField(TEXT("display_name"), Enum->GetDisplayNameTextByValue(NumericValue).ToString());
        }
        return MakeShared<FJsonValueObject>(EnumObj);
    }

    static TSharedPtr<FJsonValue> ExportByteValueDeep(FByteProperty* ByteProperty, const void* ValuePtr, UObject* Owner)
    {
        if (!ByteProperty || !ValuePtr)
        {
            return MakeShared<FJsonValueNull>();
        }

        const uint8 NumericValue = ByteProperty->GetPropertyValue(ValuePtr);
        if (!ByteProperty->Enum)
        {
            return MakeShared<FJsonValueNumber>(static_cast<double>(NumericValue));
        }

        TSharedPtr<FJsonObject> EnumObj = MakeShared<FJsonObject>();
        EnumObj->SetNumberField(TEXT("numeric_value"), static_cast<double>(NumericValue));
        EnumObj->SetStringField(TEXT("raw"), SafeExportPropertyValue(ByteProperty, ValuePtr, Owner));
        EnumObj->SetStringField(TEXT("enum_name"), ByteProperty->Enum->GetName());
        EnumObj->SetStringField(TEXT("enum_path"), ByteProperty->Enum->GetPathName());
        EnumObj->SetStringField(TEXT("value_name"), ByteProperty->Enum->GetNameStringByValue(NumericValue));
        EnumObj->SetStringField(TEXT("display_name"), ByteProperty->Enum->GetDisplayNameTextByValue(NumericValue).ToString());
        return MakeShared<FJsonValueObject>(EnumObj);
    }

    static TSharedPtr<FJsonValue> ExportPropertyValueDeep(FProperty* Property, const void* ValuePtr, UObject* Owner, UObject* RootAsset, int32 Depth, TSet<const UObject*>& Visited)
    {
        if (!Property || !ValuePtr)
        {
            return MakeShared<FJsonValueNull>();
        }

        if (Depth > 36)
        {
            TSharedPtr<FJsonObject> TruncatedObj = MakeShared<FJsonObject>();
            TruncatedObj->SetBoolField(TEXT("truncated_by_depth_limit"), true);
            TruncatedObj->SetStringField(TEXT("raw"), SafeExportPropertyValue(Property, ValuePtr, Owner));
            return MakeShared<FJsonValueObject>(TruncatedObj);
        }

        if (FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
        {
            return MakeShared<FJsonValueBoolean>(BoolProperty->GetPropertyValue(ValuePtr));
        }

        if (FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
        {
            return ExportEnumValueDeep(EnumProperty, ValuePtr, Owner);
        }

        if (FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
        {
            return ExportByteValueDeep(ByteProperty, ValuePtr, Owner);
        }

        if (FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property))
        {
            if (NumericProperty->IsFloatingPoint())
            {
                return MakeShared<FJsonValueNumber>(NumericProperty->GetFloatingPointPropertyValue(ValuePtr));
            }
            if (NumericProperty->IsInteger())
            {
                return MakeShared<FJsonValueNumber>(static_cast<double>(NumericProperty->GetSignedIntPropertyValue(ValuePtr)));
            }
        }

        if (FStrProperty* StrProperty = CastField<FStrProperty>(Property))
        {
            return MakeShared<FJsonValueString>(StrProperty->GetPropertyValue(ValuePtr));
        }

        if (FNameProperty* NameProperty = CastField<FNameProperty>(Property))
        {
            return MakeShared<FJsonValueString>(NameProperty->GetPropertyValue(ValuePtr).ToString());
        }

        if (FTextProperty* TextProperty = CastField<FTextProperty>(Property))
        {
            return MakeShared<FJsonValueString>(TextProperty->GetPropertyValue(ValuePtr).ToString());
        }

        if (FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
        {
            UObject* ObjectValue = ObjectProperty->GetObjectPropertyValue(ValuePtr);
            return MakeShared<FJsonValueObject>(ExportObjectReference(ObjectValue, RootAsset, Depth, Visited));
        }

        if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
        {
            return ExportStructValueDeep(StructProperty, ValuePtr, Owner, RootAsset, Depth + 1, Visited);
        }

        if (FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
        {
            return ExportArrayValueDeep(ArrayProperty, ValuePtr, Owner, RootAsset, Depth + 1, Visited);
        }

        if (FSetProperty* SetProperty = CastField<FSetProperty>(Property))
        {
            return ExportSetValueDeep(SetProperty, ValuePtr, Owner, RootAsset, Depth + 1, Visited);
        }

        if (FMapProperty* MapProperty = CastField<FMapProperty>(Property))
        {
            return ExportMapValueDeep(MapProperty, ValuePtr, Owner, RootAsset, Depth + 1, Visited);
        }

        return MakeShared<FJsonValueString>(SafeExportPropertyValue(Property, ValuePtr, Owner));
    }

    static TSharedPtr<FJsonObject> ExportPropertyDeep(FProperty* Property, const void* ContainerPtr, UObject* Owner, UObject* RootAsset, int32 Depth, TSet<const UObject*>& Visited)
    {
        TSharedPtr<FJsonObject> PropObj = MakeShared<FJsonObject>();
        if (!Property || !ContainerPtr)
        {
            return PropObj;
        }

        AddPropertyMetadata(PropObj, Property);

        if (Property->ArrayDim > 1)
        {
            TArray<TSharedPtr<FJsonValue>> Values;
            for (int32 StaticIndex = 0; StaticIndex < Property->ArrayDim; ++StaticIndex)
            {
                const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(ContainerPtr, StaticIndex);
                Values.Add(ExportPropertyValueDeep(Property, ValuePtr, Owner, RootAsset, Depth + 1, Visited));
            }
            PropObj->SetArrayField(TEXT("values"), Values);
        }
        else
        {
            const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(ContainerPtr);
            PropObj->SetStringField(TEXT("raw"), SafeExportPropertyValue(Property, ValuePtr, Owner));
            PropObj->SetField(TEXT("value"), ExportPropertyValueDeep(Property, ValuePtr, Owner, RootAsset, Depth + 1, Visited));
        }

        return PropObj;
    }

    static TArray<TSharedPtr<FJsonValue>> ExportObjectPropertiesDeep(UObject* Object, UObject* RootAsset, int32 Depth, TSet<const UObject*>& Visited)
    {
        TArray<TSharedPtr<FJsonValue>> Properties;
        if (!Object || !Object->GetClass())
        {
            return Properties;
        }

        for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
        {
            FProperty* Property = *It;
            if (!Property)
            {
                continue;
            }

            Properties.Add(MakeShared<FJsonValueObject>(ExportPropertyDeep(Property, Object, Object, RootAsset, Depth + 1, Visited)));
        }

        return Properties;
    }

    static TArray<TSharedPtr<FJsonValue>> ExportEditableObjectPropertiesDeep(UObject* Object, UObject* RootAsset, int32 Depth, TSet<const UObject*>& Visited)
    {
        TArray<TSharedPtr<FJsonValue>> Properties;
        if (!Object || !Object->GetClass())
        {
            return Properties;
        }

        for (TFieldIterator<FProperty> It(Object->GetClass(), EFieldIteratorFlags::IncludeSuper); It; ++It)
        {
            FProperty* Property = *It;
            if (!Property || Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated | CPF_DuplicateTransient | CPF_NonPIEDuplicateTransient))
            {
                continue;
            }
            if (!Property->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible | CPF_BlueprintReadOnly | CPF_Config))
            {
                continue;
            }

            Properties.Add(MakeShared<FJsonValueObject>(ExportPropertyDeep(Property, Object, Object, RootAsset, Depth + 1, Visited)));
        }

        return Properties;
    }

    static TSharedPtr<FJsonObject> ExportUObjectDeep(UObject* Object, UObject* RootAsset, int32 Depth, TSet<const UObject*>& Visited)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        if (!Object)
        {
            Obj->SetBoolField(TEXT("is_null"), true);
            return Obj;
        }

        Obj->SetBoolField(TEXT("is_null"), false);
        Obj->SetStringField(TEXT("name"), Object->GetName());
        Obj->SetStringField(TEXT("path"), Object->GetPathName());
        Obj->SetStringField(TEXT("class"), Object->GetClass() ? Object->GetClass()->GetName() : TEXT(""));
        Obj->SetStringField(TEXT("outer"), Object->GetOuter() ? Object->GetOuter()->GetPathName() : TEXT(""));
        Obj->SetStringField(TEXT("package"), Object->GetOutermost() ? Object->GetOutermost()->GetName() : TEXT(""));
        Obj->SetStringField(TEXT("object_flags_hex"), FString::Printf(TEXT("0x%08x"), static_cast<uint32>(Object->GetFlags())));

        if (Visited.Contains(Object))
        {
            Obj->SetBoolField(TEXT("already_visited"), true);
            return Obj;
        }

        if (Depth > 18)
        {
            Obj->SetBoolField(TEXT("truncated_by_object_depth_limit"), true);
            return Obj;
        }

        Visited.Add(Object);
        Obj->SetArrayField(TEXT("properties"), ExportObjectPropertiesDeep(Object, RootAsset ? RootAsset : Object, Depth + 1, Visited));
        return Obj;
    }

    static FString BytesToHexString(const uint8* Data, int32 NumBytes)
    {
        if (!Data || NumBytes <= 0)
        {
            return TEXT("");
        }

        FString Out;
        Out.Reserve(NumBytes * 2);
        for (int32 Index = 0; Index < NumBytes; ++Index)
        {
            Out += FString::Printf(TEXT("%02x"), Data[Index]);
        }
        return Out;
    }

    static FString GuidToExportString(const FGuid& Guid)
    {
        return Guid.IsValid() ? Guid.ToString(EGuidFormats::DigitsWithHyphens) : TEXT("");
    }

    static FString EnumValueToString(const UEnum* Enum, int64 Value)
    {
        if (!Enum)
        {
            return FString::Printf(TEXT("%lld"), Value);
        }
        return Enum->GetNameStringByValue(Value);
    }

    static bool IsLocalObjectPathForRoot(const UObject* Object, const UObject* RootAsset)
    {
        if (!Object || !RootAsset)
        {
            return false;
        }

        const FString RootPath = RootAsset->GetPathName();
        const FString ObjectPath = Object->GetPathName();
        return ObjectPath.StartsWith(RootPath + TEXT(":")) || ObjectPath.StartsWith(RootPath + TEXT("."));
    }

    static bool IsWorthDeepExportingLocalObject(const UObject* Object)
    {
        if (!Object || !Object->GetClass())
        {
            return false;
        }

        if (Object->IsA<UClass>() || Object->IsA<UPackage>())
        {
            return false;
        }

        const FString ClassName = Object->GetClass()->GetName();
        if (ClassName.Contains(TEXT("Niagara")))
        {
            return true;
        }
        if (ClassName.Contains(TEXT("EdGraph")))
        {
            return true;
        }

        return false;
    }

    static TSharedPtr<FJsonObject> ExportNiagaraTypeDefinitionJson(const FNiagaraTypeDefinition& TypeDef)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetBoolField(TEXT("is_valid"), TypeDef.IsValid());
        Obj->SetStringField(TEXT("name"), TypeDef.GetName());
        Obj->SetStringField(TEXT("display_name"), TypeDef.GetNameText().ToString());
        Obj->SetNumberField(TEXT("size_bytes"), TypeDef.IsValid() ? TypeDef.GetSize() : 0);
        Obj->SetNumberField(TEXT("alignment"), TypeDef.IsValid() ? TypeDef.GetAlignment() : 0);
        Obj->SetBoolField(TEXT("is_data_interface"), TypeDef.IsValid() ? TypeDef.IsDataInterface() : false);
        Obj->SetBoolField(TEXT("is_uobject"), TypeDef.IsValid() ? TypeDef.IsUObject() : false);
        Obj->SetBoolField(TEXT("is_enum"), TypeDef.IsValid() ? TypeDef.IsEnum() : false);

        if (UObject* TypeObject = TypeDef.IsValid() ? TypeDef.GetClass() : nullptr)
        {
            Obj->SetStringField(TEXT("class_path"), TypeObject->GetPathName());
        }
        if (UStruct* Struct = TypeDef.IsValid() ? TypeDef.GetStruct() : nullptr)
        {
            Obj->SetStringField(TEXT("struct_path"), Struct->GetPathName());
        }
        if (UEnum* Enum = TypeDef.IsValid() ? TypeDef.GetEnum() : nullptr)
        {
            Obj->SetStringField(TEXT("enum_path"), Enum->GetPathName());
        }
        return Obj;
    }

    static TSharedPtr<FJsonObject> ExportNiagaraVariableBaseJson(const FNiagaraVariableBase& Variable)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        const bool bIsValid = Variable.IsValid();
        Obj->SetStringField(TEXT("name"), Variable.GetName().ToString());
        Obj->SetBoolField(TEXT("is_valid"), bIsValid);
        Obj->SetBoolField(TEXT("is_data_interface"), bIsValid ? Variable.IsDataInterface() : false);
        Obj->SetBoolField(TEXT("is_uobject"), bIsValid ? Variable.IsUObject() : false);
        Obj->SetNumberField(TEXT("size_bytes"), bIsValid ? Variable.GetSizeInBytes() : 0);
        Obj->SetObjectField(TEXT("type"), ExportNiagaraTypeDefinitionJson(Variable.GetType()));
        return Obj;
    }

    static TSharedPtr<FJsonValue> MakeVector2Json(double X, double Y)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("x"), X);
        Obj->SetNumberField(TEXT("y"), Y);
        return MakeShared<FJsonValueObject>(Obj);
    }

    static TSharedPtr<FJsonValue> MakeVector3Json(double X, double Y, double Z)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("x"), X);
        Obj->SetNumberField(TEXT("y"), Y);
        Obj->SetNumberField(TEXT("z"), Z);
        return MakeShared<FJsonValueObject>(Obj);
    }

    static TSharedPtr<FJsonValue> MakeVector4Json(double X, double Y, double Z, double W)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("x"), X);
        Obj->SetNumberField(TEXT("y"), Y);
        Obj->SetNumberField(TEXT("z"), Z);
        Obj->SetNumberField(TEXT("w"), W);
        return MakeShared<FJsonValueObject>(Obj);
    }

    static TSharedPtr<FJsonObject> DecodeNiagaraParameterValue(const FNiagaraVariable& Variable, const uint8* Data)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetBoolField(TEXT("has_data"), Data != nullptr);
        if (!Data)
        {
            return Obj;
        }

        if (!Variable.IsValid())
        {
            Obj->SetBoolField(TEXT("invalid_variable"), true);
            return Obj;
        }

        const FNiagaraTypeDefinition& TypeDef = Variable.GetType();
        Obj->SetStringField(TEXT("as_string"), TypeDef.ToString(Data));

        if (TypeDef == FNiagaraTypeDefinition::GetFloatDef())
        {
            float Value = 0.0f;
            FMemory::Memcpy(&Value, Data, sizeof(float));
            Obj->SetNumberField(TEXT("float"), Value);
        }
        else if (TypeDef == FNiagaraTypeDefinition::GetIntDef())
        {
            int32 Value = 0;
            FMemory::Memcpy(&Value, Data, sizeof(int32));
            Obj->SetNumberField(TEXT("int32"), Value);
        }
        else if (TypeDef == FNiagaraTypeDefinition::GetBoolDef())
        {
            FNiagaraBool Value;
            FMemory::Memcpy(&Value, Data, sizeof(FNiagaraBool));
            Obj->SetBoolField(TEXT("bool"), Value.GetValue());
            Obj->SetNumberField(TEXT("raw_bool_int"), Value.GetRawValue());
        }
        else if (TypeDef == FNiagaraTypeDefinition::GetVec2Def())
        {
            FVector2D Value;
            FMemory::Memcpy(&Value, Data, sizeof(FVector2D));
            Obj->SetField(TEXT("vector2"), MakeVector2Json(Value.X, Value.Y));
        }
        else if (TypeDef == FNiagaraTypeDefinition::GetVec3Def())
        {
            FVector Value;
            FMemory::Memcpy(&Value, Data, sizeof(FVector));
            Obj->SetField(TEXT("vector3"), MakeVector3Json(Value.X, Value.Y, Value.Z));
        }
        else if (TypeDef == FNiagaraTypeDefinition::GetVec4Def())
        {
            FVector4 Value;
            FMemory::Memcpy(&Value, Data, sizeof(FVector4));
            Obj->SetField(TEXT("vector4"), MakeVector4Json(Value.X, Value.Y, Value.Z, Value.W));
        }
        else if (TypeDef == FNiagaraTypeDefinition::GetColorDef())
        {
            FLinearColor Value;
            FMemory::Memcpy(&Value, Data, sizeof(FLinearColor));
            TSharedPtr<FJsonObject> ColorObj = MakeShared<FJsonObject>();
            ColorObj->SetNumberField(TEXT("r"), Value.R);
            ColorObj->SetNumberField(TEXT("g"), Value.G);
            ColorObj->SetNumberField(TEXT("b"), Value.B);
            ColorObj->SetNumberField(TEXT("a"), Value.A);
            Obj->SetObjectField(TEXT("linear_color"), ColorObj);
        }
        else if (TypeDef == FNiagaraTypeDefinition::GetQuatDef())
        {
            FQuat Value;
            FMemory::Memcpy(&Value, Data, sizeof(FQuat));
            Obj->SetField(TEXT("quat"), MakeVector4Json(Value.X, Value.Y, Value.Z, Value.W));
        }

        return Obj;
    }

    static TSharedPtr<FJsonObject> ExportNiagaraParameterStoreTyped(const FNiagaraParameterStore& Store, UObject* RootAsset, const FString& StoreLabel)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("label"), StoreLabel);
        Obj->SetNumberField(TEXT("num_parameters"), Store.Num());
        Obj->SetNumberField(TEXT("layout_version"), Store.GetLayoutVersion());
        Obj->SetNumberField(TEXT("parameter_data_num_bytes"), Store.GetParameterDataArray().Num());
        Obj->SetNumberField(TEXT("data_interface_count"), Store.GetDataInterfaces().Num());
        Obj->SetNumberField(TEXT("uobject_count"), Store.GetUObjects().Num());
        Obj->SetBoolField(TEXT("parameters_dirty"), Store.GetParametersDirty() != 0);
        Obj->SetBoolField(TEXT("interfaces_dirty"), Store.GetInterfacesDirty() != 0);
        Obj->SetBoolField(TEXT("uobjects_dirty"), Store.GetUObjectsDirty() != 0);

#if WITH_EDITORONLY_DATA
        Obj->SetStringField(TEXT("debug_name"), Store.DebugName);
#endif

        TArray<FNiagaraVariable> Parameters;
        Store.GetParameters(Parameters);
        Parameters.Sort([&Store](const FNiagaraVariable& A, const FNiagaraVariable& B)
        {
            return Store.IndexOf(A) < Store.IndexOf(B);
        });

        TArray<TSharedPtr<FJsonValue>> ParameterItems;
        for (const FNiagaraVariable& Variable : Parameters)
        {
            TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
            ParamObj->SetObjectField(TEXT("variable"), ExportNiagaraVariableBaseJson(Variable));
            const int32 OffsetOrIndex = Store.IndexOf(Variable);
            ParamObj->SetNumberField(TEXT("offset_or_index"), OffsetOrIndex);

            if (Variable.IsValid() && Variable.IsDataInterface())
            {
                UNiagaraDataInterface* DataInterface = Store.GetDataInterface(Variable);
                ParamObj->SetStringField(TEXT("storage_kind"), TEXT("data_interface"));
                TSet<const UObject*> Visited;
                ParamObj->SetObjectField(TEXT("data_interface"), ExportObjectReference(DataInterface, RootAsset, 0, Visited));
            }
            else if (Variable.IsValid() && Variable.IsUObject())
            {
                UObject* ObjectValue = Store.GetUObject(Variable);
                ParamObj->SetStringField(TEXT("storage_kind"), TEXT("uobject"));
                TSet<const UObject*> Visited;
                ParamObj->SetObjectField(TEXT("object"), ExportObjectReference(ObjectValue, RootAsset, 0, Visited));
            }
            else
            {
                const uint8* ParameterData = Variable.IsValid() ? Store.GetParameterData(Variable) : nullptr;
                const int32 SizeBytes = Variable.IsValid() ? Variable.GetSizeInBytes() : 0;
                ParamObj->SetStringField(TEXT("storage_kind"), TEXT("parameter_data"));
                ParamObj->SetNumberField(TEXT("size_bytes"), SizeBytes);
                ParamObj->SetStringField(TEXT("bytes_hex"), BytesToHexString(ParameterData, SizeBytes));
                ParamObj->SetObjectField(TEXT("decoded_value"), DecodeNiagaraParameterValue(Variable, ParameterData));
            }

#if WITH_EDITORONLY_DATA
            if (const FGuid* Guid = Store.ParameterGuidMapping.Find(Variable))
            {
                ParamObj->SetStringField(TEXT("guid"), GuidToExportString(*Guid));
            }
#endif

            ParameterItems.Add(MakeShared<FJsonValueObject>(ParamObj));
        }
        Obj->SetArrayField(TEXT("parameters"), ParameterItems);
        return Obj;
    }

    static TSharedPtr<FJsonObject> ExportNiagaraUserParameterStoreTyped(const FNiagaraUserRedirectionParameterStore& Store, UObject* RootAsset, const FString& StoreLabel)
    {
        TSharedPtr<FJsonObject> Obj = ExportNiagaraParameterStoreTyped(Store, RootAsset, StoreLabel);
        TArray<FNiagaraVariable> UserParameters;
        Store.GetUserParameters(UserParameters);

        TArray<TSharedPtr<FJsonValue>> UserItems;
        for (const FNiagaraVariable& UserVariable : UserParameters)
        {
            TSharedPtr<FJsonObject> UserObj = MakeShared<FJsonObject>();
            UserObj->SetObjectField(TEXT("user_variable"), ExportNiagaraVariableBaseJson(UserVariable));
            UserObj->SetNumberField(TEXT("offset_or_index"), Store.IndexOf(UserVariable));
            if (UserVariable.IsValid() && !UserVariable.IsDataInterface() && !UserVariable.IsUObject())
            {
                const uint8* ParameterData = Store.GetParameterData(UserVariable);
                UserObj->SetObjectField(TEXT("decoded_value"), DecodeNiagaraParameterValue(UserVariable, ParameterData));
            }
            else
            {
                UserObj->SetStringField(TEXT("decoded_value_note"), TEXT("User parameter is an object/data-interface or invalid; see base parameters array for object reference details."));
            }
            UserItems.Add(MakeShared<FJsonValueObject>(UserObj));
        }
        Obj->SetArrayField(TEXT("user_parameters"), UserItems);
        Obj->SetNumberField(TEXT("num_user_parameters"), UserItems.Num());
        return Obj;
    }

    static TSharedPtr<FJsonObject> ExportNiagaraScriptSummary(const UNiagaraScript* Script, UObject* RootAsset, const FString& Label)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("label"), Label);
        if (!Script)
        {
            Obj->SetBoolField(TEXT("is_null"), true);
            return Obj;
        }

        Obj->SetBoolField(TEXT("is_null"), false);
        Obj->SetStringField(TEXT("name"), Script->GetName());
        Obj->SetStringField(TEXT("path"), Script->GetPathName());
        Obj->SetStringField(TEXT("class"), Script->GetClass() ? Script->GetClass()->GetName() : TEXT(""));
        Obj->SetStringField(TEXT("usage"), EnumValueToString(StaticEnum<ENiagaraScriptUsage>(), static_cast<int64>(Script->GetUsage())));
        Obj->SetStringField(TEXT("usage_id"), GuidToExportString(Script->GetUsageId()));
        Obj->SetObjectField(TEXT("rapid_iteration_parameters"), ExportNiagaraParameterStoreTyped(Script->RapidIterationParameters, RootAsset, Label + TEXT(".RapidIterationParameters")));

#if WITH_EDITORONLY_DATA
        const UNiagaraScriptSourceBase* Source = Script->GetSource(FGuid());
        TSet<const UObject*> SourceVisited;
        Obj->SetObjectField(TEXT("source"), ExportObjectReference(const_cast<UNiagaraScriptSourceBase*>(Source), RootAsset, 0, SourceVisited));
#endif

        TSet<const UObject*> ScriptVisited;
        Obj->SetObjectField(TEXT("object_ref"), ExportObjectReference(const_cast<UNiagaraScript*>(Script), RootAsset, 0, ScriptVisited));
        return Obj;
    }

    static void AddScriptSummaryIfValid(TArray<TSharedPtr<FJsonValue>>& Scripts, const UNiagaraScript* Script, UObject* RootAsset, const FString& Label)
    {
        if (Script)
        {
            Scripts.Add(MakeShared<FJsonValueObject>(ExportNiagaraScriptSummary(Script, RootAsset, Label)));
        }
    }

    static TArray<TSharedPtr<FJsonValue>> ExportSelectedRawProperties(UObject* Object, const TArray<FString>& PropertyNames);

    static TSharedPtr<FJsonObject> ExportNiagaraRendererSummary(UNiagaraRendererProperties* Renderer, UObject* RootAsset, int32 Index)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("index"), Index);
        if (!Renderer)
        {
            Obj->SetBoolField(TEXT("is_null"), true);
            return Obj;
        }

        Obj->SetBoolField(TEXT("is_null"), false);
        Obj->SetStringField(TEXT("name"), Renderer->GetName());
        Obj->SetStringField(TEXT("path"), Renderer->GetPathName());
        Obj->SetStringField(TEXT("class"), Renderer->GetClass() ? Renderer->GetClass()->GetName() : TEXT(""));
        Obj->SetBoolField(TEXT("enabled"), Renderer->GetIsEnabled());

        TSet<const UObject*> RendererVisited;
        Obj->SetObjectField(TEXT("object_ref"), ExportObjectReference(Renderer, RootAsset, 0, RendererVisited));

        TArray<FString> RendererProps;
        RendererProps.Add(TEXT("Material"));
        RendererProps.Add(TEXT("SourceMode"));
        RendererProps.Add(TEXT("Alignment"));
        RendererProps.Add(TEXT("FacingMode"));
        RendererProps.Add(TEXT("SortMode"));
        RendererProps.Add(TEXT("SubImageSize"));
        RendererProps.Add(TEXT("PositionBinding"));
        RendererProps.Add(TEXT("ColorBinding"));
        RendererProps.Add(TEXT("VelocityBinding"));
        RendererProps.Add(TEXT("SpriteRotationBinding"));
        RendererProps.Add(TEXT("SpriteSizeBinding"));
        RendererProps.Add(TEXT("SpriteFacingBinding"));
        RendererProps.Add(TEXT("SpriteAlignmentBinding"));
        RendererProps.Add(TEXT("NormalizedAgeBinding"));
        Obj->SetStringField(TEXT("properties_policy"), TEXT("v31 selected renderer properties only; full renderer reflection omitted to keep Niagara export compact"));
        Obj->SetArrayField(TEXT("properties"), ExportSelectedRawProperties(Renderer, RendererProps));
        return Obj;
    }

    static FString CleanDisplayString(FString Value)
    {
        Value.ReplaceInline(TEXT("\r"), TEXT(" "));
        Value.ReplaceInline(TEXT("\n"), TEXT(" "));
        Value.TrimStartAndEndInline();
        return Value;
    }

    static TSharedPtr<FJsonObject> MakeReadableDecodedValue(const TSharedPtr<FJsonObject>& DecodedValue)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        if (!DecodedValue.IsValid())
        {
            Obj->SetBoolField(TEXT("has_data"), false);
            return Obj;
        }

        bool bHasData = false;
        if (DecodedValue->TryGetBoolField(TEXT("has_data"), bHasData))
        {
            Obj->SetBoolField(TEXT("has_data"), bHasData);
        }

        FString AsString;
        if (DecodedValue->TryGetStringField(TEXT("as_string"), AsString))
        {
            Obj->SetStringField(TEXT("display"), CleanDisplayString(AsString));
        }

        double NumberValue = 0.0;
        if (DecodedValue->TryGetNumberField(TEXT("float"), NumberValue))
        {
            Obj->SetNumberField(TEXT("float"), NumberValue);
        }
        if (DecodedValue->TryGetNumberField(TEXT("int32"), NumberValue))
        {
            Obj->SetNumberField(TEXT("int32"), NumberValue);
        }
        if (DecodedValue->TryGetNumberField(TEXT("raw_bool_int"), NumberValue))
        {
            Obj->SetNumberField(TEXT("raw_bool_int"), NumberValue);
        }

        bool BoolValue = false;
        if (DecodedValue->TryGetBoolField(TEXT("bool"), BoolValue))
        {
            Obj->SetBoolField(TEXT("bool"), BoolValue);
        }

        if (DecodedValue->HasTypedField<EJson::Object>(TEXT("vector2")))
        {
            Obj->SetObjectField(TEXT("vector2"), DecodedValue->GetObjectField(TEXT("vector2")));
        }
        if (DecodedValue->HasTypedField<EJson::Object>(TEXT("vector3")))
        {
            Obj->SetObjectField(TEXT("vector3"), DecodedValue->GetObjectField(TEXT("vector3")));
        }
        if (DecodedValue->HasTypedField<EJson::Object>(TEXT("vector4")))
        {
            Obj->SetObjectField(TEXT("vector4"), DecodedValue->GetObjectField(TEXT("vector4")));
        }
        if (DecodedValue->HasTypedField<EJson::Object>(TEXT("linear_color")))
        {
            Obj->SetObjectField(TEXT("linear_color"), DecodedValue->GetObjectField(TEXT("linear_color")));
        }
        if (DecodedValue->HasTypedField<EJson::Object>(TEXT("quat")))
        {
            Obj->SetObjectField(TEXT("quat"), DecodedValue->GetObjectField(TEXT("quat")));
        }

        if (!Obj->HasField(TEXT("display")))
        {
            Obj->SetStringField(TEXT("display"), TEXT(""));
        }
        return Obj;
    }

    static void ParseReadableParameterName(const FString& FullName, FString& OutScope, FString& OutModule, FString& OutInput)
    {
        OutScope.Empty();
        OutModule.Empty();
        OutInput.Empty();

        FString Working = FullName;
        if (Working.StartsWith(TEXT("Constants.")))
        {
            Working = Working.RightChop(10);
        }

        TArray<FString> Parts;
        Working.ParseIntoArray(Parts, TEXT("."), true);
        if (Parts.Num() >= 3)
        {
            OutScope = Parts[0];
            OutModule = Parts[1];
            for (int32 Index = 2; Index < Parts.Num(); ++Index)
            {
                if (!OutInput.IsEmpty())
                {
                    OutInput += TEXT(".");
                }
                OutInput += Parts[Index];
            }
        }
        else if (Parts.Num() == 2)
        {
            OutModule = Parts[0];
            OutInput = Parts[1];
        }
        else if (Parts.Num() == 1)
        {
            OutModule = Parts[0];
        }
    }

    static FString GetReadableValueDisplay(const TSharedPtr<FJsonObject>& ValueObj)
    {
        if (!ValueObj.IsValid())
        {
            return TEXT("");
        }
        FString Display;
        ValueObj->TryGetStringField(TEXT("display"), Display);
        return Display;
    }

    static TSharedPtr<FJsonObject> ExportReadableParameter(const FNiagaraVariable& Variable, const FNiagaraParameterStore& Store, const FString& ScriptLabel)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        const FString FullName = Variable.GetName().ToString();

        FString Scope;
        FString Module;
        FString Input;
        ParseReadableParameterName(FullName, Scope, Module, Input);

        Obj->SetStringField(TEXT("script"), ScriptLabel);
        Obj->SetStringField(TEXT("full_name"), FullName);
        Obj->SetStringField(TEXT("scope"), Scope);
        Obj->SetStringField(TEXT("module"), Module);
        Obj->SetStringField(TEXT("input"), Input);
        Obj->SetObjectField(TEXT("type"), ExportNiagaraTypeDefinitionJson(Variable.GetType()));

        if (Variable.IsValid() && !Variable.IsDataInterface() && !Variable.IsUObject())
        {
            const uint8* ParameterData = Store.GetParameterData(Variable);
            Obj->SetObjectField(TEXT("value"), MakeReadableDecodedValue(DecodeNiagaraParameterValue(Variable, ParameterData)));
        }
        else
        {
            Obj->SetStringField(TEXT("value_note"), TEXT("Object/data-interface parameter; see typed script summary for object reference."));
        }

        return Obj;
    }

    static TArray<TSharedPtr<FJsonValue>> ExportReadableModulesFromParameterStore(const FNiagaraParameterStore& Store, const FString& ScriptLabel)
    {
        TArray<FNiagaraVariable> Parameters;
        Store.GetParameters(Parameters);
        Parameters.Sort([&Store](const FNiagaraVariable& A, const FNiagaraVariable& B)
        {
            return Store.IndexOf(A) < Store.IndexOf(B);
        });

        TArray<FString> ModuleOrder;
        TMap<FString, TArray<TSharedPtr<FJsonValue>>> InputsByModule;

        for (const FNiagaraVariable& Variable : Parameters)
        {
            if (!Variable.IsValid())
            {
                continue;
            }

            FString Scope;
            FString Module;
            FString Input;
            ParseReadableParameterName(Variable.GetName().ToString(), Scope, Module, Input);
            if (Module.IsEmpty())
            {
                Module = TEXT("<unknown_module>");
            }

            if (!InputsByModule.Contains(Module))
            {
                ModuleOrder.Add(Module);
            }

            TSharedPtr<FJsonObject> InputObj = ExportReadableParameter(Variable, Store, ScriptLabel);
            InputsByModule.FindOrAdd(Module).Add(MakeShared<FJsonValueObject>(InputObj));
        }

        TArray<TSharedPtr<FJsonValue>> Modules;
        for (const FString& ModuleName : ModuleOrder)
        {
            TSharedPtr<FJsonObject> ModuleObj = MakeShared<FJsonObject>();
            ModuleObj->SetStringField(TEXT("module"), ModuleName);
            ModuleObj->SetNumberField(TEXT("input_count"), InputsByModule.FindChecked(ModuleName).Num());
            ModuleObj->SetArrayField(TEXT("inputs"), InputsByModule.FindChecked(ModuleName));
            Modules.Add(MakeShared<FJsonValueObject>(ModuleObj));
        }

        return Modules;
    }

    static TArray<TSharedPtr<FJsonValue>> ExportReadableFlatParametersFromScript(const UNiagaraScript* Script, const FString& ScriptLabel)
    {
        TArray<TSharedPtr<FJsonValue>> Items;
        if (!Script)
        {
            return Items;
        }

        TArray<FNiagaraVariable> Parameters;
        Script->RapidIterationParameters.GetParameters(Parameters);
        Parameters.Sort([Script](const FNiagaraVariable& A, const FNiagaraVariable& B)
        {
            return Script->RapidIterationParameters.IndexOf(A) < Script->RapidIterationParameters.IndexOf(B);
        });

        for (const FNiagaraVariable& Variable : Parameters)
        {
            Items.Add(MakeShared<FJsonValueObject>(ExportReadableParameter(Variable, Script->RapidIterationParameters, ScriptLabel)));
        }
        return Items;
    }

    static TSharedPtr<FJsonObject> ExportReadableScriptStage(const UNiagaraScript* Script, const FString& StageName, const FString& ScriptLabel)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("stage"), StageName);
        Obj->SetStringField(TEXT("script_label"), ScriptLabel);
        if (!Script)
        {
            Obj->SetBoolField(TEXT("is_null"), true);
            Obj->SetNumberField(TEXT("module_count"), 0);
            Obj->SetArrayField(TEXT("modules"), TArray<TSharedPtr<FJsonValue>>());
            return Obj;
        }

        Obj->SetBoolField(TEXT("is_null"), false);
        Obj->SetStringField(TEXT("script_name"), Script->GetName());
        Obj->SetStringField(TEXT("usage"), EnumValueToString(StaticEnum<ENiagaraScriptUsage>(), static_cast<int64>(Script->GetUsage())));
        TArray<TSharedPtr<FJsonValue>> Modules = ExportReadableModulesFromParameterStore(Script->RapidIterationParameters, ScriptLabel);
        Obj->SetNumberField(TEXT("module_count"), Modules.Num());
        Obj->SetArrayField(TEXT("modules"), Modules);
        return Obj;
    }

    static void AddReadableScriptStageIfValid(TArray<TSharedPtr<FJsonValue>>& Stages, const UNiagaraScript* Script, const FString& StageName, const FString& ScriptLabel)
    {
        if (Script)
        {
            Stages.Add(MakeShared<FJsonValueObject>(ExportReadableScriptStage(Script, StageName, ScriptLabel)));
        }
    }

    static bool IsNiagaraNodeClassNamed(const UEdGraphNode* Node, const TCHAR* ExpectedClassName)
    {
        return Node && Node->GetClass() && Node->GetClass()->GetName().Equals(ExpectedClassName, ESearchCase::IgnoreCase);
    }

    static bool IsNiagaraParameterMapPin(const UEdGraphPin* Pin)
    {
        if (!Pin)
        {
            return false;
        }
        const FString Category = Pin->PinType.PinCategory.ToString();
        const FString SubCategory = Pin->PinType.PinSubCategory.ToString();
        const FString TypeObjectPath = Pin->PinType.PinSubCategoryObject.IsValid() ? Pin->PinType.PinSubCategoryObject->GetPathName() : TEXT("");
        return Category.Contains(TEXT("ParameterMap"), ESearchCase::IgnoreCase)
            || SubCategory.Contains(TEXT("ParameterMap"), ESearchCase::IgnoreCase)
            || TypeObjectPath.Contains(TEXT("NiagaraParameterMap"), ESearchCase::IgnoreCase)
            || Pin->PinName.ToString().Equals(TEXT("Map"), ESearchCase::IgnoreCase);
    }

    static bool IsNiagaraDynamicAddPin(const UEdGraphPin* Pin)
    {
        return Pin
            && Pin->PinType.PinCategory == UEdGraphSchema_Niagara::PinCategoryMisc
            && Pin->PinType.PinSubCategory == FName(TEXT("DynamicAddPin"));
    }

    static UEdGraphPin* FindNiagaraParameterMapPin(const UEdGraphNode* Node, EEdGraphPinDirection Direction)
    {
        if (!Node)
        {
            return nullptr;
        }
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin && Pin->Direction == Direction && IsNiagaraParameterMapPin(Pin))
            {
                return Pin;
            }
        }
        return nullptr;
    }

    static UEdGraphPin* FindFirstNiagaraTypedOutputPin(const UEdGraphNode* Node)
    {
        if (!Node)
        {
            return nullptr;
        }
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin && Pin->Direction == EGPD_Output && !IsNiagaraParameterMapPin(Pin) && !IsNiagaraDynamicAddPin(Pin))
            {
                return Pin;
            }
        }
        return nullptr;
    }

    static UEdGraphPin* GetSingleLinkedPinSafe(const UEdGraphPin* Pin)
    {
        if (!Pin || Pin->LinkedTo.Num() <= 0)
        {
            return nullptr;
        }
        for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
        {
            if (LinkedPin)
            {
                return LinkedPin;
            }
        }
        return nullptr;
    }

    static void CollectNiagaraUpstreamParameterMapSetNodes(const UEdGraphPin* StartingMapInput, TArray<UEdGraphNode*>& OutMapSetNodes)
    {
        const UEdGraphPin* CurrentInput = StartingMapInput;
        TSet<const UEdGraphNode*> VisitedNodes;

        for (int32 Depth = 0; CurrentInput && Depth < 256; ++Depth)
        {
            UEdGraphPin* LinkedOutput = GetSingleLinkedPinSafe(CurrentInput);
            UEdGraphNode* Node = LinkedOutput ? LinkedOutput->GetOwningNode() : nullptr;
            if (!Node || VisitedNodes.Contains(Node))
            {
                break;
            }

            VisitedNodes.Add(Node);

            if (IsNiagaraNodeClassNamed(Node, TEXT("NiagaraNodeParameterMapSet")) && !OutMapSetNodes.Contains(Node))
            {
                OutMapSetNodes.Add(Node);
            }

            CurrentInput = FindNiagaraParameterMapPin(Node, EGPD_Input);
        }
    }

    static void CollectNiagaraGraphParameterMapSetNodes(const UEdGraph* Graph, TArray<UEdGraphNode*>& OutMapSetNodes)
    {
        if (!Graph)
        {
            return;
        }

        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (IsNiagaraNodeClassNamed(Node, TEXT("NiagaraNodeParameterMapSet")) && !OutMapSetNodes.Contains(Node))
            {
                OutMapSetNodes.Add(Node);
            }
        }
    }

    static FString GetNiagaraFunctionNodeName(const UNiagaraNodeFunctionCall* Node)
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

    static FString GetNiagaraFunctionScriptPath(const UNiagaraNodeFunctionCall* Node)
    {
        if (Node && Node->FunctionScript)
        {
            return Node->FunctionScript->GetPathName();
        }
        return FString();
    }

    static FString NiagaraShortTypeFromStructPath(const FString& StructPath)
    {
        if (StructPath.Contains(TEXT("NiagaraFloat"), ESearchCase::IgnoreCase)) { return TEXT("float"); }
        if (StructPath.Contains(TEXT("NiagaraBool"), ESearchCase::IgnoreCase)) { return TEXT("bool"); }
        if (StructPath.Contains(TEXT("NiagaraInt32"), ESearchCase::IgnoreCase)) { return TEXT("int32"); }
        if (StructPath.Contains(TEXT("NiagaraVec2"), ESearchCase::IgnoreCase)) { return TEXT("vector2"); }
        if (StructPath.Contains(TEXT("NiagaraVec3"), ESearchCase::IgnoreCase) || StructPath.Contains(TEXT("Vector"), ESearchCase::IgnoreCase)) { return TEXT("vector3"); }
        if (StructPath.Contains(TEXT("NiagaraVec4"), ESearchCase::IgnoreCase)) { return TEXT("vector4"); }
        if (StructPath.Contains(TEXT("NiagaraColor"), ESearchCase::IgnoreCase) || StructPath.Contains(TEXT("LinearColor"), ESearchCase::IgnoreCase)) { return TEXT("linear_color"); }
        return TEXT("float");
    }

    static FString NiagaraShortTypeFromPin(const UEdGraphPin* Pin)
    {
        if (!Pin)
        {
            return TEXT("float");
        }
        const FString Category = Pin->PinType.PinCategory.ToString();
        const FString SubCategory = Pin->PinType.PinSubCategory.ToString();
        const FString TypeObjectPath = Pin->PinType.PinSubCategoryObject.IsValid() ? Pin->PinType.PinSubCategoryObject->GetPathName() : TEXT("");
        const FString Combined = Category + TEXT(" ") + SubCategory + TEXT(" ") + TypeObjectPath;
        return NiagaraShortTypeFromStructPath(Combined);
    }

    static FString NiagaraShortTypeFromTypeDef(const FNiagaraTypeDefinition& TypeDef)
    {
        if (!TypeDef.IsValid())
        {
            return TEXT("float");
        }
        FString Combined = TypeDef.GetName();
        if (UStruct* Struct = TypeDef.GetStruct())
        {
            Combined += TEXT(" ") + Struct->GetPathName();
        }
        if (UObject* ClassObj = TypeDef.GetClass())
        {
            Combined += TEXT(" ") + ClassObj->GetPathName();
        }
        return NiagaraShortTypeFromStructPath(Combined);
    }


    // v31: small, explicit Niagara graph debug export helpers.
    // Do not dump whole graph/node/renderer objects by default: that made normal
    // Niagara export jump to 10-20 MB.  Curves and renderer-facing settings are
    // exported through focused fields instead.
    static void AddRawPropertySummaryIfFound(TArray<TSharedPtr<FJsonValue>>& OutProperties, UObject* Object, const TCHAR* PropertyName)
    {
        if (!Object || !PropertyName)
        {
            return;
        }

        FProperty* Property = FindFProperty<FProperty>(Object->GetClass(), PropertyName);
        if (!Property)
        {
            return;
        }

        const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Object);
        TSharedPtr<FJsonObject> PropObj = MakeShared<FJsonObject>();
        PropObj->SetStringField(TEXT("name"), Property->GetName());
        PropObj->SetStringField(TEXT("type"), Property->GetClass() ? Property->GetClass()->GetName() : TEXT(""));
        PropObj->SetStringField(TEXT("cpp_type"), Property->GetCPPType());
        PropObj->SetStringField(TEXT("raw"), SafeExportPropertyValue(Property, ValuePtr, Object));
        OutProperties.Add(MakeShared<FJsonValueObject>(PropObj));
    }

    static TArray<TSharedPtr<FJsonValue>> ExportSelectedRawProperties(UObject* Object, const TArray<FString>& PropertyNames)
    {
        TArray<TSharedPtr<FJsonValue>> Properties;
        for (const FString& PropertyName : PropertyNames)
        {
            AddRawPropertySummaryIfFound(Properties, Object, *PropertyName);
        }
        return Properties;
    }

    static UObject* GetObjectPropertyValueByName(UObject* Object, const TCHAR* PropertyName)
    {
        if (!Object || !Object->GetClass() || !PropertyName)
        {
            return nullptr;
        }
        FObjectPropertyBase* ObjectProperty = FindFProperty<FObjectPropertyBase>(Object->GetClass(), PropertyName);
        if (!ObjectProperty)
        {
            return nullptr;
        }
        const void* ValuePtr = ObjectProperty->ContainerPtrToValuePtr<void>(Object);
        return ObjectProperty->GetObjectPropertyValue(ValuePtr);
    }

    static TSharedPtr<FJsonObject> ExportCompactDataInterfaceDebug(UObject* DataInterface)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        if (!DataInterface)
        {
            Obj->SetBoolField(TEXT("is_null"), true);
            return Obj;
        }

        const FString ClassName = DataInterface->GetClass() ? DataInterface->GetClass()->GetName() : TEXT("");
        Obj->SetBoolField(TEXT("is_null"), false);
        Obj->SetStringField(TEXT("name"), DataInterface->GetName());
        Obj->SetStringField(TEXT("path"), DataInterface->GetPathName());
        Obj->SetStringField(TEXT("class"), ClassName);
        Obj->SetStringField(TEXT("export_policy"), TEXT("selected raw properties only; enough to see curve keys without dumping full local object graph"));

        TArray<FString> Names;
        Names.Add(TEXT("Curve"));
        Names.Add(TEXT("XCurve"));
        Names.Add(TEXT("YCurve"));
        Names.Add(TEXT("ZCurve"));
        Names.Add(TEXT("RedCurve"));
        Names.Add(TEXT("GreenCurve"));
        Names.Add(TEXT("BlueCurve"));
        Names.Add(TEXT("AlphaCurve"));
        Names.Add(TEXT("bUseLUT"));
        Names.Add(TEXT("bExposeCurve"));
        Names.Add(TEXT("bOptimizeLUT"));
        Names.Add(TEXT("OptimizeThreshold"));
        Names.Add(TEXT("ExposedName"));
        Obj->SetArrayField(TEXT("selected_properties"), ExportSelectedRawProperties(DataInterface, Names));
        return Obj;
    }

    static TSharedPtr<FJsonObject> ExportNiagaraNodeInputDebugInfo(const UEdGraphNode* Node)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        if (!Node)
        {
            Obj->SetBoolField(TEXT("is_null"), true);
            return Obj;
        }

        UObject* MutableNode = const_cast<UEdGraphNode*>(Node);
        Obj->SetBoolField(TEXT("is_null"), false);
        Obj->SetStringField(TEXT("name"), Node->GetName());
        Obj->SetStringField(TEXT("path"), Node->GetPathName());
        Obj->SetStringField(TEXT("class"), Node->GetClass() ? Node->GetClass()->GetName() : TEXT(""));
        Obj->SetStringField(TEXT("note"), TEXT("NiagaraNodeInput used as a Dynamic Input value. If it owns a DataInterfaceCurve, selected curve keys are exported below."));

        TArray<FString> NodeProps;
        NodeProps.Add(TEXT("Input"));
        NodeProps.Add(TEXT("Usage"));
        NodeProps.Add(TEXT("CallSortPriority"));
        NodeProps.Add(TEXT("ExposureOptions"));
        Obj->SetArrayField(TEXT("selected_node_properties"), ExportSelectedRawProperties(MutableNode, NodeProps));

        UObject* DataInterface = GetObjectPropertyValueByName(MutableNode, TEXT("DataInterface"));
        Obj->SetObjectField(TEXT("data_interface"), ExportCompactDataInterfaceDebug(DataInterface));
        return Obj;
    }

    static bool SplitNiagaraOverrideAlias(const FString& Alias, FString& OutOwner, FString& OutInput)
    {
        int32 DotIndex = INDEX_NONE;
        if (!Alias.FindChar(TEXT('.'), DotIndex) || DotIndex <= 0 || DotIndex >= Alias.Len() - 1)
        {
            return false;
        }
        OutOwner = Alias.Left(DotIndex);
        OutInput = Alias.Mid(DotIndex + 1);
        return !OutOwner.IsEmpty() && !OutInput.IsEmpty();
    }

    static bool IsExportableNiagaraOverrideInputPin(const UEdGraphPin* Pin)
    {
        // Do not filter by substring "Add": valid Niagara dynamic input instances are
        // commonly named Add_Float001 / Add_Float003, and their override pins are
        // Add_Float003.A / Add_Float003.B.  The actual UI "add input" pins are
        // already filtered by IsNiagaraDynamicAddPin().
        return Pin
            && Pin->Direction == EGPD_Input
            && !IsNiagaraParameterMapPin(Pin)
            && !IsNiagaraDynamicAddPin(Pin);
    }

    static bool IsLikelyNiagaraStackModuleNode(const UNiagaraNodeFunctionCall* Node)
    {
        const FString Path = GetNiagaraFunctionScriptPath(Node);
        return !Path.IsEmpty() && !Path.Contains(TEXT("/DynamicInputs/"), ESearchCase::IgnoreCase);
    }

    static TSharedPtr<FJsonObject> ExportNiagaraLinkedInputTreeFromPin(const UEdGraphPin* OverridePin, int32 Depth);

    static TSharedPtr<FJsonObject> ExportNiagaraParameterMapGetTree(const UEdGraphNode* GetNode, const UEdGraphPin* OutputPin)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        const FString PinName = OutputPin ? OutputPin->PinName.ToString() : TEXT("");
        Obj->SetStringField(TEXT("parameter"), PinName);
        Obj->SetStringField(TEXT("type"), NiagaraShortTypeFromPin(OutputPin));
        return Obj;
    }

    static void ExportNiagaraFunctionInputOverrides(const UNiagaraNodeFunctionCall* FunctionNode, TSharedPtr<FJsonObject> InputsObj, int32 Depth)
    {
        if (!FunctionNode || !InputsObj.IsValid() || Depth > 32)
        {
            return;
        }

        const FString FunctionName = GetNiagaraFunctionNodeName(FunctionNode);
        UEdGraphPin* FunctionMapInput = FindNiagaraParameterMapPin(FunctionNode, EGPD_Input);

        // Nested Niagara dynamic inputs do not always have their override Map Set
        // immediately before the function call.  UE4.27 can insert other function
        // calls in the parameter-map chain, so walking only one link exports
        // incomplete trees, e.g. Max -> Add with Add.inputs == {}.
        //
        // Walk the upstream parameter-map chain and collect the closest Map Set
        // nodes first.  Only pins whose alias owner matches this function instance
        // are exported, so stale/foreign Map Set pins are ignored.
        TArray<UEdGraphNode*> OverrideNodes;
        CollectNiagaraUpstreamParameterMapSetNodes(FunctionMapInput, OverrideNodes);

        // Some nested Dynamic Input functions in UE4.27 keep their input override
        // Map Set nodes outside the direct upstream chain visible from the function
        // map input.  The active child function node is already reached from a
        // linked tree, so using its exact instance name as the owner filter is a
        // safe fallback and lets us export Add/Multiply/Sine/Lerp child inputs.
        CollectNiagaraGraphParameterMapSetNodes(FunctionNode->GetGraph(), OverrideNodes);

        if (OverrideNodes.Num() <= 0)
        {
            return;
        }

        TSet<FString> ExportedInputs;
        for (UEdGraphNode* OverrideNode : OverrideNodes)
        {
            if (!IsNiagaraNodeClassNamed(OverrideNode, TEXT("NiagaraNodeParameterMapSet")))
            {
                continue;
            }

            for (UEdGraphPin* Pin : OverrideNode->Pins)
            {
                if (!IsExportableNiagaraOverrideInputPin(Pin))
                {
                    continue;
                }

                FString Owner;
                FString InputName;
                if (!SplitNiagaraOverrideAlias(Pin->PinName.ToString(), Owner, InputName))
                {
                    continue;
                }
                if (!Owner.Equals(FunctionName, ESearchCase::IgnoreCase))
                {
                    continue;
                }
                if (ExportedInputs.Contains(InputName))
                {
                    continue;
                }

                TSharedPtr<FJsonObject> ChildTree = ExportNiagaraLinkedInputTreeFromPin(Pin, Depth + 1);
                if (ChildTree.IsValid())
                {
                    InputsObj->SetObjectField(InputName, ChildTree);
                    ExportedInputs.Add(InputName);
                }
            }
        }
    }

    static TSharedPtr<FJsonObject> ExportNiagaraFunctionCallTree(const UNiagaraNodeFunctionCall* FunctionNode, const UEdGraphPin* OutputPin, int32 Depth)
    {
        if (!FunctionNode || Depth > 32)
        {
            return nullptr;
        }

        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("function"), GetNiagaraFunctionScriptPath(FunctionNode));
        Obj->SetStringField(TEXT("function_name"), GetNiagaraFunctionNodeName(FunctionNode));
        Obj->SetStringField(TEXT("type"), NiagaraShortTypeFromPin(OutputPin));

        TSharedPtr<FJsonObject> InputsObj = MakeShared<FJsonObject>();
        ExportNiagaraFunctionInputOverrides(FunctionNode, InputsObj, Depth + 1);
        Obj->SetObjectField(TEXT("inputs"), InputsObj);
        return Obj;
    }

    static TSharedPtr<FJsonObject> ExportNiagaraLinkedInputTreeFromPin(const UEdGraphPin* OverridePin, int32 Depth)
    {
        if (!OverridePin || Depth > 32)
        {
            return nullptr;
        }

        UEdGraphPin* LinkedPin = GetSingleLinkedPinSafe(OverridePin);
        if (!LinkedPin)
        {
            FString Literal = OverridePin->DefaultValue;
            if (Literal.IsEmpty() && !OverridePin->DefaultTextValue.IsEmpty())
            {
                Literal = OverridePin->DefaultTextValue.ToString();
            }
            if (Literal.IsEmpty())
            {
                Literal = OverridePin->AutogeneratedDefaultValue;
            }
            if (Literal.IsEmpty())
            {
                return nullptr;
            }

            TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
            Obj->SetStringField(TEXT("literal"), Literal);
            Obj->SetStringField(TEXT("type"), NiagaraShortTypeFromPin(OverridePin));
            return Obj;
        }

        UEdGraphNode* SourceNode = LinkedPin->GetOwningNode();
        if (!SourceNode)
        {
            return nullptr;
        }
        if (IsNiagaraNodeClassNamed(SourceNode, TEXT("NiagaraNodeParameterMapGet")))
        {
            return ExportNiagaraParameterMapGetTree(SourceNode, LinkedPin);
        }
        if (const UNiagaraNodeFunctionCall* FunctionNode = Cast<UNiagaraNodeFunctionCall>(SourceNode))
        {
            return ExportNiagaraFunctionCallTree(FunctionNode, LinkedPin, Depth + 1);
        }

        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("source_node_class"), SourceNode->GetClass() ? SourceNode->GetClass()->GetName() : TEXT(""));
        Obj->SetStringField(TEXT("source_pin"), LinkedPin->PinName.ToString());
        Obj->SetStringField(TEXT("type"), NiagaraShortTypeFromPin(OverridePin));
        if (IsNiagaraNodeClassNamed(SourceNode, TEXT("NiagaraNodeInput")))
        {
            Obj->SetObjectField(TEXT("node_input_debug"), ExportNiagaraNodeInputDebugInfo(SourceNode));
        }
        return Obj;
    }

    static void CollectOrderedNiagaraModuleNodesFromOutput(const UEdGraphNode* OutputNode, TArray<UNiagaraNodeFunctionCall*>& OutModuleNodes)
    {
        UEdGraphNode* PreviousNode = const_cast<UEdGraphNode*>(OutputNode);
        while (PreviousNode)
        {
            UEdGraphPin* PreviousInputPin = FindNiagaraParameterMapPin(PreviousNode, EGPD_Input);
            UEdGraphPin* LinkedOutputPin = GetSingleLinkedPinSafe(PreviousInputPin);
            UEdGraphNode* CurrentNode = LinkedOutputPin ? LinkedOutputPin->GetOwningNode() : nullptr;
            if (!CurrentNode)
            {
                break;
            }

            if (UNiagaraNodeFunctionCall* ModuleNode = Cast<UNiagaraNodeFunctionCall>(CurrentNode))
            {
                if (IsLikelyNiagaraStackModuleNode(ModuleNode))
                {
                    OutModuleNodes.Insert(ModuleNode, 0);
                }
            }
            PreviousNode = CurrentNode;
        }
    }

    static UNiagaraScriptSource* GetNiagaraScriptSource(const UNiagaraScript* Script)
    {
        if (!Script)
        {
            return nullptr;
        }

        const UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(Script->GetSource(FGuid()));
        return const_cast<UNiagaraScriptSource*>(Source);
    }

    static bool IsReimportableNiagaraInputTree(const TSharedPtr<FJsonObject>& TreeObj, FString& OutReason, int32 Depth = 0)
    {
        if (!TreeObj.IsValid())
        {
            OutReason = TEXT("empty tree node");
            return false;
        }
        if (Depth > 64)
        {
            OutReason = TEXT("tree depth limit exceeded");
            return false;
        }

        FString LiteralValue;
        if (TreeObj->TryGetStringField(TEXT("literal"), LiteralValue))
        {
            return !LiteralValue.IsEmpty();
        }

        FString ParameterName;
        if (TreeObj->TryGetStringField(TEXT("parameter"), ParameterName))
        {
            if (ParameterName.IsEmpty())
            {
                OutReason = TEXT("empty parameter reference");
                return false;
            }
            return true;
        }

        FString UnsupportedClass;
        if (TreeObj->TryGetStringField(TEXT("source_node_class"), UnsupportedClass))
        {
            OutReason = FString::Printf(TEXT("unsupported source node class %s"), *UnsupportedClass);
            return false;
        }

        FString FunctionPath;
        if (TreeObj->TryGetStringField(TEXT("function"), FunctionPath))
        {
            if (FunctionPath.IsEmpty())
            {
                OutReason = TEXT("empty function path");
                return false;
            }

            const TSharedPtr<FJsonObject>* InputsObjectPtr = nullptr;
            if (TreeObj->TryGetObjectField(TEXT("inputs"), InputsObjectPtr) && InputsObjectPtr && InputsObjectPtr->IsValid())
            {
                for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*InputsObjectPtr)->Values)
                {
                    TSharedPtr<FJsonObject> ChildTree = Pair.Value.IsValid() ? Pair.Value->AsObject() : nullptr;
                    if (!ChildTree.IsValid())
                    {
                        OutReason = FString::Printf(TEXT("input %s is not an object"), *Pair.Key);
                        return false;
                    }

                    FString ChildReason;
                    if (!IsReimportableNiagaraInputTree(ChildTree, ChildReason, Depth + 1))
                    {
                        OutReason = FString::Printf(TEXT("%s.%s"), *Pair.Key, *ChildReason);
                        return false;
                    }
                }
            }
            return true;
        }

        OutReason = TEXT("tree node is neither literal, parameter, nor function");
        return false;
    }

    static void ExportNiagaraInputOverrideActionsForScript(const UNiagaraScript* Script, const FString& StageName, const FString& TargetName, TArray<TSharedPtr<FJsonValue>>& OutActions, TArray<TSharedPtr<FJsonValue>>* OutSkippedActions = nullptr)
    {
        UNiagaraScriptSource* Source = GetNiagaraScriptSource(Script);
        UEdGraph* Graph = Source ? Source->NodeGraph : nullptr;
        if (!Graph)
        {
            return;
        }

        TArray<UNiagaraNodeFunctionCall*> OrderedModules;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            UNiagaraNodeOutput* OutputNode = Cast<UNiagaraNodeOutput>(Node);
            if (!OutputNode || OutputNode->GetUsage() != Script->GetUsage())
            {
                continue;
            }
            CollectOrderedNiagaraModuleNodesFromOutput(OutputNode, OrderedModules);
        }

        TSet<FString> SeenActionKeys;
        for (UNiagaraNodeFunctionCall* ModuleNode : OrderedModules)
        {
            if (!ModuleNode)
            {
                continue;
            }
            const FString ModuleName = GetNiagaraFunctionNodeName(ModuleNode);
            UEdGraphPin* ModuleMapInput = FindNiagaraParameterMapPin(ModuleNode, EGPD_Input);

            // Top-level module input overrides are not guaranteed to live on the
            // ParameterMapSet immediately before the module node.  Particle Spawn
            // graphs in particular can contain extra stack/dynamic-input nodes in
            // between, so reading only PreviousOutput->OwningNode silently missed
            // reimportable overrides such as InitializeParticle.Color/Size and
            // SphereLocation.Radius.  Use the same robust search strategy as nested
            // dynamic input export: walk upstream first, then scan all MapSet nodes
            // in the graph and filter strictly by the exact module instance owner.
            TArray<UEdGraphNode*> OverrideNodes;
            CollectNiagaraUpstreamParameterMapSetNodes(ModuleMapInput, OverrideNodes);
            CollectNiagaraGraphParameterMapSetNodes(ModuleNode->GetGraph(), OverrideNodes);

            for (UEdGraphNode* OverrideNode : OverrideNodes)
            {
                if (!IsNiagaraNodeClassNamed(OverrideNode, TEXT("NiagaraNodeParameterMapSet")))
                {
                    continue;
                }

                for (UEdGraphPin* Pin : OverrideNode->Pins)
                {
                    if (!IsExportableNiagaraOverrideInputPin(Pin))
                    {
                        continue;
                    }

                    FString Owner;
                    FString InputName;
                    if (!SplitNiagaraOverrideAlias(Pin->PinName.ToString(), Owner, InputName))
                    {
                        continue;
                    }
                    if (!Owner.Equals(ModuleName, ESearchCase::IgnoreCase))
                    {
                        continue;
                    }

                    const FString Key = TargetName + TEXT("|") + StageName + TEXT("|") + ModuleName + TEXT("|") + InputName;
                    if (SeenActionKeys.Contains(Key))
                    {
                        continue;
                    }

                    TSharedPtr<FJsonObject> TreeObj = ExportNiagaraLinkedInputTreeFromPin(Pin, 0);
                    if (!TreeObj.IsValid())
                    {
                        continue;
                    }

                    FString NonReimportableReason;
                    const bool bReimportableTree = IsReimportableNiagaraInputTree(TreeObj, NonReimportableReason);

                    const FString ResolvedTargetName = TargetName.IsEmpty() ? TEXT("system") : TargetName;

                    auto BuildActionObject = [&]()
                    {
                        TSharedPtr<FJsonObject> ActionObj = MakeShared<FJsonObject>();
                        ActionObj->SetStringField(TEXT("target"), ResolvedTargetName);
                        if (!ResolvedTargetName.Equals(TEXT("system"), ESearchCase::IgnoreCase))
                        {
                            ActionObj->SetStringField(TEXT("emitter"), ResolvedTargetName);
                        }
                        ActionObj->SetStringField(TEXT("stage"), StageName);
                        ActionObj->SetStringField(TEXT("module"), ModuleName);
                        ActionObj->SetStringField(TEXT("input"), InputName);
                        ActionObj->SetStringField(TEXT("type"), NiagaraShortTypeFromPin(Pin));
                        ActionObj->SetObjectField(TEXT("tree"), TreeObj);
                        return ActionObj;
                    };

                    if (!bReimportableTree)
                    {
                        // v30: do not silently drop Niagara graph shapes we cannot safely recreate yet.
                        // FloatFromCurve / Curve DI / NiagaraNodeInput curve links are exported here
                        // for full inspection, but stay outside active import_actions.input_overrides.
                        if (OutSkippedActions)
                        {
                            TSharedPtr<FJsonObject> DebugObj = BuildActionObject();
                            DebugObj->SetStringField(TEXT("op"), TEXT("dynamic_input_tree_debug"));
                            DebugObj->SetBoolField(TEXT("reimportable"), false);
                            DebugObj->SetStringField(TEXT("skip_reason"), NonReimportableReason);
                            OutSkippedActions->Add(MakeShared<FJsonValueObject>(DebugObj));
                        }
                        continue;
                    }

                    SeenActionKeys.Add(Key);

                    TSharedPtr<FJsonObject> ActionObj = BuildActionObject();
                    ActionObj->SetStringField(TEXT("op"), TEXT("dynamic_input_tree"));
                    OutActions.Add(MakeShared<FJsonValueObject>(ActionObj));
                }
            }
        }
    }

    static TSharedPtr<FJsonObject> ExportNiagaraImportActions(UNiagaraSystem* System)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        TArray<TSharedPtr<FJsonValue>> UserParameters;
        TArray<TSharedPtr<FJsonValue>> EmitterActions;
        TArray<TSharedPtr<FJsonValue>> InputOverrides;
        TArray<TSharedPtr<FJsonValue>> SkippedInputOverrides;

        if (!System)
        {
            Obj->SetArrayField(TEXT("user_parameters"), UserParameters);
            Obj->SetArrayField(TEXT("emitters"), EmitterActions);
            Obj->SetArrayField(TEXT("input_overrides"), InputOverrides);
            Obj->SetArrayField(TEXT("skipped_input_overrides"), SkippedInputOverrides);
            Obj->SetNumberField(TEXT("emitter_action_count"), 0);
            Obj->SetNumberField(TEXT("input_override_count"), 0);
            Obj->SetNumberField(TEXT("skipped_input_override_count"), 0);
            return Obj;
        }

        TArray<FNiagaraVariable> Parameters;
        System->GetExposedParameters().GetParameters(Parameters);
        Parameters.Sort([](const FNiagaraVariable& A, const FNiagaraVariable& B)
        {
            return A.GetName().ToString() < B.GetName().ToString();
        });

        for (const FNiagaraVariable& Variable : Parameters)
        {
            if (!Variable.IsValid() || Variable.IsDataInterface() || Variable.IsUObject())
            {
                continue;
            }
            TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
            ParamObj->SetStringField(TEXT("name"), Variable.GetName().ToString());
            ParamObj->SetStringField(TEXT("type"), NiagaraShortTypeFromTypeDef(Variable.GetType()));
            const uint8* ParameterData = System->GetExposedParameters().GetParameterData(Variable);
            ParamObj->SetObjectField(TEXT("default_value"), MakeReadableDecodedValue(DecodeNiagaraParameterValue(Variable, ParameterData)));
            UserParameters.Add(MakeShared<FJsonValueObject>(ParamObj));
        }

        ExportNiagaraInputOverrideActionsForScript(System->GetSystemSpawnScript(), TEXT("System Spawn"), TEXT("system"), InputOverrides, &SkippedInputOverrides);
        ExportNiagaraInputOverrideActionsForScript(System->GetSystemUpdateScript(), TEXT("System Update"), TEXT("system"), InputOverrides, &SkippedInputOverrides);

        const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();

        // Reimportable emitter topology.
        //
        // The first emitter is treated as the existing base emitter in the target asset.
        // Additional emitter handles are exported as duplicate actions so a roundtrip into
        // a clean/one-emitter NiagaraSystem does not silently lose secondary emitters.
        //
        // This intentionally uses only the already-supported importer operation:
        //   op=duplicate, duplicate_from_index=0, name=<handle name>
        //
        // If the target already has an emitter with the same name, the importer skips it.
        for (int32 EmitterIndex = 1; EmitterIndex < Handles.Num(); ++EmitterIndex)
        {
            const FNiagaraEmitterHandle& Handle = Handles[EmitterIndex];
            UNiagaraEmitter* Emitter = Handle.GetInstance();
            if (!Emitter)
            {
                continue;
            }

            TSharedPtr<FJsonObject> EmitterObj = MakeShared<FJsonObject>();
            EmitterObj->SetStringField(TEXT("op"), TEXT("duplicate"));
            EmitterObj->SetStringField(TEXT("name"), Handle.GetName().ToString());
            EmitterObj->SetStringField(TEXT("unique_instance_name"), Handle.GetUniqueInstanceName());
            EmitterObj->SetNumberField(TEXT("duplicate_from_index"), 0);
            EmitterObj->SetNumberField(TEXT("source_index"), 0);
            EmitterObj->SetNumberField(TEXT("original_index"), EmitterIndex);
            EmitterObj->SetBoolField(TEXT("enabled"), Handle.GetIsEnabled());
            EmitterObj->SetStringField(TEXT("note"), TEXT("Recreate this extra emitter by duplicating emitter index 0 before applying input_overrides."));
            EmitterActions.Add(MakeShared<FJsonValueObject>(EmitterObj));
        }

        for (const FNiagaraEmitterHandle& Handle : Handles)
        {
            UNiagaraEmitter* Emitter = Handle.GetInstance();
            if (!Emitter)
            {
                continue;
            }
            const FString TargetName = Handle.GetName().ToString();
#if WITH_EDITORONLY_DATA
            ExportNiagaraInputOverrideActionsForScript(Emitter->EmitterSpawnScriptProps.Script, TEXT("Emitter Spawn"), TargetName, InputOverrides, &SkippedInputOverrides);
            ExportNiagaraInputOverrideActionsForScript(Emitter->EmitterUpdateScriptProps.Script, TEXT("Emitter Update"), TargetName, InputOverrides, &SkippedInputOverrides);
#endif
            ExportNiagaraInputOverrideActionsForScript(Emitter->SpawnScriptProps.Script, TEXT("Particle Spawn"), TargetName, InputOverrides, &SkippedInputOverrides);
            ExportNiagaraInputOverrideActionsForScript(Emitter->UpdateScriptProps.Script, TEXT("Particle Update"), TargetName, InputOverrides, &SkippedInputOverrides);
        }

        Obj->SetStringField(TEXT("format"), TEXT("N2C_NIAGARA_IMPORT_ACTIONS_V5_FULL_EXPORT_DEBUG_TREES"));
        Obj->SetStringField(TEXT("note"), TEXT("Best-effort reimport actions generated from live Niagara graph links. input_overrides are active/reimportable; skipped_input_overrides are full debug exports for graph shapes that are visible in the asset but not yet safe to recreate, such as FloatFromCurve/curve nodes."));
        Obj->SetArrayField(TEXT("user_parameters"), UserParameters);
        Obj->SetArrayField(TEXT("emitters"), EmitterActions);
        Obj->SetArrayField(TEXT("input_overrides"), InputOverrides);
        Obj->SetArrayField(TEXT("skipped_input_overrides"), SkippedInputOverrides);
        Obj->SetNumberField(TEXT("user_parameter_count"), UserParameters.Num());
        Obj->SetNumberField(TEXT("emitter_action_count"), EmitterActions.Num());
        Obj->SetNumberField(TEXT("input_override_count"), InputOverrides.Num());
        Obj->SetNumberField(TEXT("skipped_input_override_count"), SkippedInputOverrides.Num());
        return Obj;
    }

    static FString GetRawPropertyValueByName(UObject* Object, const TCHAR* PropertyName)
    {
        if (!Object || !Object->GetClass() || !PropertyName)
        {
            return TEXT("");
        }

        FProperty* Property = FindFProperty<FProperty>(Object->GetClass(), PropertyName);
        if (!Property)
        {
            return TEXT("");
        }

        const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Object);
        return SafeExportPropertyValue(Property, ValuePtr, Object);
    }

    static FString GetObjectPropertyPathByName(UObject* Object, const TCHAR* PropertyName)
    {
        if (!Object || !Object->GetClass() || !PropertyName)
        {
            return TEXT("");
        }

        FObjectPropertyBase* Property = FindFProperty<FObjectPropertyBase>(Object->GetClass(), PropertyName);
        if (!Property)
        {
            return TEXT("");
        }

        const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Object);
        UObject* Value = Property->GetObjectPropertyValue(ValuePtr);
        return Value ? Value->GetPathName() : TEXT("");
    }

    static FString GetReadableRendererType(const FString& ClassName)
    {
        if (ClassName.Contains(TEXT("Sprite")))
        {
            return TEXT("sprite");
        }
        if (ClassName.Contains(TEXT("Mesh")))
        {
            return TEXT("mesh");
        }
        if (ClassName.Contains(TEXT("Ribbon")))
        {
            return TEXT("ribbon");
        }
        if (ClassName.Contains(TEXT("Light")))
        {
            return TEXT("light");
        }
        if (ClassName.Contains(TEXT("Component")))
        {
            return TEXT("component");
        }
        return TEXT("unknown");
    }

    static TSharedPtr<FJsonObject> ExportReadableRenderer(UNiagaraRendererProperties* Renderer, int32 Index)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("index"), Index);
        if (!Renderer)
        {
            Obj->SetBoolField(TEXT("is_null"), true);
            return Obj;
        }

        const FString ClassName = Renderer->GetClass() ? Renderer->GetClass()->GetName() : TEXT("");
        Obj->SetBoolField(TEXT("is_null"), false);
        Obj->SetStringField(TEXT("renderer_type"), GetReadableRendererType(ClassName));
        Obj->SetStringField(TEXT("class"), ClassName);
        Obj->SetStringField(TEXT("name"), Renderer->GetName());
        Obj->SetBoolField(TEXT("enabled"), Renderer->GetIsEnabled());
        Obj->SetStringField(TEXT("material"), GetObjectPropertyPathByName(Renderer, TEXT("Material")));
        Obj->SetStringField(TEXT("sort_mode"), GetRawPropertyValueByName(Renderer, TEXT("SortMode")));
        Obj->SetStringField(TEXT("facing_mode"), GetRawPropertyValueByName(Renderer, TEXT("FacingMode")));
        Obj->SetStringField(TEXT("alignment"), GetRawPropertyValueByName(Renderer, TEXT("Alignment")));
        Obj->SetStringField(TEXT("source_mode"), GetRawPropertyValueByName(Renderer, TEXT("SourceMode")));
        Obj->SetStringField(TEXT("sub_image_size"), GetRawPropertyValueByName(Renderer, TEXT("SubImageSize")));
        return Obj;
    }

    static void AddUniqueString(TArray<FString>& Values, const FString& Value)
    {
        if (!Value.IsEmpty() && !Values.Contains(Value))
        {
            Values.Add(Value);
        }
    }

    static void AppendModuleNamesFromScript(const UNiagaraScript* Script, TArray<FString>& InOutModuleNames)
    {
        if (!Script)
        {
            return;
        }

        TArray<FNiagaraVariable> Parameters;
        Script->RapidIterationParameters.GetParameters(Parameters);
        for (const FNiagaraVariable& Variable : Parameters)
        {
            FString Scope;
            FString Module;
            FString Input;
            ParseReadableParameterName(Variable.GetName().ToString(), Scope, Module, Input);
            AddUniqueString(InOutModuleNames, Module);
        }
    }

    static void AppendFlatParametersFromScript(const UNiagaraScript* Script, const FString& ScriptLabel, TArray<TSharedPtr<FJsonValue>>& InOutParameters)
    {
        TArray<TSharedPtr<FJsonValue>> ScriptParams = ExportReadableFlatParametersFromScript(Script, ScriptLabel);
        for (const TSharedPtr<FJsonValue>& Param : ScriptParams)
        {
            InOutParameters.Add(Param);
        }
    }

    static void SetStringArrayField(TSharedPtr<FJsonObject> Obj, const FString& FieldName, const TArray<FString>& Values)
    {
        TArray<TSharedPtr<FJsonValue>> JsonValues;
        for (const FString& Value : Values)
        {
            JsonValues.Add(MakeShared<FJsonValueString>(Value));
        }
        Obj->SetArrayField(FieldName, JsonValues);
    }

    static TSharedPtr<FJsonObject> BuildReadableVisualInference(const TArray<FString>& ModuleNames, const TArray<TSharedPtr<FJsonValue>>& Renderers)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("note"), TEXT("Best-effort AI hint inferred from module names, renderer classes and decoded rapid-iteration parameters. Use readable_stack stages and typed summaries as the source of truth."));

        TArray<FString> SpawnShapeHints;
        TArray<FString> MotionHints;
        TArray<FString> AppearanceHints;

        for (const FString& Module : ModuleNames)
        {
            if (Module.Contains(TEXT("SphereLocation"))) { AddUniqueString(SpawnShapeHints, TEXT("sphere_or_spherical_shell_spawn_volume")); }
            if (Module.Contains(TEXT("BoxLocation"))) { AddUniqueString(SpawnShapeHints, TEXT("box_spawn_volume")); }
            if (Module.Contains(TEXT("CylinderLocation"))) { AddUniqueString(SpawnShapeHints, TEXT("cylinder_spawn_volume")); }
            if (Module.Contains(TEXT("Torus"))) { AddUniqueString(SpawnShapeHints, TEXT("torus_or_ring_spawn_volume")); }
            if (Module.Contains(TEXT("Cone"))) { AddUniqueString(MotionHints, TEXT("cone_directional_velocity_or_mask")); }
            if (Module.Contains(TEXT("Velocity"))) { AddUniqueString(MotionHints, TEXT("initial_or_updated_velocity")); }
            if (Module.Contains(TEXT("Gravity"))) { AddUniqueString(MotionHints, TEXT("gravity_force")); }
            if (Module.Contains(TEXT("Drag"))) { AddUniqueString(MotionHints, TEXT("drag_damping")); }
            if (Module.Contains(TEXT("Color"))) { AddUniqueString(AppearanceHints, TEXT("color_or_alpha_change")); }
            if (Module.Contains(TEXT("Scale"))) { AddUniqueString(AppearanceHints, TEXT("scale_change")); }
        }

        for (const TSharedPtr<FJsonValue>& RendererValue : Renderers)
        {
            TSharedPtr<FJsonObject> RendererObj = RendererValue.IsValid() ? RendererValue->AsObject() : nullptr;
            if (!RendererObj.IsValid())
            {
                continue;
            }
            FString RendererType;
            if (RendererObj->TryGetStringField(TEXT("renderer_type"), RendererType))
            {
                AddUniqueString(AppearanceHints, RendererType + TEXT("_renderer"));
            }
        }

        SetStringArrayField(Obj, TEXT("spawn_shape_hints"), SpawnShapeHints);
        SetStringArrayField(Obj, TEXT("motion_hints"), MotionHints);
        SetStringArrayField(Obj, TEXT("appearance_hints"), AppearanceHints);
        return Obj;
    }

    static TSharedPtr<FJsonObject> ExportReadableEmitterStack(const FNiagaraEmitterHandle& Handle, UNiagaraSystem* System, UObject* RootAsset, int32 Index)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("index"), Index);
        Obj->SetStringField(TEXT("handle_name"), Handle.GetName().ToString());
        Obj->SetStringField(TEXT("unique_instance_name"), Handle.GetUniqueInstanceName());
        Obj->SetBoolField(TEXT("enabled"), Handle.GetIsEnabled());

        UNiagaraEmitter* Emitter = Handle.GetInstance();
        if (!Emitter)
        {
            Obj->SetBoolField(TEXT("has_instance"), false);
            Obj->SetArrayField(TEXT("stages"), TArray<TSharedPtr<FJsonValue>>());
            return Obj;
        }

        Obj->SetBoolField(TEXT("has_instance"), true);
        Obj->SetStringField(TEXT("emitter_name"), Emitter->GetName());
        Obj->SetStringField(TEXT("unique_emitter_name"), Emitter->GetUniqueEmitterName());
        Obj->SetStringField(TEXT("sim_target"), EnumValueToString(StaticEnum<ENiagaraSimTarget>(), static_cast<int64>(Emitter->SimTarget)));
        Obj->SetBoolField(TEXT("local_space"), Emitter->bLocalSpace);

        TArray<TSharedPtr<FJsonValue>> Stages;
#if WITH_EDITORONLY_DATA
        AddReadableScriptStageIfValid(Stages, Emitter->EmitterSpawnScriptProps.Script, TEXT("Emitter Spawn"), TEXT("EmitterSpawnScript"));
        AddReadableScriptStageIfValid(Stages, Emitter->EmitterUpdateScriptProps.Script, TEXT("Emitter Update"), TEXT("EmitterUpdateScript"));
#endif
        AddReadableScriptStageIfValid(Stages, Emitter->SpawnScriptProps.Script, TEXT("Particle Spawn"), TEXT("ParticleSpawnScript"));
        AddReadableScriptStageIfValid(Stages, Emitter->UpdateScriptProps.Script, TEXT("Particle Update"), TEXT("ParticleUpdateScript"));
        Obj->SetArrayField(TEXT("stages"), Stages);
        Obj->SetNumberField(TEXT("stage_count"), Stages.Num());

        TArray<TSharedPtr<FJsonValue>> Renderers;
        const TArray<UNiagaraRendererProperties*>& RendererProperties = Emitter->GetRenderers();
        for (int32 RendererIndex = 0; RendererIndex < RendererProperties.Num(); ++RendererIndex)
        {
            Renderers.Add(MakeShared<FJsonValueObject>(ExportReadableRenderer(RendererProperties[RendererIndex], RendererIndex)));
        }
        Obj->SetArrayField(TEXT("renderers"), Renderers);
        Obj->SetNumberField(TEXT("renderer_count"), Renderers.Num());

        TArray<FString> ModuleNames;
#if WITH_EDITORONLY_DATA
        AppendModuleNamesFromScript(Emitter->EmitterSpawnScriptProps.Script, ModuleNames);
        AppendModuleNamesFromScript(Emitter->EmitterUpdateScriptProps.Script, ModuleNames);
#endif
        AppendModuleNamesFromScript(Emitter->SpawnScriptProps.Script, ModuleNames);
        AppendModuleNamesFromScript(Emitter->UpdateScriptProps.Script, ModuleNames);
        ModuleNames.Sort();
        SetStringArrayField(Obj, TEXT("module_names"), ModuleNames);

        TArray<TSharedPtr<FJsonValue>> FlatParameters;
#if WITH_EDITORONLY_DATA
        AppendFlatParametersFromScript(Emitter->EmitterSpawnScriptProps.Script, TEXT("EmitterSpawnScript"), FlatParameters);
        AppendFlatParametersFromScript(Emitter->EmitterUpdateScriptProps.Script, TEXT("EmitterUpdateScript"), FlatParameters);
#endif
        AppendFlatParametersFromScript(Emitter->SpawnScriptProps.Script, TEXT("ParticleSpawnScript"), FlatParameters);
        AppendFlatParametersFromScript(Emitter->UpdateScriptProps.Script, TEXT("ParticleUpdateScript"), FlatParameters);
        Obj->SetArrayField(TEXT("key_parameters_flat"), FlatParameters);
        Obj->SetObjectField(TEXT("inferred_visual"), BuildReadableVisualInference(ModuleNames, Renderers));

        return Obj;
    }

    static TSharedPtr<FJsonObject> ExportNiagaraReadableStack(UNiagaraSystem* System, UObject* RootAsset)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("format"), TEXT("N2C_NIAGARA_READABLE_STACK_V1"));
        Obj->SetStringField(TEXT("purpose"), TEXT("AI-first compact view: stages -> modules -> inputs/values, plus renderers and visual hints. This is easier to read than raw reflection and should be used before local_subobjects/full_reflection."));
        if (!System)
        {
            Obj->SetBoolField(TEXT("available"), false);
            return Obj;
        }

        Obj->SetBoolField(TEXT("available"), true);
        Obj->SetStringField(TEXT("system_name"), System->GetName());
        Obj->SetStringField(TEXT("system_path"), System->GetPathName());
        Obj->SetNumberField(TEXT("warmup_time"), System->GetWarmupTime());
        Obj->SetNumberField(TEXT("warmup_tick_count"), System->GetWarmupTickCount());
        Obj->SetNumberField(TEXT("emitter_count"), System->GetEmitterHandles().Num());

        TArray<TSharedPtr<FJsonValue>> SystemStages;
        AddReadableScriptStageIfValid(SystemStages, System->GetSystemSpawnScript(), TEXT("System Spawn"), TEXT("SystemSpawnScript"));
        AddReadableScriptStageIfValid(SystemStages, System->GetSystemUpdateScript(), TEXT("System Update"), TEXT("SystemUpdateScript"));
        Obj->SetArrayField(TEXT("system_stages"), SystemStages);

        TArray<TSharedPtr<FJsonValue>> Emitters;
        const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();
        for (int32 Index = 0; Index < Handles.Num(); ++Index)
        {
            Emitters.Add(MakeShared<FJsonValueObject>(ExportReadableEmitterStack(Handles[Index], System, RootAsset, Index)));
        }
        Obj->SetArrayField(TEXT("emitters"), Emitters);
        return Obj;
    }

    static TSharedPtr<FJsonObject> ExportNiagaraEmitterSummary(const FNiagaraEmitterHandle& Handle, UNiagaraSystem* System, UObject* RootAsset, int32 Index)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("index"), Index);
        Obj->SetStringField(TEXT("handle_name"), Handle.GetName().ToString());
        Obj->SetStringField(TEXT("handle_id"), GuidToExportString(Handle.GetId()));
        Obj->SetStringField(TEXT("handle_id_name"), Handle.GetIdName().ToString());
        Obj->SetBoolField(TEXT("handle_enabled"), Handle.GetIsEnabled());
        Obj->SetBoolField(TEXT("handle_valid"), Handle.IsValid());
        Obj->SetStringField(TEXT("unique_instance_name"), Handle.GetUniqueInstanceName());

#if WITH_EDITORONLY_DATA
        Obj->SetBoolField(TEXT("handle_isolated"), Handle.IsIsolated());
#endif

        UNiagaraEmitter* Emitter = Handle.GetInstance();
        if (!Emitter)
        {
            Obj->SetBoolField(TEXT("has_instance"), false);
            return Obj;
        }

        Obj->SetBoolField(TEXT("has_instance"), true);
        Obj->SetStringField(TEXT("emitter_name"), Emitter->GetName());
        Obj->SetStringField(TEXT("emitter_path"), Emitter->GetPathName());
        Obj->SetStringField(TEXT("emitter_class"), Emitter->GetClass() ? Emitter->GetClass()->GetName() : TEXT(""));
        Obj->SetStringField(TEXT("unique_emitter_name"), Emitter->GetUniqueEmitterName());
        Obj->SetBoolField(TEXT("is_valid"), Emitter->IsValid());
        Obj->SetBoolField(TEXT("is_ready_to_run"), Emitter->IsReadyToRun());
        Obj->SetBoolField(TEXT("local_space"), Emitter->bLocalSpace);
        Obj->SetStringField(TEXT("sim_target"), EnumValueToString(StaticEnum<ENiagaraSimTarget>(), static_cast<int64>(Emitter->SimTarget)));
        Obj->SetBoolField(TEXT("fixed_bounds_enabled"), Emitter->bFixedBounds);
        Obj->SetStringField(TEXT("fixed_bounds"), Emitter->FixedBounds.ToString());
        Obj->SetBoolField(TEXT("determinism"), Emitter->bDeterminism);
        Obj->SetNumberField(TEXT("random_seed"), Emitter->RandomSeed);
        Obj->SetStringField(TEXT("allocation_mode"), EnumValueToString(StaticEnum<EParticleAllocationMode>(), static_cast<int64>(Emitter->AllocationMode)));
        Obj->SetNumberField(TEXT("pre_allocation_count"), Emitter->PreAllocationCount);

        TArray<TSharedPtr<FJsonValue>> Scripts;
        AddScriptSummaryIfValid(Scripts, Emitter->SpawnScriptProps.Script, RootAsset, TEXT("ParticleSpawnScript"));
        AddScriptSummaryIfValid(Scripts, Emitter->UpdateScriptProps.Script, RootAsset, TEXT("ParticleUpdateScript"));
#if WITH_EDITORONLY_DATA
        AddScriptSummaryIfValid(Scripts, Emitter->EmitterSpawnScriptProps.Script, RootAsset, TEXT("EmitterSpawnScript"));
        AddScriptSummaryIfValid(Scripts, Emitter->EmitterUpdateScriptProps.Script, RootAsset, TEXT("EmitterUpdateScript"));
#endif
        AddScriptSummaryIfValid(Scripts, Emitter->GetGPUComputeScript(), RootAsset, TEXT("GPUComputeScript"));

        int32 EventIndex = 0;
        for (const FNiagaraEventScriptProperties& EventProps : Emitter->GetEventHandlers())
        {
            AddScriptSummaryIfValid(Scripts, EventProps.Script, RootAsset, FString::Printf(TEXT("EventHandler_%d"), EventIndex));
            ++EventIndex;
        }
        Obj->SetArrayField(TEXT("scripts"), Scripts);
        Obj->SetNumberField(TEXT("script_count"), Scripts.Num());

        TArray<TSharedPtr<FJsonValue>> Renderers;
        const TArray<UNiagaraRendererProperties*>& RendererProperties = Emitter->GetRenderers();
        for (int32 RendererIndex = 0; RendererIndex < RendererProperties.Num(); ++RendererIndex)
        {
            Renderers.Add(MakeShared<FJsonValueObject>(ExportNiagaraRendererSummary(RendererProperties[RendererIndex], RootAsset, RendererIndex)));
        }
        Obj->SetArrayField(TEXT("renderers"), Renderers);
        Obj->SetNumberField(TEXT("renderer_count"), Renderers.Num());

        TSet<const UObject*> EmitterVisited;
        Obj->SetObjectField(TEXT("object_ref"), ExportObjectReference(Emitter, RootAsset, 0, EmitterVisited));
        return Obj;
    }

    static bool ShouldIncludeCompactLocalSubobjectExport(const UObject* Object)
    {
        if (!Object || !Object->GetClass())
        {
            return false;
        }

        const FString ClassName = Object->GetClass()->GetName();
        return ClassName.Contains(TEXT("NiagaraDataInterfaceCurve"))
            || ClassName.Contains(TEXT("NiagaraDataInterfaceVector2DCurve"));
    }

    static bool IsInterestingCompactNiagaraSubobject(const UObject* Object)
    {
        if (!Object || !Object->GetClass())
        {
            return false;
        }

        const FString ClassName = Object->GetClass()->GetName();
        const FString PathName = Object->GetPathName();
        if (ClassName.Contains(TEXT("NiagaraDataInterfaceCurve")) || ClassName.Contains(TEXT("NiagaraDataInterfaceVector2DCurve")))
        {
            return PathName.Contains(TEXT("FloatFromCurve"), ESearchCase::IgnoreCase)
                || PathName.Contains(TEXT("ScaleSpriteSize"), ESearchCase::IgnoreCase)
                || PathName.Contains(TEXT("Curve"), ESearchCase::IgnoreCase);
        }
        return false;
    }

    static TSharedPtr<FJsonObject> ExportCompactLocalSubobjectItem(UObject* LocalObject)
    {
        TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("name"), LocalObject->GetName());
        Item->SetStringField(TEXT("path"), LocalObject->GetPathName());
        Item->SetStringField(TEXT("class"), LocalObject->GetClass() ? LocalObject->GetClass()->GetName() : TEXT(""));
        Item->SetStringField(TEXT("outer"), LocalObject->GetOuter() ? LocalObject->GetOuter()->GetPathName() : TEXT(""));
        if (ShouldIncludeCompactLocalSubobjectExport(LocalObject))
        {
            Item->SetObjectField(TEXT("compact_debug"), ExportCompactDataInterfaceDebug(LocalObject));
        }
        return Item;
    }

    static TSharedPtr<FJsonObject> ExportLocalNiagaraSubobjects(UObject* RootAsset)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("export_policy"), TEXT("v32 compact diagnostics: class counts + selected curve/data-interface subobjects only; full graph/node dumps are intentionally omitted because they duplicate readable_stack/import_actions and can exceed 10 MB"));
        TArray<UObject*> LocalObjects;
        if (!RootAsset)
        {
            Obj->SetNumberField(TEXT("total_local_object_count"), 0);
            Obj->SetArrayField(TEXT("interesting_objects"), TArray<TSharedPtr<FJsonValue>>());
            return Obj;
        }

        for (TObjectIterator<UObject> It; It; ++It)
        {
            UObject* Candidate = *It;
            if (!Candidate || Candidate == RootAsset)
            {
                continue;
            }
            if (!IsWorthDeepExportingLocalObject(Candidate))
            {
                continue;
            }
            if (!IsLocalObjectPathForRoot(Candidate, RootAsset) && !Candidate->IsIn(RootAsset))
            {
                continue;
            }
            LocalObjects.Add(Candidate);
        }

        LocalObjects.Sort([](const UObject& A, const UObject& B)
        {
            return A.GetPathName() < B.GetPathName();
        });

        TMap<FString, int32> ClassCounts;
        TArray<TSharedPtr<FJsonValue>> InterestingItems;
        for (UObject* LocalObject : LocalObjects)
        {
            const FString ClassName = LocalObject && LocalObject->GetClass() ? LocalObject->GetClass()->GetName() : TEXT("<null>");
            int32& Count = ClassCounts.FindOrAdd(ClassName);
            ++Count;

            if (IsInterestingCompactNiagaraSubobject(LocalObject))
            {
                InterestingItems.Add(MakeShared<FJsonValueObject>(ExportCompactLocalSubobjectItem(LocalObject)));
            }
        }

        TArray<TSharedPtr<FJsonValue>> ClassCountItems;
        ClassCounts.KeySort([](const FString& A, const FString& B) { return A < B; });
        for (const TPair<FString, int32>& Pair : ClassCounts)
        {
            TSharedPtr<FJsonObject> CountObj = MakeShared<FJsonObject>();
            CountObj->SetStringField(TEXT("class"), Pair.Key);
            CountObj->SetNumberField(TEXT("count"), Pair.Value);
            ClassCountItems.Add(MakeShared<FJsonValueObject>(CountObj));
        }

        Obj->SetNumberField(TEXT("total_local_object_count"), LocalObjects.Num());
        Obj->SetArrayField(TEXT("class_counts"), ClassCountItems);
        Obj->SetNumberField(TEXT("interesting_object_count"), InterestingItems.Num());
        Obj->SetArrayField(TEXT("interesting_objects"), InterestingItems);
        Obj->SetBoolField(TEXT("full_objects_omitted"), true);
        return Obj;
    }

    static TSharedPtr<FJsonObject> ExportNiagaraSystemSummary(UObject* Asset)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        UNiagaraSystem* System = Cast<UNiagaraSystem>(Asset);
        if (!System)
        {
            Obj->SetBoolField(TEXT("available"), false);
            Obj->SetStringField(TEXT("reason"), TEXT("Asset is not UNiagaraSystem; typed summary is only available for Niagara System assets."));
            return Obj;
        }

        Obj->SetBoolField(TEXT("available"), true);
        Obj->SetStringField(TEXT("system_name"), System->GetName());
        Obj->SetStringField(TEXT("system_path"), System->GetPathName());
        Obj->SetBoolField(TEXT("is_valid"), System->IsValid());
        Obj->SetBoolField(TEXT("is_ready_to_run"), System->IsReadyToRun());
        Obj->SetNumberField(TEXT("num_emitters"), System->GetEmitterHandles().Num());
        Obj->SetBoolField(TEXT("needs_warmup"), System->NeedsWarmup());
        Obj->SetNumberField(TEXT("warmup_time"), System->GetWarmupTime());
        Obj->SetNumberField(TEXT("warmup_tick_count"), System->GetWarmupTickCount());
        Obj->SetNumberField(TEXT("warmup_tick_delta"), System->GetWarmupTickDelta());

        if (System->GetMaxDeltaTime().IsSet())
        {
            Obj->SetNumberField(TEXT("max_delta_time"), System->GetMaxDeltaTime().GetValue());
        }

        Obj->SetObjectField(TEXT("exposed_parameters"), ExportNiagaraUserParameterStoreTyped(System->GetExposedParameters(), Asset, TEXT("System.ExposedParameters")));

        // v32: Do not export the old deep system_scripts / emitters arrays in the
        // default Niagara JSON. They duplicated readable_stack/import_actions and
        // were the main remaining bloat after v31 (~180 KB on the 3-emitter
        // explosion asset) without adding information that AI should read first.
        // Keep counts and an explicit policy marker; the authoritative default
        // view is niagara_summary.readable_stack, with curve-only diagnostics in
        // local_subobjects and skipped_input_overrides.
        Obj->SetNumberField(TEXT("system_script_count"), 2);
        Obj->SetBoolField(TEXT("system_scripts_omitted_in_compact_export"), true);
        Obj->SetStringField(TEXT("system_scripts_note"), TEXT("Deep system script summaries are omitted in compact export; use readable_stack.system_stages for modules/inputs/values."));

        const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();
        Obj->SetNumberField(TEXT("emitter_summary_count"), Handles.Num());
        Obj->SetBoolField(TEXT("emitters_omitted_in_compact_export"), true);
        Obj->SetStringField(TEXT("emitters_note"), TEXT("Deep emitter summaries are omitted in compact export; use readable_stack.emitters for emitter metadata, stages, modules, inputs, renderers and visual hints."));

        Obj->SetObjectField(TEXT("readable_stack"), ExportNiagaraReadableStack(System, Asset));

        // v30: keep local graph/node subobjects in the default Niagara export.
        // This makes hand-authored modules such as FloatFromCurve / curve inputs auditable even
        // when they are not yet reimportable through import_actions.input_overrides.
        Obj->SetObjectField(TEXT("local_subobjects"), ExportLocalNiagaraSubobjects(Asset));
        return Obj;
    }

    static TSharedPtr<FJsonObject> ExportNamedNiagaraProperty(UObject* Asset, const TCHAR* PropertyName)
    {
        TSharedPtr<FJsonObject> SectionObj = MakeShared<FJsonObject>();
        if (!Asset || !PropertyName)
        {
            SectionObj->SetBoolField(TEXT("found"), false);
            return SectionObj;
        }

        FProperty* Property = FindFProperty<FProperty>(Asset->GetClass(), PropertyName);
        if (!Property)
        {
            SectionObj->SetBoolField(TEXT("found"), false);
            SectionObj->SetStringField(TEXT("property_name"), PropertyName);
            return SectionObj;
        }

        TSet<const UObject*> Visited;
        SectionObj->SetBoolField(TEXT("found"), true);
        SectionObj->SetObjectField(TEXT("property"), ExportPropertyDeep(Property, Asset, Asset, Asset, 0, Visited));
        return SectionObj;
    }

    static void AddNamedNiagaraSection(TSharedPtr<FJsonObject> Sections, UObject* Asset, const TCHAR* SectionName, const TCHAR* PropertyName)
    {
        if (!Sections.IsValid())
        {
            return;
        }

        Sections->SetObjectField(SectionName, ExportNamedNiagaraProperty(Asset, PropertyName));
    }

    static TSharedPtr<FJsonObject> ExportNiagaraImportantSections(UObject* Asset)
    {
        TSharedPtr<FJsonObject> Sections = MakeShared<FJsonObject>();
        AddNamedNiagaraSection(Sections, Asset, TEXT("emitter_handles"), TEXT("EmitterHandles"));
        AddNamedNiagaraSection(Sections, Asset, TEXT("exposed_parameters"), TEXT("ExposedParameters"));
        AddNamedNiagaraSection(Sections, Asset, TEXT("editor_parameters"), TEXT("EditorParameters"));
        AddNamedNiagaraSection(Sections, Asset, TEXT("system_spawn_script"), TEXT("SystemSpawnScript"));
        AddNamedNiagaraSection(Sections, Asset, TEXT("system_update_script"), TEXT("SystemUpdateScript"));
        AddNamedNiagaraSection(Sections, Asset, TEXT("system_compiled_data"), TEXT("SystemCompiledData"));
        AddNamedNiagaraSection(Sections, Asset, TEXT("editor_data"), TEXT("EditorData"));
        AddNamedNiagaraSection(Sections, Asset, TEXT("fixed_bounds"), TEXT("FixedBounds"));
        AddNamedNiagaraSection(Sections, Asset, TEXT("scalability_overrides"), TEXT("ScalabilityOverrides"));
        AddNamedNiagaraSection(Sections, Asset, TEXT("system_scalability_overrides"), TEXT("SystemScalabilityOverrides"));
        AddNamedNiagaraSection(Sections, Asset, TEXT("parameter_collection_overrides"), TEXT("ParameterCollectionOverrides"));
        AddNamedNiagaraSection(Sections, Asset, TEXT("scratch_pad_scripts"), TEXT("ScratchPadScripts"));
        AddNamedNiagaraSection(Sections, Asset, TEXT("parameter_definitions_subscriptions"), TEXT("ParameterDefinitionsSubscriptions"));
        AddNamedNiagaraSection(Sections, Asset, TEXT("warmup_time"), TEXT("WarmupTime"));
        AddNamedNiagaraSection(Sections, Asset, TEXT("warmup_tick_count"), TEXT("WarmupTickCount"));
        AddNamedNiagaraSection(Sections, Asset, TEXT("warmup_tick_delta"), TEXT("WarmupTickDelta"));
        AddNamedNiagaraSection(Sections, Asset, TEXT("baker_settings"), TEXT("BakerSettings"));
        AddNamedNiagaraSection(Sections, Asset, TEXT("baker_generated_settings"), TEXT("BakerGeneratedSettings"));
        AddNamedNiagaraSection(Sections, Asset, TEXT("user_di_names_read_in_system_scripts"), TEXT("UserDINamesReadInSystemScripts"));
        AddNamedNiagaraSection(Sections, Asset, TEXT("message_key_to_message_map"), TEXT("MessageKeyToMessageMap"));
        AddNamedNiagaraSection(Sections, Asset, TEXT("max_pool_size"), TEXT("MaxPoolSize"));
        AddNamedNiagaraSection(Sections, Asset, TEXT("pool_prime_size"), TEXT("PoolPrimeSize"));
        return Sections;
    }


    static TArray<TSharedPtr<FJsonValue>> ExportRootPropertySummary(UObject* Asset)
    {
        TArray<TSharedPtr<FJsonValue>> Properties;
        if (!Asset || !Asset->GetClass())
        {
            return Properties;
        }

        for (TFieldIterator<FProperty> It(Asset->GetClass()); It; ++It)
        {
            FProperty* Property = *It;
            if (!Property)
            {
                continue;
            }

            TSharedPtr<FJsonObject> PropObj = MakeShared<FJsonObject>();
            AddPropertyMetadata(PropObj, Property);
            const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Asset);
            PropObj->SetStringField(TEXT("raw"), SafeExportPropertyValue(Property, ValuePtr, Asset));

            if (FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
            {
                FScriptArrayHelper Helper(ArrayProperty, ValuePtr);
                PropObj->SetNumberField(TEXT("array_count"), Helper.Num());
                PropObj->SetStringField(TEXT("inner_type"), ArrayProperty->Inner ? ArrayProperty->Inner->GetCPPType() : TEXT(""));
            }
            else if (FSetProperty* SetProperty = CastField<FSetProperty>(Property))
            {
                FScriptSetHelper Helper(SetProperty, ValuePtr);
                PropObj->SetNumberField(TEXT("set_count"), Helper.Num());
                PropObj->SetStringField(TEXT("element_type"), SetProperty->ElementProp ? SetProperty->ElementProp->GetCPPType() : TEXT(""));
            }
            else if (FMapProperty* MapProperty = CastField<FMapProperty>(Property))
            {
                FScriptMapHelper Helper(MapProperty, ValuePtr);
                PropObj->SetNumberField(TEXT("map_count"), Helper.Num());
                PropObj->SetStringField(TEXT("key_type"), MapProperty->KeyProp ? MapProperty->KeyProp->GetCPPType() : TEXT(""));
                PropObj->SetStringField(TEXT("value_type"), MapProperty->ValueProp ? MapProperty->ValueProp->GetCPPType() : TEXT(""));
            }
            else if (FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
            {
                UObject* ObjectValue = ObjectProperty->GetObjectPropertyValue(ValuePtr);
                PropObj->SetStringField(TEXT("object_path"), ObjectValue ? ObjectValue->GetPathName() : TEXT(""));
                PropObj->SetStringField(TEXT("object_class"), ObjectValue && ObjectValue->GetClass() ? ObjectValue->GetClass()->GetName() : TEXT(""));
            }

            Properties.Add(MakeShared<FJsonValueObject>(PropObj));
        }

        return Properties;
    }

    static TSharedPtr<FJsonObject> ExportNiagaraObject(UObject* Asset)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        if (!Asset)
        {
            return Obj;
        }

        TSet<const UObject*> Visited;
        Obj->SetStringField(TEXT("asset_name"), Asset->GetName());
        Obj->SetStringField(TEXT("asset_path"), Asset->GetPathName());
        Obj->SetStringField(TEXT("class"), Asset->GetClass() ? Asset->GetClass()->GetName() : TEXT(""));
        Obj->SetStringField(TEXT("export_mode"), TEXT("compact_reimportable_niagara_v10"));
        Obj->SetStringField(TEXT("export_note"), TEXT("Compact reimportable Niagara export. Keeps import_actions plus readable/typed summaries, and omits huge debug reflection/local_subobject payloads by default."));
        Obj->SetStringField(TEXT("export_size_policy"), TEXT("Default compact mode: no full_reflection, no deep duplicate system_scripts/emitters arrays, no full local_subobjects, no important_sections deep property dumps. Use debug export build only when investigating Niagara internals."));

        TArray<TSharedPtr<FJsonValue>> FeatureList;
        FeatureList.Add(MakeShared<FJsonValueString>(TEXT("compact local UObject/subobject index for objects stored inside the exported Niagara asset package")));
        FeatureList.Add(MakeShared<FJsonValueString>(TEXT("typed values for bool, numeric, string/name/text, enum, byte enum, struct, array, set, map and object references")));
        FeatureList.Add(MakeShared<FJsonValueString>(TEXT("property metadata: cpp type, flags, array dimension, category/display/tooltip when present")));
        FeatureList.Add(MakeShared<FJsonValueString>(TEXT("AI-friendly sections for emitters, scripts, renderers, user/exposed parameters, warmup, bounds and pooling without recursive duplication")));
        FeatureList.Add(MakeShared<FJsonValueString>(TEXT("typed Niagara summary: system scripts, emitter handles, emitter scripts, renderer properties and decoded FNiagaraParameterStore values")));
        FeatureList.Add(MakeShared<FJsonValueString>(TEXT("local_subobjects v32 compact diagnostics only; no full graph/node dumps in default export")));
        FeatureList.Add(MakeShared<FJsonValueString>(TEXT("readable_stack: AI-friendly stages -> modules -> inputs/values, renderers and inferred visual hints")));
        FeatureList.Add(MakeShared<FJsonValueString>(TEXT("import_actions: generated user_parameters, extra emitter duplicate actions and dynamic_input_tree input_overrides for reimportable Niagara formulas")));
        Obj->SetArrayField(TEXT("export_features"), FeatureList);

        TArray<TSharedPtr<FJsonValue>> RecommendedReadOrder;
        RecommendedReadOrder.Add(MakeShared<FJsonValueString>(TEXT("niagara_asset.import_actions.emitters")));
        RecommendedReadOrder.Add(MakeShared<FJsonValueString>(TEXT("niagara_asset.import_actions.input_overrides")));
        RecommendedReadOrder.Add(MakeShared<FJsonValueString>(TEXT("niagara_asset.niagara_summary.readable_stack")));
        RecommendedReadOrder.Add(MakeShared<FJsonValueString>(TEXT("niagara_asset.niagara_summary.emitters[].scripts[].rapid_iteration_parameters")));
        RecommendedReadOrder.Add(MakeShared<FJsonValueString>(TEXT("niagara_asset.niagara_summary.emitters[].renderers")));
        RecommendedReadOrder.Add(MakeShared<FJsonValueString>(TEXT("niagara_asset.niagara_summary.exposed_parameters")));
        RecommendedReadOrder.Add(MakeShared<FJsonValueString>(TEXT("niagara_asset.niagara_summary.local_subobjects (placeholder only in compact mode)")));
        Obj->SetArrayField(TEXT("recommended_read_order"), RecommendedReadOrder);

        Obj->SetStringField(TEXT("properties_note"), TEXT("Compatibility/root summary only. Start from niagara_summary.readable_stack for human/AI reading. Use typed summaries for exact parameter values and local_subobjects/full_reflection only when deeper graph/object details are needed."));
        Obj->SetArrayField(TEXT("properties"), ExportRootPropertySummary(Asset));
        Obj->SetObjectField(TEXT("import_actions"), ExportNiagaraImportActions(Cast<UNiagaraSystem>(Asset)));
        Obj->SetObjectField(TEXT("niagara_summary"), ExportNiagaraSystemSummary(Asset));

        TSharedPtr<FJsonObject> ImportantSectionsPlaceholder = MakeShared<FJsonObject>();
        ImportantSectionsPlaceholder->SetBoolField(TEXT("omitted_by_default"), true);
        ImportantSectionsPlaceholder->SetStringField(TEXT("reason"), TEXT("Compact export mode: important_sections deep property dumps are skipped to keep Niagara JSON small."));
        Obj->SetObjectField(TEXT("important_sections"), ImportantSectionsPlaceholder);

        TSharedPtr<FJsonObject> FullReflectionPlaceholder = MakeShared<FJsonObject>();
        FullReflectionPlaceholder->SetBoolField(TEXT("omitted_by_default"), true);
        FullReflectionPlaceholder->SetStringField(TEXT("reason"), TEXT("Compact export mode: full_reflection is skipped by default. Use a debug/full-reflection export build only when needed."));
        Obj->SetObjectField(TEXT("full_reflection"), FullReflectionPlaceholder);

        return Obj;
    }

    static void AddGraphUnique(TArray<UEdGraph*>& OutGraphs, UEdGraph* Graph)
    {
        if (Graph && !OutGraphs.Contains(Graph))
        {
            OutGraphs.Add(Graph);
        }
    }

    static bool IsKnownBlueprintGraph(const UBlueprint* Blueprint, UEdGraph* Graph)
    {
        if (!Blueprint || !Graph)
        {
            return false;
        }
        if (Blueprint->FunctionGraphs.Contains(Graph) ||
            Blueprint->UbergraphPages.Contains(Graph) ||
            Blueprint->MacroGraphs.Contains(Graph) ||
            Blueprint->DelegateSignatureGraphs.Contains(Graph))
        {
            return true;
        }
        for (const FBPInterfaceDescription& InterfaceDesc : Blueprint->ImplementedInterfaces)
        {
            if (InterfaceDesc.Graphs.Contains(Graph))
            {
                return true;
            }
        }
        return false;
    }

    static bool IsGeneratedUbergraphNestedGraph(UEdGraph* Graph)
    {
        if (!Graph)
        {
            return false;
        }

        const FString Path = Graph->GetPathName();
        // AnimBlueprints can expose duplicated nested state/transition graphs under
        // ExecuteUbergraph_* after compile/save. The real authoring graphs are already
        // available under AnimGraph; exporting both doubles collapsed_graph entries.
        return Path.Contains(TEXT(":ExecuteUbergraph_"));
    }

    static bool ShouldIncludeSupplementalGraph(UBlueprint* Blueprint, UEdGraph* Graph)
    {
        if (!Blueprint || !Graph || IsKnownBlueprintGraph(Blueprint, Graph))
        {
            return false;
        }

        // Keep generated ExecuteUbergraph_* subgraphs out even if GetGraphKind() would
        // classify a nested animation state/transition graph as collapsed_graph.
        if (IsGeneratedUbergraphNestedGraph(Graph))
        {
            return false;
        }

        const FString Kind = GetGraphKind(Graph, Blueprint);
        if (Kind == TEXT("timeline_graph") || Kind == TEXT("collapsed_graph") || Kind == TEXT("interface_graph"))
        {
            return true;
        }

        // UE4.27 may materialize compiler/helper graphs after save + reopen under the
        // Blueprint package (EdGraph_0/1/2, ExecuteUbergraph_*, custom-event thunk
        // graphs, etc.). They duplicate information already available in FunctionGraphs
        // or EventGraph and should not become top-level AI export entries.
        return false;
    }

    static TArray<UEdGraph*> CollectAllBlueprintGraphs(UBlueprint* Blueprint)
    {
        TArray<UEdGraph*> Graphs;
        if (!Blueprint)
        {
            return Graphs;
        }

        for (UEdGraph* Graph : Blueprint->FunctionGraphs) { AddGraphUnique(Graphs, Graph); }
        for (UEdGraph* Graph : Blueprint->UbergraphPages) { AddGraphUnique(Graphs, Graph); }
        for (UEdGraph* Graph : Blueprint->MacroGraphs) { AddGraphUnique(Graphs, Graph); }
        for (UEdGraph* Graph : Blueprint->DelegateSignatureGraphs) { AddGraphUnique(Graphs, Graph); }
        for (const FBPInterfaceDescription& InterfaceDesc : Blueprint->ImplementedInterfaces)
        {
            for (UEdGraph* Graph : InterfaceDesc.Graphs)
            {
                AddGraphUnique(Graphs, Graph);
            }
        }

        // Extra pass catches real nested graphs only (timeline-owned or collapsed graphs).
        // Do not add every graph from the Blueprint package: after save/reopen UE4.27 also
        // exposes compiler/helper graphs such as EdGraph_1 and ExecuteUbergraph_*; exporting
        // them polluted the function list with duplicates.
        TArray<UObject*> ChildObjects;
        GetObjectsWithOuter(Blueprint, ChildObjects, true);
        for (UObject* ChildObject : ChildObjects)
        {
            UEdGraph* ChildGraph = Cast<UEdGraph>(ChildObject);
            if (ShouldIncludeSupplementalGraph(Blueprint, ChildGraph))
            {
                AddGraphUnique(Graphs, ChildGraph);
            }
        }

        if (Blueprint->GetOutermost())
        {
            for (TObjectIterator<UEdGraph> It; It; ++It)
            {
                UEdGraph* Graph = *It;
                if (Graph && Graph->GetOutermost() == Blueprint->GetOutermost() && ShouldIncludeSupplementalGraph(Blueprint, Graph))
                {
                    AddGraphUnique(Graphs, Graph);
                }
            }
        }

        Graphs.Sort([](const UEdGraph& A, const UEdGraph& B)
        {
            return A.GetPathName() < B.GetPathName();
        });
        return Graphs;
    }

    static TSharedPtr<FJsonObject> ExportBlueprintGraphCoverage(UBlueprint* Blueprint, const TArray<UEdGraph*>& Graphs)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        int32 FunctionCount = 0;
        int32 EventGraphCount = 0;
        int32 MacroCount = 0;
        int32 DelegateCount = 0;
        int32 InterfaceCount = 0;
        int32 TimelineGraphCount = 0;
        int32 CollapsedGraphCount = 0;
        int32 ConstructionScriptCount = 0;
        int32 ExtraCount = 0;

        TArray<TSharedPtr<FJsonValue>> GraphIndex;
        for (UEdGraph* Graph : Graphs)
        {
            if (!Graph)
            {
                continue;
            }
            const FString Kind = GetGraphKind(Graph, Blueprint);
            if (Kind == TEXT("function")) { ++FunctionCount; }
            else if (Kind == TEXT("event_graph")) { ++EventGraphCount; }
            else if (Kind == TEXT("macro")) { ++MacroCount; }
            else if (Kind == TEXT("delegate_signature")) { ++DelegateCount; }
            else if (Kind == TEXT("interface_graph")) { ++InterfaceCount; }
            else if (Kind == TEXT("timeline_graph")) { ++TimelineGraphCount; }
            else if (Kind == TEXT("collapsed_graph")) { ++CollapsedGraphCount; }
            else if (Kind == TEXT("construction_script")) { ++ConstructionScriptCount; }
            else { ++ExtraCount; }

            TSharedPtr<FJsonObject> GraphObj = MakeShared<FJsonObject>();
            GraphObj->SetStringField(TEXT("name"), Graph->GetName());
            GraphObj->SetStringField(TEXT("path"), Graph->GetPathName());
            GraphObj->SetStringField(TEXT("graph_type"), Kind);
            GraphObj->SetStringField(TEXT("graph_class"), Graph->GetClass() ? Graph->GetClass()->GetName() : TEXT(""));
            GraphObj->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());
            GraphIndex.Add(MakeShared<FJsonValueObject>(GraphObj));
        }

        Obj->SetNumberField(TEXT("total_graphs"), Graphs.Num());
        Obj->SetNumberField(TEXT("functions"), FunctionCount);
        Obj->SetNumberField(TEXT("event_graphs"), EventGraphCount);
        Obj->SetNumberField(TEXT("macros"), MacroCount);
        Obj->SetNumberField(TEXT("delegate_signatures"), DelegateCount);
        Obj->SetNumberField(TEXT("interface_graphs"), InterfaceCount);
        Obj->SetNumberField(TEXT("timeline_graphs"), TimelineGraphCount);
        Obj->SetNumberField(TEXT("collapsed_graphs"), CollapsedGraphCount);
        Obj->SetNumberField(TEXT("construction_scripts"), ConstructionScriptCount);
        Obj->SetNumberField(TEXT("extra_graphs"), ExtraCount);
        Obj->SetArrayField(TEXT("graphs"), GraphIndex);
        return Obj;
    }

    static TSharedPtr<FJsonObject> ExportSCSNodeRecursive(const USCS_Node* Node, UObject* RootAsset, int32 Depth, TSet<const UObject*>& Visited)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        if (!Node)
        {
            return Obj;
        }

        Obj->SetStringField(TEXT("variable_name"), Node->GetVariableName().ToString());
        Obj->SetStringField(TEXT("component_class"), Node->ComponentClass ? Node->ComponentClass->GetPathName() : TEXT(""));
        Obj->SetStringField(TEXT("node_guid"), Node->VariableGuid.ToString(EGuidFormats::DigitsWithHyphens));
        if (Node->ComponentTemplate)
        {
            Obj->SetObjectField(TEXT("component_template"), ExportObjectReference(Node->ComponentTemplate, RootAsset, Depth + 1, Visited, false));
            Obj->SetArrayField(TEXT("component_template_defaults"), ExportEditableObjectPropertiesDeep(Node->ComponentTemplate, RootAsset, Depth + 1, Visited));
        }

        TArray<TSharedPtr<FJsonValue>> Children;
        for (USCS_Node* Child : Node->GetChildNodes())
        {
            Children.Add(MakeShared<FJsonValueObject>(ExportSCSNodeRecursive(Child, RootAsset, Depth + 1, Visited)));
        }
        Obj->SetArrayField(TEXT("children"), Children);
        return Obj;
    }

    static TSharedPtr<FJsonObject> ExportComponentHierarchy(UBlueprint* Blueprint)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        TArray<TSharedPtr<FJsonValue>> Components;
        if (!Blueprint || !Blueprint->SimpleConstructionScript)
        {
            Obj->SetBoolField(TEXT("has_simple_construction_script"), false);
            Obj->SetArrayField(TEXT("root_nodes"), Components);
            return Obj;
        }

        Obj->SetBoolField(TEXT("has_simple_construction_script"), true);
        TSet<const UObject*> Visited;
        for (USCS_Node* RootNode : Blueprint->SimpleConstructionScript->GetRootNodes())
        {
            Components.Add(MakeShared<FJsonValueObject>(ExportSCSNodeRecursive(RootNode, Blueprint, 0, Visited)));
        }
        Obj->SetArrayField(TEXT("root_nodes"), Components);
        return Obj;
    }

    static void AddReflectedBoolIfFound(TSharedPtr<FJsonObject> Obj, UObject* Source, const TCHAR* FieldName)
    {
        if (!Obj.IsValid() || !Source)
        {
            return;
        }
        if (const FBoolProperty* BoolProp = FindFProperty<FBoolProperty>(Source->GetClass(), FieldName))
        {
            Obj->SetBoolField(FieldName, BoolProp->GetPropertyValue_InContainer(Source));
        }
    }

    static TArray<TSharedPtr<FJsonValue>> ExportImplementedInterfaces(UBlueprint* Blueprint)
    {
        TArray<TSharedPtr<FJsonValue>> Interfaces;
        if (!Blueprint)
        {
            return Interfaces;
        }

        for (const FBPInterfaceDescription& InterfaceDesc : Blueprint->ImplementedInterfaces)
        {
            TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
            UClass* InterfaceClass = InterfaceDesc.Interface;
            Obj->SetStringField(TEXT("interface_class"), InterfaceClass ? InterfaceClass->GetPathName() : TEXT(""));
            TArray<TSharedPtr<FJsonValue>> Graphs;
            for (UEdGraph* Graph : InterfaceDesc.Graphs)
            {
                TSharedPtr<FJsonObject> GraphObj = MakeShared<FJsonObject>();
                GraphObj->SetStringField(TEXT("name"), Graph ? Graph->GetName() : TEXT(""));
                GraphObj->SetStringField(TEXT("path"), Graph ? Graph->GetPathName() : TEXT(""));
                GraphObj->SetStringField(TEXT("class"), Graph && Graph->GetClass() ? Graph->GetClass()->GetName() : TEXT(""));
                Graphs.Add(MakeShared<FJsonValueObject>(GraphObj));
            }
            Obj->SetArrayField(TEXT("graphs"), Graphs);
            Interfaces.Add(MakeShared<FJsonValueObject>(Obj));
        }
        return Interfaces;
    }

    static TArray<TSharedPtr<FJsonValue>> ExportInheritedVariables(UBlueprint* Blueprint)
    {
        TArray<TSharedPtr<FJsonValue>> Vars;
        if (!Blueprint || !Blueprint->ParentClass)
        {
            return Vars;
        }
        for (TFieldIterator<FProperty> It(Blueprint->ParentClass, EFieldIteratorFlags::IncludeSuper); It; ++It)
        {
            FProperty* Property = *It;
            if (!Property)
            {
                continue;
            }
            TSharedPtr<FJsonObject> VarObj = MakeShared<FJsonObject>();
            AddPropertyMetadata(VarObj, Property);
            VarObj->SetStringField(TEXT("owner_class"), Property->GetOwnerClass() ? Property->GetOwnerClass()->GetPathName() : TEXT(""));
            Vars.Add(MakeShared<FJsonValueObject>(VarObj));
        }
        return Vars;
    }

    static TArray<TSharedPtr<FJsonValue>> ExportInheritedFunctions(UBlueprint* Blueprint)
    {
        TArray<TSharedPtr<FJsonValue>> Functions;
        if (!Blueprint || !Blueprint->ParentClass)
        {
            return Functions;
        }
        for (TFieldIterator<UFunction> It(Blueprint->ParentClass, EFieldIteratorFlags::IncludeSuper); It; ++It)
        {
            UFunction* Function = *It;
            if (!Function)
            {
                continue;
            }
            TSharedPtr<FJsonObject> FnObj = MakeShared<FJsonObject>();
            FnObj->SetStringField(TEXT("name"), Function->GetName());
            FnObj->SetStringField(TEXT("path"), Function->GetPathName());
            FnObj->SetStringField(TEXT("owner_class"), Function->GetOwnerClass() ? Function->GetOwnerClass()->GetPathName() : TEXT(""));
            FnObj->SetNumberField(TEXT("function_flags_raw"), static_cast<double>(Function->FunctionFlags));
            FnObj->SetBoolField(TEXT("blueprint_callable"), Function->HasAnyFunctionFlags(FUNC_BlueprintCallable));
            FnObj->SetBoolField(TEXT("blueprint_pure"), Function->HasAnyFunctionFlags(FUNC_BlueprintPure));
            FnObj->SetBoolField(TEXT("const"), Function->HasAnyFunctionFlags(FUNC_Const));
            Functions.Add(MakeShared<FJsonValueObject>(FnObj));
        }
        return Functions;
    }

    static FString BlueprintTypeToString(EBlueprintType Type)
    {
        switch (Type)
        {
        case BPTYPE_Normal: return TEXT("normal");
        case BPTYPE_Const: return TEXT("const");
        case BPTYPE_MacroLibrary: return TEXT("macro_library");
        case BPTYPE_Interface: return TEXT("interface");
        case BPTYPE_LevelScript: return TEXT("level_script");
        case BPTYPE_FunctionLibrary: return TEXT("function_library");
        default: return FString::Printf(TEXT("unknown_%d"), static_cast<int32>(Type));
        }
    }

    static TSharedPtr<FJsonObject> ExportFullClassMetadata(UBlueprint* Blueprint)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        if (!Blueprint)
        {
            return Obj;
        }

        Obj->SetStringField(TEXT("blueprint_type"), BlueprintTypeToString(Blueprint->BlueprintType));
        Obj->SetNumberField(TEXT("blueprint_type_raw"), static_cast<int32>(Blueprint->BlueprintType));
        Obj->SetStringField(TEXT("parent_class"), Blueprint->ParentClass ? Blueprint->ParentClass->GetPathName() : TEXT(""));
        Obj->SetStringField(TEXT("generated_class"), Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetPathName() : TEXT(""));
        Obj->SetStringField(TEXT("skeleton_generated_class"), Blueprint->SkeletonGeneratedClass ? Blueprint->SkeletonGeneratedClass->GetPathName() : TEXT(""));
        Obj->SetArrayField(TEXT("implemented_interfaces"), ExportImplementedInterfaces(Blueprint));
        Obj->SetObjectField(TEXT("component_hierarchy"), ExportComponentHierarchy(Blueprint));
        Obj->SetArrayField(TEXT("inherited_variables"), ExportInheritedVariables(Blueprint));
        Obj->SetArrayField(TEXT("inherited_functions"), ExportInheritedFunctions(Blueprint));
        AddReflectedBoolIfFound(Obj, Blueprint, TEXT("bRunConstructionScriptOnDrag"));
        AddReflectedBoolIfFound(Obj, Blueprint, TEXT("bRunConstructionScriptInSequencer"));
        AddReflectedBoolIfFound(Obj, Blueprint, TEXT("bNativize"));
        return Obj;
    }

    static TArray<TSharedPtr<FJsonValue>> ExportClassDefaults(UBlueprint* Blueprint)
    {
        TArray<TSharedPtr<FJsonValue>> Defaults;
        if (!Blueprint || !Blueprint->GeneratedClass)
        {
            return Defaults;
        }

        UObject* CDO = Blueprint->GeneratedClass->GetDefaultObject(false);
        if (!CDO)
        {
            return Defaults;
        }

        TSet<const UObject*> Visited;
        for (TFieldIterator<FProperty> It(CDO->GetClass(), EFieldIteratorFlags::IncludeSuper); It; ++It)
        {
            FProperty* Property = *It;
            if (!Property || Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated | CPF_DuplicateTransient | CPF_NonPIEDuplicateTransient))
            {
                continue;
            }
            // Keep the default export useful and bounded: export properties visible/editable to Blueprint/code review.
            if (!Property->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible | CPF_BlueprintReadOnly | CPF_Config))
            {
                continue;
            }
            Defaults.Add(MakeShared<FJsonValueObject>(ExportPropertyDeep(Property, CDO, CDO, Blueprint, 0, Visited)));
        }
        return Defaults;
    }

    static TArray<TSharedPtr<FJsonValue>> ExportTimelineTemplates(UBlueprint* Blueprint)
    {
        TArray<TSharedPtr<FJsonValue>> Timelines;
        if (!Blueprint)
        {
            return Timelines;
        }

        TArray<UObject*> ChildObjects;
        GetObjectsWithOuter(Blueprint, ChildObjects, true);
        TSet<const UObject*> Visited;
        for (UObject* ChildObject : ChildObjects)
        {
            if (!ChildObject || !ChildObject->GetClass())
            {
                continue;
            }
            const FString ClassName = ChildObject->GetClass()->GetName();
            if (!ClassName.Contains(TEXT("Timeline")))
            {
                continue;
            }
            TSharedPtr<FJsonObject> TimelineObj = MakeShared<FJsonObject>();
            TimelineObj->SetStringField(TEXT("name"), ChildObject->GetName());
            TimelineObj->SetStringField(TEXT("path"), ChildObject->GetPathName());
            TimelineObj->SetStringField(TEXT("class"), ClassName);
            TimelineObj->SetArrayField(TEXT("properties"), ExportObjectPropertiesDeep(ChildObject, Blueprint, 0, Visited));
            Timelines.Add(MakeShared<FJsonValueObject>(TimelineObj));
        }
        return Timelines;
    }

    static TSharedPtr<FJsonObject> ExportNodeConstructorCompatibility()
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        TArray<TSharedPtr<FJsonValue>> Supported;
        const TCHAR* SupportedNodes[] = {
            TEXT("FunctionEntry"), TEXT("FunctionResult"), TEXT("Branch/IfThenElse"), TEXT("Sequence"),
            TEXT("Select"), TEXT("K2Node_CommutativeAssociativeBinaryOperator"), TEXT("SwitchEnum"),
            TEXT("EnumEquality"), TEXT("EnumInequality"), TEXT("CallFunction"), TEXT("CallArrayFunction Array_Length/Get/Add/AddUnique/Clear/Contains/Find/Set/Remove/RemoveItem"),
            TEXT("MacroInstance: IsValid/ForEachLoop/ForLoop/ForLoopWithBreak/IncrementInt/WhileLoop/DoOnce/Gate"),
            TEXT("CustomEvent"), TEXT("SpawnActorFromClass"), TEXT("DynamicCast"), TEXT("InterfaceCall"),
            TEXT("GetDataTableRow typed"), TEXT("Delegate bind/call/clear/create against existing dispatcher"), TEXT("ComponentBoundEvent"),
            TEXT("VariableGet"), TEXT("VariableSet"), TEXT("AddComponent K2 node + SCS hierarchy"),
            TEXT("Delay/latent EventGraph calls"), TEXT("Timeline template + tracks"),
            TEXT("BreakStruct"), TEXT("MakeStruct"), TEXT("SetFieldsInStruct")
        };
        for (const TCHAR* NodeName : SupportedNodes)
        {
            Supported.Add(MakeShared<FJsonValueString>(NodeName));
        }
        Obj->SetArrayField(TEXT("supported_importer_constructors"), Supported);

        TArray<TSharedPtr<FJsonValue>> SafeSkipped;
        SafeSkipped.Add(MakeShared<FJsonValueString>(TEXT("Brand-new Event Dispatcher signature creation is not implemented; delegate nodes require an existing compiled dispatcher property.")));
        SafeSkipped.Add(MakeShared<FJsonValueString>(TEXT("P3 graph patch actions support collapsed, interface, Construction Script, Widget Blueprint K2, and Animation Blueprint AnimGraph targets. Unsupported specialized node constructors remain guarded.")));
        SafeSkipped.Add(MakeShared<FJsonValueString>(TEXT("P4 accepts UE4.27 serialized default text for Set/Map and resolved object/class/soft-reference member variables; unsupported or unresolved pin types are rejected before mutation.")));
        SafeSkipped.Add(MakeShared<FJsonValueString>(TEXT("Niagara round-trip remains partial P5 scope.")));
        SafeSkipped.Add(MakeShared<FJsonValueString>(TEXT("Unknown node classes are reported as unsupported before mutation or as warnings when safe-skipped.")));
        Obj->SetArrayField(TEXT("unsupported_but_safe_skipped"), SafeSkipped);
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

    Root->SetObjectField(TEXT("class_metadata"), ExportFullClassMetadata(Blueprint));
    Root->SetArrayField(TEXT("class_defaults"), ExportClassDefaults(Blueprint));
    Root->SetObjectField(TEXT("component_hierarchy"), ExportComponentHierarchy(Blueprint));
    Root->SetArrayField(TEXT("timelines"), ExportTimelineTemplates(Blueprint));
    Root->SetObjectField(TEXT("node_constructor_compatibility"), ExportNodeConstructorCompatibility());

    TArray<TSharedPtr<FJsonValue>> Variables;
    for (const FBPVariableDescription& Var : Blueprint->NewVariables)
    {
        Variables.Add(MakeShared<FJsonValueObject>(BlueprintVariableToJson(Var, Blueprint)));
    }
    Root->SetArrayField(TEXT("variables"), Variables);

    TArray<UEdGraph*> AllGraphs = CollectAllBlueprintGraphs(Blueprint);
    Root->SetObjectField(TEXT("graph_coverage"), ExportBlueprintGraphCoverage(Blueprint, AllGraphs));

    TArray<TSharedPtr<FJsonValue>> Functions;
    for (const UEdGraph* Graph : AllGraphs)
    {
        if (Graph)
        {
            Functions.Add(MakeShared<FJsonValueObject>(GraphToFunctionJson(Graph, Blueprint)));
        }
    }
    Root->SetArrayField(TEXT("functions"), Functions);

    TArray<TSharedPtr<FJsonValue>> Warnings;
    Warnings.Add(MakeShared<FJsonValueString>(TEXT("This export is optimized for AI/code review and patch generation, not a full binary-safe Blueprint clone.")));
    Warnings.Add(MakeShared<FJsonValueString>(TEXT("Use FunctionEntry nodes and graph_type to separate functions. Do not rely on one EventGraph as one function.")));
    Warnings.Add(MakeShared<FJsonValueString>(TEXT("Enum pins contain internal_name and display_name when the enum object is available.")));
    Warnings.Add(MakeShared<FJsonValueString>(TEXT("Blueprint export now includes all known graph containers plus package-owned extra graphs, component hierarchy, interface context, class defaults, comments/reroutes and editor layout fields.")));
    Warnings.Add(MakeShared<FJsonValueString>(TEXT("Collapsed graph import is still guarded: these graphs are exported fully for review and comparison, while unsupported constructors are reported before mutation.")));
    Root->SetArrayField(TEXT("warnings"), Warnings);

    return SerializeJsonObject(Root, OutJson);
}



bool FN2CAIExport::BuildEnumAssetAIJson(UObject* EnumAsset, FString& OutJson, FString& OutError)
{
    using namespace N2CAIExport_Private;

    OutJson.Empty();
    OutError.Empty();

    UEnum* Enum = Cast<UEnum>(EnumAsset);
    if (!Enum)
    {
        OutError = EnumAsset
            ? FString::Printf(TEXT("Asset is not an Enum asset: %s"), *EnumAsset->GetPathName())
            : TEXT("Invalid Enum asset");
        return false;
    }

    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("schema"), TEXT("N2C_AI_EXPORT_V2"));
    Root->SetStringField(TEXT("export_kind"), TEXT("Enum"));
    Root->SetStringField(TEXT("exported_at"), FDateTime::Now().ToIso8601());
    Root->SetStringField(TEXT("engine_target"), TEXT("UE4.27"));

    TSharedPtr<FJsonObject> Metadata = MakeShared<FJsonObject>();
    Metadata->SetStringField(TEXT("enum_name"), Enum->GetName());
    Metadata->SetStringField(TEXT("enum_path"), Enum->GetPathName());
    Metadata->SetStringField(TEXT("asset_class"), Enum->GetClass() ? Enum->GetClass()->GetName() : TEXT(""));
    Metadata->SetStringField(TEXT("package_name"), Enum->GetOutermost() ? Enum->GetOutermost()->GetName() : TEXT(""));
    Metadata->SetBoolField(TEXT("is_asset_dirty"), Enum->GetOutermost() ? Enum->GetOutermost()->IsDirty() : false);
    Root->SetObjectField(TEXT("metadata"), Metadata);

    TArray<TSharedPtr<FJsonValue>> Values;
    int32 ExportedValueCount = 0;
    int32 HiddenValueCount = 0;
    for (int32 Index = 0; Index < Enum->NumEnums(); ++Index)
    {
        const bool bHidden = Enum->HasMetaData(TEXT("Hidden"), Index);
        const FString InternalName = Enum->GetNameStringByIndex(Index);
        const bool bLooksLikeMax = InternalName.EndsWith(TEXT("_MAX")) || InternalName.Equals(TEXT("MAX"), ESearchCase::IgnoreCase);
        if (bHidden || bLooksLikeMax)
        {
            ++HiddenValueCount;
            continue;
        }

        TSharedPtr<FJsonObject> ValueObj = MakeShared<FJsonObject>();
        ValueObj->SetNumberField(TEXT("index"), Index);
        ValueObj->SetNumberField(TEXT("value"), static_cast<double>(Enum->GetValueByIndex(Index)));
        ValueObj->SetStringField(TEXT("internal_name"), InternalName);
        ValueObj->SetStringField(TEXT("display_name"), Enum->GetDisplayNameTextByIndex(Index).ToString());
        if (Enum->HasMetaData(TEXT("ToolTip"), Index))
        {
            ValueObj->SetStringField(TEXT("tooltip"), Enum->GetMetaData(TEXT("ToolTip"), Index));
        }
        Values.Add(MakeShared<FJsonValueObject>(ValueObj));
        ++ExportedValueCount;
    }

    Root->SetNumberField(TEXT("value_count"), ExportedValueCount);
    Root->SetNumberField(TEXT("hidden_or_max_value_count"), HiddenValueCount);
    Root->SetArrayField(TEXT("values"), Values);

    TArray<TSharedPtr<FJsonValue>> Warnings;
    Warnings.Add(MakeShared<FJsonValueString>(TEXT("Enum export is read-only and AI-friendly. It keeps value index, numeric value, internal name and display name.")));
    Warnings.Add(MakeShared<FJsonValueString>(TEXT("Hidden values and generated _MAX entries are skipped by default.")));
    Root->SetArrayField(TEXT("warnings"), Warnings);

    return SerializeJsonObject(Root, OutJson);
}

bool FN2CAIExport::BuildStructAssetAIJson(UObject* StructAsset, FString& OutJson, FString& OutError)
{
    using namespace N2CAIExport_Private;

    OutJson.Empty();
    OutError.Empty();

    UScriptStruct* ScriptStruct = Cast<UScriptStruct>(StructAsset);
    if (!ScriptStruct)
    {
        OutError = StructAsset
            ? FString::Printf(TEXT("Asset is not a Struct asset: %s"), *StructAsset->GetPathName())
            : TEXT("Invalid Struct asset");
        return false;
    }

    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("schema"), TEXT("N2C_AI_EXPORT_V2"));
    Root->SetStringField(TEXT("export_kind"), TEXT("Struct"));
    Root->SetStringField(TEXT("exported_at"), FDateTime::Now().ToIso8601());
    Root->SetStringField(TEXT("engine_target"), TEXT("UE4.27"));

    TSharedPtr<FJsonObject> Metadata = MakeShared<FJsonObject>();
    Metadata->SetStringField(TEXT("struct_name"), ScriptStruct->GetName());
    Metadata->SetStringField(TEXT("struct_path"), ScriptStruct->GetPathName());
    Metadata->SetStringField(TEXT("asset_class"), ScriptStruct->GetClass() ? ScriptStruct->GetClass()->GetName() : TEXT(""));
    Metadata->SetStringField(TEXT("package_name"), ScriptStruct->GetOutermost() ? ScriptStruct->GetOutermost()->GetName() : TEXT(""));
    Metadata->SetBoolField(TEXT("is_asset_dirty"), ScriptStruct->GetOutermost() ? ScriptStruct->GetOutermost()->IsDirty() : false);
    Metadata->SetNumberField(TEXT("structure_size"), ScriptStruct->GetStructureSize());
    Metadata->SetNumberField(TEXT("min_alignment"), ScriptStruct->GetMinAlignment());
    Root->SetObjectField(TEXT("metadata"), Metadata);

    TArray<TSharedPtr<FJsonValue>> Fields;
    int32 FieldCount = 0;

    TArray<uint8> DefaultStructData;
    const int32 StructSize = ScriptStruct->GetStructureSize();
    if (StructSize > 0)
    {
        DefaultStructData.SetNumZeroed(StructSize);
        ScriptStruct->InitializeStruct(DefaultStructData.GetData());
    }

    TSet<const UObject*> Visited;
    for (TFieldIterator<FProperty> It(ScriptStruct); It; ++It)
    {
        FProperty* Property = *It;
        if (!Property)
        {
            continue;
        }

        TSharedPtr<FJsonObject> FieldObj;
        if (DefaultStructData.Num() > 0)
        {
            FieldObj = ExportPropertyDeep(Property, DefaultStructData.GetData(), ScriptStruct, ScriptStruct, 0, Visited);
        }
        else
        {
            FieldObj = MakeShared<FJsonObject>();
            AddPropertyMetadata(FieldObj, Property);
        }

        FieldObj->SetNumberField(TEXT("field_index"), FieldCount);
        Fields.Add(MakeShared<FJsonValueObject>(FieldObj));
        ++FieldCount;
    }

    if (DefaultStructData.Num() > 0)
    {
        ScriptStruct->DestroyStruct(DefaultStructData.GetData());
    }

    Root->SetNumberField(TEXT("field_count"), FieldCount);
    Root->SetArrayField(TEXT("fields"), Fields);

    TArray<TSharedPtr<FJsonValue>> Warnings;
    Warnings.Add(MakeShared<FJsonValueString>(TEXT("Struct export is read-only and AI-friendly. It exports reflected fields, property flags, metadata and initialized default values when UE exposes them.")));
    Warnings.Add(MakeShared<FJsonValueString>(TEXT("UserDefinedStruct internal field names may contain Unreal-generated suffixes; prefer display_name/category metadata when present.")));
    Root->SetArrayField(TEXT("warnings"), Warnings);

    return SerializeJsonObject(Root, OutJson);
}

bool FN2CAIExport::BuildNiagaraAssetAIJson(UObject* NiagaraAsset, FString& OutJson, FString& OutError)
{
    using namespace N2CAIExport_Private;

    OutJson.Empty();
    OutError.Empty();

    if (!NiagaraAsset)
    {
        OutError = TEXT("Invalid Niagara asset");
        return false;
    }

    const FString ClassName = NiagaraAsset->GetClass() ? NiagaraAsset->GetClass()->GetName() : TEXT("");
    if (!ClassName.Contains(TEXT("Niagara")))
    {
        OutError = FString::Printf(TEXT("Asset is not a Niagara asset: %s"), *NiagaraAsset->GetPathName());
        return false;
    }

    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("schema"), TEXT("N2C_AI_EXPORT_V2"));
    Root->SetStringField(TEXT("export_kind"), TEXT("Niagara"));
    Root->SetStringField(TEXT("exported_at"), FDateTime::Now().ToIso8601());
    Root->SetStringField(TEXT("engine_target"), TEXT("UE4.27"));

    TSharedPtr<FJsonObject> Metadata = MakeShared<FJsonObject>();
    Metadata->SetStringField(TEXT("asset_name"), NiagaraAsset->GetName());
    Metadata->SetStringField(TEXT("asset_path"), NiagaraAsset->GetPathName());
    Metadata->SetStringField(TEXT("asset_class"), ClassName);
    Metadata->SetStringField(TEXT("package_name"), NiagaraAsset->GetOutermost() ? NiagaraAsset->GetOutermost()->GetName() : TEXT(""));
    Metadata->SetBoolField(TEXT("is_asset_dirty"), NiagaraAsset->GetOutermost() ? NiagaraAsset->GetOutermost()->IsDirty() : false);
    Root->SetObjectField(TEXT("metadata"), Metadata);

    Root->SetObjectField(TEXT("niagara_asset"), ExportNiagaraObject(NiagaraAsset));

    TArray<TSharedPtr<FJsonValue>> Warnings;
    Warnings.Add(MakeShared<FJsonValueString>(TEXT("Niagara export is a read-only compact typed summary export for AI inspection.")));
    Warnings.Add(MakeShared<FJsonValueString>(TEXT("It exports emitter/script/renderer summaries, compact Niagara subobject diagnostics, decoded FNiagaraParameterStore values and compact reflection sections without recursive duplication.")));
    Warnings.Add(MakeShared<FJsonValueString>(TEXT("It is still not a binary-safe Niagara graph clone, but it now exports reimportable dynamic_input_tree input_overrides plus skipped_input_overrides plus selected curve/data-interface diagnostics for inspection of non-reimportable curve/dynamic-input graph shapes.")));
    Warnings.Add(MakeShared<FJsonValueString>(TEXT("Niagara Editor toolbar buttons are available for export and parameter-only import; Content Browser context menu is still supported.")));
    Root->SetArrayField(TEXT("warnings"), Warnings);

    return SerializeJsonObjectCompact(Root, OutJson);
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
    Root->SetStringField(TEXT("safety_note"), TEXT("Read-only compact typed Niagara export. This plugin does not import or mutate Niagara graphs."));

    return SerializeJsonObjectCompact(Root, OutJson);
}
