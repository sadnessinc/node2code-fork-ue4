[CmdletBinding()]
param(
    [string]$PluginRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path,
    [string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

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

$plugin = (Resolve-Path -LiteralPath $PluginRoot).Path
$descriptor = Get-Content -LiteralPath (Join-Path $plugin 'NodeToCode.uplugin') -Raw | ConvertFrom-Json
$releaseVersion = [int]$descriptor.Version
$finalManualFileName = "N2C_FINAL_MANUAL_ALL_STRICT_SUPPORTED_V$releaseVersion.json"
if (-not $OutputDirectory) { $OutputDirectory = Join-Path (Split-Path $plugin -Parent) '..\Saved\NodeToCode\Releases' }
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null

$stage = Join-Path ([IO.Path]::GetTempPath()) ("N2C_SourceStage_{0}" -f ([Guid]::NewGuid().ToString('N')))
$stagePlugin = Join-Path $stage 'node2code'
New-Item -ItemType Directory -Path $stagePlugin -Force | Out-Null

try {
    $include = @('NodeToCode.uplugin', 'README.md', 'CODEX_PLUGIN_START_HERE.md', 'N2C_AI_JSON_IMPORT_AUTHORING_RULES.md', $finalManualFileName, 'LICENSE', 'Source', 'Resources', 'Config', 'Content', 'Scripts', 'CHANGELOG.md')
    foreach ($relative in $include) {
        $source = Join-Path $plugin $relative
        if (-not (Test-Path -LiteralPath $source)) { continue }
        if (Test-Path -LiteralPath $source -PathType Container) { Copy-Item -LiteralPath $source -Destination $stagePlugin -Recurse -Force }
        else { Copy-Item -LiteralPath $source -Destination $stagePlugin -Force }
    }

    $forbiddenDirectoryPattern = '(^|[\\/])(Binaries|Intermediate|Saved|\.vs|DerivedDataCache|__pycache__)([\\/]|$)'
    $forbiddenExtensionPattern = '\.(dll|pdb|obj|lib|exp|log|dmp|pyc|pyo)$'
    Get-ChildItem -LiteralPath $stagePlugin -Recurse -Force | Sort-Object FullName -Descending | ForEach-Object {
        $relative = $_.FullName.Substring($stagePlugin.Length + 1)
        if ($relative -match $forbiddenDirectoryPattern -or (-not $_.PSIsContainer -and $_.Name -match $forbiddenExtensionPattern)) {
            Remove-Item -LiteralPath $_.FullName -Recurse -Force
        }
    }

    $files = @(Get-ChildItem -LiteralPath $stagePlugin -Recurse -File | Where-Object Name -ne 'PACKAGE_MANIFEST.json' | Sort-Object FullName)
    $manifestRows = foreach ($file in $files) {
        [ordered]@{
            path = $file.FullName.Substring($stagePlugin.Length + 1).Replace('\', '/')
            size = $file.Length
            sha256 = Get-N2CFileSha256 -LiteralPath $file.FullName
        }
    }
    $manifest = [ordered]@{
        schema = 'N2C_PACKAGE_MANIFEST_V2'
        version = [int]$descriptor.Version
        version_name = [string]$descriptor.VersionName
        source_only = $true
        top_level = 'node2code'
        manifest_excludes_self = $true
        generated_utc = (Get-Date).ToUniversalTime().ToString('o')
        file_count = $manifestRows.Count
        files = @($manifestRows)
    }
    $manifestPath = Join-Path $stagePlugin 'PACKAGE_MANIFEST.json'
    $manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $manifestPath -Encoding UTF8
    Copy-Item -LiteralPath $manifestPath -Destination (Join-Path $plugin 'PACKAGE_MANIFEST.json') -Force

    $safeVersion = ([string]$descriptor.VersionName) -replace '[^A-Za-z0-9._-]', '_'
    $archive = Join-Path $OutputDirectory ("NodeToCode_{0}_source.zip" -f $safeVersion)
    if (Test-Path -LiteralPath $archive) { Remove-Item -LiteralPath $archive -Force }

    # Create entries explicitly with forward slashes. ZipFile.CreateFromDirectory
    # on Windows/PowerShell 5.1 may store backslashes in entry names, which caused
    # the old root check to reject its own valid archive.
    $archiveStream = [IO.File]::Open($archive, [IO.FileMode]::CreateNew, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    $zipCreate = New-Object IO.Compression.ZipArchive($archiveStream, [IO.Compression.ZipArchiveMode]::Create, $false)
    try {
        $stageFiles = @(Get-ChildItem -LiteralPath $stage -Recurse -File | Sort-Object FullName)
        foreach ($file in $stageFiles) {
            $entryName = $file.FullName.Substring($stage.Length + 1).Replace('\', '/')
            [IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
                $zipCreate,
                $file.FullName,
                $entryName,
                [IO.Compression.CompressionLevel]::Optimal
            ) | Out-Null
        }
    }
    finally {
        $zipCreate.Dispose()
        $archiveStream.Dispose()
    }

    $zip = [IO.Compression.ZipFile]::OpenRead($archive)
    try {
        $entries = @($zip.Entries | Where-Object { -not [string]::IsNullOrEmpty($_.Name) })
        if ($entries.Count -eq 0) { throw 'Archive contains no files.' }
        foreach ($entry in $entries) {
            $normalizedName = $entry.FullName.Replace('\', '/')
            if (-not $normalizedName.StartsWith('node2code/')) { throw "Unexpected archive root: $normalizedName" }
            if ($normalizedName -match $forbiddenDirectoryPattern -or $entry.Name -match $forbiddenExtensionPattern) { throw "Forbidden archive entry: $normalizedName" }
        }
        foreach ($row in $manifestRows) {
            $entryName = 'node2code/' + $row.path
            $entry = $zip.GetEntry($entryName)
            if (-not $entry -or $entry.Length -ne $row.size) { throw "Package entry missing or wrong size: $entryName" }
            $stream = $entry.Open()
            $sha = [Security.Cryptography.SHA256]::Create()
            try { $hash = ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '').ToLowerInvariant() }
            finally { $sha.Dispose(); $stream.Dispose() }
            if ($hash -ne $row.sha256) { throw "Package hash mismatch: $entryName" }
        }
    }
    finally { $zip.Dispose() }

    $archiveHash = Get-N2CFileSha256 -LiteralPath $archive
    Set-Content -LiteralPath "$archive.sha256" -Value "$archiveHash  $([IO.Path]::GetFileName($archive))" -Encoding ASCII
    Write-Host "N2C_PACKAGE|result=PASS|archive=$archive|files=$($manifestRows.Count + 1)|bytes=$((Get-Item -LiteralPath $archive).Length)|sha256=$archiveHash"
    exit 0
}
finally {
    if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue }
}
