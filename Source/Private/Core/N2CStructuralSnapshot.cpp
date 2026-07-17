#include "Core/N2CStructuralSnapshot.h"
#include "Core/N2CMacroReference.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/DataTable.h"
#include "K2Node_BaseMCDelegate.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CastByteToEnum.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_CreateDelegate.h"
#include "K2Node_EnumLiteral.h"
#include "K2Node_Event.h"
#include "K2Node_ForEachElementInEnum.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_GetDataTableRow.h"
#include "K2Node_GetEnumeratorName.h"
#include "K2Node_InputAction.h"
#include "K2Node_InputAxisEvent.h"
#include "K2Node_InputAxisKeyEvent.h"
#include "K2Node_InputKey.h"
#include "K2Node_Composite.h"
#include "K2Node_StructOperation.h"
#include "K2Node_SwitchEnum.h"
#include "K2Node_Tunnel.h"
#include "K2Node_Variable.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/SecureHash.h"
#include "UObject/UObjectHash.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/UnrealType.h"

namespace N2CStructuralSnapshot_Private
{
    static FString ObjectPath(const UObject* Object) { return Object ? Object->GetPathName() : FString(); }

    static TSharedPtr<FJsonObject> PinType(const FEdGraphPinType& Type)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetStringField(TEXT("category"), Type.PinCategory.ToString());
        Out->SetStringField(TEXT("subcategory"), Type.PinSubCategory.ToString());
        Out->SetStringField(TEXT("subcategory_object"), ObjectPath(Type.PinSubCategoryObject.Get()));
        Out->SetNumberField(TEXT("container"), static_cast<int32>(Type.ContainerType));
        Out->SetStringField(TEXT("map_value_category"), Type.PinValueType.TerminalCategory.ToString());
        Out->SetStringField(TEXT("map_value_subcategory"), Type.PinValueType.TerminalSubCategory.ToString());
        Out->SetStringField(TEXT("map_value_object"), ObjectPath(Type.PinValueType.TerminalSubCategoryObject.Get()));
        Out->SetBoolField(TEXT("reference"), Type.bIsReference);
        Out->SetBoolField(TEXT("const"), Type.bIsConst);
        Out->SetBoolField(TEXT("weak"), Type.bIsWeakPointer);
        return Out;
    }

    static FString GraphKind(const UBlueprint* Blueprint, const UEdGraph* Graph)
    {
        if (Blueprint->UbergraphPages.Contains(const_cast<UEdGraph*>(Graph)))
        {
            return Graph->GetFName() == UEdGraphSchema_K2::FN_UserConstructionScript ? TEXT("construction_script") : TEXT("ubergraph");
        }
        if (Blueprint->FunctionGraphs.Contains(const_cast<UEdGraph*>(Graph))) return TEXT("function");
        if (Blueprint->MacroGraphs.Contains(const_cast<UEdGraph*>(Graph))) return TEXT("macro");
        if (Blueprint->DelegateSignatureGraphs.Contains(const_cast<UEdGraph*>(Graph))) return TEXT("dispatcher_signature");
        if (Graph->GetOuter() && Graph->GetOuter()->IsA<UK2Node_Composite>()) return TEXT("collapsed_graph");
        return TEXT("other");
    }

    static FString SemanticIdentity(const UEdGraphNode* Node)
    {
        FString Identity = Node->GetClass()->GetPathName();
        if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
        {
            Identity += TEXT("|function=") + Call->FunctionReference.GetMemberName().ToString();
            Identity += TEXT("|owner=") + ObjectPath(Call->FunctionReference.GetMemberParentClass());
        }
        else if (const UK2Node_Variable* Variable = Cast<UK2Node_Variable>(Node))
        {
            Identity += TEXT("|member=") + Variable->VariableReference.GetMemberName().ToString();
            Identity += TEXT("|owner=") + ObjectPath(Variable->VariableReference.GetMemberParentClass());
        }
        else if (const UK2Node_Event* Event = Cast<UK2Node_Event>(Node))
        {
            Identity += TEXT("|event=") + Event->EventReference.GetMemberName().ToString();
            Identity += TEXT("|owner=") + ObjectPath(Event->EventReference.GetMemberParentClass());
        }
        else if (const UK2Node_MacroInstance* Macro = Cast<UK2Node_MacroInstance>(Node))
        {
            const UEdGraph* MacroGraph = Macro->GetMacroGraph();
            const UBlueprint* MacroOwner = MacroGraph ? FBlueprintEditorUtils::FindBlueprintForGraph(const_cast<UEdGraph*>(MacroGraph)) : nullptr;
            Identity += TEXT("|macro_owner=") + ObjectPath(MacroOwner);
            Identity += TEXT("|macro_graph=") + ObjectPath(MacroGraph);
        }
        else if (const UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
        {
            Identity += TEXT("|entry=") + Entry->FunctionReference.GetMemberName().ToString();
        }
        else if (Cast<UK2Node_FunctionResult>(Node)) Identity += TEXT("|result");
        else if (const UK2Node_Composite* Composite = Cast<UK2Node_Composite>(Node))
        {
            Identity += TEXT("|composite_bound=") + ObjectPath(Composite->BoundGraph);
            Identity += FString::Printf(TEXT("|entry_exit=%d:%d"), Composite->GetEntryNode() ? 1 : 0, Composite->GetExitNode() ? 1 : 0);
        }
        else if (const UK2Node_Tunnel* Tunnel = Cast<UK2Node_Tunnel>(Node))
        {
            Identity += FString::Printf(TEXT("|tunnel=%d:%d"), Tunnel->bCanHaveInputs ? 1 : 0, Tunnel->bCanHaveOutputs ? 1 : 0);
        }
        else if (const UK2Node_StructOperation* StructNode = Cast<UK2Node_StructOperation>(Node))
        {
            Identity += TEXT("|struct=") + ObjectPath(StructNode->StructType);
        }
        else if (const UK2Node_SwitchEnum* Switch = Cast<UK2Node_SwitchEnum>(Node)) Identity += TEXT("|enum=") + ObjectPath(Switch->Enum);
        else if (const UK2Node_EnumLiteral* Literal = Cast<UK2Node_EnumLiteral>(Node)) Identity += TEXT("|enum=") + ObjectPath(Literal->Enum);
        else if (const UK2Node_ForEachElementInEnum* ForEach = Cast<UK2Node_ForEachElementInEnum>(Node)) Identity += TEXT("|enum=") + ObjectPath(ForEach->Enum);
        else if (const UK2Node_CastByteToEnum* CastNode = Cast<UK2Node_CastByteToEnum>(Node)) Identity += TEXT("|enum=") + ObjectPath(CastNode->Enum);
        else if (Cast<UK2Node_GetEnumeratorName>(Node))
        {
            const UEnum* Enum = nullptr;
            for (const UEdGraphPin* Pin : Node->Pins)
            {
                if (!Pin || Pin->PinName != TEXT("Enumerator")) continue;
                const UEdGraphPin* TypePin = Pin->LinkedTo.Num() > 0 ? Pin->LinkedTo[0] : Pin;
                Enum = TypePin ? Cast<UEnum>(TypePin->PinType.PinSubCategoryObject.Get()) : nullptr;
                break;
            }
            Identity += TEXT("|enum=") + ObjectPath(Enum);
        }
        else if (const UK2Node_GetDataTableRow* GetRow = Cast<UK2Node_GetDataTableRow>(Node))
        {
            const UEdGraphPin* TablePin = GetRow->GetDataTablePin();
            Identity += TEXT("|table=") + ObjectPath(TablePin ? TablePin->DefaultObject : nullptr);
            Identity += TEXT("|row_struct=") + ObjectPath(GetRow->GetDataTableRowStructType());
        }
        else if (const UK2Node_BaseMCDelegate* Delegate = Cast<UK2Node_BaseMCDelegate>(Node))
        {
            const FProperty* Property = Delegate->GetProperty();
            Identity += TEXT("|delegate=") + Delegate->GetPropertyName().ToString();
            Identity += TEXT("|owner=") + ObjectPath(Property ? Property->GetOwnerClass() : nullptr);
        }
        else if (const UK2Node_CreateDelegate* CreateDelegate = Cast<UK2Node_CreateDelegate>(Node)) Identity += TEXT("|function=") + CreateDelegate->SelectedFunctionName.ToString();
        else if (const UK2Node_ComponentBoundEvent* Bound = Cast<UK2Node_ComponentBoundEvent>(Node))
        {
            Identity += TEXT("|component=") + Bound->ComponentPropertyName.ToString();
            Identity += TEXT("|delegate=") + Bound->DelegatePropertyName.ToString();
            Identity += TEXT("|owner=") + ObjectPath(Bound->DelegateOwnerClass);
        }
        else if (const UK2Node_InputAction* ActionInput = Cast<UK2Node_InputAction>(Node)) Identity += TEXT("|action=") + ActionInput->InputActionName.ToString();
        else if (const UK2Node_InputAxisEvent* AxisInput = Cast<UK2Node_InputAxisEvent>(Node)) Identity += TEXT("|axis=") + AxisInput->InputAxisName.ToString();
        else if (const UK2Node_InputAxisKeyEvent* AxisKeyInput = Cast<UK2Node_InputAxisKeyEvent>(Node)) Identity += TEXT("|key=") + AxisKeyInput->AxisKey.GetFName().ToString();
        else if (const UK2Node_InputKey* KeyInput = Cast<UK2Node_InputKey>(Node)) Identity += TEXT("|key=") + KeyInput->InputKey.GetFName().ToString();
        return Identity;
    }

    static TSharedPtr<FJsonObject> NodeObject(const UEdGraphNode* Node, const FString& NodeKey, const TMap<const UEdGraphNode*, FString>& Keys)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetStringField(TEXT("identity"), NodeKey);
        Out->SetStringField(TEXT("node_class"), Node->GetClass()->GetPathName());
        Out->SetStringField(TEXT("semantic_identity"), SemanticIdentity(Node));
        Out->SetStringField(TEXT("node_guid"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
        if (const UK2Node_MacroInstance* Macro = Cast<UK2Node_MacroInstance>(Node))
        {
            const UEdGraph* MacroGraph = Macro->GetMacroGraph();
            const UBlueprint* Owner = MacroGraph ? FBlueprintEditorUtils::FindBlueprintForGraph(const_cast<UEdGraph*>(MacroGraph)) : nullptr;
            Out->SetStringField(TEXT("macro_owner_asset"), ObjectPath(Owner));
            Out->SetStringField(TEXT("macro_graph_path"), ObjectPath(MacroGraph));
            Out->SetStringField(TEXT("macro_graph_name"), MacroGraph ? MacroGraph->GetName() : FString());
            Out->SetStringField(TEXT("macro_signature_hash"), FN2CMacroReference::SignatureHash(MacroGraph));
            Out->SetObjectField(TEXT("resolved_wildcard_type"), PinType(Macro->ResolvedWildcardType));
        }
        if (const UK2Node_Composite* Composite = Cast<UK2Node_Composite>(Node))
        {
            Out->SetStringField(TEXT("bound_graph_path"), ObjectPath(Composite->BoundGraph));
            Out->SetStringField(TEXT("entry_tunnel_guid"), Composite->GetEntryNode() ? Composite->GetEntryNode()->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens) : FString());
            Out->SetStringField(TEXT("exit_tunnel_guid"), Composite->GetExitNode() ? Composite->GetExitNode()->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens) : FString());
        }
        TArray<TSharedPtr<FJsonValue>> Pins;
        TArray<const UEdGraphPin*> SortedPins;
        for (const UEdGraphPin* Pin : Node->Pins) if (Pin) SortedPins.Add(Pin);
        if (!Node->IsA<UK2Node_Tunnel>()) SortedPins.Sort([](const UEdGraphPin& A, const UEdGraphPin& B) { return A.PinName.LexicalLess(B.PinName); });
        int32 PinOrder = 0;
        for (const UEdGraphPin* Pin : SortedPins)
        {
            TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
            PinObj->SetNumberField(TEXT("order"), PinOrder++);
            PinObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
            PinObj->SetNumberField(TEXT("direction"), static_cast<int32>(Pin->Direction));
            PinObj->SetObjectField(TEXT("type"), PinType(Pin->PinType));
            FString DefaultValue = Pin->DefaultValue;
            TArray<FString> TupleParts;
            if (DefaultValue.ParseIntoArray(TupleParts, TEXT(","), false) > 1)
            {
                bool bNumericTuple = true;
                FString CanonicalTuple;
                for (FString Part : TupleParts)
                {
                    Part.TrimStartAndEndInline();
                    if (!Part.IsNumeric())
                    {
                        bNumericTuple = false;
                        break;
                    }
                    if (!CanonicalTuple.IsEmpty()) CanonicalTuple += TEXT(",");
                    CanonicalTuple += FString::Printf(TEXT("%.9g"), FCString::Atod(*Part));
                }
                if (bNumericTuple) DefaultValue = CanonicalTuple;
            }
            if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean &&
                (DefaultValue.Equals(TEXT("true"), ESearchCase::IgnoreCase) ||
                 DefaultValue.Equals(TEXT("false"), ESearchCase::IgnoreCase)))
            {
                DefaultValue = DefaultValue.ToLower();
            }
            PinObj->SetStringField(TEXT("default_value"), DefaultValue);
            PinObj->SetStringField(TEXT("default_object"), ObjectPath(Pin->DefaultObject));
            PinObj->SetStringField(TEXT("default_text"), Pin->DefaultTextValue.ToString());
            PinObj->SetStringField(TEXT("persistent_guid"), Pin->PersistentGuid.ToString(EGuidFormats::DigitsWithHyphens));
            PinObj->SetBoolField(TEXT("orphan"), Pin->bOrphanedPin);
            TArray<FString> Links;
            for (const UEdGraphPin* Linked : Pin->LinkedTo)
            {
                const FString* Other = Linked && Linked->GetOwningNode() ? Keys.Find(Linked->GetOwningNode()) : nullptr;
                if (Other) Links.Add(*Other + TEXT("/") + Linked->PinName.ToString());
            }
            Links.Sort();
            TArray<TSharedPtr<FJsonValue>> LinkValues; for (const FString& Link : Links) LinkValues.Add(MakeShared<FJsonValueString>(Link));
            PinObj->SetArrayField(TEXT("links"), LinkValues);
            Pins.Add(MakeShared<FJsonValueObject>(PinObj));
        }
        Out->SetArrayField(TEXT("pins"), Pins);
        return Out;
    }

    static TSharedPtr<FJsonObject> GraphObject(UBlueprint* Blueprint, UEdGraph* Graph)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        const FString Kind = GraphKind(Blueprint, Graph);
        const FString Identity = Kind + TEXT("|") + Graph->GetName() + TEXT("|") + Graph->GetPathName();
        Out->SetStringField(TEXT("identity"), Identity);
        Out->SetStringField(TEXT("name"), Graph->GetName());
        Out->SetStringField(TEXT("kind"), Kind);
        Out->SetStringField(TEXT("path"), Graph->GetPathName());
        Out->SetStringField(TEXT("owner"), ObjectPath(FBlueprintEditorUtils::FindBlueprintForGraph(Graph)));
        Out->SetStringField(TEXT("schema_class"), Graph->GetSchema() ? Graph->GetSchema()->GetClass()->GetPathName() : FString());

        TArray<UEdGraphNode*> Nodes; for (UEdGraphNode* Node : Graph->Nodes) if (Node) Nodes.Add(Node);
        Nodes.Sort([](const UEdGraphNode& A, const UEdGraphNode& B)
        {
            const FString ASem = SemanticIdentity(&A), BSem = SemanticIdentity(&B);
            return ASem == BSem ? A.NodeGuid < B.NodeGuid : ASem < BSem;
        });
        TMap<FString, int32> Counts; TMap<const UEdGraphNode*, FString> Keys;
        for (const UEdGraphNode* Node : Nodes)
        {
            const FString Base = SemanticIdentity(Node); const int32 Index = Counts.FindOrAdd(Base)++;
            Keys.Add(Node, Base + FString::Printf(TEXT("#%d"), Index));
        }
        TArray<TSharedPtr<FJsonValue>> NodeValues;
        for (const UEdGraphNode* Node : Nodes) NodeValues.Add(MakeShared<FJsonValueObject>(NodeObject(Node, Keys.FindChecked(Node), Keys)));
        Out->SetArrayField(TEXT("nodes"), NodeValues);
        return Out;
    }

    static FString MetadataValue(const FBPVariableDescription& Variable, const FName Key)
    {
        const int32 Index = Variable.FindMetaDataEntryIndexForKey(Key);
        return Index != INDEX_NONE ? Variable.MetaDataArray[Index].DataValue : FString();
    }

    static bool Serialize(const TSharedPtr<FJsonObject>& Object, FString& Out)
    {
        TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
        return Object.IsValid() && FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
    }

    static TSharedPtr<FJsonObject> DiffObject(const FString& Category, const FString& Detail)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetStringField(TEXT("category"), Category); Out->SetStringField(TEXT("detail"), Detail); return Out;
    }
}

