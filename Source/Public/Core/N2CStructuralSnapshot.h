#pragma once

#include "CoreMinimal.h"

class UBlueprint;
class FJsonObject;

class FN2CStructuralSnapshot
{
public:
    static bool Build(UBlueprint* Blueprint, TSharedPtr<FJsonObject>& OutSnapshot, FString& OutHash, FString& OutError);
    static bool Compare(const TSharedPtr<FJsonObject>& Expected, const TSharedPtr<FJsonObject>& Actual, TSharedPtr<FJsonObject>& OutDiff, FString& OutError);
    static bool ValidateExpectedContract(const TSharedPtr<FJsonObject>& Contract, const TSharedPtr<FJsonObject>& Snapshot, TSharedPtr<FJsonObject>& OutDiff, FString& OutError);
    static bool HashJson(const TSharedPtr<FJsonObject>& Object, FString& OutHash, FString& OutError);
};