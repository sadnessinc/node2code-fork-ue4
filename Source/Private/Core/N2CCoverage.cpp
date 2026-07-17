// Copyright (c) 2026. Coverage classification shared by export, apply and verification.

#include "Core/N2CCoverage.h"
#include "Core/N2CMacroReference.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/DataTable.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/SecureHash.h"
#include "K2Node_BaseMCDelegate.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CastByteToEnum.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_CreateDelegate.h"
#include "K2Node_EnumEquality.h"
#include "K2Node_EnumLiteral.h"
#include "K2Node_ForEachElementInEnum.h"
#include "K2Node_GetDataTableRow.h"
#include "K2Node_GetEnumeratorName.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_InputAction.h"
#include "K2Node_InputAxisEvent.h"
#include "K2Node_InputAxisKeyEvent.h"
#include "K2Node_InputKey.h"
#include "K2Node_Composite.h"
#include "K2Node_Tunnel.h"
#include "K2Node_MakeArray.h"
#include "K2Node_StructOperation.h"
#include "K2Node_SwitchEnum.h"
#include "K2Node_CallFunctionOnMember.h"
#include "K2Node_Event.h"
#include "K2Node_GetArrayItem.h"
#include "K2Node_Variable.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/UObjectHash.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace N2CCoverage_Private
{
    static const TCHAR* CoverageSchema = TEXT("N2C_COVERAGE_V1");

    static FString GetString(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field)
    {
        FString Value;
        return Obj.IsValid() && Obj->TryGetStringField(Field, Value) ? Value : FString();
    }

    static bool HasField(const TSharedPtr<FJsonObject>& Obj, const FString& Name)
    {
        return Obj.IsValid() && Obj->HasField(Name);
    }

    static bool HasAnyField(const TSharedPtr<FJsonObject>& Obj, const TArray<FString>& Names)
    {
        for (const FString& Name : Names)
        {
            if (HasField(Obj, Name) && (!GetString(Obj, *Name).IsEmpty() || Obj->HasTypedField<EJson::Array>(Name) || Obj->HasTypedField<EJson::Object>(Name) || Obj->HasTypedField<EJson::Boolean>(Name) || Obj->HasTypedField<EJson::Number>(Name)))
            {
                return true;
            }
        }
        return false;
    }


    static bool IsArrayAlias(const FString& Lower)
    {
        return Lower == TEXT("arrayget") || Lower == TEXT("arrayadd") ||
            Lower == TEXT("arrayaddunique") || Lower == TEXT("arrayset") ||
            Lower == TEXT("arrayremove") || Lower == TEXT("arrayremoveitem") ||
            Lower == TEXT("arraycontains") || Lower == TEXT("arrayfind") ||
            Lower == TEXT("arraylength") || Lower == TEXT("arrayclear");
    }

    static bool IsDelayAlias(const FString& Lower)
    {
        return Lower == TEXT("delay") || Lower == TEXT("latent_delay");
    }

    static void SetKnownSupport(FN2CCoverageIssue& Out, const TCHAR* Handler, const TCHAR* Reason)
    {
        Out.Status = TEXT("supported_untested");
        Out.ConstructorHandler = Handler;
        Out.Reason = Reason;
        Out.VerificationGap = TEXT("reopen_not_verified");
        Out.LossKind = TEXT("none");
        Out.ExpectedLoss.Empty();
    }

    static void SetVerified(FN2CCoverageIssue& Out, const TCHAR* Handler, const TCHAR* Fixture, const TCHAR* Reason)
    {
        Out.Status = TEXT("verified");
        Out.ConstructorHandler = Handler;
        Out.VerificationFixture = Fixture;
        Out.bReopenVerified = true;
        Out.Reason = Reason;
        Out.VerificationGap.Empty();
        Out.LossKind = TEXT("none");
        Out.ExpectedLoss.Empty();
    }

    static void SetGuarded(FN2CCoverageIssue& Out, const TCHAR* Handler, const TCHAR* Reason, const TArray<FString>& Required, const TArray<FString>& Missing)
    {
        Out.Status = TEXT("guarded");
        Out.ConstructorHandler = Handler;
        Out.Reason = Reason;
        Out.RequiredMetadata = Required;
        Out.MissingMetadata = Missing;
        Out.ExpectedLoss = TEXT("rejected before mutation when required metadata is absent or unresolved");
        Out.LossKind = TEXT("runtime_semantic");
    }

    static void Require(FN2CCoverageIssue& Out, const TCHAR* Handler, const TCHAR* Reason, const TArray<FString>& Required, const TSharedPtr<FJsonObject>& Node)
    {
        TArray<FString> Missing;
        for (const FString& Field : Required)
        {
            if (!HasAnyField(Node, { Field }))
            {
                Missing.Add(Field);
            }
        }
        if (Missing.Num() > 0)
        {
            SetGuarded(Out, Handler, Reason, Required, Missing);
        }
        else
        {
            SetKnownSupport(Out, Handler, TEXT("required metadata is captured; no matching reopen fixture"));
            Out.RequiredMetadata = Required;
        }
    }

    static TArray<TSharedPtr<FJsonValue>> StringArray(const TArray<FString>& Values)
    {
        TArray<TSharedPtr<FJsonValue>> Result;
        for (const FString& Value : Values)
        {
            Result.Add(MakeShared<FJsonValueString>(Value));
        }
        return Result;
    }

    static void AddStatusCounts(TSharedPtr<FJsonObject>& Obj, const TMap<FString, int32>& Counts)
    {
        static const TCHAR* Statuses[] = { TEXT("verified"), TEXT("supported_untested"), TEXT("guarded"), TEXT("partial"), TEXT("unsupported"), TEXT("cosmetic_only"), TEXT("dependency_only") };
        TSharedPtr<FJsonObject> StatusCounts = MakeShared<FJsonObject>();
        for (const TCHAR* Status : Statuses)
        {
            StatusCounts->SetNumberField(Status, Counts.FindRef(Status));
        }
        Obj->SetObjectField(TEXT("status_counts"), StatusCounts);
    }

    static FString HashSource(const FString& Source)
    {
        FTCHARToUTF8 Utf8(*Source);
        uint8 Digest[FSHA1::DigestSize];
        FSHA1::HashBuffer(Utf8.Get(), static_cast<uint64>(Utf8.Length()), Digest);
        return BytesToHex(Digest, UE_ARRAY_COUNT(Digest));
    }

    static bool IsFunctionAction(const FString& Type)
    {
        return Type == TEXT("add_or_replace_function") || Type == TEXT("replace_function_body");
    }

    static TSharedPtr<FJsonObject> PinTypeObject(const FEdGraphPinType& PinType)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("category"), PinType.PinCategory.ToString());
        Obj->SetStringField(TEXT("subcategory"), PinType.PinSubCategory.ToString());
        Obj->SetNumberField(TEXT("container_type"), static_cast<int32>(PinType.ContainerType));
        Obj->SetBoolField(TEXT("is_reference"), PinType.bIsReference);
        Obj->SetBoolField(TEXT("is_const"), PinType.bIsConst);
        Obj->SetStringField(TEXT("subtype_path"), PinType.PinSubCategoryObject.IsValid() ? PinType.PinSubCategoryObject->GetPathName() : TEXT(""));
        return Obj;
    }

    static FString GraphKind(const UBlueprint* Blueprint, const UEdGraph* Graph)
    {
        if (!Blueprint || !Graph) return TEXT("unknown");
        UEdGraph* Mutable = const_cast<UEdGraph*>(Graph);
        if (Blueprint->UbergraphPages.Contains(Mutable)) return Graph->GetFName() == UEdGraphSchema_K2::FN_UserConstructionScript ? TEXT("construction_script") : TEXT("event_graph");
        if (Blueprint->FunctionGraphs.Contains(Mutable)) return TEXT("function");
        if (Blueprint->MacroGraphs.Contains(Mutable)) return TEXT("macro");
        if (Blueprint->DelegateSignatureGraphs.Contains(Mutable)) return TEXT("delegate_signature");
        if (Graph->GetOuter() && Graph->GetOuter()->IsA<UK2Node_Composite>()) return TEXT("collapsed_graph");
        return TEXT("extra_graph");
    }

    static FString GraphIdentity(const UBlueprint* Blueprint, const UEdGraph* Graph)
    {
        return FString::Printf(TEXT("%s|%s|%s|%s"), Blueprint ? *Blueprint->GetPathName() : TEXT(""), *GraphKind(Blueprint, Graph), Graph ? *Graph->GetName() : TEXT(""), Graph ? *Graph->GetPathName() : TEXT(""));
    }

    static FString TunnelRole(const UK2Node_Tunnel* Tunnel)
    {
        if (!Tunnel) return FString();
        if (Tunnel->bCanHaveOutputs && !Tunnel->bCanHaveInputs) return TEXT("entry");
        if (Tunnel->bCanHaveInputs && !Tunnel->bCanHaveOutputs) return TEXT("exit");
        return TEXT("unsupported");
    }
}

