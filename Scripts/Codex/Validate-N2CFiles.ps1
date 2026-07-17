[CmdletBinding()]
param(
    [string]$PluginRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path,
    [switch]$AllowBuildProducts
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
trap {
    $failureLine = 0
    $failurePosition = ''
    $failureStack = ''
    if ($null -ne $_.InvocationInfo) {
        $failureLine = [int]$_.InvocationInfo.ScriptLineNumber
        $failurePosition = [string]$_.InvocationInfo.PositionMessage
    }
    if ($null -ne $_.ScriptStackTrace) { $failureStack = [string]$_.ScriptStackTrace }
    [Console]::Error.WriteLine(('N2C_STATIC_VALIDATION_EXCEPTION|line={0}|message={1}|position={2}|stack={3}' -f $failureLine, [string]$_.Exception.Message, $failurePosition.Replace("`r", ' ').Replace("`n", ' '), $failureStack.Replace("`r", ' ').Replace("`n", ' ')))
    exit 1
}
$plugin = (Resolve-Path -LiteralPath $PluginRoot).Path

function Get-N2CJsonPropertyValue {
    param(
        [Parameter(Mandatory)]$Object,
        [Parameter(Mandatory)][string[]]$Names,
        [switch]$Required,
        [string]$Context = 'JSON object'
    )

    if ($null -eq $Object) {
        if ($Required) { throw ("{0}: object is null while resolving {1}." -f $Context, ($Names -join '/')) }
        return $null
    }

    foreach ($name in $Names) {
        $property = $Object.PSObject.Properties[$name]
        if ($null -ne $property) { return $property.Value }
    }

    if ($Required) {
        throw ("{0}: required property is missing; expected one of [{1}]." -f $Context, ($Names -join ', '))
    }
    return $null
}

function Get-N2CFileSha256 {
    param([Parameter(Mandatory)][string]$LiteralPath)
    $stream = [IO.File]::Open($LiteralPath, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '').ToLowerInvariant()
    }
    finally {
        $sha.Dispose()
        $stream.Dispose()
    }
}

function Get-N2CCollectionCount {
    param([AllowNull()][object]$Value)

    # Do not rely on @($Value) for explicit null. Windows PowerShell 5.1 can
    # preserve a bound null placeholder in a way that produces a one-element
    # array. Handle null first, then enumerate real collections explicitly.
    if ($null -eq $Value) { return [int]0 }
    if ($Value -is [string]) { return [int]1 }
    if ($Value -is [System.Collections.IEnumerable]) {
        [int]$itemCount = 0
        foreach ($item in $Value) { $itemCount = $itemCount + 1 }
        return $itemCount
    }
    return [int]1
}

function Assert-N2CCollectionCount {
    param(
        [AllowNull()][object]$Value,
        [Parameter(Mandatory)][int]$Expected,
        [Parameter(Mandatory)][string]$Label
    )
    $actual = [int](Get-N2CCollectionCount -Value $Value)
    if ($actual -ne $Expected) {
        throw ("Validator collection-count self-test failed for {0}: expected={1}; actual={2}." -f $Label, $Expected, $actual)
    }
}

function Test-N2CValidatorHelpers {
    $empty = @()
    $single = @('one')
    $multiple = @('one','two','three')
    $scalarObject = [pscustomobject]@{ Name = 'scalar' }
    $pipelineEmpty = @(1 | Where-Object { $false })
    $pipelineSingle = @(1 | Where-Object { $true })
    $regexMatches = [regex]::Matches('aaa', 'a')
    Assert-N2CCollectionCount -Value $null -Expected 0 -Label 'null'
    Assert-N2CCollectionCount -Value $empty -Expected 0 -Label 'empty array'
    Assert-N2CCollectionCount -Value $single -Expected 1 -Label 'one-element array'
    Assert-N2CCollectionCount -Value $multiple -Expected 3 -Label 'multi-element array'
    Assert-N2CCollectionCount -Value 'scalar string' -Expected 1 -Label 'scalar string'
    Assert-N2CCollectionCount -Value $scalarObject -Expected 1 -Label 'scalar object'
    Assert-N2CCollectionCount -Value $pipelineEmpty -Expected 0 -Label 'empty pipeline result'
    Assert-N2CCollectionCount -Value $pipelineSingle -Expected 1 -Label 'single pipeline result'
    Assert-N2CCollectionCount -Value $regexMatches -Expected 3 -Label 'regex MatchCollection'
    $jsonProbe = [pscustomobject]@{ present = 7 }
    if ([int](Get-N2CJsonPropertyValue -Object $jsonProbe -Names @('present') -Required -Context 'validator helper self-test') -ne 7) { throw 'Validator JSON accessor self-test failed for present property.' }
    if ($null -ne (Get-N2CJsonPropertyValue -Object $jsonProbe -Names @('optional_missing') -Context 'validator helper self-test')) { throw 'Validator JSON accessor self-test failed for missing optional property.' }
}
Test-N2CValidatorHelpers

$descriptorPath = Join-Path $plugin 'NodeToCode.uplugin'
$descriptor = Get-Content -LiteralPath $descriptorPath -Raw | ConvertFrom-Json
$releaseVersion = [int](Get-N2CJsonPropertyValue -Object $descriptor -Names @('Version') -Required -Context 'NodeToCode.uplugin')
$releaseVersionName = [string](Get-N2CJsonPropertyValue -Object $descriptor -Names @('VersionName') -Required -Context 'NodeToCode.uplugin')
if ($releaseVersion -lt 101) { throw "Invalid plugin Version: $releaseVersion" }
$versionNameMatch = [regex]::Match($releaseVersionName, '^1\.2\.(?<patch>[0-9]+)-ue427-[A-Za-z0-9._-]+$')
if (-not $versionNameMatch.Success) { throw "VersionName does not match 1.2.<patch>-ue427-<label>: $releaseVersionName" }
$expectedPatch = $releaseVersion - 100
if ([int]$versionNameMatch.Groups['patch'].Value -ne $expectedPatch) {
    throw "Version/VersionName mismatch: Version $releaseVersion requires 1.2.$expectedPatch, got $releaseVersionName"
}
$finalManualFileName = "N2C_FINAL_MANUAL_ALL_STRICT_SUPPORTED_V$releaseVersion.json"
$currentReleaseNotesFileName = "N2C_RELEASE_NOTES_$releaseVersion.md"
$currentStaticAuditFileName = "N2C_STATIC_AUDIT_$releaseVersion.json"
$currentContractCatalogFileName = "N2C_IMPORT_CONTRACT_MATRIX_CATALOG_$releaseVersion.md"

