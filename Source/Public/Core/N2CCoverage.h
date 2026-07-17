// Copyright (c) 2026. Coverage classification shared by export, apply and verification.

#pragma once

#include "CoreMinimal.h"

class UBlueprint;
class UEdGraph;
class UEdGraphNode;
class FJsonObject;

struct FN2CCoverageIssue
{
    FString AssetPath;
    FString GraphPath;
    FString NodeGuid;
    FString NodeClass;
    FString Variant;
    FString Status;
    FString ConstructorHandler;
    TArray<FString> RequiredMetadata;
    TArray<FString> MissingMetadata;
    FString VerificationFixture;
    FString VerificationGap;
    FString FunctionBoundaryFingerprint;
    FString FunctionBoundaryRole;
    FString GraphBoundaryFingerprint;
    FString GraphBoundaryRole;
    FString OwningGraphIdentity;
    FString BoundGraphIdentity;
    FString PersistenceResult;
    bool bReopenVerified = false;
    FString LossKind;
    FString ExpectedLoss;
    FString Reason;
};

/** One result for a patch before any transaction or Blueprint mutation. */
struct FN2CPreflightResult
{
    bool bAllowed = false;
    bool bHasRuntimeBlockers = false;
    bool bHasVerificationGaps = false;
    bool bHasCosmeticWarnings = false;
    bool bSidecarStale = false;
    int32 RuntimeBlockerCount = 0;
    int32 VerificationGapCount = 0;
    int32 CosmeticWarningCount = 0;
    TArray<FN2CCoverageIssue> Issues;
};

/** Single source of truth for metadata-aware P0 coverage decisions. */
class FN2CCoverageClassifier
{
public:
    static void ClassifyLiveNode(const UBlueprint* Blueprint, const UEdGraph* Graph, const UEdGraphNode* Node, FN2CCoverageIssue& OutIssue);
    static void ClassifyPatchNode(const TSharedPtr<FJsonObject>& NodeObj, bool bFunctionGraph, FN2CCoverageIssue& OutIssue);
    static FString BuildFunctionBoundaryFingerprint(const UBlueprint* Blueprint, const UEdGraph* Graph, const UEdGraphNode* Node);
    static FString BuildGraphBoundaryFingerprint(const UBlueprint* Blueprint, const UEdGraph* Graph, const UEdGraphNode* Node);
    static bool BlocksStrictApply(const FN2CCoverageIssue& Issue);
    static bool AllowsApply(const FN2CCoverageIssue& Issue, bool bDeveloperOverride);
    static bool PreflightPatch(const TSharedPtr<FJsonObject>& PatchRoot, bool bDeveloperOverride, FN2CPreflightResult& OutResult);
    static bool BuildBlueprintSidecar(const UBlueprint* Blueprint, const FString& SourceJson, FString& OutJson, FString& OutPrimaryReason);
    static bool BuildAggregateCoverage(const TArray<FString>& SidecarJsons, TSharedPtr<FJsonObject>& OutSummary);
    static bool ValidateSidecarForSource(const FString& SidecarJson, const FString& SourceJson, bool& bOutStale, FString& OutReason);
    static TSharedPtr<FJsonObject> ToJson(const FN2CCoverageIssue& Issue);
};