FString FN2CCoverageClassifier::BuildFunctionBoundaryFingerprint(const UBlueprint* Blueprint, const UEdGraph* Graph, const UEdGraphNode* Node)
{
    if (!Graph || !Node || (!Node->IsA<UK2Node_FunctionEntry>() && !Node->IsA<UK2Node_FunctionResult>()))
    {
        return FString();
    }

    FString Canonical;
    Canonical += FString::Printf(TEXT("asset=%s\ngraph=%s\nschema=%s\nrole=%s\n"),
        Blueprint ? *Blueprint->GetPathName() : TEXT(""), *Graph->GetPathName(),
        Graph->GetSchema() ? *Graph->GetSchema()->GetClass()->GetPathName() : TEXT(""),
        Node->IsA<UK2Node_FunctionEntry>() ? TEXT("entry") : TEXT("result"));

    int32 FunctionFlags = 0;
    int32 ResultCount = 0;
    for (const UEdGraphNode* GraphNode : Graph->Nodes)
    {
        if (const UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(GraphNode))
        {
            FunctionFlags = Entry->GetExtraFlags();
        }
        if (GraphNode && GraphNode->IsA<UK2Node_FunctionResult>())
        {
            ++ResultCount;
        }
    }
    Canonical += FString::Printf(TEXT("function_flags=%d\npure=%d\nresult_count=%d\n"),
        FunctionFlags, (FunctionFlags & FUNC_BlueprintPure) != 0 ? 1 : 0, ResultCount);

    int32 PinIndex = 0;
    for (const UEdGraphPin* Pin : Node->Pins)
    {
        if (!Pin)
        {
            continue;
        }
        const UObject* SubType = Pin->PinType.PinSubCategoryObject.Get();
        const UObject* DefaultObject = Pin->DefaultObject;
        Canonical += FString::Printf(
            TEXT("pin[%d]=%d|%s|%s|%s|%d|%s|%d|%d|%s|%s|%s\n"), PinIndex++,
            static_cast<int32>(Pin->Direction), *Pin->PinName.ToString(),
            *Pin->PinType.PinCategory.ToString(), *Pin->PinType.PinSubCategory.ToString(),
            static_cast<int32>(Pin->PinType.ContainerType), SubType ? *SubType->GetPathName() : TEXT(""),
            Pin->PinType.bIsReference ? 1 : 0, Pin->PinType.bIsConst ? 1 : 0,
            *Pin->DefaultValue, DefaultObject ? *DefaultObject->GetPathName() : TEXT(""), *Pin->DefaultTextValue.ToString());

        TArray<FString> Links;
        for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
        {
            const UEdGraphNode* LinkedNode = LinkedPin ? LinkedPin->GetOwningNode() : nullptr;
            if (LinkedPin && LinkedNode)
            {
                Links.Add(FString::Printf(TEXT("%s|%s|%d"),
                    LinkedNode->GetClass() ? *LinkedNode->GetClass()->GetPathName() : TEXT(""),
                    *LinkedPin->PinName.ToString(), static_cast<int32>(LinkedPin->Direction)));
            }
        }
        Links.Sort();
        for (const FString& Link : Links)
        {
            Canonical += FString::Printf(TEXT("link=%s\n"), *Link);
        }
    }

    return N2CCoverage_Private::HashSource(Canonical);
}

FString FN2CCoverageClassifier::BuildGraphBoundaryFingerprint(const UBlueprint* Blueprint, const UEdGraph* Graph, const UEdGraphNode* Node)
{
    using namespace N2CCoverage_Private;
    const UK2Node_Tunnel* Tunnel = Cast<UK2Node_Tunnel>(Node);
    if (!Blueprint || !Graph || !Tunnel || (Node->GetClass() != UK2Node_Tunnel::StaticClass() && !Node->IsA<UK2Node_Composite>())) return FString();
    FString Canonical = FString::Printf(TEXT("class=%s\nowner=%s\ngraph=%s\nkind=%s\nrole=%s\ninputs=%d\noutputs=%d\neditable=%d\n"),
        Node->GetClass() ? *Node->GetClass()->GetPathName() : TEXT(""), *Blueprint->GetPathName(), *GraphIdentity(Blueprint, Graph), *GraphKind(Blueprint, Graph), *TunnelRole(Tunnel),
        Tunnel->bCanHaveInputs ? 1 : 0, Tunnel->bCanHaveOutputs ? 1 : 0, Tunnel->IsEditable() ? 1 : 0);
    for (int32 Index = 0; Index < Node->Pins.Num(); ++Index)
    {
        const UEdGraphPin* Pin = Node->Pins[Index]; if (!Pin) continue;
        const UObject* SubType = Pin->PinType.PinSubCategoryObject.Get();
        Canonical += FString::Printf(TEXT("pin[%d]=%d|%s|%s|%s|%d|%s|%d|%d|%s|%s|%s\n"), Index, static_cast<int32>(Pin->Direction), *Pin->PinName.ToString(), *Pin->PinType.PinCategory.ToString(), *Pin->PinType.PinSubCategory.ToString(), static_cast<int32>(Pin->PinType.ContainerType), SubType ? *SubType->GetPathName() : TEXT(""), Pin->PinType.bIsReference ? 1 : 0, Pin->PinType.bIsConst ? 1 : 0, *Pin->DefaultValue, Pin->DefaultObject ? *Pin->DefaultObject->GetPathName() : TEXT(""), *Pin->DefaultTextValue.ToString());
        TArray<FString> Links;
        for (const UEdGraphPin* Linked : Pin->LinkedTo) if (Linked && Linked->GetOwningNode()) Links.Add(FString::Printf(TEXT("%s|%s|%d"), *Linked->GetOwningNode()->GetClass()->GetPathName(), *Linked->PinName.ToString(), static_cast<int32>(Linked->Direction)));
        Links.Sort(); for (const FString& Link : Links) Canonical += TEXT("link=") + Link + TEXT("\n");
    }
    if (const UK2Node_Composite* Composite = Cast<UK2Node_Composite>(Node))
    {
        const UEdGraph* Bound = Composite->BoundGraph;
        const UK2Node_Tunnel* Entry = Composite->GetEntryNode(); const UK2Node_Tunnel* Exit = Composite->GetExitNode();
        Canonical += FString::Printf(TEXT("bound=%s\nentry=%s\nexit=%s\n"), *GraphIdentity(Blueprint, Bound), Entry ? *Entry->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens) : TEXT(""), Exit ? *Exit->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens) : TEXT(""));
        for (const UEdGraphPin* OuterPin : Composite->Pins) if (OuterPin) Canonical += FString::Printf(TEXT("map=%d|%s|%s\n"), static_cast<int32>(OuterPin->Direction), *OuterPin->PinName.ToString(), OuterPin->Direction == EGPD_Input ? TEXT("entry") : TEXT("exit"));
    }
    return HashSource(Canonical);
}

