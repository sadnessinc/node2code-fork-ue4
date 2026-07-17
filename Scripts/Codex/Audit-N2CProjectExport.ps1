[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ProjectRoot,
    [string]$EngineRoot,
    [string]$ArchivePath,
    [string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
Add-Type -AssemblyName System.IO.Compression.FileSystem
$root = (Resolve-Path -LiteralPath $ProjectRoot).Path
if (-not $ArchivePath) {
    $latest = Get-ChildItem -LiteralPath (Join-Path $root 'Saved\NodeToCode\ProjectExports') -Filter 'N2C_Project_*.zip' -File | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $latest) { throw 'No N2C project export archive was found.' }
    $ArchivePath = $latest.FullName
}
$archiveFullPath = (Resolve-Path -LiteralPath $ArchivePath).Path
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $root ('Saved\NodeToCode\ProjectExportAudits\' + (Get-Date -Format 'yyyyMMdd_HHmmss')) }
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null

# These are the exact families promoted by the P0 fixture packs. Variants still
# have to carry their required metadata and be classified verified; membership
# in this set never promotes a record by itself.
$p0ClassNames = @(
    'K2Node_FunctionEntry','K2Node_FunctionResult',
    'K2Node_InputAction','K2Node_InputKey','K2Node_InputAxisEvent','K2Node_InputAxisKeyEvent',
    'K2Node_CreateWidget','K2Node_MakeArray','K2Node_GetArrayItem','K2Node_GetDataTableRow',
    'K2Node_MakeStruct','K2Node_BreakStruct','K2Node_SetFieldsInStruct','K2Node_SetMembersInStruct',
    'K2Node_SwitchEnum','K2Node_EnumEquality','K2Node_EnumInequality','K2Node_EnumLiteral',
    'K2Node_ForEachElementInEnum','K2Node_CastByteToEnum','K2Node_GetEnumeratorNameAsString',
    'K2Node_ComponentBoundEvent','K2Node_AddDelegate','K2Node_RemoveDelegate','K2Node_ClearDelegate',
    'K2Node_CallDelegate','K2Node_CreateDelegate','K2Node_AssignDelegate','K2Node_Message',
    'K2Node_VariableGet','K2Node_VariableSet','K2Node_CallFunction','K2Node_CallFunctionOnMember',
    'K2Node_CallArrayFunction','K2Node_CallParentFunction','K2Node_Event','K2Node_CustomEvent',
    'K2Node_Knot','K2Node_Self','K2Node_IfThenElse','K2Node_ExecutionSequence','K2Node_Select',
    'K2Node_CommutativeAssociativeBinaryOperator','K2Node_SpawnActorFromClass','K2Node_DynamicCast',
    'K2Node_MacroInstance','K2Node_Tunnel','K2Node_Composite'
)
$p0Classes = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
foreach ($name in $p0ClassNames) { [void]$p0Classes.Add($name) }

$coverageRows = [System.Collections.Generic.List[object]]::new()
$boundaryRows = [System.Collections.Generic.List[object]]::new()
$graphBoundaryRows = [System.Collections.Generic.List[object]]::new()
$assetMetadata = @{}
$sidecarCount = 0
$unknownPromoted = 0
$zip = [IO.Compression.ZipFile]::OpenRead($archiveFullPath)
try {
    foreach ($entry in $zip.Entries) {
        if (-not $entry.FullName.EndsWith('.coverage.json',[StringComparison]::OrdinalIgnoreCase)) { continue }
        $sidecarCount++
        $reader = [IO.StreamReader]::new($entry.Open())
        try { $sidecar = ($reader.ReadToEnd() | ConvertFrom-Json) } finally { $reader.Dispose() }
        $assetMetadata[[string]$sidecar.asset_path] = [pscustomobject]@{ parent_class=[string]$sidecar.blueprint_parent_class; blueprint_type=[string]$sidecar.blueprint_type }
        foreach ($issue in @($sidecar.issues)) {
            $class = [string]$issue.node_class
            $status = [string]$issue.status
            $claimedP0 = $p0Classes.Contains($class)
            $missing = @($issue.missing_metadata | ForEach-Object { [string]$_ })
            $required = @($issue.required_metadata | ForEach-Object { [string]$_ })
            $isBlocker = $status -notin @('verified','cosmetic_only','dependency_only')
            if ($class -match 'unknown' -and $status -eq 'verified') { $unknownPromoted++ }
            $coverageRows.Add([pscustomobject]@{
                asset_path = [string]$issue.asset_path
                graph_path = [string]$issue.graph_path
                node_guid = [string]$issue.node_guid
                node_class = $class
                variant = [string]$issue.variant
                status = $status
                required_metadata = $required -join ';'
                missing_metadata = $missing -join ';'
                missing_metadata_count = $missing.Count
                fixture = [string]$issue.verification_fixture
                classifier_reason = [string]$issue.reason
                persistence_result = [string]$issue.persistence_result
                function_boundary_signature_fingerprint = [string]$issue.function_boundary_signature_fingerprint
                graph_boundary_fingerprint = [string]$issue.graph_boundary_fingerprint
                graph_boundary_role = [string]$issue.graph_boundary_role
                owning_graph_identity = [string]$issue.owning_graph_identity
                bound_graph_identity = [string]$issue.bound_graph_identity
                claimed_p0 = $claimedP0
                strict_blocker = $isBlocker
                final_result = $(if($claimedP0 -and $status -ne 'verified'){'FAIL'}else{'PASS'})
            })

            if ($class -match '^K2Node_Function(Entry|Result)$') {
                $fp = [string]$issue.function_boundary_signature_fingerprint
                $pass = $status -eq 'verified' -and $fp.Length -gt 0 -and [string]$issue.persistence_result -eq 'PASS'
                $boundaryRows.Add([pscustomobject]@{
                    asset_path=$issue.asset_path;graph_path=$issue.graph_path;node_guid=$issue.node_guid;node_class=$class
                    fingerprint=$fp;matched_fixture=$issue.verification_fixture;classifier_result=$status
                    persistence_result=$issue.persistence_result;final_result=$(if($pass){'PASS'}else{'FAIL'})
                    reason=$(if($pass){''}else{$issue.reason})
                })
            }
            if ($class -in @('K2Node_Tunnel','K2Node_Composite')) {
                $fp = [string]$issue.graph_boundary_fingerprint
                $pass = $status -eq 'verified' -and $fp.Length -gt 0 -and [string]$issue.persistence_result -eq 'PASS'
                $graphBoundaryRows.Add([pscustomobject]@{
                    asset_path=$issue.asset_path;graph_path=$issue.graph_path;stable_node_identifier=$issue.node_guid;node_class=$class
                    boundary_role=[string]$issue.graph_boundary_role;fingerprint=$fp;matched_fixture=$issue.verification_fixture
                    classifier_result=$status;persistence_result=$issue.persistence_result;final_reason=$(if($pass){''}else{$issue.reason});final_result=$(if($pass){'PASS'}else{'FAIL'})
                })
            }
        }
    }
} finally { $zip.Dispose() }

$coverage = @($coverageRows)
$boundaries = @($boundaryRows)
$graphBoundaries = @($graphBoundaryRows)
$p0 = @($coverage | Where-Object claimed_p0)
$blockers = @($coverage | Where-Object strict_blocker)
$p0NonVerified = @($p0 | Where-Object status -ne 'verified')
$boundaryUnmatched = @($boundaries | Where-Object final_result -eq 'FAIL')
$graphBoundaryUnmatched = @($graphBoundaries | Where-Object final_result -eq 'FAIL')

$coverage | Export-Csv -LiteralPath (Join-Path $OutputDirectory 'coverage_records.csv') -NoTypeInformation -Encoding UTF8
$blockers | Export-Csv -LiteralPath (Join-Path $OutputDirectory 'coverage_blockers.csv') -NoTypeInformation -Encoding UTF8
$coverage | Group-Object asset_path | ForEach-Object {
    $g=@($_.Group); [pscustomobject]@{asset_path=$_.Name;total=$g.Count;verified=@($g|Where-Object status -eq 'verified').Count;nonverified=@($g|Where-Object status -ne 'verified').Count;strict_blockers=@($g|Where-Object strict_blocker).Count;claimed_p0=@($g|Where-Object claimed_p0).Count;claimed_p0_nonverified=@($g|Where-Object {$_.claimed_p0 -and $_.status -ne 'verified'}).Count}
} | Sort-Object asset_path | Export-Csv -LiteralPath (Join-Path $OutputDirectory 'coverage_by_asset.csv') -NoTypeInformation -Encoding UTF8
$coverage | Group-Object node_class | ForEach-Object {
    $g=@($_.Group); [pscustomobject]@{node_class=$_.Name;total=$g.Count;verified=@($g|Where-Object status -eq 'verified').Count;supported_untested=@($g|Where-Object status -eq 'supported_untested').Count;guarded=@($g|Where-Object status -eq 'guarded').Count;partial=@($g|Where-Object status -eq 'partial').Count;unsupported=@($g|Where-Object status -eq 'unsupported').Count;cosmetic_only=@($g|Where-Object status -eq 'cosmetic_only').Count;dependency_only=@($g|Where-Object status -eq 'dependency_only').Count;claimed_p0=$g[0].claimed_p0}
} | Sort-Object node_class | Export-Csv -LiteralPath (Join-Path $OutputDirectory 'coverage_by_node_class.csv') -NoTypeInformation -Encoding UTF8
$p0 | Group-Object node_class | ForEach-Object {
    $g=@($_.Group);$non=@($g|Where-Object status -ne 'verified');$fixtures=@($g.fixture|Where-Object {$_}|Sort-Object -Unique)
    [pscustomobject]@{node_class=$_.Name;production_total=$g.Count;verified_count=@($g|Where-Object status -eq 'verified').Count;nonverified_count=$non.Count;missing_metadata_count=@($g|Where-Object missing_metadata_count -gt 0).Count;fixture_name=$fixtures -join ';';result=$(if($non.Count -eq 0){'PASS'}else{'FAIL'})}
} | Sort-Object node_class | Export-Csv -LiteralPath (Join-Path $OutputDirectory 'p0_acceptance_matrix.csv') -NoTypeInformation -Encoding UTF8

$boundaries | Export-Csv -LiteralPath (Join-Path $OutputDirectory 'function_boundary_records.csv') -NoTypeInformation -Encoding UTF8
$boundaries | Group-Object fingerprint | ForEach-Object { [pscustomobject]@{fingerprint=$_.Name;record_count=$_.Count;node_classes=($_.Group.node_class|Sort-Object -Unique)-join ';';result=$(if($_.Group.final_result -contains 'FAIL'){'FAIL'}else{'PASS'})} } | Export-Csv -LiteralPath (Join-Path $OutputDirectory 'function_boundary_fingerprints.csv') -NoTypeInformation -Encoding UTF8
$boundaries | Select-Object fingerprint,matched_fixture,classifier_result,persistence_result -Unique | Export-Csv -LiteralPath (Join-Path $OutputDirectory 'function_boundary_fixture_map.csv') -NoTypeInformation -Encoding UTF8
$boundaries | Group-Object asset_path | ForEach-Object { $g=@($_.Group);[pscustomobject]@{asset_path=$_.Name;total=$g.Count;verified=@($g|Where-Object final_result -eq 'PASS').Count;nonverified=@($g|Where-Object final_result -eq 'FAIL').Count} } | Sort-Object asset_path | Export-Csv -LiteralPath (Join-Path $OutputDirectory 'function_boundary_by_asset.csv') -NoTypeInformation -Encoding UTF8
$boundaryUnmatched | Export-Csv -LiteralPath (Join-Path $OutputDirectory 'function_boundary_unmatched.csv') -NoTypeInformation -Encoding UTF8

$graphBoundaries | Export-Csv -LiteralPath (Join-Path $OutputDirectory 'graph_boundary_records.csv') -NoTypeInformation -Encoding UTF8
$graphBoundaries | Group-Object fingerprint | ForEach-Object { [pscustomobject]@{fingerprint=$_.Name;record_count=$_.Count;node_classes=($_.Group.node_class|Sort-Object -Unique)-join ';';result=$(if($_.Group.final_result -contains 'FAIL'){'FAIL'}else{'PASS'})} } | Export-Csv -LiteralPath (Join-Path $OutputDirectory 'graph_boundary_fingerprints.csv') -NoTypeInformation -Encoding UTF8
$graphBoundaries | Select-Object fingerprint,matched_fixture,classifier_result,persistence_result -Unique | Export-Csv -LiteralPath (Join-Path $OutputDirectory 'graph_boundary_fixture_map.csv') -NoTypeInformation -Encoding UTF8
$graphBoundaries | Group-Object asset_path | ForEach-Object { $g=@($_.Group);[pscustomobject]@{asset_path=$_.Name;total=$g.Count;verified=@($g|Where-Object final_result -eq 'PASS').Count;nonverified=@($g|Where-Object final_result -eq 'FAIL').Count} } | Sort-Object asset_path | Export-Csv -LiteralPath (Join-Path $OutputDirectory 'graph_boundary_by_asset.csv') -NoTypeInformation -Encoding UTF8
$graphBoundaryUnmatched | Export-Csv -LiteralPath (Join-Path $OutputDirectory 'graph_boundary_unmatched.csv') -NoTypeInformation -Encoding UTF8

# BACK2DEAD_CURRENT_WORK_SCOPE_V1: exact requested assets, all BFL_* assets,
# directly observed combat boundary owners, and Blueprint AI base classes.
$exactScope = @(
 '/Game/Room/RoomContent/A_Room.A_Room','/Game/Components/AC_Attack.AC_Attack','/Game/Components/Effects/AC_Effects.AC_Effects','/Game/Components/AC_Health.AC_Health','/Game/Components/AC_Mana.AC_Mana','/Game/Components/AC_Buffs.AC_Buffs',
 '/Game/Artifacts/BFL_ArtifactUI.BFL_ArtifactUI','/Game/BFL_ArtifactRoll_New.BFL_ArtifactRoll_New','/Game/BFL_GlobalRandom.BFL_GlobalRandom','/Game/BFL_PlayerStats_New.BFL_PlayerStats_New','/Game/Components/Effects/BFL_Effects.BFL_Effects','/Game/Interfaces/BFL_Debug.BFL_Debug','/Game/Random_Generator/BFL_RandomRarity.BFL_RandomRarity','/Game/Random_Generator/Colors/BFL_RarityUI.BFL_RarityUI','/Game/Room/RoomContent/BFL_RoomFunctions.BFL_RoomFunctions','/Game/Unit/Mobs/AI_Enemy/BFL_AIDebug.BFL_AIDebug','/Game/Unit/Mobs/AI_Enemy/BFL_EnemyAI.BFL_EnemyAI',
 '/Game/Unit/Mobs/AI_Enemy/AC_CombatPressureDirector.AC_CombatPressureDirector','/Game/Unit/Mobs/AI_Enemy/AIC_Enemy.AIC_Enemy','/Game/Unit/Mobs/AI_Enemy/BTDecorator_CheckDisableOrDead.BTDecorator_CheckDisableOrDead','/Game/Unit/Mobs/AI_Enemy/BTDecorator_CompareSelectedAction.BTDecorator_CompareSelectedAction','/Game/Unit/Mobs/AI_Enemy/BTService_Enemy_UpdateBlackboard.BTService_Enemy_UpdateBlackboard','/Game/Unit/Mobs/AI_Enemy/BTTask_Enemy_CastSelectedSpell.BTTask_Enemy_CastSelectedSpell','/Game/Unit/Mobs/AI_Enemy/BTTask_Enemy_FindCombatPosition.BTTask_Enemy_FindCombatPosition','/Game/Unit/Mobs/AI_Enemy/BTTask_Enemy_SelectAction.BTTask_Enemy_SelectAction','/Game/Unit/Mobs/AI_Enemy/BTTask_Enemy_StopLogic.BTTask_Enemy_StopLogic','/Game/Unit/Mobs/AI_Enemy/I_DangerToken.I_DangerToken','/Game/Unit/Mobs/Enemy.Enemy','/Game/Unit/Mobs/MainSpawner.MainSpawner'
)
$scopeAssets = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
foreach($asset in $exactScope){if($assetMetadata.ContainsKey($asset)){[void]$scopeAssets.Add($asset)}}
foreach($asset in $assetMetadata.Keys){$leaf=($asset -split '/')[-1].Split('.')[0];$parent=[string]$assetMetadata[$asset].parent_class;if($leaf.StartsWith('BFL_') -or $parent -match '(BTTask_BlueprintBase|BTService_BlueprintBase|BTDecorator_BlueprintBase|AIController)'){[void]$scopeAssets.Add($asset)}}
foreach($row in $graphBoundaries){if($row.asset_path -like '/Game/Components/*'){[void]$scopeAssets.Add([string]$row.asset_path)}}
$scopeRecords = @($coverage | Where-Object {$scopeAssets.Contains($_.asset_path)})
$scopeRuntime = @($scopeRecords | Where-Object {$_.node_class -notmatch 'AnimGraph|AnimState|AnimTransition|Niagara|InputTouch' -and $_.status -notin @('cosmetic_only','dependency_only')})
$scopeBlockers = @($scopeRuntime | Where-Object status -ne 'verified')
$scopeAssetRows = @($scopeAssets | Sort-Object | ForEach-Object {$asset=$_;$g=@($scopeRecords|Where-Object asset_path -eq $asset);$runtime=@($scopeRuntime|Where-Object asset_path -eq $asset);$non=@($runtime|Where-Object status -ne 'verified');[pscustomobject]@{scope='BACK2DEAD_CURRENT_WORK_SCOPE_V1';asset_path=$asset;parent_class=[string]$assetMetadata[$asset].parent_class;record_count=$g.Count;runtime_record_count=$runtime.Count;verified=@($runtime|Where-Object status -eq 'verified').Count;nonverified=$non.Count;verdict=$(if($non.Count -eq 0){'PASS'}else{'FAIL'})}})
$scopeAssetRows | Export-Csv -LiteralPath (Join-Path $OutputDirectory 'current_work_scope_assets.csv') -NoTypeInformation -Encoding UTF8
$scopeRecords | Export-Csv -LiteralPath (Join-Path $OutputDirectory 'current_work_scope_records.csv') -NoTypeInformation -Encoding UTF8
$scopeAssetRows | Export-Csv -LiteralPath (Join-Path $OutputDirectory 'current_work_scope_by_asset.csv') -NoTypeInformation -Encoding UTF8
$scopeBlockers | Export-Csv -LiteralPath (Join-Path $OutputDirectory 'current_work_scope_blockers.csv') -NoTypeInformation -Encoding UTF8
$scopeAssetRows | Select-Object scope,asset_path,runtime_record_count,verified,nonverified,verdict | Export-Csv -LiteralPath (Join-Path $OutputDirectory 'current_work_scope_acceptance_matrix.csv') -NoTypeInformation -Encoding UTF8

$statusCounts = [ordered]@{}
foreach($group in ($coverage | Group-Object status | Sort-Object Name)) { $statusCounts[$group.Name]=$group.Count }
$result = if($p0NonVerified.Count -eq 0 -and $boundaryUnmatched.Count -eq 0 -and $graphBoundaryUnmatched.Count -eq 0 -and $scopeBlockers.Count -eq 0 -and $unknownPromoted -eq 0){'PASS'}else{'FAIL'}
$summary = [ordered]@{
    schema='N2C_PRODUCTION_P0_AUDIT_V2';archive=$archiveFullPath;blueprint_sidecars=$sidecarCount
    coverage_records=$coverage.Count;status_counts=$statusCounts;strict_blockers=$blockers.Count
    claimed_p0_records=$p0.Count;claimed_p0_nonverified=$p0NonVerified.Count;unknown_promoted=$unknownPromoted
    function_boundary_records=$boundaries.Count;function_boundary_unique_fingerprints=@($boundaries|Select-Object -ExpandProperty fingerprint -Unique).Count
    function_boundary_unmatched=$boundaryUnmatched.Count;result=$result
    graph_boundary_records=$graphBoundaries.Count;graph_boundary_unique_fingerprints=@($graphBoundaries|Select-Object -ExpandProperty fingerprint -Unique).Count;graph_boundary_unmatched=$graphBoundaryUnmatched.Count
    current_work_scope='BACK2DEAD_CURRENT_WORK_SCOPE_V1';current_work_assets=$scopeAssets.Count;current_work_records=$scopeRecords.Count;current_work_runtime_nonverified=$scopeBlockers.Count
}
$summary | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'coverage_summary.json') -Encoding UTF8
$summary | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'audit_summary.json') -Encoding UTF8
Write-Host "N2C_AUDIT|result=$result|sidecars=$sidecarCount|records=$($coverage.Count)|p0=$($p0.Count)|p0_nonverified=$($p0NonVerified.Count)|function_boundaries=$($boundaries.Count)|graph_boundaries=$($graphBoundaries.Count)|graph_unmatched=$($graphBoundaryUnmatched.Count)|current_work_assets=$($scopeAssets.Count)|current_work_nonverified=$($scopeBlockers.Count)|unknown_promoted=$unknownPromoted|output=$OutputDirectory"
exit $(if($result -eq 'PASS'){0}else{1})