function Resolve-N2CManualEdge {
    param(
        [Parameter(Mandatory)]$Edge,
        [Parameter(Mandatory)][string]$Context
    )

    $hasNodeIdForm = ($null -ne $Edge.PSObject.Properties['from_node_id'] -or $null -ne $Edge.PSObject.Properties['to_node_id'])
    $hasCompactForm = ($null -ne $Edge.PSObject.Properties['from'] -or $null -ne $Edge.PSObject.Properties['to'])

    if ($hasNodeIdForm -and $hasCompactForm) {
        throw ("{0}: edge mixes from_node_id/to_node_id with from/to." -f $Context)
    }

    $sourceId = [string](Get-N2CJsonPropertyValue -Object $Edge -Names @('from_node_id','from') -Required -Context $Context)
    $targetId = [string](Get-N2CJsonPropertyValue -Object $Edge -Names @('to_node_id','to') -Required -Context $Context)
    $fromPin = [string](Get-N2CJsonPropertyValue -Object $Edge -Names @('from_pin') -Required -Context $Context)
    $toPin = [string](Get-N2CJsonPropertyValue -Object $Edge -Names @('to_pin') -Required -Context $Context)

    if ([string]::IsNullOrWhiteSpace($sourceId) -or [string]::IsNullOrWhiteSpace($targetId)) {
        throw ("{0}: source or target node id is empty." -f $Context)
    }
    if ([string]::IsNullOrWhiteSpace($fromPin) -or [string]::IsNullOrWhiteSpace($toPin)) {
        throw ("{0}: source or target pin name is empty." -f $Context)
    }

    $schemaName = 'compact'
    if ($hasNodeIdForm) { $schemaName = 'node_id' }
    return [pscustomobject]@{
        SourceId = $sourceId
        TargetId = $targetId
        FromPin = $fromPin
        ToPin = $toPin
        Schema = $schemaName
    }
}
$required = @(
    'NodeToCode.uplugin',
    'README.md',
    'CODEX_PLUGIN_START_HERE.md',
    'N2C_AI_JSON_IMPORT_AUTHORING_RULES.md',
    $finalManualFileName,
    'Source\NodeToCode.Build.cs',
    'Source\Private\Core\N2CPatchImporter.cpp',
    'Source\Private\N2CVerificationTests.cpp',
    'Source\Tests\Fixtures\N2C_P0_FIXTURE_MANIFEST_V1.json',
    'Source\Tests\Fixtures\ManualReplay\N2C_MANUAL_REPLAY_MANIFEST_V1.json',
    'Source\Tests\Fixtures\N2C_NODE_TEST_REQUIREMENTS_V1.json',
    'Source\Tests\Fixtures\N2C_IMPORT_CONTRACT_MATRIX_V1.json',
    'Source\Documentation\N2C_DOCUMENT_AUTHORITY.md',
    'Source\Documentation\N2C_CURRENT_STATE.md',
    ("Source\Documentation\{0}" -f $currentContractCatalogFileName),
    ("Source\Documentation\{0}" -f $currentReleaseNotesFileName),
    ("Source\Documentation\{0}" -f $currentStaticAuditFileName),
    'Source\Documentation\N2C_AUTOMATION_FAILURE_AUDIT_20260716_143553.md',
    'Source\Documentation\N2C_AUTOMATION_REPORT_20260716_092715.md',
    'Source\Documentation\N2C_MANUAL_DRY_RUN_STRUCT_PIN_AUDIT_20260716.md',
    'Source\Documentation\N2C_AUTOMATION_REPORT_20260716_090558.md',
    'Source\Documentation\N2C_FAILED_IMPORT_PROJECT_EXPORT_AUDIT_20260716_031216.md',
    'Source\Documentation\N2C_AUTOMATION_REPORT_20260716_014238.md',
    'Source\Documentation\N2C_RELEASE_GATE_LOG_AUDIT_182.md',
    'Source\Documentation\N2C_RELEASE_NOTES_182.md',
    'Source\Documentation\N2C_STATIC_AUDIT_182.json',
    'Source\Documentation\N2C_PROGRESS_SELFTEST_CLEANUP_HOTFIX_181.md',
    'Source\Documentation\N2C_RELEASE_NOTES_181.md',
    'Source\Documentation\N2C_STATIC_AUDIT_181.json',
    'Source\Documentation\N2C_POWERSHELL_PROGRESS_PARSER_HOTFIX_180.md',
    'Source\Documentation\N2C_RELEASE_NOTES_180.md',
    'Source\Documentation\N2C_STATIC_AUDIT_180.json',
    'Source\Documentation\N2C_MANUAL_REPLAY_FIXES_PROGRESS_179.md',
    'Source\Documentation\N2C_RELEASE_NOTES_179.md',
    'Source\Documentation\N2C_STATIC_AUDIT_179.json',
    'Source\Documentation\N2C_POWERSHELL_PROCESS_AUDIT_178.md',
    'Source\Documentation\N2C_RELEASE_NOTES_178.md',
    'Source\Documentation\N2C_POWERSHELL_RUNTIME_HOTFIX_177.md',
    'Source\Documentation\N2C_RELEASE_NOTES_177.md',
    'Source\Documentation\N2C_POWERSHELL_PARSER_HOTFIX_176.md',
    'Source\Documentation\N2C_RELEASE_NOTES_176.md',
    'Source\Documentation\N2C_FULL_REVIEW_173.md',
    'Source\Documentation\N2C_AUTOMATION_RUNNER_UX_FIX_175.md',
    'Source\Documentation\N2C_RELEASE_NOTES_175.md',
    'Source\Documentation\N2C_AUTOMATION_RUNNER_HOTFIX_174.md',
    'Source\Documentation\N2C_RELEASE_NOTES_174.md',
    'Source\Documentation\N2C_TODO.md',
    'Source\Documentation\N2C_ARCHITECTURE_MAP.md',
    'Source\Documentation\N2C_MANUAL_REPLAY_AUTOMATION.md',
    'Scripts\RUN_N2C_AUTOMATION_AND_PACK.cmd',
    'Scripts\Codex\Audit-N2CProjectExport.ps1',
    'Scripts\Codex\Build-N2CEditor.ps1',
    'Scripts\Codex\Invoke-N2CFullValidation.ps1',
    'Scripts\Codex\Package-N2CPlugin.ps1',
    'Scripts\Codex\Run-N2CProjectExport.ps1',
    'Scripts\Codex\Run-N2CVerification.ps1',
    'Scripts\Codex\Search-UE427Source.ps1',
    'Scripts\Codex\Test-N2CPowerShellSyntax.ps1',
    'Scripts\Codex\Validate-N2CFiles.ps1',
    'Scripts\Codex\Validate-N2CImportContractMatrix.py',
    'Scripts\Codex\validate_n2c_release.py'
)
$missing = @($required | Where-Object { -not (Test-Path -LiteralPath (Join-Path $plugin $_) -PathType Leaf) })
if ((Get-N2CCollectionCount -Value $missing)) { throw "Missing required node2code files: $($missing -join ', ')" }

$authoringRulesPath = Join-Path $plugin 'N2C_AI_JSON_IMPORT_AUTHORING_RULES.md'
$authoringRules = Get-Content -LiteralPath $authoringRulesPath -Raw
foreach ($requiredAuthoringText in @('mandatory first document','N2C_PROJECT_PATCH_V1','InputPin','OutputPin','unsupported','Alternative','N2C_NEW_NODE_TEST_GATE','K2Node_Composite_<n>','canonical identity','StructType->GetFName()','K2Node_MakeStruct','K2Node_BreakStruct','Option 0','Option 1')) {
    if ($authoringRules.IndexOf($requiredAuthoringText, [System.StringComparison]::OrdinalIgnoreCase) -lt 0) {
        throw "AI JSON authoring rules missing required contract text: $requiredAuthoringText"
    }
}