void FN2CCoverageClassifier::ClassifyPatchNode(const TSharedPtr<FJsonObject>& NodeObj, bool bFunctionGraph, FN2CCoverageIssue& Out)
{
    using namespace N2CCoverage_Private;
    Out = FN2CCoverageIssue();
    Out.NodeClass = GetString(NodeObj, TEXT("type"));
    if (Out.NodeClass.IsEmpty()) { Out.NodeClass = GetString(NodeObj, TEXT("class")); }
    Out.Variant = Out.NodeClass;
    Out.Status = TEXT("unsupported");
    Out.ConstructorHandler = TEXT("none");
    Out.Reason = TEXT("no CreatePatchNode branch or specialised constructor helper");
    Out.ExpectedLoss = TEXT("runtime semantics cannot be reconstructed safely");
    Out.LossKind = TEXT("runtime_semantic");

    const FString Lower = Out.NodeClass.ToLower();

    // Keep coverage preflight aligned with CreatePatchNode's stable shorthand
    // aliases. These aliases resolve to fixed Kismet library functions and do
    // not need caller-supplied function_path metadata.
    if (IsArrayAlias(Lower))
    {
        SetVerified(Out, TEXT("CreateCallArrayFunctionNode(alias)"),
            TEXT("NodeToCode.Verification.P0Core.FlowArraysOperators"),
            TEXT("stable array alias maps to a fixed KismetArrayLibrary function"));
        Out.Variant = FString::Printf(TEXT("%s:alias"), *Out.NodeClass);
        return;
    }
    if (IsDelayAlias(Lower))
    {
        SetVerified(Out, TEXT("CreateCallFunctionNode(Delay alias)"),
            TEXT("NodeToCode.Verification.P2.Delay.EventGraph"),
            TEXT("stable Delay alias maps to KismetSystemLibrary::Delay"));
        Out.Variant = FString::Printf(TEXT("%s:alias"), *Out.NodeClass);
        return;
    }

    if (Lower.Contains(TEXT("comment")))
    {
        Out.Status = TEXT("cosmetic_only"); Out.ConstructorHandler = TEXT("not imported (comment/layout policy)");
        Out.Reason = TEXT("comment nodes do not change runtime graph semantics");
        Out.ExpectedLoss = TEXT("comment text, bubble state, size and placement"); Out.LossKind = TEXT("comment_layout_only"); return;
    }
    if (Lower.Contains(TEXT("k2node_knot")) || Lower == TEXT("knot") || Lower.Contains(TEXT("k2node_self")) || Lower == TEXT("self"))
    {
        Out.Status = TEXT("verified");
        Out.ConstructorHandler = Lower.Contains(TEXT("knot")) ? TEXT("CreatePatchNode -> FGraphNodeCreator<UK2Node_Knot>") : TEXT("CreateNativeK2NodeUnfinished(K2Node_Self)");
        Out.VerificationFixture = TEXT("P0_65_Connectivity_TEMPLATE.json"); Out.bReopenVerified = true;
        Out.Reason = TEXT("P0ConnectivityRuntime and P0ConnectivityReopen"); Out.LossKind = TEXT("none"); Out.ExpectedLoss.Empty(); return;
    }
    if (Lower.Contains(TEXT("getarrayitem")))
    {
        if (!HasField(NodeObj, TEXT("return_by_ref"))) { SetGuarded(Out, TEXT("CreateGetArrayItemNode"), TEXT("array item constructor requires return-by-reference metadata"), { TEXT("return_by_ref") }, { TEXT("return_by_ref") }); return; }
        bool bByRef = false; NodeObj->TryGetBoolField(TEXT("return_by_ref"), bByRef);
        Out.RequiredMetadata = { TEXT("return_by_ref") }; Out.Variant = FString::Printf(TEXT("%s:return_by_ref=%s"), *Out.NodeClass, bByRef ? TEXT("true") : TEXT("false"));
        if (!bByRef) { Out.Status = TEXT("verified"); Out.ConstructorHandler = TEXT("CreateGetArrayItemNode"); Out.VerificationFixture = TEXT("P0_65_Connectivity_TEMPLATE.json"); Out.bReopenVerified = true; Out.Reason = TEXT("P0ConnectivityRuntime and P0ConnectivityReopen cover return_by_ref=false"); Out.LossKind = TEXT("none"); Out.ExpectedLoss.Empty(); }
        else { SetVerified(Out, TEXT("CreateGetArrayItemNode"), TEXT("NodeToCode.Verification.P0Core.FlowArraysOperators"), TEXT("copy and reference variants covered by generated fresh-process fixture")); Out.RequiredMetadata = { TEXT("return_by_ref") }; }
        return;
    }
    if (Lower.Contains(TEXT("functionentry")) || Lower == TEXT("entry") || Lower.Contains(TEXT("functionresult")) || Lower == TEXT("return"))
    {
        Out.FunctionBoundaryRole = Lower.Contains(TEXT("entry")) ? TEXT("entry") : TEXT("result");
        Out.FunctionBoundaryFingerprint = GetString(NodeObj, TEXT("function_boundary_signature_fingerprint"));
        if (bFunctionGraph)
        {
            SetVerified(Out, Out.FunctionBoundaryRole == TEXT("entry") ? TEXT("FindOrCreateEntryNode") : TEXT("FindOrCreateResultNode/CreateAdditionalResultNode"),
                TEXT("NodeToCode.Verification.P0FunctionBoundaries.RepresentativeFreshProcess"),
                TEXT("impure, pure and multiple-result function boundaries persist through a fresh process"));
            Out.RequiredMetadata = { TEXT("graph_name"), TEXT("graph_type") };
        }
        else
        {
            Require(Out, Out.FunctionBoundaryRole == TEXT("entry") ? TEXT("FindOrCreateEntryNode") : TEXT("FindOrCreateResultNode/CreateAdditionalResultNode"),
                TEXT("code=function_boundary_identity_missing; live boundary accounting requires graph identity and a durable signature fingerprint"),
                { TEXT("graph_name"), TEXT("graph_type"), TEXT("function_boundary_signature_fingerprint") }, NodeObj);
            if (Out.MissingMetadata.Num() == 0)
            {
                SetVerified(Out, Out.FunctionBoundaryRole == TEXT("entry") ? TEXT("FindOrCreateEntryNode") : TEXT("FindOrCreateResultNode/CreateAdditionalResultNode"),
                    TEXT("NodeToCode.Verification.P0FunctionBoundaries.ProductionFingerprintCoverage"),
                    TEXT("record-level function boundary fingerprint is captured and representative fresh-process persistence is verified"));
                Out.RequiredMetadata = { TEXT("graph_name"), TEXT("graph_type"), TEXT("function_boundary_signature_fingerprint") };
                Out.PersistenceResult = TEXT("PASS");
                Out.Variant = FString::Printf(TEXT("%s:%s:%s"), *Out.NodeClass, *Out.FunctionBoundaryRole, *Out.FunctionBoundaryFingerprint);
            }
        }
        return;
    }
    if (Lower.Contains(TEXT("variableget")) || Lower.Contains(TEXT("variableset")))
    {
        if (!HasAnyField(NodeObj, { TEXT("member_name"), TEXT("variable_name") })) { SetGuarded(Out, TEXT("CreatePatchNode -> variable node"), TEXT("variable branch rejects an empty member name"), { TEXT("member_name") }, { TEXT("member_name") }); }
        else { SetVerified(Out, TEXT("CreatePatchNode -> variable node"), TEXT("NodeToCode.Verification.P0Core.Variables"), TEXT("generated fresh-process member variable identity persistence")); Out.RequiredMetadata = { TEXT("member_name") }; }
        return;
    }
    if (Lower.Contains(TEXT("callfunction")) || Lower.Contains(TEXT("callarrayfunction")) || Lower.Contains(TEXT("callparentfunction")))
    {
        TArray<FString> Required = { TEXT("function_path"), TEXT("function_owner_class") }; if (Lower.Contains(TEXT("onmember"))) { Required.Add(TEXT("member_variable_name")); }
        Require(Out, Lower.Contains(TEXT("array")) ? TEXT("CreateCallArrayFunctionNode") : TEXT("CreateCallFunctionNode/CreateCallFunctionSubclassNode"), TEXT("function constructor requires a resolved function identity"), Required, NodeObj);
        if (Out.MissingMetadata.Num() == 0) { SetVerified(Out, Lower.Contains(TEXT("array")) ? TEXT("CreateCallArrayFunctionNode") : TEXT("CreateCallFunctionNode/CreateCallFunctionSubclassNode"), TEXT("NodeToCode.Verification.P0Core.FunctionCalls / FlowArraysOperators"), TEXT("representative generated fresh-process function call persistence")); Out.RequiredMetadata = Required; }
        return;
    }
    if (Lower.Contains(TEXT("k2node_event")) || Lower == TEXT("event")) { Require(Out, TEXT("CreateBuiltinEventNode"), TEXT("event constructor requires event name, owner and override metadata"), { TEXT("event_name"), TEXT("event_owner_class"), TEXT("event_is_override") }, NodeObj); if (Out.MissingMetadata.Num()==0){SetVerified(Out,TEXT("CreateBuiltinEventNode"),TEXT("NodeToCode.Verification.P0Core.BuiltInEvents / WidgetEvents / AIEvents"),TEXT("representative generated fresh-process built-in event persistence"));Out.RequiredMetadata={TEXT("event_name"),TEXT("event_owner_class"),TEXT("event_is_override")};} return; }
    if (Lower.Contains(TEXT("macroinstance")) || Lower.Contains(TEXT("macro_instance")))
    {
        const TArray<FString> Required = { TEXT("macro_owner_path"), TEXT("macro_graph_path"), TEXT("macro_name"), TEXT("macro_graph_guid"), TEXT("macro_owner_package"), TEXT("macro_dependency_origin"), TEXT("macro_graph_kind"), TEXT("macro_owner_type"), TEXT("macro_signature_hash"), TEXT("tunnel_signature"), TEXT("instance_pin_contract"), TEXT("wildcard_pin_types") }; TArray<FString> Missing;
        for (const FString& Field : Required) { if (!HasAnyField(NodeObj, { Field })) { Missing.Add(Field); } }
        Out.ConstructorHandler = TEXT("CreateMacroInstanceNode"); Out.RequiredMetadata = Required;
        if (Missing.Num() > 0) { Out.Status = TEXT("partial"); Out.MissingMetadata = Missing; Out.Reason = TEXT("macro owner/graph identity or wildcard pin contract is not captured"); Out.ExpectedLoss = TEXT("macro identity or wildcard/container type may be reconstructed ambiguously"); Out.LossKind = TEXT("runtime_semantic"); }
        else { Out.Status = TEXT("verified"); Out.VerificationFixture = TEXT("NodeToCode.Verification.P0StandardMacros / P0ExternalMacros"); Out.bReopenVerified = true; Out.Reason = TEXT("durable macro identity and fresh-process persistence are verified"); Out.ExpectedLoss.Empty(); Out.LossKind = TEXT("none"); }
        return;
    }
    if (Lower.Contains(TEXT("makestruct")) || Lower.Contains(TEXT("breakstruct")) || Lower.Contains(TEXT("setfieldsinstruct")) || Lower.Contains(TEXT("setmembersinstruct")))
    {
        Require(Out, TEXT("CreateStructBackedNode/CreateSetFieldsInStructNode"), TEXT("code=struct_identity_missing; struct constructor requires durable struct and member-pin identity"), { TEXT("struct_path"), TEXT("member_pin_identity") }, NodeObj);
        if (Out.MissingMetadata.Num() == 0)
        {
            SetVerified(Out, TEXT("CreateStructBackedNode/CreateSetFieldsInStructNode"), Lower.Contains(TEXT("setfields")) || Lower.Contains(TEXT("setmembers")) ? TEXT("NodeToCode.Verification.P0Specialized.Struct.SetFields") : TEXT("NodeToCode.Verification.P0Specialized.Struct.MakeBreak"), TEXT("generated fresh-process struct identity and pin persistence"));
            Out.RequiredMetadata = { TEXT("struct_path"), TEXT("member_pin_identity") };
        }
        return;
    }
    if (Lower.Contains(TEXT("switchenum")) || Lower.Contains(TEXT("enumequality")) || Lower.Contains(TEXT("enuminequality")) ||
        Lower.Contains(TEXT("getenumeratornameasstring")) || Lower.Contains(TEXT("foreachelementinenum")) ||
        Lower.Contains(TEXT("castbytetoenum")) || Lower.Contains(TEXT("enumliteral")))
    {
        TArray<FString> Required = { TEXT("enum_path") };
        if (Lower.Contains(TEXT("switchenum"))) Required.Add(TEXT("enum_cases"));
        Require(Out, TEXT("enum specialised constructor"), TEXT("code=enum_identity_missing; enum constructor requires exact enum identity and switch cases where applicable"), Required, NodeObj);
        if (Out.MissingMetadata.Num() == 0)
        {
            SetVerified(Out, TEXT("enum specialised constructor"), Lower.Contains(TEXT("switchenum")) || Lower.Contains(TEXT("equality")) ? TEXT("NodeToCode.Verification.P0Specialized.Enum.CompareSwitch") : TEXT("NodeToCode.Verification.P0Specialized.Enum.UtilityNodes"), TEXT("generated fresh-process enum identity persistence"));
            Out.RequiredMetadata = Required;
        }
        return;
    }
    if (Lower.Contains(TEXT("getdatatablerow")))
    {
        bool bTableLinked = false;
        NodeObj->TryGetBoolField(TEXT("data_table_pin_linked"), bTableLinked);
        const TArray<FString> Required = bTableLinked
            ? TArray<FString>{ TEXT("data_table_pin_linked"), TEXT("row_struct_path") }
            : TArray<FString>{ TEXT("data_table_path"), TEXT("row_struct_path") };
        Require(Out, TEXT("CreateGetDataTableRowNode"), TEXT("code=datatable_identity_missing; typed DataTable constructor requires literal table identity or linked-pin row-struct identity"), Required, NodeObj);
        if (Out.MissingMetadata.Num() == 0)
        {
            SetVerified(Out, TEXT("CreateGetDataTableRowNode"), bTableLinked ? TEXT("NodeToCode.Verification.P0Specialized.DataTable.GetRowLinked") : TEXT("NodeToCode.Verification.P0Specialized.DataTable.GetRow"), TEXT("generated fresh-process DataTable and OutRow persistence"));
            Out.RequiredMetadata = Required;
            Out.Variant = FString::Printf(TEXT("%s:data_table=%s"), *Out.NodeClass, bTableLinked ? TEXT("linked") : TEXT("literal"));
        }
        return;
    }
    if (Lower.Contains(TEXT("timeline"))) { Require(Out, TEXT("CreateTimelineNode"), TEXT("timeline constructor requires template and track payload"), { TEXT("timeline_template"), TEXT("timeline_tracks") }, NodeObj); return; }
    if (Lower.Contains(TEXT("addcomponent"))) { Require(Out, TEXT("CreateAddComponentNode"), TEXT("component constructor requires component class and SCS identity"), { TEXT("component_class"), TEXT("scs_hierarchy") }, NodeObj); return; }
    if (Lower.Contains(TEXT("delegate")) || Lower.Contains(TEXT("componentboundevent")))
    {
        TArray<FString> Required = { TEXT("delegate_owner_class"), TEXT("delegate_property") };
        if (Lower.Contains(TEXT("componentboundevent"))) Required.Add(TEXT("component_property_name"));
        if (Lower.Contains(TEXT("createdelegate"))) Required = { TEXT("selected_function_name") };
        Require(Out, TEXT("delegate constructor"), TEXT("code=delegate_identity_mismatch; delegate constructor requires exact owner/property/function identity"), Required, NodeObj);
        if (Out.MissingMetadata.Num() == 0)
        {
            SetVerified(Out, TEXT("delegate constructor"), Lower.Contains(TEXT("componentboundevent")) ? TEXT("NodeToCode.Verification.P0Delegates.ComponentBoundEvent") : TEXT("NodeToCode.Verification.P0Delegates.CallBind / CreateAssign"), TEXT("generated fresh-process delegate identity persistence"));
            Out.RequiredMetadata = Required;
        }
        return;
    }
    if (Lower.Contains(TEXT("createwidget")))
    {
        bool bClassLinked = false;
        NodeObj->TryGetBoolField(TEXT("class_pin_linked"), bClassLinked);
        const TArray<FString> Required = bClassLinked
            ? TArray<FString>{ TEXT("class_pin_linked"), TEXT("result_class_path") }
            : TArray<FString>{ TEXT("class_path") };
        Require(Out, TEXT("CreateNativeK2NodeUnfinished(K2Node_CreateWidget)"), TEXT("code=create_widget_class_identity_missing; CreateWidget requires literal class identity or linked-class result identity"), Required, NodeObj);
        if (Out.MissingMetadata.Num() == 0) { SetVerified(Out, TEXT("CreateNativeK2NodeUnfinished(K2Node_CreateWidget)"), bClassLinked ? TEXT("NodeToCode.Verification.P0UIInput.CreateWidgetLinkedClass") : TEXT("NodeToCode.Verification.P0UIInput.CreateWidget"), TEXT("generated fresh-process widget class persistence")); Out.RequiredMetadata = Required; Out.Variant = FString::Printf(TEXT("%s:class=%s"), *Out.NodeClass, bClassLinked ? TEXT("linked") : TEXT("literal")); }
        return;
    }
    if (Lower.Contains(TEXT("inputaction")))
    {
        Require(Out, TEXT("K2Node_InputAction"), TEXT("code=input_action_identity_missing; input action requires mapping identity and persisted flags"), { TEXT("input_action_name"), TEXT("consume_input"), TEXT("execute_when_paused"), TEXT("override_parent_binding") }, NodeObj);
        if (Out.MissingMetadata.Num() == 0) { SetVerified(Out, TEXT("K2Node_InputAction"), TEXT("NodeToCode.Verification.P0UIInput.InputActionGraphNode"), TEXT("generated fresh-process UK2Node_InputAction persistence")); }
        return;
    }
    if (Lower.Contains(TEXT("inputaxis")))
    {
        const TArray<FString> Required = Lower.Contains(TEXT("key")) ? TArray<FString>{ TEXT("key_name"), TEXT("consume_input"), TEXT("execute_when_paused"), TEXT("override_parent_binding") } : TArray<FString>{ TEXT("input_axis_name"), TEXT("consume_input"), TEXT("execute_when_paused"), TEXT("override_parent_binding") };
        Require(Out, TEXT("K2Node_InputAxisEvent/InputAxisKeyEvent"), TEXT("axis event requires mapping/key identity and persisted flags"), Required, NodeObj);
        if (Out.MissingMetadata.Num() == 0) { SetVerified(Out, TEXT("K2Node_InputAxisEvent/InputAxisKeyEvent"), TEXT("NodeToCode.Verification.P0UIInput.InputActionAxis"), TEXT("generated fresh-process input axis persistence")); }
        return;
    }
    if (Lower.Contains(TEXT("inputkey")))
    {
        Require(Out, TEXT("K2Node_InputKey"), TEXT("code=input_key_identity_missing; key event requires key, modifier and persisted input flags"), { TEXT("key_name"), TEXT("shift"), TEXT("ctrl"), TEXT("alt"), TEXT("cmd"), TEXT("consume_input"), TEXT("execute_when_paused"), TEXT("override_parent_binding") }, NodeObj);
        if (Out.MissingMetadata.Num() == 0) { SetVerified(Out, TEXT("K2Node_InputKey"), TEXT("NodeToCode.Verification.P0UIInput.InputKeyGraphNode"), TEXT("generated fresh-process UK2Node_InputKey persistence")); }
        return;
    }
    if (Lower.Contains(TEXT("message")))
    {
        Require(Out, TEXT("CreateCallFunctionSubclassNode(K2Node_Message)"), TEXT("interface message requires exact function identity"), { TEXT("function_path"), TEXT("function_owner_class") }, NodeObj);
        if (Out.MissingMetadata.Num() == 0) { SetVerified(Out, TEXT("CreateCallFunctionSubclassNode(K2Node_Message)"), TEXT("NodeToCode.Verification.P0UIInput.InterfaceMessage"), TEXT("generated fresh-process interface message persistence")); }
        return;
    }
    if (Lower.Contains(TEXT("composite")))
    {
        const TArray<FString> Required = { TEXT("owner_blueprint_path"), TEXT("owning_graph_identity"), TEXT("owning_graph_kind"), TEXT("composite_node_identity"), TEXT("bound_graph_identity"), TEXT("bound_graph_kind"), TEXT("outer_pin_signature"), TEXT("entry_tunnel_identity"), TEXT("exit_tunnel_identity"), TEXT("outer_to_inner_pin_mapping"), TEXT("graph_boundary_fingerprint") };
        Require(Out, TEXT("CreateOrReplaceCollapsedGraph"), TEXT("code=composite_bound_graph_identity_missing; collapsed graph requires exact owner, BoundGraph and pin mapping identity"), Required, NodeObj);
        if (Out.MissingMetadata.Num() == 0) { SetVerified(Out, TEXT("CreateOrReplaceCollapsedGraph"), TEXT("NodeToCode.Verification.P0GraphBoundaries.Composite"), TEXT("collapsed graph owner, BoundGraph, tunnels and pin mapping persist through a fresh process")); Out.RequiredMetadata = Required; Out.GraphBoundaryFingerprint = GetString(NodeObj, TEXT("graph_boundary_fingerprint")); Out.OwningGraphIdentity = GetString(NodeObj, TEXT("owning_graph_identity")); Out.BoundGraphIdentity = GetString(NodeObj, TEXT("bound_graph_identity")); Out.PersistenceResult = TEXT("PASS"); }
        return;
    }
    if (Lower.Contains(TEXT("tunnel")))
    {
        const TArray<FString> Required = { TEXT("owner_blueprint_path"), TEXT("owning_graph_identity"), TEXT("owning_graph_kind"), TEXT("owning_graph_name"), TEXT("owning_graph_path"), TEXT("tunnel_role"), TEXT("can_have_inputs"), TEXT("can_have_outputs"), TEXT("editable"), TEXT("user_defined_pin_signature"), TEXT("graph_boundary_fingerprint") };
        Require(Out, TEXT("ResolveGraphBoundaryTunnel"), TEXT("code=graph_owner_identity_missing; tunnel requires exact graph identity, role and ordered pin signature"), Required, NodeObj);
        if (Out.MissingMetadata.Num() == 0 && GetString(NodeObj, TEXT("tunnel_role")) != TEXT("unsupported")) { SetVerified(Out, TEXT("ResolveGraphBoundaryTunnel"), TEXT("NodeToCode.Verification.P0GraphBoundaries.MacroTunnel / CollapsedGraphTunnel"), TEXT("graph-owned tunnel signature persists through a fresh process")); Out.RequiredMetadata = Required; Out.GraphBoundaryFingerprint = GetString(NodeObj, TEXT("graph_boundary_fingerprint")); Out.GraphBoundaryRole = GetString(NodeObj, TEXT("tunnel_role")); Out.OwningGraphIdentity = GetString(NodeObj, TEXT("owning_graph_identity")); Out.PersistenceResult = TEXT("PASS"); }
        else if (Out.MissingMetadata.Num() == 0) SetGuarded(Out, TEXT("ResolveGraphBoundaryTunnel"), TEXT("code=graph_boundary_variant_unsupported; tunnel is neither a distinct entry nor exit boundary"), Required, { TEXT("supported_tunnel_role") });
        return;
    }
    if (Lower.Contains(TEXT("temporaryvariable")) || Lower.Contains(TEXT("assignmentstatement")) || Lower.Contains(TEXT("persistentframe"))) { SetGuarded(Out, TEXT("graph action ownership path"), TEXT("internal graph node requires owning graph identity"), { TEXT("owning_graph_identity") }, { TEXT("owning_graph_identity") }); return; }
    if (Lower.Contains(TEXT("makearray"))) { Require(Out, TEXT("CreatePatchNode MakeArray"), TEXT("code=make_array_type_missing; typed MakeArray requires exported value_pin_type"), { TEXT("value_pin_type"), TEXT("input_count") }, NodeObj); if (Out.MissingMetadata.Num()==0){SetVerified(Out,TEXT("CreatePatchNode MakeArray"),TEXT("NodeToCode.Verification.P0Core.MakeArrayTyped"),TEXT("typed MakeArray container pins persist through a fresh process"));Out.RequiredMetadata={TEXT("value_pin_type"),TEXT("input_count")};} return; }
    if (Lower.Contains(TEXT("ifthenelse")) || Lower.Contains(TEXT("executionsequence")) || Lower.Contains(TEXT("select")) || Lower.Contains(TEXT("commutativeassociativebinaryoperator")) || Lower.Contains(TEXT("customevent")) || Lower.Contains(TEXT("spawnactorfromclass")) || Lower.Contains(TEXT("dynamiccast")))
    {
        SetVerified(Out, TEXT("CreatePatchNode direct constructor"), TEXT("NodeToCode.Verification.P0Core.FlowArraysOperators / CastsAndReferences / BuiltInEvents"), TEXT("representative generated fresh-process core constructor pack"));
        return;
    }
}