bool FN2CStructuralSnapshot::HashJson(const TSharedPtr<FJsonObject>& Object, FString& OutHash, FString& OutError)
{
    using namespace N2CStructuralSnapshot_Private;
    FString Text; if (!Serialize(Object, Text)) { OutError = TEXT("snapshot_serialize_failed"); return false; }
    FSHAHash Hash; FSHA1::HashBuffer(TCHAR_TO_UTF8(*Text), FTCHARToUTF8(*Text).Length(), Hash.Hash);
    OutHash = Hash.ToString(); return true;
}

bool FN2CStructuralSnapshot::Build(UBlueprint* Blueprint, TSharedPtr<FJsonObject>& OutSnapshot, FString& OutHash, FString& OutError)
{
    using namespace N2CStructuralSnapshot_Private;
    if (!Blueprint) { OutError = TEXT("snapshot_blueprint_invalid"); return false; }
    OutSnapshot = MakeShared<FJsonObject>(); OutSnapshot->SetStringField(TEXT("schema"), TEXT("N2C_STRUCTURAL_BASELINE_V3"));
    TSharedPtr<FJsonObject> Asset = MakeShared<FJsonObject>();
    Asset->SetStringField(TEXT("asset_path"), Blueprint->GetPathName()); Asset->SetStringField(TEXT("blueprint_class"), Blueprint->GetClass()->GetPathName());
    Asset->SetStringField(TEXT("parent_class"), ObjectPath(Blueprint->ParentClass)); Asset->SetNumberField(TEXT("blueprint_type"), static_cast<int32>(Blueprint->BlueprintType)); Asset->SetNumberField(TEXT("compile_status"), static_cast<int32>(Blueprint->Status));
    TArray<FString> Interfaces; for (const FBPInterfaceDescription& Interface : Blueprint->ImplementedInterfaces) Interfaces.Add(ObjectPath(Interface.Interface)); Interfaces.Sort();
    TArray<TSharedPtr<FJsonValue>> InterfaceValues; for (const FString& Interface : Interfaces) InterfaceValues.Add(MakeShared<FJsonValueString>(Interface)); Asset->SetArrayField(TEXT("interfaces"), InterfaceValues);
    OutSnapshot->SetObjectField(TEXT("asset"), Asset);

    TArray<UEdGraph*> Graphs; Graphs.Append(Blueprint->UbergraphPages); Graphs.Append(Blueprint->FunctionGraphs); Graphs.Append(Blueprint->MacroGraphs); Graphs.Append(Blueprint->DelegateSignatureGraphs); TArray<UObject*> Children; GetObjectsWithOuter(Blueprint, Children, true); for (UObject* Child : Children) { UEdGraph* ChildGraph = Cast<UEdGraph>(Child); if (ChildGraph && ChildGraph->GetOuter() && ChildGraph->GetOuter()->IsA<UK2Node_Composite>()) Graphs.AddUnique(ChildGraph); }
    Graphs.Sort([Blueprint](const UEdGraph& A, const UEdGraph& B) { return (GraphKind(Blueprint, &A) + A.GetPathName()) < (GraphKind(Blueprint, &B) + B.GetPathName()); });
    TArray<TSharedPtr<FJsonValue>> GraphValues; for (UEdGraph* Graph : Graphs) if (Graph) GraphValues.Add(MakeShared<FJsonValueObject>(GraphObject(Blueprint, Graph))); OutSnapshot->SetArrayField(TEXT("graphs"), GraphValues);

    TArray<const FBPVariableDescription*> Variables; for (const FBPVariableDescription& Variable : Blueprint->NewVariables) Variables.Add(&Variable);
    Variables.Sort([](const FBPVariableDescription& A, const FBPVariableDescription& B) { return A.VarName.LexicalLess(B.VarName); });
    TArray<TSharedPtr<FJsonValue>> VariableValues;
    for (const FBPVariableDescription* Variable : Variables)
    {
        FString DefaultValue = Variable->DefaultValue;
        if (Blueprint->GeneratedClass)
        {
            UObject* CDO = Blueprint->GeneratedClass->GetDefaultObject(false);
            if (CDO)
            {
                if (FProperty* Property = FindFProperty<FProperty>(Blueprint->GeneratedClass, Variable->VarName))
                {
                    if (void* Address = Property->ContainerPtrToValuePtr<void>(CDO))
                    {
                        DefaultValue.Reset();
                        Property->ExportTextItem(DefaultValue, Address, Address, CDO, PPF_SerializedAsImportText);
                    }
                }
            }
        }
        TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>(); Item->SetStringField(TEXT("name"), Variable->VarName.ToString()); Item->SetStringField(TEXT("category"), Variable->Category.ToString());
        Item->SetObjectField(TEXT("type"), PinType(Variable->VarType)); Item->SetStringField(TEXT("default"), DefaultValue); Item->SetStringField(TEXT("tooltip"), MetadataValue(*Variable, FBlueprintMetadata::MD_Tooltip)); Item->SetNumberField(TEXT("property_flags"), static_cast<double>(Variable->PropertyFlags)); VariableValues.Add(MakeShared<FJsonValueObject>(Item));
    }
    OutSnapshot->SetArrayField(TEXT("variables"), VariableValues);

    TArray<TSharedPtr<FJsonValue>> Functions;
    for (UEdGraph* Graph : Blueprint->FunctionGraphs)
    {
        if (!Graph) continue; TSharedPtr<FJsonObject> Function = MakeShared<FJsonObject>(); Function->SetStringField(TEXT("name"), Graph->GetName());
        TArray<UK2Node_FunctionEntry*> Entries; Graph->GetNodesOfClass(Entries); TArray<UK2Node_FunctionResult*> Results; Graph->GetNodesOfClass(Results);
        UK2Node_FunctionEntry* Entry = Entries.Num() ? Entries[0] : nullptr; Function->SetNumberField(TEXT("flags"), Entry ? Entry->GetFunctionFlags() : 0); Function->SetBoolField(TEXT("pure"), Entry && ((Entry->GetFunctionFlags() & FUNC_BlueprintPure) != 0)); Function->SetBoolField(TEXT("const"), Entry && ((Entry->GetFunctionFlags() & FUNC_Const) != 0));
        Function->SetStringField(TEXT("category"), Entry ? Entry->MetaData.Category.ToString() : FString()); Function->SetStringField(TEXT("tooltip"), Entry ? Entry->MetaData.ToolTip.ToString() : FString()); Function->SetNumberField(TEXT("local_count"), Entry ? Entry->LocalVariables.Num() : 0);
        bool bInternalExec = false; if (Entry)
        {
            UEdGraphPin* ThenPin = Entry->FindPin(UEdGraphSchema_K2::PN_Then); for (UEdGraphPin* Linked : ThenPin ? ThenPin->LinkedTo : TArray<UEdGraphPin*>()) if (Linked && Cast<UK2Node_FunctionResult>(Linked->GetOwningNode())) bInternalExec = true;
        }
        Function->SetBoolField(TEXT("internal_entry_result_exec"), bInternalExec); Function->SetNumberField(TEXT("entry_count"), Entries.Num()); Function->SetNumberField(TEXT("result_count"), Results.Num()); Functions.Add(MakeShared<FJsonValueObject>(Function));
    }
    OutSnapshot->SetArrayField(TEXT("functions"), Functions);
    return HashJson(OutSnapshot, OutHash, OutError);
}

