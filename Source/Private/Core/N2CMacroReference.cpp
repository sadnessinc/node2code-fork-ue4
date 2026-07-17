#include "Core/N2CMacroReference.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Tunnel.h"
#include "Misc/SecureHash.h"

namespace N2CMacroReference_Private
{
    static FString GetString(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
    {
        FString Value;
        return Object.IsValid() && Object->TryGetStringField(Field, Value) ? Value : FString();
    }

    static FString ContainerName(EPinContainerType Type)
    {
        switch (Type) { case EPinContainerType::Array: return TEXT("Array"); case EPinContainerType::Set: return TEXT("Set"); case EPinContainerType::Map: return TEXT("Map"); default: return TEXT("None"); }
    }

    static FString TypeRecord(const FEdGraphPinType& Type)
    {
        const UObject* SubObject = Type.PinSubCategoryObject.Get();
        const UObject* ValueObject = Type.PinValueType.TerminalSubCategoryObject.Get();
        return FString::Printf(TEXT("%s|%s|%s|%s|%s|%s|%s|%d|%d|%d"), *Type.PinCategory.ToString(), *Type.PinSubCategory.ToString(), SubObject ? *SubObject->GetPathName() : TEXT(""), *ContainerName(Type.ContainerType), *Type.PinValueType.TerminalCategory.ToString(), *Type.PinValueType.TerminalSubCategory.ToString(), ValueObject ? *ValueObject->GetPathName() : TEXT(""), Type.bIsReference ? 1 : 0, Type.bIsConst ? 1 : 0, Type.bIsWeakPointer ? 1 : 0);
    }

    static TSharedPtr<FJsonObject> TypeJson(const FEdGraphPinType& Type)
    {
        TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
        Object->SetStringField(TEXT("category"), Type.PinCategory.ToString()); Object->SetStringField(TEXT("sub_category"), Type.PinSubCategory.ToString()); Object->SetStringField(TEXT("container"), ContainerName(Type.ContainerType));
        Object->SetStringField(TEXT("sub_category_object"), Type.PinSubCategoryObject.IsValid() ? Type.PinSubCategoryObject->GetPathName() : FString()); Object->SetBoolField(TEXT("is_reference"), Type.bIsReference); Object->SetBoolField(TEXT("is_const"), Type.bIsConst); Object->SetBoolField(TEXT("is_weak_pointer"), Type.bIsWeakPointer);
        TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>(); Value->SetStringField(TEXT("category"), Type.PinValueType.TerminalCategory.ToString()); Value->SetStringField(TEXT("sub_category"), Type.PinValueType.TerminalSubCategory.ToString()); Value->SetStringField(TEXT("sub_category_object"), Type.PinValueType.TerminalSubCategoryObject.IsValid() ? Type.PinValueType.TerminalSubCategoryObject->GetPathName() : FString()); Object->SetObjectField(TEXT("value_type"), Value);
        return Object;
    }

    static TSharedPtr<FJsonObject> PinJson(const UEdGraphPin* Pin)
    {
        TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>(); if (!Pin) return Object;
        Object->SetStringField(TEXT("name"), Pin->PinName.ToString()); Object->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output")); Object->SetObjectField(TEXT("pin_type"), TypeJson(Pin->PinType)); Object->SetStringField(TEXT("default_value"), Pin->DefaultValue); Object->SetStringField(TEXT("default_object"), Pin->DefaultObject ? Pin->DefaultObject->GetPathName() : FString()); Object->SetStringField(TEXT("default_text"), Pin->DefaultTextValue.ToString()); Object->SetBoolField(TEXT("orphan"), Pin->bOrphanedPin); return Object;
    }

    static void BuildTunnelRecords(const UEdGraph* Graph, TArray<FString>& Records, TArray<TSharedPtr<FJsonValue>>* Json)
    {
        if (!Graph) return;
        for (const UEdGraphNode* Node : Graph->Nodes)
        {
            const UK2Node_Tunnel* Tunnel = Cast<UK2Node_Tunnel>(Node); if (!Tunnel || Tunnel->GetClass() != UK2Node_Tunnel::StaticClass()) continue;
            const FString Role = Tunnel->bCanHaveInputs && !Tunnel->bCanHaveOutputs ? TEXT("exit") : !Tunnel->bCanHaveInputs && Tunnel->bCanHaveOutputs ? TEXT("entry") : TEXT("tunnel");
            for (const UEdGraphPin* Pin : Tunnel->Pins)
            {
                if (!Pin || Pin->ParentPin) continue;
                Records.Add(Role + TEXT("|") + Pin->PinName.ToString() + TEXT("|") + (Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output")) + TEXT("|") + TypeRecord(Pin->PinType) + TEXT("|") + Pin->GetDefaultAsString());
                if (Json) { TSharedPtr<FJsonObject> Item = PinJson(Pin); Item->SetStringField(TEXT("tunnel_role"), Role); Json->Add(MakeShared<FJsonValueObject>(Item)); }
            }
        }
        Records.Sort();
        if (Json) Json->Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B) { FString AN, BN, AR, BR; A->AsObject()->TryGetStringField(TEXT("name"), AN); B->AsObject()->TryGetStringField(TEXT("name"), BN); A->AsObject()->TryGetStringField(TEXT("tunnel_role"), AR); B->AsObject()->TryGetStringField(TEXT("tunnel_role"), BR); return AR + AN < BR + BN; });
    }

    static FString Hash(const FString& Text)
    {
        FTCHARToUTF8 Utf8(*Text); uint8 Digest[FSHA1::DigestSize]; FSHA1::HashBuffer(Utf8.Get(), Utf8.Length(), Digest); return BytesToHex(Digest, FSHA1::DigestSize);
    }

    static UBlueprint* Owner(const UEdGraph* Graph) { return Graph ? Graph->GetTypedOuter<UBlueprint>() : nullptr; }
}

FString FN2CMacroReference::SignatureHash(const UEdGraph* MacroGraph)
{
    TArray<FString> Records; N2CMacroReference_Private::BuildTunnelRecords(MacroGraph, Records, nullptr); return N2CMacroReference_Private::Hash(FString::Join(Records, TEXT("\n")));
}

void FN2CMacroReference::AppendIdentity(const TSharedPtr<FJsonObject>& Object, const UEdGraph* MacroGraph, const UK2Node_MacroInstance* Instance)
{
    using namespace N2CMacroReference_Private; if (!Object.IsValid() || !MacroGraph) return; UBlueprint* Blueprint = Owner(MacroGraph); const FString OwnerPath = Blueprint ? Blueprint->GetPathName() : FString(); const FString Package = Blueprint && Blueprint->GetOutermost() ? Blueprint->GetOutermost()->GetName() : FString();
    Object->SetStringField(TEXT("macro_owner_path"), OwnerPath); Object->SetStringField(TEXT("macro_blueprint_path"), OwnerPath); Object->SetStringField(TEXT("macro_graph_path"), MacroGraph->GetPathName()); Object->SetStringField(TEXT("macro_name"), MacroGraph->GetName()); Object->SetStringField(TEXT("macro_graph_guid"), MacroGraph->GraphGuid.ToString(EGuidFormats::DigitsWithHyphens)); Object->SetStringField(TEXT("macro_owner_package"), Package); Object->SetStringField(TEXT("macro_dependency_origin"), Package.StartsWith(TEXT("/Engine/")) ? TEXT("engine") : Package.StartsWith(TEXT("/Game/")) ? TEXT("project") : TEXT("plugin")); Object->SetStringField(TEXT("macro_graph_kind"), Blueprint && Blueprint->MacroGraphs.Contains(const_cast<UEdGraph*>(MacroGraph)) ? TEXT("macro") : TEXT("unsupported")); Object->SetStringField(TEXT("macro_owner_type"), Blueprint ? (Blueprint->BlueprintType == BPTYPE_MacroLibrary ? TEXT("macro_library") : Blueprint->BlueprintType == BPTYPE_Normal ? TEXT("blueprint") : TEXT("unsupported")) : TEXT("missing")); Object->SetStringField(TEXT("macro_signature_hash"), SignatureHash(MacroGraph));
    TArray<FString> Records; TArray<TSharedPtr<FJsonValue>> Tunnel; BuildTunnelRecords(MacroGraph, Records, &Tunnel); Object->SetArrayField(TEXT("tunnel_signature"), Tunnel);
    TArray<TSharedPtr<FJsonValue>> Contract, Wildcards; if (Instance) { Object->SetObjectField(TEXT("resolved_wildcard_type"), TypeJson(Instance->ResolvedWildcardType)); for (const UEdGraphPin* Pin : Instance->Pins) { if (!Pin) continue; Contract.Add(MakeShared<FJsonValueObject>(PinJson(Pin))); bool bSourceWildcard = false; for (const UEdGraphNode* Node : MacroGraph->Nodes) if (const UK2Node_Tunnel* TunnelNode = Cast<UK2Node_Tunnel>(Node)) for (const UEdGraphPin* Source : TunnelNode->Pins) if (Source && Source->PinName == Pin->PinName && Source->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard) bSourceWildcard = true; if (bSourceWildcard) Wildcards.Add(MakeShared<FJsonValueObject>(PinJson(Pin))); } }
    Object->SetArrayField(TEXT("instance_pin_contract"), Contract); Object->SetArrayField(TEXT("wildcard_pin_types"), Wildcards);
}

bool FN2CMacroReference::ResolveAndValidate(const TSharedPtr<FJsonObject>& NodeObject, UBlueprint* TargetBlueprint, UEdGraph*& OutMacroGraph, FString& OutErrorCode, FString& OutDetail)
{
    using namespace N2CMacroReference_Private; OutMacroGraph = nullptr; OutErrorCode.Empty(); OutDetail.Empty();
    const FString OwnerPath = GetString(NodeObject, TEXT("macro_owner_path")); const FString GraphPath = GetString(NodeObject, TEXT("macro_graph_path")); const FString Name = GetString(NodeObject, TEXT("macro_name")); const FString ExpectedHash = GetString(NodeObject, TEXT("macro_signature_hash"));
    if (OwnerPath.IsEmpty()) { OutErrorCode = TEXT("macro_owner_missing"); OutDetail = TEXT("macro_owner_path is empty"); return false; }
    UBlueprint* OwnerBlueprint = LoadObject<UBlueprint>(nullptr, *OwnerPath); if (!OwnerBlueprint) { OutErrorCode = TEXT("macro_owner_missing"); OutDetail = OwnerPath; return false; }
    if (OwnerBlueprint->BlueprintType != BPTYPE_MacroLibrary && OwnerBlueprint->BlueprintType != BPTYPE_Normal) { OutErrorCode = TEXT("macro_reference_unsupported"); OutDetail = TEXT("wrong owner type"); return false; }
    if (OwnerBlueprint->BlueprintType == BPTYPE_Normal && OwnerBlueprint != TargetBlueprint) { OutErrorCode = TEXT("macro_reference_unsupported"); OutDetail = TEXT("UE4.27 forbids cross-Blueprint macro references unless the owner is a Blueprint Macro Library"); return false; }
    if (GraphPath.IsEmpty()) { OutErrorCode = TEXT("macro_graph_missing"); OutDetail = TEXT("macro_graph_path is empty"); return false; }
    for (UEdGraph* Candidate : OwnerBlueprint->MacroGraphs) if (Candidate && Candidate->GetPathName() == GraphPath) { OutMacroGraph = Candidate; break; }
    if (!OutMacroGraph || OutMacroGraph->GetTypedOuter<UBlueprint>() != OwnerBlueprint || (!Name.IsEmpty() && OutMacroGraph->GetName() != Name)) { OutMacroGraph = nullptr; OutErrorCode = TEXT("macro_graph_missing"); OutDetail = GraphPath; return false; }
    const FString Kind = GetString(NodeObject, TEXT("macro_graph_kind")); if (Kind != TEXT("macro")) { OutMacroGraph = nullptr; OutErrorCode = TEXT("macro_reference_unsupported"); OutDetail = TEXT("macro_graph_kind must be macro"); return false; }
    const FString Guid = GetString(NodeObject, TEXT("macro_graph_guid")); if (!Guid.IsEmpty() && Guid != OutMacroGraph->GraphGuid.ToString(EGuidFormats::DigitsWithHyphens)) { OutMacroGraph = nullptr; OutErrorCode = TEXT("macro_graph_missing"); OutDetail = TEXT("macro graph GUID mismatch"); return false; }
    if (ExpectedHash.IsEmpty() || ExpectedHash != SignatureHash(OutMacroGraph)) { OutMacroGraph = nullptr; OutErrorCode = TEXT("macro_signature_mismatch"); OutDetail = TEXT("normalized tunnel signature hash mismatch"); return false; }
    const TArray<TSharedPtr<FJsonValue>>* Tunnel = nullptr; const TArray<TSharedPtr<FJsonValue>>* Pins = nullptr; const TArray<TSharedPtr<FJsonValue>>* Wildcards = nullptr; if (!NodeObject->TryGetArrayField(TEXT("tunnel_signature"), Tunnel) || !Tunnel || !NodeObject->TryGetArrayField(TEXT("instance_pin_contract"), Pins) || !Pins || !NodeObject->TryGetArrayField(TEXT("wildcard_pin_types"), Wildcards) || !Wildcards) { OutMacroGraph = nullptr; OutErrorCode = TEXT("macro_signature_mismatch"); OutDetail = TEXT("macro pin/signature contract missing"); return false; }
    return true;
}