void FN2CCoverageClassifier::ClassifyLiveNode(const UBlueprint* Blueprint, const UEdGraph* Graph, const UEdGraphNode* Node, FN2CCoverageIssue& Out)
{
    using namespace N2CCoverage_Private;
    TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
    NodeObj->SetStringField(TEXT("class"), Node && Node->GetClass() ? Node->GetClass()->GetName() : TEXT(""));
    NodeObj->SetStringField(TEXT("graph_name"), Graph ? Graph->GetName() : TEXT(""));
    NodeObj->SetStringField(TEXT("graph_type"), Graph && Graph->GetSchema() ? Graph->GetSchema()->GetClass()->GetName() : TEXT(""));
    if (Node) { NodeObj->SetStringField(TEXT("node_guid"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens)); }
    if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
    {
        if (UFunction* Function = Call->GetTargetFunction()) { NodeObj->SetStringField(TEXT("function_path"), Function->GetPathName()); NodeObj->SetStringField(TEXT("function_owner_class"), Function->GetOwnerClass() ? Function->GetOwnerClass()->GetPathName() : TEXT("")); }
        if (const UK2Node_CallFunctionOnMember* Member = Cast<UK2Node_CallFunctionOnMember>(Node)) { NodeObj->SetStringField(TEXT("member_variable_name"), Member->MemberVariableToCallOn.GetMemberName().ToString()); }
    }
    else if (const UK2Node_Event* Event = Cast<UK2Node_Event>(Node)) { NodeObj->SetStringField(TEXT("event_name"), Event->EventReference.GetMemberName().ToString()); NodeObj->SetStringField(TEXT("event_owner_class"), Event->EventReference.GetMemberParentClass() ? Event->EventReference.GetMemberParentClass()->GetPathName() : TEXT("")); NodeObj->SetBoolField(TEXT("event_is_override"), Event->bOverrideFunction); }
    else if (const UK2Node_Variable* Variable = Cast<UK2Node_Variable>(Node)) { NodeObj->SetStringField(TEXT("member_name"), Variable->VariableReference.GetMemberName().ToString()); }
    if (const UK2Node_MacroInstance* Macro = Cast<UK2Node_MacroInstance>(Node)) { FN2CMacroReference::AppendIdentity(NodeObj, Macro->GetMacroGraph(), Macro); }
    if (const UK2Node_GetArrayItem* ArrayItem = Cast<UK2Node_GetArrayItem>(Node)) { const UEdGraphPin* Result = ArrayItem->GetResultPin(); NodeObj->SetBoolField(TEXT("return_by_ref"), Result && Result->PinType.bIsReference); }
    if (const UK2Node_StructOperation* StructNode = Cast<UK2Node_StructOperation>(Node))
    {
        NodeObj->SetStringField(TEXT("struct_path"), StructNode->StructType ? StructNode->StructType->GetPathName() : TEXT(""));
        TArray<TSharedPtr<FJsonValue>> Members;
        for (const UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct) continue;
            TSharedPtr<FJsonObject> Member = MakeShared<FJsonObject>(); Member->SetStringField(TEXT("pin_name"), Pin->PinName.ToString());
            Members.Add(MakeShared<FJsonValueObject>(Member));
        }
        NodeObj->SetArrayField(TEXT("member_pin_identity"), Members);
    }
    UEnum* EnumIdentity = nullptr;
    if (const UK2Node_SwitchEnum* Switch = Cast<UK2Node_SwitchEnum>(Node))
    {
        EnumIdentity = Switch->Enum; TArray<TSharedPtr<FJsonValue>> Cases; for (const FName Entry : Switch->EnumEntries) Cases.Add(MakeShared<FJsonValueString>(Entry.ToString())); NodeObj->SetArrayField(TEXT("enum_cases"), Cases);
    }
    else if (const UK2Node_EnumLiteral* Literal = Cast<UK2Node_EnumLiteral>(Node)) EnumIdentity = Literal->Enum;
    else if (const UK2Node_ForEachElementInEnum* ForEach = Cast<UK2Node_ForEachElementInEnum>(Node)) EnumIdentity = ForEach->Enum;
    else if (const UK2Node_CastByteToEnum* CastNode = Cast<UK2Node_CastByteToEnum>(Node)) EnumIdentity = CastNode->Enum;
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
        for (const UEdGraphPin* Pin : Node->Pins) if (Pin && Pin->Direction == EGPD_Input) { EnumIdentity = Cast<UEnum>(Pin->PinType.PinSubCategoryObject.Get()); if (EnumIdentity) break; }
    }
    if (EnumIdentity) NodeObj->SetStringField(TEXT("enum_path"), EnumIdentity->GetPathName());
    if (const UK2Node_GetDataTableRow* GetRow = Cast<UK2Node_GetDataTableRow>(Node))
    {
        const UEdGraphPin* TablePin = GetRow->GetDataTablePin();
        NodeObj->SetStringField(TEXT("data_table_path"), TablePin && TablePin->DefaultObject ? TablePin->DefaultObject->GetPathName() : TEXT(""));
        NodeObj->SetBoolField(TEXT("data_table_pin_linked"), TablePin && TablePin->LinkedTo.Num() > 0);
        NodeObj->SetStringField(TEXT("row_struct_path"), GetRow->GetDataTableRowStructType() ? GetRow->GetDataTableRowStructType()->GetPathName() : TEXT(""));
    }
    if (const UK2Node_BaseMCDelegate* Delegate = Cast<UK2Node_BaseMCDelegate>(Node))
    {
        const FProperty* Property = Delegate->GetProperty(); NodeObj->SetStringField(TEXT("delegate_property"), Delegate->GetPropertyName().ToString());
        NodeObj->SetStringField(TEXT("delegate_owner_class"), Property && Property->GetOwnerClass() ? Property->GetOwnerClass()->GetPathName() : TEXT(""));
    }
    if (const UK2Node_CreateDelegate* CreateDelegate = Cast<UK2Node_CreateDelegate>(Node)) NodeObj->SetStringField(TEXT("selected_function_name"), CreateDelegate->SelectedFunctionName.ToString());
    if (const UK2Node_ComponentBoundEvent* Bound = Cast<UK2Node_ComponentBoundEvent>(Node))
    {
        NodeObj->SetStringField(TEXT("component_property_name"), Bound->ComponentPropertyName.ToString()); NodeObj->SetStringField(TEXT("delegate_property"), Bound->DelegatePropertyName.ToString()); NodeObj->SetStringField(TEXT("delegate_owner_class"), Bound->DelegateOwnerClass ? Bound->DelegateOwnerClass->GetPathName() : TEXT(""));
    }
    if (const UK2Node_InputAction* ActionInput = Cast<UK2Node_InputAction>(Node))
    {
        NodeObj->SetStringField(TEXT("input_action_name"), ActionInput->InputActionName.ToString()); NodeObj->SetBoolField(TEXT("consume_input"), ActionInput->bConsumeInput); NodeObj->SetBoolField(TEXT("execute_when_paused"), ActionInput->bExecuteWhenPaused); NodeObj->SetBoolField(TEXT("override_parent_binding"), ActionInput->bOverrideParentBinding);
    }
    else if (const UK2Node_InputAxisEvent* AxisInput = Cast<UK2Node_InputAxisEvent>(Node))
    {
        NodeObj->SetStringField(TEXT("input_axis_name"), AxisInput->InputAxisName.ToString()); NodeObj->SetBoolField(TEXT("consume_input"), AxisInput->bConsumeInput); NodeObj->SetBoolField(TEXT("execute_when_paused"), AxisInput->bExecuteWhenPaused); NodeObj->SetBoolField(TEXT("override_parent_binding"), AxisInput->bOverrideParentBinding);
    }
    else if (const UK2Node_InputAxisKeyEvent* AxisKeyInput = Cast<UK2Node_InputAxisKeyEvent>(Node))
    {
        NodeObj->SetStringField(TEXT("key_name"), AxisKeyInput->AxisKey.GetFName().ToString()); NodeObj->SetBoolField(TEXT("consume_input"), AxisKeyInput->bConsumeInput); NodeObj->SetBoolField(TEXT("execute_when_paused"), AxisKeyInput->bExecuteWhenPaused); NodeObj->SetBoolField(TEXT("override_parent_binding"), AxisKeyInput->bOverrideParentBinding);
    }
    else if (const UK2Node_InputKey* KeyInput = Cast<UK2Node_InputKey>(Node))
    {
        NodeObj->SetStringField(TEXT("key_name"), KeyInput->InputKey.GetFName().ToString()); NodeObj->SetBoolField(TEXT("shift"), KeyInput->bShift); NodeObj->SetBoolField(TEXT("ctrl"), KeyInput->bControl); NodeObj->SetBoolField(TEXT("alt"), KeyInput->bAlt); NodeObj->SetBoolField(TEXT("cmd"), KeyInput->bCommand); NodeObj->SetBoolField(TEXT("consume_input"), KeyInput->bConsumeInput); NodeObj->SetBoolField(TEXT("execute_when_paused"), KeyInput->bExecuteWhenPaused); NodeObj->SetBoolField(TEXT("override_parent_binding"), KeyInput->bOverrideParentBinding);
    }
    const FString LiveLower = Node && Node->GetClass() ? Node->GetClass()->GetName().ToLower() : FString();
    if (LiveLower.Contains(TEXT("createwidget")))
    {
        const UEdGraphPin* ClassPin = Node->FindPin(TEXT("Class")); if (!ClassPin) ClassPin = Node->FindPin(TEXT("WidgetType"));
        const UEdGraphPin* ResultPin = Node->FindPin(TEXT("ReturnValue"));
        NodeObj->SetStringField(TEXT("class_path"), ClassPin && ClassPin->DefaultObject ? ClassPin->DefaultObject->GetPathName() : TEXT(""));
        NodeObj->SetBoolField(TEXT("class_pin_linked"), ClassPin && ClassPin->LinkedTo.Num() > 0);
        NodeObj->SetStringField(TEXT("result_class_path"), ResultPin && ResultPin->PinType.PinSubCategoryObject.IsValid() ? ResultPin->PinType.PinSubCategoryObject->GetPathName() : TEXT(""));
    }
    if (const UK2Node_MakeArray* MakeArray = Cast<UK2Node_MakeArray>(Node))
    {
        const UEdGraphPin* TypePin = nullptr;
        int32 InputCount = 0;
        for (const UEdGraphPin* Pin : Node->Pins) if (Pin && Pin->Direction == EGPD_Input) { ++InputCount; if (!TypePin) TypePin = Pin; }
        if (!TypePin) TypePin = MakeArray->GetOutputPin();
        if (TypePin) { FEdGraphPinType ElementType = TypePin->PinType; ElementType.ContainerType = EPinContainerType::None; NodeObj->SetObjectField(TEXT("value_pin_type"), PinTypeObject(ElementType)); }
        NodeObj->SetNumberField(TEXT("input_count"), InputCount);
    }
    if (Node && (Node->IsA<UK2Node_FunctionEntry>() || Node->IsA<UK2Node_FunctionResult>()))
    {
        NodeObj->SetStringField(TEXT("function_boundary_signature_fingerprint"), BuildFunctionBoundaryFingerprint(Blueprint, Graph, Node));
        NodeObj->SetStringField(TEXT("function_boundary_role"), Node->IsA<UK2Node_FunctionEntry>() ? TEXT("entry") : TEXT("result"));
    }
    const UK2Node_Tunnel* BoundaryTunnel = Cast<UK2Node_Tunnel>(Node);
    if (BoundaryTunnel && (Node->GetClass() == UK2Node_Tunnel::StaticClass() || Node->IsA<UK2Node_Composite>()))
    {
        const UK2Node_Tunnel* Tunnel = BoundaryTunnel;
        const FString Role = TunnelRole(Tunnel); const FString OwnerIdentity = GraphIdentity(Blueprint, Graph);
        NodeObj->SetStringField(TEXT("owner_blueprint_path"), Blueprint ? Blueprint->GetPathName() : TEXT("")); NodeObj->SetStringField(TEXT("owning_graph_identity"), OwnerIdentity); NodeObj->SetStringField(TEXT("owning_graph_kind"), GraphKind(Blueprint, Graph)); NodeObj->SetStringField(TEXT("owning_graph_name"), Graph ? Graph->GetName() : TEXT("")); NodeObj->SetStringField(TEXT("owning_graph_path"), Graph ? Graph->GetPathName() : TEXT("")); NodeObj->SetStringField(TEXT("tunnel_role"), Role); NodeObj->SetBoolField(TEXT("can_have_inputs"), Tunnel->bCanHaveInputs); NodeObj->SetBoolField(TEXT("can_have_outputs"), Tunnel->bCanHaveOutputs); NodeObj->SetBoolField(TEXT("editable"), Tunnel->IsEditable());
        TArray<TSharedPtr<FJsonValue>> Signature; for (const UEdGraphPin* Pin : Node->Pins) if (Pin) { TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>(); P->SetStringField(TEXT("name"), Pin->PinName.ToString()); P->SetNumberField(TEXT("direction"), static_cast<int32>(Pin->Direction)); P->SetObjectField(TEXT("pin_type"), PinTypeObject(Pin->PinType)); P->SetStringField(TEXT("default_value"), Pin->DefaultValue); P->SetStringField(TEXT("default_object"), Pin->DefaultObject ? Pin->DefaultObject->GetPathName() : TEXT("")); P->SetStringField(TEXT("default_text"), Pin->DefaultTextValue.ToString()); Signature.Add(MakeShared<FJsonValueObject>(P)); } NodeObj->SetArrayField(TEXT("user_defined_pin_signature"), Signature);
        NodeObj->SetStringField(TEXT("graph_boundary_fingerprint"), BuildGraphBoundaryFingerprint(Blueprint, Graph, Node));
        if (const UK2Node_Composite* Composite = Cast<UK2Node_Composite>(Node)) { const UEdGraph* Bound = Composite->BoundGraph; NodeObj->SetStringField(TEXT("composite_node_identity"), OwnerIdentity + TEXT("|") + Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens)); NodeObj->SetStringField(TEXT("bound_graph_identity"), GraphIdentity(Blueprint, Bound)); NodeObj->SetStringField(TEXT("bound_graph_kind"), GraphKind(Blueprint, Bound)); NodeObj->SetArrayField(TEXT("outer_pin_signature"), Signature); NodeObj->SetStringField(TEXT("entry_tunnel_identity"), Composite->GetEntryNode() ? GraphIdentity(Blueprint, Bound) + TEXT("|entry") : TEXT("")); NodeObj->SetStringField(TEXT("exit_tunnel_identity"), Composite->GetExitNode() ? GraphIdentity(Blueprint, Bound) + TEXT("|exit") : TEXT("")); TArray<TSharedPtr<FJsonValue>> Mapping; for (const UEdGraphPin* Pin : Composite->Pins) if (Pin) { TSharedPtr<FJsonObject> M=MakeShared<FJsonObject>(); M->SetStringField(TEXT("outer_pin"),Pin->PinName.ToString()); M->SetNumberField(TEXT("outer_direction"),static_cast<int32>(Pin->Direction)); M->SetStringField(TEXT("inner_role"),Pin->Direction==EGPD_Input?TEXT("entry"):TEXT("exit")); M->SetStringField(TEXT("inner_pin"),Pin->PinName.ToString()); Mapping.Add(MakeShared<FJsonValueObject>(M)); } NodeObj->SetArrayField(TEXT("outer_to_inner_pin_mapping"),Mapping); }
    }
    ClassifyPatchNode(NodeObj, false, Out);
    Out.AssetPath = Blueprint ? Blueprint->GetPathName() : FString(); Out.GraphPath = Graph ? Graph->GetPathName() : FString(); Out.NodeGuid = GetString(NodeObj, TEXT("node_guid"));
    if (!Out.FunctionBoundaryFingerprint.IsEmpty() && Out.Status == TEXT("verified"))
    {
        Out.VerificationFixture += TEXT(":") + Out.FunctionBoundaryFingerprint;
    }
}