bool FN2CStructuralSnapshot::Compare(const TSharedPtr<FJsonObject>& Expected, const TSharedPtr<FJsonObject>& Actual, TSharedPtr<FJsonObject>& OutDiff, FString& OutError)
{
    using namespace N2CStructuralSnapshot_Private;
    FString ExpectedText, ActualText; if (!Serialize(Expected, ExpectedText) || !Serialize(Actual, ActualText)) { OutError = TEXT("snapshot_serialize_failed"); return false; }
    OutDiff = MakeShared<FJsonObject>(); OutDiff->SetStringField(TEXT("schema"), TEXT("N2C_STRUCTURAL_DIFF_V1"));
    TArray<TSharedPtr<FJsonValue>> Missing, Unexpected, Changed;
    if (ExpectedText != ActualText)
    {
        int32 Index = 0, Common = FMath::Min(ExpectedText.Len(), ActualText.Len()); while (Index < Common && ExpectedText[Index] == ActualText[Index]) ++Index;
        Changed.Add(MakeShared<FJsonValueObject>(DiffObject(TEXT("persistence"), FString::Printf(TEXT("first_difference=%d"), Index))));
    }
    OutDiff->SetArrayField(TEXT("missing"), Missing); OutDiff->SetArrayField(TEXT("unexpected"), Unexpected); OutDiff->SetArrayField(TEXT("changed"), Changed); OutDiff->SetNumberField(TEXT("first_difference"), Changed.Num() ? 0 : -1);
    return Changed.Num() == 0;
}