# A plugin installed in a UE project legitimately gains top-level Binaries/ and
# Intermediate/ after the first build. The orchestrator validates that working tree
# with -AllowBuildProducts. Strict source-only hygiene is still enforced by the
# packaging stage and by this script when the switch is omitted.
$forbiddenDirs = if ($AllowBuildProducts) { @('Saved','.vs','DerivedDataCache','__pycache__') } else { @('Binaries','Intermediate','Saved','.vs','DerivedDataCache','__pycache__') }
foreach ($dir in $forbiddenDirs) {
    $hits = @(Get-ChildItem -LiteralPath $plugin -Directory -Recurse -Force | Where-Object Name -eq $dir)
    if ((Get-N2CCollectionCount -Value $hits)) { throw "Forbidden directory present: $($hits[0].FullName)" }
}
$forbiddenExtensions = @('.dll','.pdb','.obj','.lib','.exp','.log','.dmp','.pyc','.pyo')
$forbiddenFiles = @(Get-ChildItem -LiteralPath $plugin -File -Recurse -Force | Where-Object {
    $relative = $_.FullName.Substring($plugin.Length).TrimStart([char[]]@('\','/'))
    $isAllowedBuildProduct = $AllowBuildProducts -and ($relative -match '^(Binaries|Intermediate)[\\/]')
    (-not $isAllowedBuildProduct) -and ($forbiddenExtensions -contains $_.Extension.ToLowerInvariant())
})
if ((Get-N2CCollectionCount -Value $forbiddenFiles)) { throw "Forbidden generated/binary file present: $($forbiddenFiles[0].FullName)" }


# Parse every PowerShell script with the Windows PowerShell parser before any
# build or Editor process starts. Syntax parsing is combined with AST rules for
# automatic-variable collisions that ordinary delimiter checks cannot detect.
$powerShellFiles = @(Get-ChildItem -LiteralPath (Join-Path $plugin 'Scripts') -Filter *.ps1 -File -Recurse | Sort-Object FullName)
$powerShellAsts = @{}
$protectedAutomaticVariables = @(
    'PID','Host','PSHOME','PSScriptRoot','PSCommandPath','MyInvocation',
    'Args','Error','Matches','Input','ExecutionContext','PSBoundParameters',
    'LASTEXITCODE','Home','Profile','PWD','ShellId','StackTrace','NestedPromptLevel',
    'PSVersionTable'
)
$forbiddenAutomaticReferences = @('PID','Args','Matches')
foreach ($powerShellFile in $powerShellFiles) {
    $parseTokens = $null
    $parseErrors = $null
    $scriptAst = [System.Management.Automation.Language.Parser]::ParseFile($powerShellFile.FullName, [ref]$parseTokens, [ref]$parseErrors)
    if ((Get-N2CCollectionCount -Value @($parseErrors)) -gt 0) {
        $firstParseError = @($parseErrors)[0]
        throw "PowerShell parser error: $($powerShellFile.FullName):$($firstParseError.Extent.StartLineNumber):$($firstParseError.Extent.StartColumnNumber): $($firstParseError.Message)"
    }
    $powerShellAsts[$powerShellFile.FullName] = $scriptAst

    if ($powerShellFile.Name -eq 'Validate-N2CFiles.ps1') {
        $unsafeCountMembers = @($scriptAst.FindAll({
            param($astNode)
            if ($astNode -isnot [System.Management.Automation.Language.MemberExpressionAst]) { return $false }
            return [string]$astNode.Member.Value -eq 'Count'
        }, $true))
        if ((Get-N2CCollectionCount -Value $unsafeCountMembers) -gt 0) {
            $firstUnsafeCount = $unsafeCountMembers[0]
            throw "Validate-N2CFiles.ps1 must use Get-N2CCollectionCount instead of direct .Count at line $($firstUnsafeCount.Extent.StartLineNumber)."
        }


        $jsonRootVariables = @('descriptor','p0','manual','finalManual','nodeMatrix','contractMatrix','capability','nodeCoverageRow','guardCoverageRow')
        $unsafeJsonMembers = @($scriptAst.FindAll({
            param($astNode)
            if ($astNode -isnot [System.Management.Automation.Language.MemberExpressionAst]) { return $false }
            if ($astNode.Expression -isnot [System.Management.Automation.Language.VariableExpressionAst]) { return $false }
            $rootName = [string]$astNode.Expression.VariablePath.UserPath
            return $jsonRootVariables -contains $rootName
        }, $true))
        if ((Get-N2CCollectionCount -Value $unsafeJsonMembers) -gt 0) {
            $firstUnsafeJsonMember = $unsafeJsonMembers[0]
            throw "Validate-N2CFiles.ps1 must use Get-N2CJsonPropertyValue instead of direct JSON member access at line $($firstUnsafeJsonMember.Extent.StartLineNumber)."
        }
    }

    $scriptSource = Get-Content -LiteralPath $powerShellFile.FullName -Raw
    if (-not $scriptSource.Contains('$ErrorActionPreference = ''Stop''')) {
        throw "PowerShell strict error policy missing: $($powerShellFile.FullName)"
    }
    if (-not $scriptSource.Contains('Set-StrictMode -Version 2.0')) {
        throw "PowerShell StrictMode missing: $($powerShellFile.FullName)"
    }

    $parameterAsts = @($scriptAst.FindAll({
        param($astNode)
        $astNode -is [System.Management.Automation.Language.ParameterAst]
    }, $true))
    foreach ($parameterAst in $parameterAsts) {
        $parameterName = [string]$parameterAst.Name.VariablePath.UserPath
        if ($protectedAutomaticVariables -contains $parameterName) {
            throw "PowerShell automatic variable used as parameter: $($powerShellFile.FullName):$($parameterAst.Extent.StartLineNumber): $('$' + $parameterName)"
        }
    }

    $assignments = @($scriptAst.FindAll({
        param($astNode)
        $astNode -is [System.Management.Automation.Language.AssignmentStatementAst]
    }, $true))
    foreach ($assignment in $assignments) {
        if ($assignment.Left -isnot [System.Management.Automation.Language.VariableExpressionAst]) { continue }
        $assignedVariable = [string]$assignment.Left.VariablePath.UserPath
        if ($protectedAutomaticVariables -contains $assignedVariable) {
            throw "PowerShell automatic variable assignment: $($powerShellFile.FullName):$($assignment.Extent.StartLineNumber): $('$' + $assignedVariable)"
        }
    }

    $variableAsts = @($scriptAst.FindAll({
        param($astNode)
        $astNode -is [System.Management.Automation.Language.VariableExpressionAst]
    }, $true))
    foreach ($variableAst in $variableAsts) {
        $variableName = [string]$variableAst.VariablePath.UserPath
        if ($forbiddenAutomaticReferences -contains $variableName) {
            throw "PowerShell forbidden automatic variable reference: $($powerShellFile.FullName):$($variableAst.Extent.StartLineNumber): $('$' + $variableName)"
        }
    }
}

# Catch the specific parser hazard where an expandable string contains
# $variable: and PowerShell treats the colon as a scope/drive separator.
# Scoped forms such as $env:NAME are valid; braced forms such as ${name}: are safe.
$validScopedPrefixes = @('env','global','script','local','private','using','variable','function','alias')
foreach ($powerShellFile in $powerShellFiles) {
    $sourceLines = @(Get-Content -LiteralPath $powerShellFile.FullName)
    for ($lineIndex = 0; $lineIndex -lt (Get-N2CCollectionCount -Value $sourceLines); $lineIndex++) {
        $sourceLine = [string]$sourceLines[$lineIndex]
        if ($sourceLine -notmatch '"') { continue }
        $unsafeMatches = [regex]::Matches($sourceLine, '(?<![`${])\$(?<name>[A-Za-z_][A-Za-z0-9_]*):')
        foreach ($unsafeMatch in $unsafeMatches) {
            $name = [string]$unsafeMatch.Groups['name'].Value
            if ($validScopedPrefixes -contains $name) { continue }
            throw ('Unsafe PowerShell interpolation: {0}:{1}: use ${{{2}}}: or the -f operator' -f $powerShellFile.FullName, ($lineIndex + 1), $name)
        }
    }
}

$jsonFiles = @(Get-ChildItem -LiteralPath $plugin -Filter *.json -File -Recurse | Sort-Object FullName)
foreach ($jsonFile in $jsonFiles) {
    try { $null = Get-Content -LiteralPath $jsonFile.FullName -Raw | ConvertFrom-Json }
    catch { throw "Invalid JSON: $($jsonFile.FullName): $($_.Exception.Message)" }
}

$p0 = Get-Content -LiteralPath (Join-Path $plugin 'Source\Tests\Fixtures\N2C_P0_FIXTURE_MANIFEST_V1.json') -Raw | ConvertFrom-Json
$p0Schema = [string](Get-N2CJsonPropertyValue -Object $p0 -Names @('schema') -Required -Context 'P0 fixture manifest')
$p0FixtureCount = [int](Get-N2CJsonPropertyValue -Object $p0 -Names @('fixture_count') -Required -Context 'P0 fixture manifest')
$p0Fixtures = @((Get-N2CJsonPropertyValue -Object $p0 -Names @('fixtures') -Required -Context 'P0 fixture manifest'))
if ($p0Schema -ne 'N2C_P0_FIXTURE_MANIFEST_V1') { throw 'Unsupported P0 fixture manifest schema.' }
if ($p0FixtureCount -ne (Get-N2CCollectionCount -Value $p0Fixtures)) { throw 'P0 fixture_count does not match fixtures[].' }
$p0FixtureIds = @($p0Fixtures | ForEach-Object { [string](Get-N2CJsonPropertyValue -Object $_ -Names @('fixture_id') -Required -Context 'P0 fixture') })
if ((Get-N2CCollectionCount -Value @($p0FixtureIds | Sort-Object -Unique)) -ne (Get-N2CCollectionCount -Value $p0Fixtures)) { throw 'Duplicate P0 fixture_id found.' }

$manual = Get-Content -LiteralPath (Join-Path $plugin 'Source\Tests\Fixtures\ManualReplay\N2C_MANUAL_REPLAY_MANIFEST_V1.json') -Raw | ConvertFrom-Json
$manualVersion = [int](Get-N2CJsonPropertyValue -Object $manual -Names @('version') -Required -Context 'ManualReplay manifest')
$manualPluginVersion = [int](Get-N2CJsonPropertyValue -Object $manual -Names @('plugin_version') -Required -Context 'ManualReplay manifest')
$manualVersionName = [string](Get-N2CJsonPropertyValue -Object $manual -Names @('version_name') -Required -Context 'ManualReplay manifest')
$manualSchema = [string](Get-N2CJsonPropertyValue -Object $manual -Names @('schema') -Required -Context 'ManualReplay manifest')
$manualCaseCount = [int](Get-N2CJsonPropertyValue -Object $manual -Names @('case_count') -Required -Context 'ManualReplay manifest')
$manualCases = @((Get-N2CJsonPropertyValue -Object $manual -Names @('cases') -Required -Context 'ManualReplay manifest'))
$manualFixtures = @((Get-N2CJsonPropertyValue -Object $manual -Names @('fixtures') -Required -Context 'ManualReplay manifest'))
if ($manualVersion -ne $releaseVersion -or $manualPluginVersion -ne $releaseVersion) { throw "ManualReplay manifest version mismatch: expected $releaseVersion, got version=$manualVersion plugin_version=$manualPluginVersion." }
if ($manualVersionName -ne $releaseVersionName) { throw "ManualReplay manifest version_name mismatch: expected $releaseVersionName, got $manualVersionName." }
if ($manualSchema -ne 'N2C_MANUAL_REPLAY_MANIFEST_V1') { throw 'Unsupported ManualReplay manifest schema.' }
if ($manualCaseCount -ne (Get-N2CCollectionCount -Value $manualCases)) { throw 'ManualReplay case_count does not match cases[].' }
$manualCaseIds = @($manualCases | ForEach-Object { [string](Get-N2CJsonPropertyValue -Object $_ -Names @('id') -Required -Context 'ManualReplay case') })
if ((Get-N2CCollectionCount -Value @($manualCaseIds | Sort-Object -Unique)) -ne (Get-N2CCollectionCount -Value $manualCases)) { throw 'Duplicate ManualReplay case id found.' }
$processGroups = @($manualCases | Group-Object { [string](Get-N2CJsonPropertyValue -Object $_ -Names @('process') -Required -Context 'ManualReplay case') })
$processCounts = @{}
foreach ($group in $processGroups) { $processCounts[[string]$group.Name] = (Get-N2CCollectionCount -Value @($group.Group)) }
if ($processCounts['main'] -ne 21 -or $processCounts['restore_first'] -ne 1 -or $processCounts['restore_second'] -ne 1) { throw 'ManualReplay process counts must be main=21, restore_first=1, restore_second=1.' }
foreach ($fixtureName in $manualFixtures) {
    if (-not (Test-Path -LiteralPath (Join-Path $plugin ('Source\Tests\Fixtures\ManualReplay\' + [string]$fixtureName)) -PathType Leaf)) { throw "ManualReplay fixture missing: $fixtureName" }
}

$finalManual = Get-Content -LiteralPath (Join-Path $plugin $finalManualFileName) -Raw | ConvertFrom-Json
$finalManualVersion = [int](Get-N2CJsonPropertyValue -Object $finalManual -Names @('target_plugin_version') -Required -Context 'final manual JSON')
$finalManualVersionName = [string](Get-N2CJsonPropertyValue -Object $finalManual -Names @('target_plugin_version_name') -Required -Context 'final manual JSON')
$finalManualAssets = @((Get-N2CJsonPropertyValue -Object $finalManual -Names @('assets') -Required -Context 'final manual JSON'))
if ($finalManualVersion -ne $releaseVersion) { throw "Final manual target_plugin_version mismatch: expected $releaseVersion, got $finalManualVersion." }
if ($finalManualVersionName -ne $releaseVersionName) { throw "Final manual target_plugin_version_name mismatch: expected $releaseVersionName, got $finalManualVersionName." }
$fixtureFinalManualPath = Join-Path $plugin ("Source\Tests\Fixtures\ManualReplay\{0}" -f $finalManualFileName)
if (-not (Test-Path -LiteralPath $fixtureFinalManualPath -PathType Leaf)) { throw "Final manual fixture copy missing: $fixtureFinalManualPath" }
if ((Get-N2CFileSha256 -LiteralPath (Join-Path $plugin $finalManualFileName)) -ne (Get-N2CFileSha256 -LiteralPath $fixtureFinalManualPath)) { throw 'Root and ManualReplay final manual JSON copies differ.' }
$collapsedActions = @($finalManualAssets | ForEach-Object {
    $assetActions = @((Get-N2CJsonPropertyValue -Object $_ -Names @('actions') -Required -Context 'final manual asset'))
    $assetActions
} | Where-Object {
    [string](Get-N2CJsonPropertyValue -Object $_ -Names @('type') -Required -Context 'final manual action') -in @('create_collapsed_graph','replace_collapsed_graph')
})
if ((Get-N2CCollectionCount -Value $collapsedActions) -ne 1) { throw "Final manual JSON must contain exactly one collapsed graph action; got $((Get-N2CCollectionCount -Value $collapsedActions))." }
$manualBoundIdentity = [string](Get-N2CJsonPropertyValue -Object $collapsedActions[0] -Names @('bound_graph_identity') -Required -Context 'final manual collapsed graph')
if ($manualBoundIdentity.Contains('.K2Node_Composite_')) { throw 'Final manual JSON hardcodes an unstable generated Composite object identity.' }
if (-not $manualBoundIdentity.EndsWith(':EventGraph.N2C_FinalCollapsed', [System.StringComparison]::Ordinal)) { throw 'Final manual JSON canonical collapsed graph identity is missing or unexpected.' }

$structPinCounts = @{
    make_struct_edges = 0
    break_struct_edges = 0
    set_fields_in_edges = 0
    set_fields_out_edges = 0
}
$edgeSchemaCounts = @{ node_id = 0; compact = 0 }
$spawnActorCount = 0
$manualPatchScopes = @{}
foreach ($manualAsset in $finalManualAssets) {
    $assetPath = [string](Get-N2CJsonPropertyValue -Object $manualAsset -Names @('blueprint_path') -Required -Context 'Final manual asset')
    $actionIndex = 0
    $manualAssetActions = @((Get-N2CJsonPropertyValue -Object $manualAsset -Names @('actions') -Required -Context ("Final manual asset '{0}'" -f $assetPath)))
    foreach ($manualAction in $manualAssetActions) {
        $actionType = [string](Get-N2CJsonPropertyValue -Object $manualAction -Names @('type') -Required -Context ("Final manual asset '{0}' action[{1}]" -f $assetPath, $actionIndex))
        if ($actionType -eq 'patch_graph') {
            $importScope = [string](Get-N2CJsonPropertyValue -Object $manualAction -Names @('import_scope','action_id') -Required -Context ("Final manual asset '{0}' action[{1}] patch_graph identity" -f $assetPath, $actionIndex))
            if ([string]::IsNullOrWhiteSpace($importScope)) { throw ("Final manual asset '{0}' action[{1}] patch_graph import_scope/action_id is empty." -f $assetPath, $actionIndex) }
            $graphName = [string](Get-N2CJsonPropertyValue -Object $manualAction -Names @('graph_name') -Required -Context ("Final manual asset '{0}' action[{1}] patch_graph graph" -f $assetPath, $actionIndex))
            $scopeKey = "{0}|{1}|{2}" -f $assetPath, $graphName, $importScope
            if ($manualPatchScopes.ContainsKey($scopeKey)) { throw ("Duplicate final manual patch_graph scope: {0}" -f $scopeKey) }
            $manualPatchScopes[$scopeKey] = $true
        }
        $nodeMap = @{}
        $manualNodes = @()
        $manualNodesValue = Get-N2CJsonPropertyValue -Object $manualAction -Names @('nodes') -Context ("Final manual asset '{0}' action[{1}]" -f $assetPath, $actionIndex)
        if ($null -ne $manualNodesValue) { $manualNodes = @($manualNodesValue) }
        foreach ($manualNode in $manualNodes) {
            if ($null -eq $manualNode) { continue }
            $nodeId = [string](Get-N2CJsonPropertyValue -Object $manualNode -Names @('id') -Required -Context ("Final manual asset '{0}' action[{1}] node" -f $assetPath, $actionIndex))
            if ([string]::IsNullOrWhiteSpace($nodeId)) { throw ("Final manual asset '{0}' action[{1}] contains an empty node id." -f $assetPath, $actionIndex) }
            if ($nodeMap.ContainsKey($nodeId)) { throw ("Final manual asset '{0}' action[{1}] contains duplicate node id '{2}'." -f $assetPath, $actionIndex, $nodeId) }
            $nodeMap[$nodeId] = $manualNode
        }

        $incomingSpawnTransforms = @{}
        $manualDataEdges = @()
        $manualDataEdgesValue = Get-N2CJsonPropertyValue -Object $manualAction -Names @('data_edges') -Context ("Final manual asset '{0}' action[{1}]" -f $assetPath, $actionIndex)
        if ($null -ne $manualDataEdgesValue) { $manualDataEdges = @($manualDataEdgesValue) }
        for ($edgeIndex = 0; $edgeIndex -lt (Get-N2CCollectionCount -Value $manualDataEdges); $edgeIndex++) {
            $manualEdge = $manualDataEdges[$edgeIndex]
            if ($null -eq $manualEdge) { continue }
            $edgeContext = "Final manual asset '{0}' action[{1}] type='{2}' data_edges[{3}]" -f $assetPath, $actionIndex, $actionType, $edgeIndex
            $resolvedEdge = Resolve-N2CManualEdge -Edge $manualEdge -Context $edgeContext
            $edgeSchemaCounts[$resolvedEdge.Schema]++

            if (-not $nodeMap.ContainsKey($resolvedEdge.SourceId)) { throw ("{0}: unknown source node id '{1}'." -f $edgeContext, $resolvedEdge.SourceId) }
            if (-not $nodeMap.ContainsKey($resolvedEdge.TargetId)) { throw ("{0}: unknown target node id '{1}'." -f $edgeContext, $resolvedEdge.TargetId) }
            $sourceNode = $nodeMap[$resolvedEdge.SourceId]
            $targetNode = $nodeMap[$resolvedEdge.TargetId]
            $fromPin = [string]$resolvedEdge.FromPin
            $toPin = [string]$resolvedEdge.ToPin
            $sourceType = [string](Get-N2CJsonPropertyValue -Object $sourceNode -Names @('type') -Required -Context ("{0} source node '{1}'" -f $edgeContext, $resolvedEdge.SourceId))
            $targetType = [string](Get-N2CJsonPropertyValue -Object $targetNode -Names @('type') -Required -Context ("{0} target node '{1}'" -f $edgeContext, $resolvedEdge.TargetId))

            if ($sourceType -eq 'K2Node_MakeStruct') {
                $structPinCounts.make_struct_edges++
                $sourceStructPath = [string](Get-N2CJsonPropertyValue -Object $sourceNode -Names @('struct_path') -Required -Context ("{0} MakeStruct" -f $edgeContext))
                $expectedPin = ($sourceStructPath -split '\.')[-1]
                if ($fromPin -ne $expectedPin) { throw "Final manual MakeStruct output must be canonical '$expectedPin', got '$fromPin'." }
            }
            if ($targetType -eq 'K2Node_BreakStruct') {
                $structPinCounts.break_struct_edges++
                $targetStructPath = [string](Get-N2CJsonPropertyValue -Object $targetNode -Names @('struct_path') -Required -Context ("{0} BreakStruct" -f $edgeContext))
                $expectedPin = ($targetStructPath -split '\.')[-1]
                if ($toPin -ne $expectedPin) { throw "Final manual BreakStruct input must be canonical '$expectedPin', got '$toPin'." }
            }
            if ($sourceType -eq 'K2Node_SetFieldsInStruct') {
                $structPinCounts.set_fields_out_edges++
                if ($fromPin -ne 'StructOut') { throw "Final manual SetFields output must be StructOut, got '$fromPin'." }
            }
            if ($targetType -eq 'K2Node_SetFieldsInStruct') {
                $structPinCounts.set_fields_in_edges++
                if ($toPin -ne 'StructRef') { throw "Final manual SetFields input must be StructRef, got '$toPin'." }
            }
            if ($targetType -eq 'K2Node_Select' -and $toPin -in @('A','B','False','True','false','true')) {
                throw "Final manual Select uses compatibility alias '$toPin'; use Option 0/Option 1 or exported enum PinName."
            }
            if ($sourceType -eq 'K2Node_Knot' -and $fromPin -ne 'OutputPin') { throw "Final manual Knot output must be OutputPin, got '$fromPin'." }
            if ($targetType -eq 'K2Node_Knot' -and $toPin -ne 'InputPin') { throw "Final manual Knot input must be InputPin, got '$toPin'." }
            if ($sourceType -eq 'K2Node_GetDataTableRow' -and $fromPin -in @('RowFound','Out Row','OutRow')) { throw "Final manual GetDataTableRow uses friendly alias '$fromPin'." }
            if ($sourceType -eq 'K2Node_SwitchEnum' -and $fromPin -eq 'Default') { throw 'Final manual SwitchEnum uses non-canonical Default output.' }
            if ($sourceType -eq 'K2Node_CreateDelegate' -and $fromPin -ne 'OutputDelegate') { throw "Final manual CreateDelegate output must be canonical OutputDelegate, got '$fromPin'." }
            if ($targetType -eq 'K2Node_SpawnActorFromClass' -and (($toPin -replace '[^A-Za-z0-9]', '').ToLowerInvariant() -eq 'spawntransform')) {
                $incomingSpawnTransforms[$resolvedEdge.TargetId] = $true
            }
        }
        foreach ($manualNodeId in @($nodeMap.Keys)) {
            $manualNode = $nodeMap[$manualNodeId]
            $manualNodeType = [string](Get-N2CJsonPropertyValue -Object $manualNode -Names @('type') -Required -Context ("Final manual node '{0}'" -f $manualNodeId))
            if ($manualNodeType -ne 'K2Node_SpawnActorFromClass') { continue }
            $spawnActorCount++
            $manualPinDefaults = Get-N2CJsonPropertyValue -Object $manualNode -Names @('pin_defaults') -Context ("Final manual node '{0}'" -f $manualNodeId)
            if ($null -ne $manualPinDefaults -and $null -ne (Get-N2CJsonPropertyValue -Object $manualPinDefaults -Names @('SpawnTransform') -Context ("Final manual node '{0}' pin_defaults" -f $manualNodeId))) {
                throw "Final manual SpawnActorFromClass '$manualNodeId' must not use a literal SpawnTransform default."
            }
            if (-not $incomingSpawnTransforms.ContainsKey($manualNodeId)) {
                throw "Final manual SpawnActorFromClass '$manualNodeId' requires an incoming SpawnTransform edge."
            }
        }
        $actionIndex++
    }
}
if ([int]$edgeSchemaCounts.node_id -lt 1 -or [int]$edgeSchemaCounts.compact -lt 1) {
    throw ("Final manual JSON must exercise both edge schemas; node_id={0}, compact={1}." -f $edgeSchemaCounts.node_id, $edgeSchemaCounts.compact)
}
foreach ($requiredStructCounter in @('make_struct_edges','break_struct_edges','set_fields_in_edges','set_fields_out_edges')) {
    if ([int]$structPinCounts[$requiredStructCounter] -lt 1) { throw "Final manual connected struct pipeline is missing: $requiredStructCounter" }
}
if ($spawnActorCount -lt 1) { throw 'Final manual JSON is missing a compile-safe SpawnActorFromClass fixture.' }

$testSource = Get-Content -LiteralPath (Join-Path $plugin 'Source\Private\N2CVerificationTests.cpp') -Raw
$requiredTests = @(
    'NodeToCode.ManualReplay.FlowAndArrays',
    'NodeToCode.ManualReplay.StructAndDataTable',
    'NodeToCode.ManualReplay.Enum',
    'NodeToCode.ManualReplay.ContextualEventGraph',
    'NodeToCode.ManualReplay.Delegates',
    'NodeToCode.ManualReplay.FunctionBoundaries',
    'NodeToCode.ManualReplay.StandardMacros',
    'NodeToCode.ManualReplay.GraphBoundaries',
    'NodeToCode.ManualReplay.Widget',
    'NodeToCode.ManualReplay.AIController',
    'NodeToCode.ManualReplay.BTTask',
    'NodeToCode.ManualReplay.BTService',
    'NodeToCode.ManualReplay.BTDecorator',
    'NodeToCode.ManualReplay.PreflightRejectsWithoutMutation',
    'NodeToCode.ManualReplay.SandboxPinPreflight',
    'NodeToCode.ManualReplay.RollbackAfterMutation',
    'NodeToCode.ManualReplay.RollbackStructuralEquality',
    'NodeToCode.RestoreFirstPass.ManualReplayPendingRestore',
    'NodeToCode.RestoreSecondPass.ManualReplayPendingRestore',
    'NodeToCode.ManualReplay.DialogDiagnostics',
    'NodeToCode.ManualReplay.ToolbarCommands',
    'NodeToCode.ManualReplay.RawByteDefaultReopenExport',
    'NodeToCode.ManualReplay.MissingEnumReject'
)
$missingTests = @($requiredTests | Where-Object { -not $testSource.Contains($_) })
if ((Get-N2CCollectionCount -Value $missingTests)) { throw "Required automation tests missing from C++ registration: $($missingTests -join ', ')" }
if (-not $testSource.Contains('NodeToCode.Verification.P0GraphBoundaries.CompositeCanonicalIdentityRepeat')) { throw 'Composite canonical identity repeat regression is missing.' }
$headlessHarnessMarkers = @(
    'ApplyPatchToBlueprint(Blueprint, Patch, FirstReport, false, false)',
    'ApplyPatchToBlueprint(Reloaded, Patch, SecondReport, false, false)',
    'ApplyPatchToBlueprint(Blueprint, GoodPatch, GoodApplyReport, false, false)',
    'DryRunPatch(Blueprint, GoodPatch, GoodDryRunReport, false)',
    'CountPendingRestoreManifests()',
    'bPersistedLinks',
    'bSecondSaved'
)
$headlessHarnessMarkers += @('Struct_ConnectedSetFields','Struct_LegacyMakeBreakAliases','StructType FName','struct_pin=%s')
foreach ($headlessHarnessMarker in $headlessHarnessMarkers) {
    if (-not $testSource.Contains($headlessHarnessMarker)) { throw "Headless regression harness marker missing: $headlessHarnessMarker" }
}
$manifestTests = @($manualCases | ForEach-Object { [string](Get-N2CJsonPropertyValue -Object $_ -Names @('test') -Required -Context 'ManualReplay case') } | Sort-Object)
$requiredSorted = @($requiredTests | Sort-Object)
if (($manifestTests -join "`n") -ne ($requiredSorted -join "`n")) { throw 'ManualReplay manifest test membership differs from the required release gate.' }


$nodeMatrix = Get-Content -LiteralPath (Join-Path $plugin 'Source\Tests\Fixtures\N2C_NODE_TEST_REQUIREMENTS_V1.json') -Raw | ConvertFrom-Json
$nodeMatrixSchema = [string](Get-N2CJsonPropertyValue -Object $nodeMatrix -Names @('schema') -Required -Context 'new-node test requirement matrix')
$nodeMatrixPolicy = [string](Get-N2CJsonPropertyValue -Object $nodeMatrix -Names @('policy') -Required -Context 'new-node test requirement matrix')
$nodeMatrixVersion = [int](Get-N2CJsonPropertyValue -Object $nodeMatrix -Names @('version') -Required -Context 'new-node test requirement matrix')
if ($nodeMatrixSchema -ne 'N2C_NODE_TEST_REQUIREMENTS_V1' -or $nodeMatrixPolicy -ne 'N2C_NEW_NODE_TEST_GATE') { throw 'Unsupported new-node test requirement matrix.' }
if ($nodeMatrixVersion -ne $releaseVersion) { throw "New-node test requirement matrix version mismatch: expected $releaseVersion, got $nodeMatrixVersion." }
$capabilities = @((Get-N2CJsonPropertyValue -Object $nodeMatrix -Names @('capabilities') -Required -Context 'new-node test requirement matrix'))
if ((Get-N2CCollectionCount -Value $capabilities) -lt 10) { throw 'New-node test requirement matrix is unexpectedly small.' }
$capabilityIds = @($capabilities | ForEach-Object { [string](Get-N2CJsonPropertyValue -Object $_ -Names @('id') -Required -Context 'capability') })
if ((Get-N2CCollectionCount -Value @($capabilityIds | Sort-Object -Unique)) -ne (Get-N2CCollectionCount -Value $capabilities)) { throw 'Duplicate capability id in new-node test requirement matrix.' }
foreach ($capability in $capabilities) {
    $capabilityId = [string](Get-N2CJsonPropertyValue -Object $capability -Names @('id') -Required -Context 'capability')
    $status = [string](Get-N2CJsonPropertyValue -Object $capability -Names @('status') -Required -Context ("capability {0}" -f $capabilityId))
    if ($status -notin @('verified','guarded','deferred')) { throw "Invalid capability status: $capabilityId=$status" }
    if ($status -eq 'verified') {
        $positiveTestsValue = Get-N2CJsonPropertyValue -Object $capability -Names @('positive_tests') -Context ("capability {0}" -f $capabilityId)
        $negativeTestsValue = Get-N2CJsonPropertyValue -Object $capability -Names @('negative_tests') -Context ("capability {0}" -f $capabilityId)
        $freshProcessValue = Get-N2CJsonPropertyValue -Object $capability -Names @('fresh_process_required') -Context ("capability {0}" -f $capabilityId)
        $positiveTests = @($positiveTestsValue)
        $negativeTests = @($negativeTestsValue)
        if ((Get-N2CCollectionCount -Value $positiveTests) -eq 0) { throw "Verified capability lacks a positive regression: $capabilityId" }
        if (-not [bool]$freshProcessValue -and $capabilityId -ne 'editor_ui_contract') {
            throw "Verified runtime capability does not require fresh-process persistence: $capabilityId"
        }
        foreach ($mappedTest in $positiveTests + $negativeTests) {
            $mappedTestName = [string]$mappedTest
            $isContractMatrixTest = $mappedTestName.StartsWith('NodeToCode.ContractMatrix.', [StringComparison]::Ordinal)
            if (($requiredTests -notcontains $mappedTestName) -and -not $isContractMatrixTest) { throw "Capability references a non-mandatory test: $capabilityId -> $mappedTestName" }
            if (-not $testSource.Contains($mappedTestName)) { throw "Capability references an unregistered test: $capabilityId -> $mappedTestName" }
        }
    }
    else {
        $releaseClaimAllowedValue = Get-N2CJsonPropertyValue -Object $capability -Names @('release_claim_allowed') -Context ("capability {0}" -f $capabilityId)
        $releaseClaimAllowed = if ($null -eq $releaseClaimAllowedValue) { $false } else { [bool]$releaseClaimAllowedValue }
        if ($releaseClaimAllowed) {
            throw "Deferred/guarded capability may not allow a verified release claim: $capabilityId"
        }
    }
}
$animationGate = @($capabilities | Where-Object { [string](Get-N2CJsonPropertyValue -Object $_ -Names @('id') -Required -Context 'capability') -eq 'animation_graph' })
$animationReleaseClaimValue = if ((Get-N2CCollectionCount -Value $animationGate) -eq 1) { Get-N2CJsonPropertyValue -Object $animationGate[0] -Names @('release_claim_allowed') -Context 'animation_graph capability' } else { $null }
$animationReleaseClaimAllowed = if ($null -eq $animationReleaseClaimValue) { $false } else { [bool]$animationReleaseClaimValue }
if ((Get-N2CCollectionCount -Value $animationGate) -ne 1 -or [string](Get-N2CJsonPropertyValue -Object $animationGate[0] -Names @('status') -Required -Context 'animation_graph capability') -ne 'deferred' -or $animationReleaseClaimAllowed) {
    throw 'Animation/AnimGraph deferred gate is missing or incorrectly claimable.'
}
$gateDocs = @(
    (Join-Path $plugin 'CODEX_PLUGIN_START_HERE.md'),
    (Join-Path $plugin 'Source\Documentation\AGENTS.md'),
    (Join-Path $plugin 'Source\Documentation\N2C_VERIFICATION_WORKFLOW.md'),
    (Join-Path $plugin 'Source\Documentation\N2C_UE427_SOURCE_WORKFLOW.md')
)
foreach ($gateDoc in $gateDocs) {
    if (-not (Get-Content -LiteralPath $gateDoc -Raw).Contains('N2C_NEW_NODE_TEST_GATE')) { throw "Mandatory new-node test gate marker missing: $gateDoc" }
}

$importerSource = Get-Content -LiteralPath (Join-Path $plugin 'Source\Private\Core\N2CPatchImporter.cpp') -Raw

$contractMatrixPath = Join-Path $plugin 'Source\Tests\Fixtures\N2C_IMPORT_CONTRACT_MATRIX_V1.json'
$contractMatrix = Get-Content -LiteralPath $contractMatrixPath -Raw | ConvertFrom-Json
$contractMatrixSchema = [string](Get-N2CJsonPropertyValue -Object $contractMatrix -Names @('schema') -Required -Context 'import contract matrix')
$contractMatrixVersion = [int](Get-N2CJsonPropertyValue -Object $contractMatrix -Names @('version') -Required -Context 'import contract matrix')
if ($contractMatrixSchema -ne 'N2C_IMPORT_CONTRACT_MATRIX_V1') { throw 'Unsupported import contract matrix schema.' }
if ($contractMatrixVersion -ne $releaseVersion) { throw "Import contract matrix version mismatch: expected $releaseVersion, got $contractMatrixVersion." }
$contractCases = @((Get-N2CJsonPropertyValue -Object $contractMatrix -Names @('cases') -Required -Context 'import contract matrix'))
$declaredContractCaseCount = [int](Get-N2CJsonPropertyValue -Object $contractMatrix -Names @('case_count') -Required -Context 'import contract matrix')
if ((Get-N2CCollectionCount -Value $contractCases) -ne $declaredContractCaseCount -or (Get-N2CCollectionCount -Value $contractCases) -lt 100) {
    throw "Import contract matrix case count mismatch/too small: declared=$declaredContractCaseCount actual=$((Get-N2CCollectionCount -Value $contractCases))."
}
$contractCaseIds = @($contractCases | ForEach-Object { [string](Get-N2CJsonPropertyValue -Object $_ -Names @('id') -Required -Context 'contract case') })
if ((Get-N2CCollectionCount -Value @($contractCaseIds | Sort-Object -Unique)) -ne (Get-N2CCollectionCount -Value $contractCaseIds)) { throw 'Duplicate id in import contract matrix.' }
foreach ($contractCase in $contractCases) {
    $contractCaseId = [string](Get-N2CJsonPropertyValue -Object $contractCase -Names @('id') -Required -Context 'contract case')
    $contractPatch = Get-N2CJsonPropertyValue -Object $contractCase -Names @('patch') -Required -Context ("contract case {0}" -f $contractCaseId)
    $expectedApplyValue = Get-N2CJsonPropertyValue -Object $contractCase -Names @('expected_apply') -Required -Context ("contract case {0}" -f $contractCaseId)
    $freshRequiredValue = Get-N2CJsonPropertyValue -Object $contractCase -Names @('fresh_session_required') -Required -Context ("contract case {0}" -f $contractCaseId)
    $reapplyRequiredValue = Get-N2CJsonPropertyValue -Object $contractCase -Names @('reapply_required') -Required -Context ("contract case {0}" -f $contractCaseId)
    if ($null -eq $contractPatch -or -not [bool]$freshRequiredValue -or -not [bool]$reapplyRequiredValue) {
        throw "Contract case is not gated by two fresh processes and idempotent reapply: $contractCaseId"
    }
    if ([bool]$expectedApplyValue) {
        $expectedObject = Get-N2CJsonPropertyValue -Object $contractCase -Names @('expected') -Required -Context ("contract case {0}" -f $contractCaseId)
        if ((Get-N2CCollectionCount -Value @($expectedObject.PSObject.Properties)) -eq 0) { throw "Positive contract case lacks a semantic assertion: $contractCaseId" }
    }

    $graphActionTypesRequiringStableScope = @(
        'patch_graph',
        'patch_event_graph',
        'patch_widget_graph',
        'patch_animation_graph',
        'add_event_graph_nodes',
        'add_nodes_to_graph'
    )
    foreach ($contractPatchProperty in @('setup_patch', 'patch')) {
        $casePatchValue = Get-N2CJsonPropertyValue -Object $contractCase -Names @($contractPatchProperty) -Context ("contract case {0}" -f $contractCaseId)
        if ($null -eq $casePatchValue) { continue }
        $seenStableScopes = @{}
        $caseActions = @((Get-N2CJsonPropertyValue -Object $casePatchValue -Names @('actions') -Required -Context ("contract case {0} {1}" -f $contractCaseId, $contractPatchProperty)))
        for ($caseActionIndex = 0; $caseActionIndex -lt (Get-N2CCollectionCount -Value $caseActions); $caseActionIndex++) {
            $caseAction = $caseActions[$caseActionIndex]
            $caseActionType = [string](Get-N2CJsonPropertyValue -Object $caseAction -Names @('type') -Required -Context ("contract case {0} {1} action[{2}]" -f $contractCaseId, $contractPatchProperty, $caseActionIndex))
            $caseActionNodesValue = Get-N2CJsonPropertyValue -Object $caseAction -Names @('nodes') -Context ("contract case {0} {1} action[{2}]" -f $contractCaseId, $contractPatchProperty, $caseActionIndex)
            $caseActionNodes = if ($null -eq $caseActionNodesValue) { @() } else { @($caseActionNodesValue) }
            if (($graphActionTypesRequiringStableScope -notcontains $caseActionType) -or (Get-N2CCollectionCount -Value $caseActionNodes) -eq 0) { continue }

            $stableScope = [string](Get-N2CJsonPropertyValue -Object $caseAction -Names @('import_scope','action_id') -Required -Context ("contract case {0} {1} action[{2}] graph identity" -f $contractCaseId, $contractPatchProperty, $caseActionIndex))
            if ([string]::IsNullOrWhiteSpace($stableScope)) {
                throw "Contract matrix node-bearing graph action lacks import_scope/action_id: $contractCaseId $contractPatchProperty action[$caseActionIndex]"
            }
            $scopeKey = ("{0}|{1}" -f $caseActionType, $stableScope)
            if ($seenStableScopes.ContainsKey($scopeKey)) {
                throw "Duplicate stable graph import scope inside contract patch: $contractCaseId $contractPatchProperty $scopeKey"
            }
            $seenStableScopes[$scopeKey] = $true
        }
    }
}

$coverageObject = Get-N2CJsonPropertyValue -Object $contractMatrix -Names @('coverage') -Required -Context 'import contract matrix'
$actionCoverage = @((Get-N2CJsonPropertyValue -Object $coverageObject -Names @('actions') -Required -Context 'contract coverage'))
$nodeCoverage = @((Get-N2CJsonPropertyValue -Object $coverageObject -Names @('nodes') -Required -Context 'contract coverage'))
$guardCoverage = @((Get-N2CJsonPropertyValue -Object $coverageObject -Names @('guards') -Required -Context 'contract coverage'))
foreach ($coverageRow in $actionCoverage + $nodeCoverage + $guardCoverage) {
    $coverageToken = [string](Get-N2CJsonPropertyValue -Object $coverageRow -Names @('token') -Required -Context 'contract coverage row')
    $coverageCases = @((Get-N2CJsonPropertyValue -Object $coverageRow -Names @('cases') -Required -Context ("coverage {0}" -f $coverageToken)))
    if ((Get-N2CCollectionCount -Value $coverageCases) -eq 0) { throw "Import contract coverage token has no tests: $coverageToken" }
    foreach ($coverageCase in $coverageCases) {
        $coverageCaseName = [string]$coverageCase
        if (-not $coverageCaseName.StartsWith('NodeToCode.', [StringComparison]::Ordinal) -and $contractCaseIds -notcontains $coverageCaseName) {
            throw "Import contract coverage references an unknown case: $coverageToken -> $coverageCaseName"
        }
        if ($coverageCaseName.StartsWith('NodeToCode.', [StringComparison]::Ordinal) -and -not $testSource.Contains($coverageCaseName)) {
            throw "Import contract coverage references an unregistered legacy test: $coverageToken -> $coverageCaseName"
        }
    }
}

$actionSupportStart = $importerSource.IndexOf('static bool IsEventGraphPatchAction', [StringComparison]::Ordinal)
$actionSupportEnd = $importerSource.IndexOf('static bool IsSupportedNodeType', [StringComparison]::Ordinal)
if ($actionSupportStart -lt 0 -or $actionSupportEnd -le $actionSupportStart) { throw 'Supported action type block could not be located.' }
$actionSupportBlock = $importerSource.Substring($actionSupportStart, $actionSupportEnd - $actionSupportStart)
$supportedActionTokens = @([regex]::Matches($actionSupportBlock, 'ActionType\s*==\s*TEXT\("([^"]+)"\)') | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique)
$mappedActionTokens = @($actionCoverage | ForEach-Object { [string](Get-N2CJsonPropertyValue -Object $_ -Names @('token') -Required -Context 'action coverage row') } | Sort-Object -Unique)
if (($supportedActionTokens -join "`n") -ne ($mappedActionTokens -join "`n")) {
    $missingActionMappings = @($supportedActionTokens | Where-Object { $mappedActionTokens -notcontains $_ })
    $unknownActionMappings = @($mappedActionTokens | Where-Object { $supportedActionTokens -notcontains $_ })
    throw "Import action contract coverage differs from importer support. Missing=$($missingActionMappings -join ','); unknown=$($unknownActionMappings -join ',')"
}
foreach ($nodeCoverageRow in $nodeCoverage) {
    $nodeToken = [string](Get-N2CJsonPropertyValue -Object $nodeCoverageRow -Names @('token') -Required -Context 'node coverage row')
    $nodeNeedle = $nodeToken.ToLowerInvariant()
    if (-not $importerSource.ToLowerInvariant().Contains($nodeNeedle)) {
        throw "Import node contract token is not present in importer source: $nodeToken"
    }
}
foreach ($guardCoverageRow in $guardCoverage) {
    $guardToken = [string](Get-N2CJsonPropertyValue -Object $guardCoverageRow -Names @('token') -Required -Context 'guard coverage row')
    if (-not $importerSource.Contains($guardToken)) { throw "Import guard contract token is not present in importer source: $guardToken" }
}
foreach ($contractTestMarker in @(
    'NodeToCode.ContractMatrix.Apply',
    'NodeToCode.ContractMatrix.VerifyFreshFirst',
    'NodeToCode.ContractMatrix.Reapply',
    'NodeToCode.ContractMatrix.VerifyFreshSecond',
    'NodeToCode.ContractMatrix.Cleanup',
    'N2C_IMPORT_CONTRACT|phase=',
    'N2C_IMPORT_CONTRACT_MATRIX_V1',
    'N2C_IMPORT_CONTRACT_STATE_V2',
    'semantic_hash',
    'BeforeSemanticHash == AfterSemanticHash'
)) {
    if (-not $testSource.Contains($contractTestMarker)) { throw "Import contract runtime harness marker missing: $contractTestMarker" }
}
foreach ($idempotenceImporterMarker in @('BuildStableGraphPatchScope','MakeStableGraphPatchNodeGuid','FindGraphNodeByStableGuid','Graph patch reused node','Graph edge reused')) {
    if (-not $importerSource.Contains($idempotenceImporterMarker)) { throw "Graph-patch idempotence marker missing: $idempotenceImporterMarker" }
}
if (-not $importerSource.Contains('CanonicalizeCompositeBoundGraphIdentity')) { throw 'Composite canonical identity importer helper is missing.' }
if (-not $importerSource.Contains('LogicalBlueprintPath')) { throw 'Transient sandbox logical Blueprint identity forwarding is missing.' }
foreach ($structPinMarker in @('UK2Node_MakeStruct','UK2Node_BreakStruct','bStructValueAlias','StructType->GetFName()')) {
    if (-not $importerSource.Contains($structPinMarker)) { throw "Struct pin compatibility marker missing: $structPinMarker" }
}
foreach ($delegatePinMarker in @('UK2Node_CreateDelegate','GetDelegateOutPin()','OutputDelegate','output_delegate','selection deferred until delegate links exist','create_delegate_signature_missing','create_delegate_function_selection_lost')) {
    if (-not $importerSource.Contains($delegatePinMarker)) { throw "CreateDelegate pin compatibility marker missing: $delegatePinMarker" }
}
if (-not $testSource.Contains('"from_pin":"Delegate"') -or -not $testSource.Contains('"from_pin":"OutputDelegate"')) { throw 'Delegate regression must exercise legacy Delegate and canonical OutputDelegate source pins.' }
foreach ($delegateTestMarker in @('"function_name":"N2C_P0_DelegateHandler"','"type":"add_or_replace_function","function_name":"N2C_P0_DelegateHandler"')) {
    if (-not $testSource.Contains($delegateTestMarker)) { throw "Same-patch CreateDelegate compile regression marker missing: $delegateTestMarker" }
}
foreach ($spawnContractMarker in @('ValidateSpawnActorTransformContract','spawn_transform_link_missing')) {
    if (-not $importerSource.Contains($spawnContractMarker)) { throw "SpawnActor compile contract marker missing: $spawnContractMarker" }
}
foreach ($spawnTestMarker in @('KismetMathLibrary.MakeTransform','"to_pin":"SpawnTransform"','SpawnActorTransformLinkReject')) {
    if (-not $testSource.Contains($spawnTestMarker)) { throw "SpawnActor compile regression test marker missing: $spawnTestMarker" }
}
foreach ($memberDefaultMarker in @('CanonicalizeStructMemberVariableDefaultValue','TryFormatK2CustomStructDefault','FDefaultValueHelper::IsStringValidRotator','member_default_import_text_invalid','pin_default_invalid','PPF_SerializedAsImportText')) {
    if (-not $importerSource.Contains($memberDefaultMarker)) { throw "Member struct-default compile contract marker missing: $memberDefaultMarker" }
}
foreach ($memberDefaultTestMarker in @('N2C_RotatorDefault','RunInvalidStructMemberDefaultReject')) {
    if (-not $testSource.Contains($memberDefaultTestMarker)) { throw "Member struct-default regression test marker missing: $memberDefaultTestMarker" }
}
foreach ($schemaDefaultMarker in @('GetPinDefaultValuesFromString','ApplySchemaPinDefaultValue','UseDefaultObject','UseDefaultText','IsPinDefaultValid')) {
    if (-not $importerSource.Contains($schemaDefaultMarker)) { throw "Schema-native pin-default marker missing: $schemaDefaultMarker" }
}
foreach ($forbiddenRawDefaultWrite in @('DataTablePin->DefaultValue = DataTable->GetPathName()','Pin->DefaultValue = GetStringFieldSafe(PinObj, TEXT("default_value"))')) {
    if ($importerSource.Contains($forbiddenRawDefaultWrite)) { throw "Raw Blueprint pin-default write bypasses schema conversion: $forbiddenRawDefaultWrite" }
}

$editorIntegrationSource = Get-Content -LiteralPath (Join-Path $plugin 'Source\Private\Core\N2CEditorIntegration.cpp') -Raw
if ($editorIntegrationSource.Contains('FCoreDelegates::OnPreExit') -or $editorIntegrationSource.Contains('ProcessPendingBackupRestoresOnPreExit')) {
    throw 'Pending restore must be startup-only and must not execute from OnPreExit.'
}
if (-not $importerSource.Contains('IsExplicitEnumBackedByteDeclaration')) { throw 'Raw Byte/enum-backed Byte distinction is missing.' }
if (-not $importerSource.Contains('N2C_ROLLBACK_RESULT|result=PASS')) { throw 'Verified rollback result marker is missing.' }
if ($importerSource.Contains('Transaction.Cancel()')) {
    $realCalls = (Get-N2CCollectionCount -Value ([regex]::Matches($importerSource, '(?m)^\s*Transaction\.Cancel\(\)')))
    if ($realCalls -gt 0) { throw 'Real Transaction.Cancel() rollback call is present.' }
}

$exporterSource = Get-Content -LiteralPath (Join-Path $plugin 'Source\Private\Core\N2CAIExport.cpp') -Raw
if (-not $exporterSource.Contains('BlueprintVariableDefaultToString')) { throw 'CDO-backed Blueprint variable default export helper is missing.' }
if (-not $exporterSource.Contains('PPF_SerializedAsImportText')) { throw 'Serialized member default export flag is missing.' }
if (-not $testSource.Contains('"shift":false,"ctrl":false,"alt":false,"cmd":false')) { throw 'Contextual InputKey fixture is missing persisted modifier flags.' }
if (-not $testSource.Contains('category=%s|subtype=%s|default=%s')) { throw 'Raw Byte export diagnostics marker is missing.' }

foreach ($k2DefaultTestMarker in @('RotatorPinDefaultReject','SpawnActorClassDefaultReject','schema_default_storage','BoundaryDefault','cdo_rotator','ContainerPtrToValuePtr<FRotator>')) {
    if (-not $testSource.Contains($k2DefaultTestMarker)) { throw "K2 Rotator default regression marker missing: $k2DefaultTestMarker" }
}

$packageScript = Get-Content -LiteralPath (Join-Path $plugin 'Scripts\Codex\Package-N2CPlugin.ps1') -Raw
$releaseMetadataScriptPaths = @(
    (Join-Path $plugin 'Scripts\Codex\Validate-N2CFiles.ps1'),
    (Join-Path $plugin 'Scripts\Codex\Package-N2CPlugin.ps1'),
    (Join-Path $plugin 'Scripts\Codex\validate_n2c_release.py')
)
foreach ($releaseMetadataScriptPath in $releaseMetadataScriptPaths) {
    $releaseMetadataSource = Get-Content -LiteralPath $releaseMetadataScriptPath -Raw
    if ([regex]::IsMatch($releaseMetadataSource, '(?i)(descriptor\.Version|\bversion)\s*(?:-ne|!=)\s*[0-9]{3}')) {
        throw "Hard-coded current release number found in release tooling: $releaseMetadataScriptPath"
    }
    if ([regex]::IsMatch($releaseMetadataSource, 'N2C_FINAL_MANUAL_ALL_STRICT_SUPPORTED_V[0-9]+\.json')) {
        throw "Hard-coded versioned final manual filename found in release tooling: $releaseMetadataScriptPath"
    }
}
foreach ($requiredPackageMarker in @('N2C_AI_JSON_IMPORT_AUTHORING_RULES.md','$finalManualFileName')) { if (-not $packageScript.Contains($requiredPackageMarker)) { throw "Package script omits required dynamic root marker: $requiredPackageMarker" } }

$allTextFiles = @(Get-ChildItem -LiteralPath $plugin -File -Recurse | Where-Object { $_.Extension.ToLowerInvariant() -in @('.md','.json','.cpp','.h','.cs','.ps1','.cmd','.ini','.uplugin') })
$secretPatterns = @(('BEGIN ' + 'PRIVATE KEY'),('gh' + 'p_[A-Za-z0-9]{20,}'),('s' + 'k-[A-Za-z0-9]{20,}'),('Bearer' + '\s+[A-Za-z0-9._-]{20,}'))
foreach ($file in $allTextFiles) {
    $content = Get-Content -LiteralPath $file.FullName -Raw
    foreach ($pattern in $secretPatterns) {
        if ($content -match $pattern) { throw "Potential secret in $($file.FullName)" }
    }
}

$orchestratorPath = Join-Path $plugin 'Scripts\Codex\Invoke-N2CFullValidation.ps1'
$orchestratorSource = Get-Content -LiteralPath $orchestratorPath -Raw
foreach ($contractOrchestratorMarker in @('NodeToCode.ContractMatrix.Apply','NodeToCode.ContractMatrix.VerifyFreshFirst','NodeToCode.ContractMatrix.Reapply','NodeToCode.ContractMatrix.VerifyFreshSecond','NodeToCode.ContractMatrix.Cleanup','N2C_IMPORT_CONTRACT_MATRIX_V1.json','11/11')) {
    if (-not $orchestratorSource.Contains($contractOrchestratorMarker)) { throw "Contract-matrix orchestrator marker missing: $contractOrchestratorMarker" }
}
foreach ($restoreHygieneMarker in @('Clear-StaleN2CAutomationRestoreQueue','N2C_AUTOMATION_RESTORE_HYGIENE|result=PASS|quarantined=','/Game/N2C_Test/Generated/','PendingRestoreCancelled\AutomationHarness')) {
    if (-not $orchestratorSource.Contains($restoreHygieneMarker)) { throw "Automation restore hygiene marker missing: $restoreHygieneMarker" }
}
$orchestratorAst = $powerShellAsts[$orchestratorPath]
if (-not $orchestratorAst -or -not $orchestratorAst.ParamBlock) { throw 'Automation orchestrator param block could not be parsed.' }
$orchestratorParameters = @($orchestratorAst.ParamBlock.Parameters | ForEach-Object { [string]$_.Name.VariablePath.UserPath })
foreach ($requiredParameter in @('NoOpenResult','KeepResultDirectory','NoPause')) {
    if ($orchestratorParameters -notcontains $requiredParameter) { throw "Automation parameter missing: $requiredParameter" }
}

$packageSource = Get-Content -LiteralPath (Join-Path $plugin 'Scripts\Codex\Package-N2CPlugin.ps1') -Raw
$searchSource = Get-Content -LiteralPath (Join-Path $plugin 'Scripts\Codex\Search-UE427Source.ps1') -Raw
$verificationSource = Get-Content -LiteralPath (Join-Path $plugin 'Scripts\Codex\Run-N2CVerification.ps1') -Raw
$projectExportSource = Get-Content -LiteralPath (Join-Path $plugin 'Scripts\Codex\Run-N2CProjectExport.ps1') -Raw
$cmdSource = Get-Content -LiteralPath (Join-Path $plugin 'Scripts\RUN_N2C_AUTOMATION_AND_PACK.cmd') -Raw

foreach ($requiredMarker in @(
    'System.Diagnostics.ProcessStartInfo', 'ReadToEndAsync()', 'WaitForExit($waitMilliseconds)',
    'Test-ProcessRunner', 'RunnerExit0.log', 'RunnerStreams.log', 'RunnerArgumentProbe.log', 'RunnerExit7.log', 'RunnerTimeout.log',
    'ConvertTo-NativeCommandLineArgument', '${DisplayName}: $resultText', 'Stopped at:',
    'New-AutomationProgressState', "'Progress: [{0}] Completed {1}/{2} | {3} | Elapsed {4}' -f", 'Running {0}/{1}: {2}', 'Finalizing UE report...', 'failed_cases:', 'Automation progress parser self-test failed',
    'Success.', 'explorer.exe', 'Get-ProcessFailureDetail', '$processId = $process.Id',
    'New-N2CZipFromDirectory', 'CreateEntryFromFile', ".Replace('\', '/')", 'Result ZIP hash mismatch:', 'Remove-N2CBundleDirectoryAfterVerifiedZip', 'N2C_BUNDLE_CLEANUP|result=PASS', 'N2C_BUNDLE_CLEANUP|result=SKIPPED|kept_directory=', 'KeepResultDirectory', 'Test-AutomationReport', 'automation_report_test_count_mismatch', 'pendingLooksComplete', 'result bundle', 'bundle_directory='
)) {
    if (-not $orchestratorSource.Contains($requiredMarker)) { throw "Automation UX/process marker missing: $requiredMarker" }
}
if ($orchestratorSource.Contains('Start-Process @startArgs')) { throw 'Legacy Start-Process child runner is still present.' }

foreach ($crashCollectionMarker in @('$runStartedUtc = [DateTime]::UtcNow','LastWriteTimeUtc -ge $runStartedUtc.AddSeconds(-5)','crash_reports_collected')) {
    if (-not $orchestratorSource.Contains($crashCollectionMarker)) { throw "Current-run crash collection marker missing: $crashCollectionMarker" }
}
if ($orchestratorSource.Contains('AddHours(-8)')) { throw 'Stale eight-hour crash collection window is still present.' }
foreach ($requiredMarker in @('CreateEntryFromFile', ".Replace('\', '/')", 'node2code/')) {
    if (-not $packageSource.Contains($requiredMarker)) { throw "Package separator marker missing: $requiredMarker" }
}
foreach ($requiredMarker in @('$sourceMatches.Count', '$searchExitCode')) {
    if (-not $searchSource.Contains($requiredMarker)) { throw "UE source-search marker missing: $requiredMarker" }
}
if ($searchSource -match '(?i)\$matches\b') { throw 'Search-UE427Source.ps1 still references automatic $Matches.' }
foreach ($requiredMarker in @('queue_completion_marker_missing', 'required_case_markers_missing', 'N2C_MANUAL_REPLAY_CASE|case=')) {
    if (-not $verificationSource.Contains($requiredMarker)) { throw "Standalone verification guard missing: $requiredMarker" }
}
foreach ($requiredMarker in @('fresh_export_archive_missing', 'LastWriteTimeUtc', 'queue_completion_marker_missing')) {
    if (-not $projectExportSource.Contains($requiredMarker)) { throw "Project export freshness guard missing: $requiredMarker" }
}
foreach ($requiredMarker in @(
    'setlocal EnableExtensions DisableDelayedExpansion', ':scan_args', 'shift', '%*',
    'Windows PowerShell 5.1 was not found.', '[0/11] PowerShell syntax preflight', 'Test-N2CPowerShellSyntax.ps1', 'Press any key to exit.', 'exit /b %EXIT_CODE%'
)) {
    if (-not $cmdSource.Contains($requiredMarker)) { throw "CMD contract marker missing: $requiredMarker" }
}
if ($cmdSource.Contains('for %%A in (%*)')) { throw 'Unsafe FOR-based CMD argument scan is still present.' }

$validatorSource = Get-Content -LiteralPath (Join-Path $plugin 'Scripts\Codex\Validate-N2CFiles.ps1') -Raw
foreach ($validatorHardeningMarker in @('Get-N2CCollectionCount','Assert-N2CCollectionCount','Test-N2CValidatorHelpers','System.Collections.IEnumerable','regex MatchCollection','N2C_STATIC_VALIDATION_EXCEPTION','must use Get-N2CCollectionCount instead of direct .Count')) {
    if (-not $validatorSource.Contains($validatorHardeningMarker)) { throw "Validator StrictMode hardening marker missing: $validatorHardeningMarker" }
}
$unsafeNullWrapperMarker = '@(' + '$Value).Length'
if ($validatorSource.Contains($unsafeNullWrapperMarker)) { throw 'Validator still uses the Windows PowerShell 5.1-unsafe null collection wrapper.' }

Write-Host "N2C_VALIDATE|result=PASS|mode=$(if($AllowBuildProducts){'working-tree'}else{'source-only'})|version=$releaseVersion|version_name=$releaseVersionName|json=$((Get-N2CCollectionCount -Value $jsonFiles))|p0_fixtures=$p0FixtureCount|manual_cases=$manualCaseCount|required_tests=$((Get-N2CCollectionCount -Value $requiredTests))|node_capabilities=$((Get-N2CCollectionCount -Value $capabilities))|contract_cases=$((Get-N2CCollectionCount -Value $contractCases))|contract_actions=$((Get-N2CCollectionCount -Value $actionCoverage))|contract_nodes=$((Get-N2CCollectionCount -Value $nodeCoverage))|contract_guards=$((Get-N2CCollectionCount -Value $guardCoverage))|edge_schema_node_id=$($edgeSchemaCounts.node_id)|edge_schema_compact=$($edgeSchemaCounts.compact)"
exit 0