bool FN2CCoverageClassifier::BlocksStrictApply(const FN2CCoverageIssue& Issue)
{
    return Issue.Status != TEXT("verified") && Issue.Status != TEXT("cosmetic_only") && Issue.Status != TEXT("dependency_only");
}

bool FN2CCoverageClassifier::AllowsApply(const FN2CCoverageIssue& Issue, bool bDeveloperOverride)
{
    if (!BlocksStrictApply(Issue)) { return true; }
    return bDeveloperOverride && Issue.Status == TEXT("supported_untested") && Issue.MissingMetadata.Num() == 0;
}

bool FN2CCoverageClassifier::PreflightPatch(const TSharedPtr<FJsonObject>& PatchRoot, bool bDeveloperOverride, FN2CPreflightResult& Out)
{
    using namespace N2CCoverage_Private;
    Out = FN2CPreflightResult();
    if (!PatchRoot.IsValid()) { return false; }
    const TArray<TSharedPtr<FJsonValue>>* Actions = nullptr;
    if (!PatchRoot->TryGetArrayField(TEXT("actions"), Actions) || !Actions) { return false; }
    for (const TSharedPtr<FJsonValue>& ActionValue : *Actions)
    {
        const TSharedPtr<FJsonObject> Action = ActionValue.IsValid() ? ActionValue->AsObject() : nullptr;
        if (!Action.IsValid()) { FN2CCoverageIssue Issue; Issue.NodeClass = TEXT("action:<invalid>"); Issue.Status = TEXT("unsupported"); Issue.Reason = TEXT("patch contains an invalid action object"); Issue.LossKind = TEXT("runtime_semantic"); Out.Issues.Add(Issue); continue; }
        const bool bFunctionGraph = IsFunctionAction(GetString(Action, TEXT("type")));
        const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
        if (!Action->TryGetArrayField(TEXT("nodes"), Nodes) || !Nodes) { continue; }
        for (const TSharedPtr<FJsonValue>& NodeValue : *Nodes)
        {
            FN2CCoverageIssue Issue; ClassifyPatchNode(NodeValue.IsValid() ? NodeValue->AsObject() : nullptr, bFunctionGraph, Issue); Out.Issues.Add(Issue);
        }
    }
    for (const FN2CCoverageIssue& Issue : Out.Issues)
    {
        if (Issue.Status == TEXT("cosmetic_only")) { Out.bHasCosmeticWarnings = true; ++Out.CosmeticWarningCount; }
        else if (Issue.Status == TEXT("supported_untested")) { Out.bHasVerificationGaps = true; ++Out.VerificationGapCount; }
        else if (BlocksStrictApply(Issue)) { Out.bHasRuntimeBlockers = true; ++Out.RuntimeBlockerCount; }
    }
    Out.bAllowed = !Out.bHasRuntimeBlockers && (!Out.bHasVerificationGaps || bDeveloperOverride);
    return Out.bAllowed;
}