bool FN2CStructuralSnapshot::ValidateExpectedContract(const TSharedPtr<FJsonObject>& Contract, const TSharedPtr<FJsonObject>& Snapshot, TSharedPtr<FJsonObject>& OutDiff, FString& OutError)
{
    using namespace N2CStructuralSnapshot_Private;
    OutDiff = MakeShared<FJsonObject>(); OutDiff->SetStringField(TEXT("schema"), TEXT("N2C_STRUCTURAL_DIFF_V1"));
    TArray<TSharedPtr<FJsonValue>> Missing, Unexpected, Changed;
    if (!Contract.IsValid() || !Snapshot.IsValid()) { OutError = TEXT("expected_contract_invalid"); return false; }
    const TArray<TSharedPtr<FJsonValue>>* RequiredNodes = nullptr; const TArray<TSharedPtr<FJsonValue>>* Graphs = nullptr;
    Contract->TryGetArrayField(TEXT("required_nodes"), RequiredNodes); Snapshot->TryGetArrayField(TEXT("graphs"), Graphs);
    if (RequiredNodes && Graphs) for (const auto& RequiredValue : *RequiredNodes)
    {
        auto Required = RequiredValue->AsObject(); FString Class, Semantic; Required->TryGetStringField(TEXT("node_class"), Class); Required->TryGetStringField(TEXT("semantic_contains"), Semantic); bool Found = false;
        for (const auto& GraphValue : *Graphs) { const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr; if (!GraphValue->AsObject()->TryGetArrayField(TEXT("nodes"), Nodes) || !Nodes) continue; for (const auto& NodeValue : *Nodes) { FString ActualClass, ActualSemantic; NodeValue->AsObject()->TryGetStringField(TEXT("node_class"), ActualClass); NodeValue->AsObject()->TryGetStringField(TEXT("semantic_identity"), ActualSemantic); if ((Class.IsEmpty() || ActualClass.EndsWith(Class)) && (Semantic.IsEmpty() || ActualSemantic.Contains(Semantic))) { Found = true; break; } } if (Found) break; }
        if (!Found) Missing.Add(MakeShared<FJsonValueObject>(DiffObject(TEXT("node"), Class + TEXT("|") + Semantic)));
    }
    const TArray<TSharedPtr<FJsonValue>>* RequiredVariables = nullptr; const TArray<TSharedPtr<FJsonValue>>* Variables = nullptr;
    Contract->TryGetArrayField(TEXT("required_variables"), RequiredVariables); Snapshot->TryGetArrayField(TEXT("variables"), Variables);
    if (RequiredVariables && Variables) for (const auto& RequiredValue : *RequiredVariables)
    {
        auto Required = RequiredValue->AsObject(); FString Name, Category, MapValue, Default; double Container = -1; Required->TryGetStringField(TEXT("name"), Name); Required->TryGetStringField(TEXT("category"), Category); Required->TryGetStringField(TEXT("map_value_category"), MapValue); Required->TryGetStringField(TEXT("default"), Default); Required->TryGetNumberField(TEXT("container"), Container); bool Found = false;
        for (const auto& ActualValue : *Variables) { auto Actual = ActualValue->AsObject(); FString AName, ADefault; Actual->TryGetStringField(TEXT("name"), AName); Actual->TryGetStringField(TEXT("default"), ADefault); const TSharedPtr<FJsonObject>* Type = nullptr; Actual->TryGetObjectField(TEXT("type"), Type); double AContainer = -1; FString AMap; if (Type && Type->IsValid()) { (*Type)->TryGetNumberField(TEXT("container"), AContainer); (*Type)->TryGetStringField(TEXT("map_value_category"), AMap); } if (AName == Name && (Container < 0 || AContainer == Container) && (MapValue.IsEmpty() || AMap == MapValue) && (Default.IsEmpty() || ADefault == Default)) { Found = true; break; } }
        if (!Found) Missing.Add(MakeShared<FJsonValueObject>(DiffObject(TEXT("variable"), Name)));
    }
    const TArray<TSharedPtr<FJsonValue>>* RequiredFunctions = nullptr; const TArray<TSharedPtr<FJsonValue>>* Functions = nullptr;
    Contract->TryGetArrayField(TEXT("required_functions"), RequiredFunctions); Snapshot->TryGetArrayField(TEXT("functions"), Functions);
    if (RequiredFunctions && Functions) for (const auto& RequiredValue : *RequiredFunctions)
    {
        auto Required = RequiredValue->AsObject(); FString Name; bool Pure = false, Internal = false; double EntryCount = -1, ResultCount = -1; Required->TryGetStringField(TEXT("name"), Name); Required->TryGetBoolField(TEXT("pure"), Pure); Required->TryGetBoolField(TEXT("internal_entry_result_exec"), Internal); Required->TryGetNumberField(TEXT("entry_count"), EntryCount); Required->TryGetNumberField(TEXT("result_count"), ResultCount); bool Found = false;
        for (const auto& ActualValue : *Functions) { auto Actual = ActualValue->AsObject(); FString AName; bool APure = false, AInternal = false; double AEntryCount = -1, AResultCount = -1; Actual->TryGetStringField(TEXT("name"), AName); Actual->TryGetBoolField(TEXT("pure"), APure); Actual->TryGetBoolField(TEXT("internal_entry_result_exec"), AInternal); Actual->TryGetNumberField(TEXT("entry_count"), AEntryCount); Actual->TryGetNumberField(TEXT("result_count"), AResultCount); if (AName == Name && APure == Pure && AInternal == Internal && (EntryCount < 0 || AEntryCount == EntryCount) && (ResultCount < 0 || AResultCount == ResultCount)) { Found = true; break; } }
        if (!Found) Missing.Add(MakeShared<FJsonValueObject>(DiffObject(TEXT("function"), Name)));
    }
    OutDiff->SetArrayField(TEXT("missing"), Missing); OutDiff->SetArrayField(TEXT("unexpected"), Unexpected); OutDiff->SetArrayField(TEXT("changed"), Changed); OutError = Missing.Num() ? TEXT("expected_contract_mismatch") : FString(); return Missing.Num() == 0;
}
