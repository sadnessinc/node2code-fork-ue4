#pragma once

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"

class FJsonObject;
class UBlueprint;
class UEdGraph;
class UK2Node_MacroInstance;

class NODETOCODE_API FN2CMacroReference
{
public:
    static FString SignatureHash(const UEdGraph* MacroGraph);
    static void AppendIdentity(const TSharedPtr<FJsonObject>& Object, const UEdGraph* MacroGraph, const UK2Node_MacroInstance* Instance = nullptr);
    static bool ResolveAndValidate(const TSharedPtr<FJsonObject>& NodeObject, UBlueprint* TargetBlueprint, UEdGraph*& OutMacroGraph, FString& OutErrorCode, FString& OutDetail);
};