bool FN2CCoverageClassifier::BuildBlueprintSidecar(const UBlueprint* Blueprint, const FString& SourceJson, FString& OutJson, FString& OutPrimaryReason)
{
    using namespace N2CCoverage_Private;
    OutJson.Empty(); OutPrimaryReason.Empty();
    if (!Blueprint) { OutPrimaryReason = TEXT("invalid Blueprint"); return false; }
    TArray<const UEdGraph*> Graphs; for (const UEdGraph* Graph : Blueprint->UbergraphPages) { Graphs.AddUnique(Graph); } for (const UEdGraph* Graph : Blueprint->FunctionGraphs) { Graphs.AddUnique(Graph); } for (const UEdGraph* Graph : Blueprint->MacroGraphs) { Graphs.AddUnique(Graph); } for (const UEdGraph* Graph : Blueprint->DelegateSignatureGraphs) { Graphs.AddUnique(Graph); } TArray<UObject*> Children; GetObjectsWithOuter(Blueprint, Children, true); for (UObject* Child : Children) { const UEdGraph* ChildGraph = Cast<UEdGraph>(Child); if (ChildGraph && ChildGraph->GetOuter() && ChildGraph->GetOuter()->IsA<UK2Node_Composite>()) Graphs.AddUnique(ChildGraph); }
    TArray<FN2CCoverageIssue> Issues; TMap<FString, int32> Counts; int32 RuntimeBlockers = 0, VerificationGaps = 0, CosmeticWarnings = 0;
    for (const UEdGraph* Graph : Graphs) { if (!Graph) { continue; } for (const UEdGraphNode* Node : Graph->Nodes) { if (!Node) { continue; } FN2CCoverageIssue Issue; ClassifyLiveNode(Blueprint, Graph, Node, Issue); Issues.Add(Issue); Counts.FindOrAdd(Issue.Status)++; if (Issue.Status == TEXT("supported_untested")) { ++VerificationGaps; } else if (Issue.Status == TEXT("cosmetic_only")) { ++CosmeticWarnings; } else if (BlocksStrictApply(Issue)) { ++RuntimeBlockers; } } }
    FString PluginVersion = TEXT("unknown"); const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("NodeToCode")); if (Plugin.IsValid()) { PluginVersion = FString::Printf(TEXT("%d / %s"), Plugin->GetDescriptor().Version, *Plugin->GetDescriptor().VersionName); }
    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>(); Root->SetStringField(TEXT("schema"), CoverageSchema); Root->SetNumberField(TEXT("coverage_version"), 1); Root->SetStringField(TEXT("plugin_version"), PluginVersion); Root->SetStringField(TEXT("engine_target"), TEXT("UE4.27.2")); Root->SetStringField(TEXT("source_schema"), TEXT("N2C_AI_EXPORT_V2")); Root->SetStringField(TEXT("asset_path"), Blueprint->GetPathName()); Root->SetStringField(TEXT("asset_origin"), Blueprint->GetPathName().StartsWith(TEXT("/Game/")) ? TEXT("game") : TEXT("dependency")); Root->SetStringField(TEXT("blueprint_parent_class"), Blueprint->ParentClass ? Blueprint->ParentClass->GetPathName() : TEXT("")); Root->SetNumberField(TEXT("blueprint_type"), static_cast<int32>(Blueprint->BlueprintType)); Root->SetStringField(TEXT("source_hash_algorithm"), TEXT("SHA-1")); Root->SetStringField(TEXT("source_hash"), HashSource(SourceJson));
    Root->SetBoolField(TEXT("export_capture_safe"), RuntimeBlockers == 0 && VerificationGaps == 0); Root->SetBoolField(TEXT("direct_import_supported"), false); Root->SetStringField(TEXT("direct_import_reason"), TEXT("normalizer_not_implemented")); Root->SetBoolField(TEXT("patch_apply_safe"), false); Root->SetBoolField(TEXT("round_trip_verified"), RuntimeBlockers == 0 && VerificationGaps == 0); Root->SetBoolField(TEXT("strict_apply_allowed"), false); AddStatusCounts(Root, Counts); Root->SetNumberField(TEXT("runtime_blocker_count"), RuntimeBlockers); Root->SetNumberField(TEXT("verification_gap_count"), VerificationGaps); Root->SetNumberField(TEXT("cosmetic_warning_count"), CosmeticWarnings);
    TArray<TSharedPtr<FJsonValue>> JsonIssues; for (const FN2CCoverageIssue& Issue : Issues) { JsonIssues.Add(MakeShared<FJsonValueObject>(ToJson(Issue))); if (OutPrimaryReason.IsEmpty() && (BlocksStrictApply(Issue) || Issue.Status == TEXT("supported_untested"))) { OutPrimaryReason = Issue.Reason; } } Root->SetArrayField(TEXT("issues"), JsonIssues);
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutJson); return FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
}

