#pragma once

#include "CoreMinimal.h"

enum class EN2CRoundTripStage : uint8
{
    Parse, Preflight, Backup, Apply, CompileAfterApply, BuildExpectedContract,
    BuildPersistenceBaseline, Save, CloseEditors, Unload, LaunchFreshProcess,
    ParseChildResult, ReloadFromDisk, CompileAfterReload, BuildReloadedSnapshot,
    ExpectedContractCompare, PersistenceCompare, WriteDiff, WriteResult,
    RestoreAfterFailure, CleanupFixture, Finalize
};

struct FN2CRoundTripStageResult
{
    EN2CRoundTripStage Stage = EN2CRoundTripStage::Parse;
    bool bAttempted = false;
    bool bPassed = false;
    FString ErrorCode;
    FString Message;
    double DurationSeconds = 0.0;
};

struct FN2CRoundTripVerificationRequest
{
    FString AssetPath;
    FString TemplateAssetPath;
    FString GeneratedFixturePath;
    FString PatchJson;
    FString PatchIdentity;
    FString ExpectedContractJson;
    FString RunId;
    FString RunDirectory;
    bool bDeveloperOverride = false;
    bool bAutomationOnly = false;
    bool bCleanupGeneratedFixture = false;
    int32 FreshProcessTimeoutSeconds = 90;
    bool bForceCompileAfterApplyFailure = false;
    bool bForceSaveFailure = false;
    bool bForceMissingChildResult = false;
    bool bForceStructuralMismatch = false;
    bool bDelayChildForTimeout = false;
    bool bForceChildReloadFailure = false;
    bool bForceMalformedChildResult = false;
    bool bForceChildIdentityMismatch = false;
};

struct FN2CRoundTripVerificationResult
{
    FString Schema = TEXT("N2C_ROUNDTRIP_VERIFY_RESULT_V3");
    FString RunId;
    FString AssetPath;
    FString PatchIdentity;
    bool bPassed = false;
    FString FailedStage;
    FString ErrorCode;
    uint32 ParentProcessId = 0;
    uint32 ChildProcessId = 0;
    int32 ChildExitCode = -1;
    FString RunDirectory;
    FString ManifestPath;
    FString ExpectedContractPath;
    FString BaselinePath;
    FString DiffPath;
    FString ChildRequestPath;
    FString ChildResultPath;
    FString FreshLogPath;
    FString BackupPath;
    bool bWeakBlueprintAliveAfterUnload = false;
    bool bPackageShellAliveAfterUnload = false;
    bool bCleanupPassed = false;
    FString Report;
    TArray<FN2CRoundTripStageResult> StageResults;
};

class FN2CRoundTripVerification
{
public:
    static bool RunParent(const FN2CRoundTripVerificationRequest& Request, FN2CRoundTripVerificationResult& OutResult);
    static bool RunFreshChildFromCommandLine(FString& OutReport);
    static const TCHAR* StageName(EN2CRoundTripStage Stage);
    static bool HasPassedStage(const FN2CRoundTripVerificationResult& Result, EN2CRoundTripStage Stage);
};