bool FN2CCoverageClassifier::BuildAggregateCoverage(const TArray<FString>& SidecarJsons, TSharedPtr<FJsonObject>& OutSummary)
{
    using namespace N2CCoverage_Private;
    OutSummary = MakeShared<FJsonObject>(); TMap<FString, int32> Totals; TArray<TSharedPtr<FJsonValue>> BlockedAssets; int32 Safe = 0, Warning = 0, Blocked = 0;
    for (const FString& Text : SidecarJsons)
    {
        TSharedPtr<FJsonObject> Sidecar; TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text); if (!FJsonSerializer::Deserialize(Reader, Sidecar) || !Sidecar.IsValid()) { ++Blocked; continue; }
        const int32 Runtime = static_cast<int32>(Sidecar->GetNumberField(TEXT("runtime_blocker_count"))); const int32 Gap = static_cast<int32>(Sidecar->GetNumberField(TEXT("verification_gap_count"))); const int32 Cosmetic = static_cast<int32>(Sidecar->GetNumberField(TEXT("cosmetic_warning_count")));
        const TSharedPtr<FJsonObject>* StatusCounts = nullptr; if (Sidecar->TryGetObjectField(TEXT("status_counts"), StatusCounts) && StatusCounts && StatusCounts->IsValid()) { for (const auto& Pair : (*StatusCounts)->Values) { Totals.FindOrAdd(Pair.Key) += static_cast<int32>(Pair.Value->AsNumber()); } }
        if (Runtime > 0) { ++Blocked; TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>(); Item->SetStringField(TEXT("asset_path"), GetString(Sidecar, TEXT("asset_path"))); Item->SetNumberField(TEXT("runtime_blocker_count"), Runtime); Item->SetNumberField(TEXT("verification_gap_count"), Gap); FString Reason; const TArray<TSharedPtr<FJsonValue>>* Issues = nullptr; if (Sidecar->TryGetArrayField(TEXT("issues"), Issues) && Issues && Issues->Num() > 0) { Reason = GetString((*Issues)[0]->AsObject(), TEXT("reason")); } Item->SetStringField(TEXT("primary_reason"), Reason); BlockedAssets.Add(MakeShared<FJsonValueObject>(Item)); }
        else if (Gap > 0 || Cosmetic > 0) { ++Warning; } else { ++Safe; }
    }
    OutSummary->SetStringField(TEXT("schema"), CoverageSchema); OutSummary->SetNumberField(TEXT("asset_count"), SidecarJsons.Num()); OutSummary->SetNumberField(TEXT("safe_asset_count"), Safe); OutSummary->SetNumberField(TEXT("warning_asset_count"), Warning); OutSummary->SetNumberField(TEXT("blocked_asset_count"), Blocked); AddStatusCounts(OutSummary, Totals); OutSummary->SetArrayField(TEXT("blocked_assets"), BlockedAssets); return true;
}

bool FN2CCoverageClassifier::ValidateSidecarForSource(const FString& SidecarJson, const FString& SourceJson, bool& bOutStale, FString& OutReason)
{
    using namespace N2CCoverage_Private;
    bOutStale = false; OutReason.Empty(); TSharedPtr<FJsonObject> Root; TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(SidecarJson);
    if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid() || GetString(Root, TEXT("schema")) != CoverageSchema) { bOutStale = true; OutReason = TEXT("sidecar schema is missing or unsupported"); return false; }
    if (static_cast<int32>(Root->GetNumberField(TEXT("coverage_version"))) != 1 || GetString(Root, TEXT("source_hash_algorithm")) != TEXT("SHA-1")) { bOutStale = true; OutReason = TEXT("sidecar version or hash algorithm is unsupported"); return false; }
    if (GetString(Root, TEXT("source_hash")) != HashSource(SourceJson)) { bOutStale = true; OutReason = TEXT("sidecar source_hash does not match the current source"); return false; }
    return true;
}

TSharedPtr<FJsonObject> FN2CCoverageClassifier::ToJson(const FN2CCoverageIssue& Issue)
{
    using namespace N2CCoverage_Private;
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>(); Obj->SetStringField(TEXT("asset_path"), Issue.AssetPath); Obj->SetStringField(TEXT("graph_path"), Issue.GraphPath); Obj->SetStringField(TEXT("node_guid"), Issue.NodeGuid); Obj->SetStringField(TEXT("node_class"), Issue.NodeClass); Obj->SetStringField(TEXT("variant"), Issue.Variant); Obj->SetStringField(TEXT("status"), Issue.Status); Obj->SetStringField(TEXT("constructor_handler"), Issue.ConstructorHandler); Obj->SetArrayField(TEXT("required_metadata"), StringArray(Issue.RequiredMetadata)); Obj->SetArrayField(TEXT("missing_metadata"), StringArray(Issue.MissingMetadata)); Obj->SetStringField(TEXT("verification_fixture"), Issue.VerificationFixture); Obj->SetStringField(TEXT("verification_gap"), Issue.VerificationGap); Obj->SetStringField(TEXT("function_boundary_signature_fingerprint"), Issue.FunctionBoundaryFingerprint); Obj->SetStringField(TEXT("function_boundary_role"), Issue.FunctionBoundaryRole); Obj->SetStringField(TEXT("graph_boundary_fingerprint"), Issue.GraphBoundaryFingerprint); Obj->SetStringField(TEXT("graph_boundary_role"), Issue.GraphBoundaryRole); Obj->SetStringField(TEXT("owning_graph_identity"), Issue.OwningGraphIdentity); Obj->SetStringField(TEXT("bound_graph_identity"), Issue.BoundGraphIdentity); Obj->SetStringField(TEXT("persistence_result"), Issue.PersistenceResult); Obj->SetBoolField(TEXT("reopen_verified"), Issue.bReopenVerified); Obj->SetStringField(TEXT("loss_kind"), Issue.LossKind); Obj->SetStringField(TEXT("expected_loss"), Issue.ExpectedLoss); Obj->SetStringField(TEXT("reason"), Issue.Reason); Obj->SetBoolField(TEXT("blocks_strict_apply"), BlocksStrictApply(Issue)); return Obj;
}

