[CmdletBinding()]
param(
    [string]$ProjectRoot,
    [string]$EngineRoot,
    [ValidateSet('Development','DebugGame')][string]$Configuration = 'Development',
    [int]$BuildTimeoutSeconds = 5400,
    [int]$AutomationTimeoutSeconds = 5400,
    [switch]$SkipBuild,
    [switch]$SkipEditorTests,
    [switch]$SkipPackage,
    [switch]$NoOpenResult,
    [switch]$KeepResultDirectory,
    [switch]$NoPause
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$pluginRoot = $null
$project = $null
$uproject = $null
$bundleRoot = $null
$bundleZip = $null
$logRoot = $null
$reportRoot = $null
$crashRoot = $null
$currentStage = 'startup'
$currentLog = $null
$runStartedUtc = [DateTime]::UtcNow
$summary = [ordered]@{
    schema = 'N2C_AUTOMATION_RESULT_V1'
    started_utc = $runStartedUtc.ToString('o')
    plugin_version = $null
    plugin_version_name = $null
    process_runner = 'NOT_RUN'
    static_validation = 'NOT_RUN'
    automation_restore_hygiene = 'NOT_RUN'
    build = 'NOT_RUN'
    contract_apply = 'NOT_RUN'
    contract_verify_fresh = 'NOT_RUN'
    contract_reapply = 'NOT_RUN'
    contract_verify_fresh_second = 'NOT_RUN'
    contract_cleanup = 'NOT_RUN'
    main_automation = 'NOT_RUN'
    restore_first_pass = 'NOT_RUN'
    restore_second_pass = 'NOT_RUN'
    package = 'NOT_RUN'
    automation_report_validation = 'NOT_RUN'
    result_bundle = $null
    result_directory = $null
    result_directory_retained = $false
    failures = @()
    logs = @()
    crash_reports_collected = 0
}

function Write-Stage([string]$Number, [string]$Text) {
    Write-Host ''
    Write-Host "[$Number] $Text" -ForegroundColor Cyan
}

function Add-Failure([string]$Stage, [string]$Detail, [string]$LogPath = $null) {
    $row = [ordered]@{ stage = $Stage; detail = $Detail }
    if ($LogPath) { $row.log = $LogPath }
    $summary.failures += [pscustomobject]$row
}

function Add-Log([string]$Path) {
    if ($Path -and -not ($summary.logs -contains $Path)) { $summary.logs += $Path }
}

function Clear-StaleN2CAutomationRestoreQueue {
    param([Parameter(Mandatory)][string]$ProjectDirectory)

    $pendingDirectory = Join-Path $ProjectDirectory 'Saved\NodeToCode\Backups\PendingRestore'
    if (-not (Test-Path -LiteralPath $pendingDirectory -PathType Container)) {
        Write-Host 'N2C_AUTOMATION_RESTORE_HYGIENE|result=PASS|quarantined=0' -ForegroundColor DarkGray
        return 0
    }

    $cancelledDirectory = Join-Path $ProjectDirectory 'Saved\NodeToCode\Backups\PendingRestoreCancelled\AutomationHarness'
    $quarantined = 0
    foreach ($manifestFile in @(Get-ChildItem -LiteralPath $pendingDirectory -Filter '*.restore' -File -ErrorAction SilentlyContinue)) {
        $fields = @{}
        foreach ($line in @(Get-Content -LiteralPath $manifestFile.FullName -ErrorAction Stop)) {
            $separator = $line.IndexOf('=')
            if ($separator -gt 0) {
                $fields[$line.Substring(0, $separator)] = $line.Substring($separator + 1)
            }
        }
        $assetPathName = [string]$fields['AssetPathName']
        $packageName = [string]$fields['PackageName']
        $isAutomationFixture = $assetPathName.StartsWith('/Game/N2C_Test/Generated/', [StringComparison]::OrdinalIgnoreCase) -or
            $packageName.StartsWith('/Game/N2C_Test/Generated/', [StringComparison]::OrdinalIgnoreCase)
        if (-not $isAutomationFixture) { continue }

        New-Item -ItemType Directory -Path $cancelledDirectory -Force | Out-Null
        $suffix = [Guid]::NewGuid().ToString('N')
        $manifestDestination = Join-Path $cancelledDirectory ("{0}.{1}.restore" -f $manifestFile.BaseName, $suffix)
        $pendingBackupCopy = [string]$fields['PendingBackupCopy']
        if ($pendingBackupCopy -and (Test-Path -LiteralPath $pendingBackupCopy -PathType Leaf)) {
            $backupDestination = Join-Path $cancelledDirectory ("{0}.{1}{2}" -f ([IO.Path]::GetFileNameWithoutExtension($pendingBackupCopy)), $suffix, ([IO.Path]::GetExtension($pendingBackupCopy)))
            Move-Item -LiteralPath $pendingBackupCopy -Destination $backupDestination -Force
        }
        Move-Item -LiteralPath $manifestFile.FullName -Destination $manifestDestination -Force
        $quarantined++
    }

    Write-Host "N2C_AUTOMATION_RESTORE_HYGIENE|result=PASS|quarantined=$quarantined" -ForegroundColor DarkGray
    return $quarantined
}


function New-AutomationProgressState {
    param([string[]]$RequiredCases, [string]$DisplayName)
    return [pscustomobject]@{
        RequiredCases = @($RequiredCases)
        DisplayName = $DisplayName
        Offset = [int64]0
        PendingText = ''
        Started = @{}
        Completed = @{}
        StartedCount = 0
        CurrentCase = $null
        LastCompletedCase = $null
        PendingCompletionEvents = New-Object System.Collections.ArrayList
        LastRenderedLength = 0
    }
}

function Read-NewAutomationLogText {
    param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)]$State)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return '' }

    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    try {
        if ($stream.Length -lt $State.Offset) {
            $State.Offset = [int64]0
            $State.PendingText = ''
        }
        if ($stream.Length -eq $State.Offset) { return '' }
        [void]$stream.Seek($State.Offset, [IO.SeekOrigin]::Begin)
        $remaining = [int64]($stream.Length - $State.Offset)
        if ($remaining -le 0) { return '' }
        if ($remaining -gt [int]::MaxValue) { throw "Automation log increment is too large: $remaining bytes" }
        $buffer = New-Object byte[] ([int]$remaining)
        $totalRead = 0
        while ($totalRead -lt $buffer.Length) {
            $read = $stream.Read($buffer, $totalRead, $buffer.Length - $totalRead)
            if ($read -le 0) { break }
            $totalRead += $read
        }
        $State.Offset += $totalRead
        if ($totalRead -le 0) { return '' }
        return [Text.Encoding]::UTF8.GetString($buffer, 0, $totalRead)
    }
    finally {
        $stream.Dispose()
    }
}

function Resolve-AutomationCaseName {
    param([string]$Candidate, [string[]]$RequiredCases)
    if (-not $Candidate) { return $null }
    foreach ($requiredCase in @($RequiredCases)) {
        if ([string]::Equals($requiredCase, $Candidate, [StringComparison]::OrdinalIgnoreCase)) {
            return $requiredCase
        }
    }
    return $null
}

function Update-AutomationProgressState {
    param([Parameter(Mandatory)][string]$LogPath, [Parameter(Mandatory)]$State)
    $newText = Read-NewAutomationLogText -Path $LogPath -State $State
    if ([string]::IsNullOrEmpty($newText)) { return }

    $combined = $State.PendingText + $newText
    $hasCompleteEnding = $combined.EndsWith("`n") -or $combined.EndsWith("`r")
    $lines = [regex]::Split($combined, "`r?`n")
    if ($hasCompleteEnding) {
        $State.PendingText = ''
    }
    else {
        # UE may flush a complete automation line before writing its trailing newline.
        # Parse such a line immediately when it already contains a complete marker;
        # keep genuinely partial text buffered until the next read.
        $pendingCandidate = [string]$lines[-1]
        $pendingLooksComplete = (
            [regex]::IsMatch($pendingCandidate, 'Test Started\. Name=\{[^}]+\}') -or
            [regex]::IsMatch($pendingCandidate, 'Test Completed\. Result=\{[^}]+\} Name=\{[^}]+\} Path=\{[^}]+\}') -or
            [regex]::IsMatch($pendingCandidate, 'N2C_MANUAL_REPLAY_CASE\|case=[^|\r\n]+\|result=(PASS|FAIL)')
        )
        if ($pendingLooksComplete) {
            $State.PendingText = ''
        }
        else {
            $State.PendingText = $pendingCandidate
            if ($lines.Count -gt 1) { $lines = $lines[0..($lines.Count - 2)] } else { $lines = @() }
        }
    }

    foreach ($line in @($lines)) {
        $startMatch = [regex]::Match($line, 'Test Started\. Name=\{(?<name>[^}]+)\}')
        if ($startMatch.Success) {
            $caseName = Resolve-AutomationCaseName -Candidate ([string]$startMatch.Groups['name'].Value) -RequiredCases $State.RequiredCases
            if ($caseName) {
                if (-not $State.Started.ContainsKey($caseName)) {
                    $State.Started[$caseName] = $true
                    $State.StartedCount++
                }
                $State.CurrentCase = $caseName
            }
        }

        $completeMatch = [regex]::Match($line, 'Test Completed\. Result=\{(?<result>[^}]+)\} Name=\{(?<name>[^}]+)\} Path=\{(?<path>[^}]+)\}')
        if ($completeMatch.Success) {
            $pathParts = ([string]$completeMatch.Groups['path'].Value) -split '\.'
            $candidate = if ($pathParts.Count) { $pathParts[-1] } else { [string]$completeMatch.Groups['name'].Value }
            $caseName = Resolve-AutomationCaseName -Candidate $candidate -RequiredCases $State.RequiredCases
            if ($caseName) {
                $wasAlreadyCompleted = $State.Completed.ContainsKey($caseName)
                $completionResult = [string]$completeMatch.Groups['result'].Value
                $State.Completed[$caseName] = $completionResult
                $State.LastCompletedCase = $caseName
                if (-not $wasAlreadyCompleted) {
                    [void]$State.PendingCompletionEvents.Add([pscustomobject]@{
                        Case = $caseName
                        Result = $completionResult
                        CompletedCount = $State.Completed.Count
                    })
                }
                if ([string]::Equals([string]$State.CurrentCase, $caseName, [StringComparison]::OrdinalIgnoreCase)) {
                    $State.CurrentCase = $null
                }
            }
        }

        $markerMatch = [regex]::Match($line, 'N2C_MANUAL_REPLAY_CASE\|case=(?<case>[^|\r\n]+)\|result=(?<result>PASS|FAIL)')
        if ($markerMatch.Success) {
            $caseName = Resolve-AutomationCaseName -Candidate ([string]$markerMatch.Groups['case'].Value) -RequiredCases $State.RequiredCases
            if ($caseName -and -not $State.Started.ContainsKey($caseName)) {
                $State.Started[$caseName] = $true
                $State.StartedCount++
                $State.CurrentCase = $caseName
            }
        }
    }
}

function Write-AutomationProgressLine {
    param([Parameter(Mandatory)]$State, [Parameter(Mandatory)][TimeSpan]$Elapsed)
    $total = [Math]::Max(1, @($State.RequiredCases).Count)

    # Keep a permanent one-line record for every completed case. The live status line
    # is still redrawn in place, but fast tests can no longer disappear between polls.
    if ($State.PendingCompletionEvents.Count -gt 0) {
        if ($State.LastRenderedLength -gt 0) {
            [Console]::Write("`r" + (' ' * $State.LastRenderedLength) + "`r")
            $State.LastRenderedLength = 0
        }
        foreach ($completionEvent in @($State.PendingCompletionEvents)) {
            $completionColor = if ([string]$completionEvent.Result -match '^(Success|Passed)$') { 'Green' } else { 'Yellow' }
            Write-Host ('Completed {0}/{1}: {2} [{3}]' -f $completionEvent.CompletedCount, $total, $completionEvent.Case, $completionEvent.Result) -ForegroundColor $completionColor
        }
        $State.PendingCompletionEvents.Clear()
    }

    $completed = $State.Completed.Count
    $barWidth = 20
    $filled = [Math]::Min($barWidth, [Math]::Floor(($completed / [double]$total) * $barWidth))
    $bar = ('#' * $filled) + ('-' * ($barWidth - $filled))
    $elapsedText = '{0:00}:{1:00}:{2:00}' -f [Math]::Floor($Elapsed.TotalHours), $Elapsed.Minutes, $Elapsed.Seconds

    if ($completed -ge $total) {
        $statusText = 'Finalizing UE report...'
    }
    elseif ($State.CurrentCase) {
        $runningOrdinal = [Math]::Min($total, [Math]::Max(1, $State.StartedCount))
        $statusText = 'Running {0}/{1}: {2}' -f $runningOrdinal, $total, $State.CurrentCase
    }
    elseif ($State.LastCompletedCase) {
        $statusText = 'Waiting after: {0}' -f $State.LastCompletedCase
    }
    else {
        $statusText = 'Waiting for UE4 automation...'
    }

    # "Completed" and "Running" are intentionally separate. Seeing
    # "Completed 7/20 | Running 8/20" is correct, not a counter mismatch.
    $line = 'Progress: [{0}] Completed {1}/{2} | {3} | Elapsed {4}' -f $bar, $completed, $total, $statusText, $elapsedText

    $maxWidth = 160
    try {
        if ([Console]::WindowWidth -gt 20) { $maxWidth = [Console]::WindowWidth - 1 }
    }
    catch {}
    if ($line.Length -gt $maxWidth) { $line = $line.Substring(0, [Math]::Max(1, $maxWidth - 3)) + '...' }
    $paddingWidth = [Math]::Max($State.LastRenderedLength, $line.Length)
    [Console]::Write("`r" + $line.PadRight($paddingWidth))
    $State.LastRenderedLength = $paddingWidth
}

function Complete-AutomationProgressLine {
    param([Parameter(Mandatory)]$State)
    if ($State.LastRenderedLength -gt 0) {
        [Console]::WriteLine()
        $State.LastRenderedLength = 0
    }
}

function Get-ProcessFailureDetail {
    param(
        [int]$ExitCode,
        [bool]$TimedOut,
        [string]$StdoutPath,
        [string]$StderrPath
    )

    if ($TimedOut) { return "timeout=true;exit_code=$ExitCode" }

    foreach ($candidatePath in @($StderrPath, $StdoutPath)) {
        if (-not $candidatePath -or -not (Test-Path -LiteralPath $candidatePath -PathType Leaf)) { continue }
        $nonEmptyLines = @(Get-Content -LiteralPath $candidatePath -ErrorAction SilentlyContinue | Where-Object {
            -not [string]::IsNullOrWhiteSpace([string]$_)
        })
        if ($nonEmptyLines.Count -gt 0) {
            return "exit_code=$ExitCode; $([string]$nonEmptyLines[-1])"
        }
    }

    return "exit_code=$ExitCode"
}

function ConvertTo-NativeCommandLineArgument {
    param([AllowNull()][AllowEmptyString()][string]$Value)

    if ($null -eq $Value -or $Value.Length -eq 0) { return '""' }
    if ($Value -notmatch '[\s"]') { return $Value }

    # Windows native processes receive one command-line string. Quote according to
    # the CommandLineToArgvW/CRT backslash-before-quote rules instead of relying on
    # Start-Process's array joining behavior.
    $builder = New-Object System.Text.StringBuilder
    [void]$builder.Append('"')
    $backslashCount = 0
    foreach ($character in $Value.ToCharArray()) {
        if ($character -eq [char]92) {
            $backslashCount++
            continue
        }
        if ($character -eq [char]34) {
            if ($backslashCount -gt 0) {
                [void]$builder.Append((('\' * ($backslashCount * 2)) -join ''))
            }
            [void]$builder.Append('\')
            [void]$builder.Append('"')
            $backslashCount = 0
            continue
        }
        if ($backslashCount -gt 0) {
            [void]$builder.Append((('\' * $backslashCount) -join ''))
            $backslashCount = 0
        }
        [void]$builder.Append($character)
    }
    if ($backslashCount -gt 0) {
        [void]$builder.Append((('\' * ($backslashCount * 2)) -join ''))
    }
    [void]$builder.Append('"')
    return $builder.ToString()
}

function Join-NativeCommandLine {
    param([string[]]$Arguments)
    return (@($Arguments | ForEach-Object { ConvertTo-NativeCommandLineArgument -Value ([string]$_) }) -join ' ')
}

function Write-Utf8NoBom {
    param([Parameter(Mandatory)][string]$Path, [AllowNull()][string]$Text)
    $parent = Split-Path -Parent $Path
    if ($parent) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
    $encoding = New-Object System.Text.UTF8Encoding($false)
    [IO.File]::WriteAllText($Path, $(if ($null -eq $Text) { '' } else { $Text }), $encoding)
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

function New-N2CZipFromDirectory {
    param(
        [Parameter(Mandatory)][string]$SourceDirectory,
        [Parameter(Mandatory)][string]$DestinationPath
    )

    $sourceRoot = (Resolve-Path -LiteralPath $SourceDirectory).Path
    $destinationParent = Split-Path -Parent $DestinationPath
    if ($destinationParent) { New-Item -ItemType Directory -Path $destinationParent -Force | Out-Null }
    if (Test-Path -LiteralPath $DestinationPath) { Remove-Item -LiteralPath $DestinationPath -Force }

    $sourceFiles = @(Get-ChildItem -LiteralPath $sourceRoot -Recurse -File | Sort-Object FullName)
    if ($sourceFiles.Count -eq 0) { throw "Cannot create result ZIP from an empty directory: $sourceRoot" }

    $archiveStream = [IO.File]::Open($DestinationPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    $zipArchive = New-Object IO.Compression.ZipArchive($archiveStream, [IO.Compression.ZipArchiveMode]::Create, $false)
    try {
        foreach ($sourceFile in $sourceFiles) {
            $entryName = $sourceFile.FullName.Substring($sourceRoot.Length).TrimStart([char[]]@('\','/')).Replace('\', '/')
            [IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
                $zipArchive,
                $sourceFile.FullName,
                $entryName,
                [IO.Compression.CompressionLevel]::Optimal
            ) | Out-Null
        }
    }
    finally {
        $zipArchive.Dispose()
        $archiveStream.Dispose()
    }

    # Verify every entry before the source directory is eligible for deletion.
    $zipRead = [IO.Compression.ZipFile]::OpenRead($DestinationPath)
    try {
        $entries = @($zipRead.Entries | Where-Object { -not [string]::IsNullOrEmpty($_.Name) })
        if ($entries.Count -ne $sourceFiles.Count) {
            throw "Result ZIP entry count mismatch: expected=$($sourceFiles.Count); actual=$($entries.Count)"
        }
        $entryMap = @{}
        foreach ($entry in $entries) {
            $normalizedEntryName = $entry.FullName.Replace('\', '/')
            if ($entryMap.ContainsKey($normalizedEntryName)) { throw "Duplicate result ZIP entry: $normalizedEntryName" }
            $entryMap[$normalizedEntryName] = $entry
        }
        foreach ($sourceFile in $sourceFiles) {
            $entryName = $sourceFile.FullName.Substring($sourceRoot.Length).TrimStart([char[]]@('\','/')).Replace('\', '/')
            if (-not $entryMap.ContainsKey($entryName)) { throw "Result ZIP entry missing: $entryName" }
            $entry = $entryMap[$entryName]
            if ($entry.Length -ne $sourceFile.Length) { throw "Result ZIP size mismatch: $entryName" }
            $sourceHash = Get-N2CFileSha256 -LiteralPath $sourceFile.FullName
            $entryStream = $entry.Open()
            $sha = [Security.Cryptography.SHA256]::Create()
            try { $entryHash = ([BitConverter]::ToString($sha.ComputeHash($entryStream))).Replace('-', '').ToLowerInvariant() }
            finally { $sha.Dispose(); $entryStream.Dispose() }
            if ($entryHash -ne $sourceHash) { throw "Result ZIP hash mismatch: $entryName" }
        }
    }
    finally { $zipRead.Dispose() }
}

function Remove-N2CBundleDirectoryAfterVerifiedZip {
    param(
        [Parameter(Mandatory)][string]$SourceDirectory,
        [Parameter(Mandatory)][string]$VerifiedZipPath
    )
    if (-not (Test-Path -LiteralPath $VerifiedZipPath -PathType Leaf)) {
        throw "Verified result ZIP does not exist: $VerifiedZipPath"
    }
    if (Test-Path -LiteralPath $SourceDirectory) {
        Remove-Item -LiteralPath $SourceDirectory -Recurse -Force -ErrorAction Stop
    }
    if (Test-Path -LiteralPath $SourceDirectory) {
        throw "Could not remove result directory after ZIP verification: $SourceDirectory"
    }
}

function Invoke-ProcessWithTimeout {
    param(
        [Parameter(Mandatory)][string]$FilePath,
        [string[]]$Arguments,
        [Parameter(Mandatory)][string]$StdoutPath,
        [int]$TimeoutSeconds,
        [string]$WorkingDirectory,
        [scriptblock]$ProgressAction,
        [int]$ProgressIntervalMilliseconds = 1000
    )

    if ($TimeoutSeconds -le 0) { throw 'TimeoutSeconds must be greater than zero.' }
    if (-not (Test-Path -LiteralPath $FilePath -PathType Leaf)) { throw "Executable not found: $FilePath" }

    $stderrPath = [IO.Path]::ChangeExtension($StdoutPath, '.stderr.log')
    $startInfo = New-Object System.Diagnostics.ProcessStartInfo
    $startInfo.FileName = $FilePath
    $startInfo.Arguments = Join-NativeCommandLine -Arguments $Arguments
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    if ($WorkingDirectory) { $startInfo.WorkingDirectory = $WorkingDirectory }

    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $startInfo
    $processId = $null
    $timedOut = $false
    $exitCode = 126
    $killOutput = ''
    try {
        if (-not $process.Start()) { throw "Process failed to start: $FilePath" }
        $processId = $process.Id
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        $stopwatch = [Diagnostics.Stopwatch]::StartNew()
        $timeoutMilliseconds64 = [int64]$TimeoutSeconds * 1000
        $finished = $false
        while (-not $finished -and $stopwatch.ElapsedMilliseconds -lt $timeoutMilliseconds64) {
            $remainingMilliseconds = $timeoutMilliseconds64 - $stopwatch.ElapsedMilliseconds
            $waitMilliseconds = [int][Math]::Min([int64][Math]::Max(100, $ProgressIntervalMilliseconds), [int64]$remainingMilliseconds)
            $finished = $process.WaitForExit($waitMilliseconds)
            if ($ProgressAction) {
                try { & $ProgressAction $stopwatch.Elapsed } catch {}
            }
        }

        if (-not $finished) {
            $timedOut = $true
            $killOutput = (& taskkill.exe /PID $processId /T /F 2>&1 | Out-String)
            $terminated = $false
            try { $terminated = $process.WaitForExit(10000) } catch {}
            if (-not $terminated) {
                try { $process.Kill() } catch {}
                try { $terminated = $process.WaitForExit(5000) } catch {}
            }
            if (-not $terminated) { throw "Timed-out process could not be terminated: $FilePath (PID $processId)" }
            $exitCode = 124
        }
        else {
            # The parameterless call completes asynchronous stream handlers before
            # ExitCode is read. System.Diagnostics.Process exposes ExitCode only
            # after the associated process has terminated.
            $process.WaitForExit()
            if ($ProgressAction) {
                try { & $ProgressAction $stopwatch.Elapsed } catch {}
            }
            $exitCode = [int]$process.ExitCode
        }
        $stopwatch.Stop()

        $stdoutText = [string]$stdoutTask.Result
        $stderrText = [string]$stderrTask.Result
        if ($killOutput) { $stderrText += "`r`n$killOutput" }
        Write-Utf8NoBom -Path $StdoutPath -Text $stdoutText
        Write-Utf8NoBom -Path $stderrPath -Text $stderrText
    }
    catch {
        $message = $_.Exception.Message
        Write-Utf8NoBom -Path $StdoutPath -Text ''
        Write-Utf8NoBom -Path $stderrPath -Text $message
        throw
    }
    finally {
        $process.Dispose()
    }

    return [pscustomobject]@{
        ExitCode = $exitCode
        TimedOut = $timedOut
        ProcessId = $processId
        Stderr = $stderrPath
        CommandLine = $startInfo.Arguments
    }
}

function Test-ProcessRunner {
    param(
        [Parameter(Mandatory)][string]$PowerShellPath,
        [Parameter(Mandatory)][string]$LogDirectory
    )

    $zeroLog = Join-Path $LogDirectory 'RunnerExit0.log'
    $zero = Invoke-ProcessWithTimeout -FilePath $PowerShellPath -Arguments @(
        '-NoLogo', '-NoProfile', '-Command', 'Write-Output N2C_RUNNER_SELFTEST_ZERO; exit 0'
    ) -StdoutPath $zeroLog -TimeoutSeconds 30
    if ($zero.TimedOut -or $zero.ExitCode -ne 0) {
        throw "Process runner zero-exit self-test failed: exit_code=$($zero.ExitCode); timeout=$($zero.TimedOut)"
    }
    if (-not (Test-Path -LiteralPath $zeroLog -PathType Leaf) -or -not (Get-Content -LiteralPath $zeroLog -Raw).Contains('N2C_RUNNER_SELFTEST_ZERO')) {
        throw 'Process runner stdout-capture self-test failed.'
    }

    $streamsLog = Join-Path $LogDirectory 'RunnerStreams.log'
    $streams = Invoke-ProcessWithTimeout -FilePath $PowerShellPath -Arguments @(
        '-NoLogo', '-NoProfile', '-Command',
        '[Console]::Out.WriteLine("N2C_RUNNER_STDOUT"); [Console]::Error.WriteLine("N2C_RUNNER_STDERR"); exit 0'
    ) -StdoutPath $streamsLog -TimeoutSeconds 30
    if ($streams.TimedOut -or $streams.ExitCode -ne 0) {
        throw "Process runner stream-capture self-test failed: exit_code=$($streams.ExitCode); timeout=$($streams.TimedOut)"
    }
    $streamsStdout = if (Test-Path -LiteralPath $streamsLog -PathType Leaf) { Get-Content -LiteralPath $streamsLog -Raw } else { '' }
    $streamsStderr = if (Test-Path -LiteralPath $streams.Stderr -PathType Leaf) { Get-Content -LiteralPath $streams.Stderr -Raw } else { '' }
    if (-not $streamsStdout.Contains('N2C_RUNNER_STDOUT') -or -not $streamsStderr.Contains('N2C_RUNNER_STDERR')) {
        throw 'Process runner stdout/stderr content self-test failed.'
    }

    $probeScript = Join-Path $LogDirectory 'Runner Argument Probe.ps1'
    $probeValue = 'N2C value with spaces tail\'
    $emptyValue = ''
    $quotedValue = 'N2C "quoted" value'
    $probeScriptText = @(
        'param([string]$Probe, [AllowEmptyString()][string]$Empty, [string]$Quoted)',
        'Write-Output ("Probe={0};EmptyLength={1};Quoted={2}" -f $Probe,$Empty.Length,$Quoted)',
        "if (`$Probe -ceq 'N2C value with spaces tail\' -and `$Empty -ceq '' -and `$Quoted -ceq ('N2C ' + [char]34 + 'quoted' + [char]34 + ' value')) { exit 0 } else { exit 9 }"
    ) -join "`r`n"
    Write-Utf8NoBom -Path $probeScript -Text $probeScriptText
    $probeLog = Join-Path $LogDirectory 'RunnerArgumentProbe.log'
    $probe = Invoke-ProcessWithTimeout -FilePath $PowerShellPath -Arguments @(
        '-NoLogo', '-NoProfile', '-File', $probeScript,
        '-Probe', $probeValue, '-Empty', $emptyValue, '-Quoted', $quotedValue
    ) -StdoutPath $probeLog -TimeoutSeconds 30
    if ($probe.TimedOut -or $probe.ExitCode -ne 0) {
        throw "Process runner argument-quoting self-test failed: exit_code=$($probe.ExitCode); timeout=$($probe.TimedOut)"
    }

    $sevenLog = Join-Path $LogDirectory 'RunnerExit7.log'
    $seven = Invoke-ProcessWithTimeout -FilePath $PowerShellPath -Arguments @(
        '-NoLogo', '-NoProfile', '-Command', 'Write-Output N2C_RUNNER_SELFTEST_SEVEN; exit 7'
    ) -StdoutPath $sevenLog -TimeoutSeconds 30
    if ($seven.TimedOut -or $seven.ExitCode -ne 7) {
        throw "Process runner nonzero-exit self-test failed: expected=7; actual=$($seven.ExitCode); timeout=$($seven.TimedOut)"
    }

    $timeoutLog = Join-Path $LogDirectory 'RunnerTimeout.log'
    $timeout = Invoke-ProcessWithTimeout -FilePath $PowerShellPath -Arguments @(
        '-NoLogo', '-NoProfile', '-Command', 'Start-Sleep -Seconds 5; exit 0'
    ) -StdoutPath $timeoutLog -TimeoutSeconds 1
    if (-not $timeout.TimedOut -or $timeout.ExitCode -ne 124) {
        throw "Process runner timeout self-test failed: expected=124/true; actual=$($timeout.ExitCode)/$($timeout.TimedOut)"
    }

    $progressProbeLog = Join-Path $LogDirectory 'RunnerProgressProbe.log'
    Write-Utf8NoBom -Path $progressProbeLog -Text (@(
        'LogAutomationController: Display: Test Started. Name={Alpha}',
        'LogAutomationController: Error: Test Completed. Result={Success} Name={Alpha} Path={NodeToCode.ManualReplay.Alpha}',
        'LogAutomationController: Display: Test Started. Name={Beta}'
    ) -join "`r`n")
    $progressProbeState = New-AutomationProgressState -RequiredCases @('Alpha','Beta') -DisplayName 'Progress probe'
    Update-AutomationProgressState -LogPath $progressProbeLog -State $progressProbeState
    if ($progressProbeState.StartedCount -ne 2 -or $progressProbeState.Completed.Count -ne 1 -or $progressProbeState.CurrentCase -ne 'Beta' -or $progressProbeState.PendingText -or $progressProbeState.PendingCompletionEvents.Count -ne 1) {
        throw "Automation progress parser self-test failed: started=$($progressProbeState.StartedCount); completed=$($progressProbeState.Completed.Count); current=$($progressProbeState.CurrentCase); pending=$($progressProbeState.PendingText); completion_events=$($progressProbeState.PendingCompletionEvents.Count)"
    }

    $cleanupProbeRoot = Join-Path $LogDirectory 'RunnerZipCleanupProbe'
    $cleanupProbeZip = Join-Path $LogDirectory 'RunnerZipCleanupProbe.zip'
    New-Item -ItemType Directory -Path $cleanupProbeRoot -Force | Out-Null
    Write-Utf8NoBom -Path (Join-Path $cleanupProbeRoot 'probe.txt') -Text 'N2C_ZIP_CLEANUP_PROBE'
    $cleanupProbeHash = Get-N2CFileSha256 -LiteralPath (Join-Path $cleanupProbeRoot 'probe.txt')
    if ($cleanupProbeHash -ne '14e1180535ab5513d8ad2830acb148eebe0eea1254ee38de51726a711ca7d842') {
        throw "SHA-256 helper self-test failed: $cleanupProbeHash"
    }
    New-N2CZipFromDirectory -SourceDirectory $cleanupProbeRoot -DestinationPath $cleanupProbeZip
    Remove-N2CBundleDirectoryAfterVerifiedZip -SourceDirectory $cleanupProbeRoot -VerifiedZipPath $cleanupProbeZip
    if ((Test-Path -LiteralPath $cleanupProbeRoot) -or -not (Test-Path -LiteralPath $cleanupProbeZip -PathType Leaf)) {
        throw 'Result ZIP cleanup self-test failed.'
    }

    Remove-Item -LiteralPath $probeScript, $progressProbeLog, $cleanupProbeZip -Force -ErrorAction SilentlyContinue
    return @(
        $zeroLog, $zero.Stderr,
        $streamsLog, $streams.Stderr,
        $probeLog, $probe.Stderr,
        $sevenLog, $seven.Stderr,
        $timeoutLog, $timeout.Stderr
    )
}

function Test-AutomationLog {
    param([string]$Path, [int]$ExitCode, [string]$Filter, [string[]]$RequiredCases)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return [pscustomobject]@{ Passed = $false; Reason = 'log_missing'; MissingCases = @(); FailedCases = @() }
    }
    $text = Get-Content -LiteralPath $Path -Raw
    $fatal = $text -match '(?im)\bFatal error:|Assertion failed:|Unhandled Exception:|Automation Test Failed'
    $completed = $text.Contains('Automation Test Queue Empty') -or $text.Contains('Automation Test Queue is empty')
    $missingCases = @()
    $failedCases = @()
    foreach ($caseName in @($RequiredCases)) {
        $passMarker = "N2C_MANUAL_REPLAY_CASE|case=$caseName|result=PASS"
        $failMarker = "N2C_MANUAL_REPLAY_CASE|case=$caseName|result=FAIL"
        if ($text.Contains($failMarker)) { $failedCases += $caseName }
        if (-not $text.Contains($passMarker)) { $missingCases += $caseName }
    }
    $failedCases = @($failedCases | Select-Object -Unique)
    $passed = $ExitCode -eq 0 -and -not $fatal -and $completed -and $missingCases.Count -eq 0 -and $failedCases.Count -eq 0
    if ($failedCases.Count) { $reason = 'failed_cases:' + ($failedCases -join ',') + ";exit_code=$ExitCode" }
    elseif ($fatal) { $reason = "fatal_or_exception_marker;exit_code=$ExitCode" }
    elseif ($ExitCode -ne 0) { $reason = "exit_code_$ExitCode" }
    elseif (-not $completed) { $reason = 'queue_completion_marker_missing' }
    elseif ($missingCases.Count) { $reason = 'required_case_markers_missing:' + ($missingCases -join ',') }
    else { $reason = 'ok' }
    return [pscustomobject]@{
        Passed = $passed
        Reason = $reason
        Filter = $Filter
        MissingCases = $missingCases
        FailedCases = $failedCases
    }
}

function Test-AutomationReport {
    param(
        [Parameter(Mandatory)][string]$ReportDirectory,
        [Parameter(Mandatory)][int]$ExpectedTests
    )

    $indexPath = Join-Path $ReportDirectory 'index.json'
    if (-not (Test-Path -LiteralPath $indexPath -PathType Leaf)) {
        return [pscustomobject]@{ Passed = $false; Reason = 'automation_report_missing'; WarningCount = 0; ErrorCount = 0; TestCount = 0; Path = $indexPath }
    }

    try {
        $reportData = Get-Content -LiteralPath $indexPath -Raw | ConvertFrom-Json
    }
    catch {
        return [pscustomobject]@{ Passed = $false; Reason = ('automation_report_invalid:' + $_.Exception.Message); WarningCount = 0; ErrorCount = 0; TestCount = 0; Path = $indexPath }
    }

    $tests = @($reportData.tests)
    $failed = [int]$reportData.failed
    $notRun = [int]$reportData.notRun
    $inProcess = [int]$reportData.inProcess
    $warningCount = 0
    $errorCount = 0
    $badTests = @()
    foreach ($testRow in $tests) {
        $warningCount += [int]$testRow.warnings
        $errorCount += [int]$testRow.errors
        if ([string]$testRow.state -ne 'Success' -or [int]$testRow.errors -ne 0) {
            $badTests += [string]$testRow.fullTestPath
        }
    }

    $passed = (
        $failed -eq 0 -and $notRun -eq 0 -and $inProcess -eq 0 -and
        $errorCount -eq 0 -and $badTests.Count -eq 0 -and $tests.Count -eq $ExpectedTests
    )
    if ($tests.Count -ne $ExpectedTests) { $reason = "automation_report_test_count_mismatch:expected=$ExpectedTests;actual=$($tests.Count)" }
    elseif ($failed -ne 0 -or $notRun -ne 0 -or $inProcess -ne 0) { $reason = "automation_report_state_mismatch:failed=$failed;not_run=$notRun;in_process=$inProcess" }
    elseif ($errorCount -ne 0 -or $badTests.Count -ne 0) { $reason = 'automation_report_errors:' + ($badTests -join ',') }
    else { $reason = 'ok' }

    return [pscustomobject]@{
        Passed = $passed
        Reason = $reason
        WarningCount = $warningCount
        ErrorCount = $errorCount
        TestCount = $tests.Count
        Path = $indexPath
    }
}

function Invoke-AutomationStage {
    param([string]$Stage, [string]$Filter, [string[]]$RequiredCases, [string]$DisplayName)
    $editor = Join-Path $EngineRoot 'Engine\Binaries\Win64\UE4Editor-Cmd.exe'
    if (-not (Test-Path -LiteralPath $editor -PathType Leaf)) { throw "UE4Editor-Cmd not found: $editor" }
    $safe = $Stage -replace '[^A-Za-z0-9_.-]', '_'
    $log = Join-Path $logRoot "$safe.log"
    $report = Join-Path $reportRoot $safe
    New-Item -ItemType Directory -Path $report -Force | Out-Null
    $editorArguments = @(
        $uproject.FullName,
        '-unattended', '-nop4', '-NoSplash', '-NullRHI', '-NoSound', '-stdout', '-FullStdOutLogOutput',
        ('-ExecCmds=Automation RunTests {0}; Quit' -f $Filter),
        '-TestExit=Automation Test Queue Empty',
        ('-ReportExportPath={0}' -f $report),
        ('-AbsLog={0}' -f $log)
    )
    $stdout = Join-Path $logRoot "$safe.process.log"
    Write-Host "$DisplayName is running..." -ForegroundColor Gray
    $progressState = New-AutomationProgressState -RequiredCases $RequiredCases -DisplayName $DisplayName
    $progressAction = {
        param([TimeSpan]$Elapsed)
        Update-AutomationProgressState -LogPath $log -State $progressState
        Write-AutomationProgressLine -State $progressState -Elapsed $Elapsed
    }
    try {
        $run = Invoke-ProcessWithTimeout -FilePath $editor -Arguments $editorArguments -StdoutPath $stdout -TimeoutSeconds $AutomationTimeoutSeconds -ProgressAction $progressAction -ProgressIntervalMilliseconds 1000
    }
    finally {
        Update-AutomationProgressState -LogPath $log -State $progressState
        Complete-AutomationProgressLine -State $progressState
    }
    Add-Log $log
    Add-Log $stdout
    Add-Log $run.Stderr
    $logCheck = Test-AutomationLog -Path $log -ExitCode $run.ExitCode -Filter $Filter -RequiredCases $RequiredCases
    $expectedTestCount = @($RequiredCases).Count
    $reportCheck = Test-AutomationReport -ReportDirectory $report -ExpectedTests $expectedTestCount
    $stagePassed = $logCheck.Passed -and $reportCheck.Passed
    if (-not $stagePassed) {
        $detailParts = @()
        if (-not $logCheck.Passed) { $detailParts += [string]$logCheck.Reason }
        if (-not $reportCheck.Passed) { $detailParts += [string]$reportCheck.Reason }
        if (($run.ExitCode -ne 0 -or $run.TimedOut) -and $logCheck.FailedCases.Count -eq 0) {
            $detailParts += Get-ProcessFailureDetail -ExitCode $run.ExitCode -TimedOut $run.TimedOut -StdoutPath $stdout -StderrPath $run.Stderr
        }
        Add-Failure $Stage ($detailParts -join '; ') $log
    }
    $resultText = if ($stagePassed) { 'PASS' } else { 'FAIL' }
    Write-Host "${DisplayName}: $resultText" -ForegroundColor $(if ($stagePassed) { 'Green' } else { 'Red' })
    Write-Host "Automation report: tests=$($reportCheck.TestCount); warnings=$($reportCheck.WarningCount); errors=$($reportCheck.ErrorCount)" -ForegroundColor $(if ($reportCheck.ErrorCount -eq 0) { 'Gray' } else { 'Red' })
    Write-Host "N2C_AUTOMATION_STAGE|stage=$Stage|result=$resultText|exit_code=$($run.ExitCode)|timeout=$($run.TimedOut)|filter=$Filter|report_tests=$($reportCheck.TestCount)|report_warnings=$($reportCheck.WarningCount)|report_errors=$($reportCheck.ErrorCount)|log=$log"
    return $stagePassed
}

function Open-ResultInExplorer([string]$Path) {
    if ($NoOpenResult -or -not $Path -or -not (Test-Path -LiteralPath $Path)) { return }
    try {
        $arguments = '/n,/select,"{0}"' -f $Path
        Start-Process -FilePath 'explorer.exe' -ArgumentList $arguments | Out-Null
    }
    catch {
        Write-Host "Could not open Windows Explorer automatically: $($_.Exception.Message)" -ForegroundColor Yellow
    }
}

function Write-FriendlyResult {
    param([string]$Result, [string]$ResultPath, [string]$Hash)
    Write-Host ''
    Write-Host ('=' * 72)
    if ($Result -eq 'PASS') {
        Write-Host 'Success.' -ForegroundColor Green
        Write-Host "Result file: $ResultPath"
        if ($Hash) { Write-Host "SHA-256: $Hash" }
        if ($KeepResultDirectory -and $bundleRoot) { Write-Host "Result directory: $bundleRoot" }
    }
    else {
        $failure = @($summary.failures | Select-Object -First 1)
        $stage = if ($failure.Count) { [string]$failure[0].stage } else { $currentStage }
        $detail = if ($failure.Count) { [string]$failure[0].detail } else { 'Unknown failure.' }
        $failureLog = if ($failure.Count -and $failure[0].PSObject.Properties.Name -contains 'log') { [string]$failure[0].log } else { $currentLog }
        Write-Host 'Error.' -ForegroundColor Red
        Write-Host "Stopped at: $stage"
        Write-Host "Details: $detail"
        if ($failureLog) { Write-Host "Log: $failureLog" }
        if ($ResultPath) { Write-Host "Result file: $ResultPath" }
    }
    Write-Host ('=' * 72)
}

try {
    Write-Host 'NodeToCode UE4.27 automation' -ForegroundColor White
    Write-Host 'The script will build the Editor target, run tests, verify restore, and package results.' -ForegroundColor Gray

    $currentStage = 'project discovery'
    $pluginRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
    if (-not $ProjectRoot) {
        $candidate = Split-Path (Split-Path $pluginRoot -Parent) -Parent
        if (Test-Path -LiteralPath $candidate -PathType Container) { $ProjectRoot = $candidate }
    }
    if (-not $ProjectRoot) { throw 'Project root could not be determined.' }
    $project = (Resolve-Path -LiteralPath $ProjectRoot).Path
    $uprojects = @(Get-ChildItem -LiteralPath $project -Filter *.uproject -File)
    if ($uprojects.Count -ne 1) { throw "Expected exactly one .uproject in '$project'; found $($uprojects.Count)." }
    $uproject = $uprojects[0]

    if (-not $EngineRoot) { $EngineRoot = $env:UE427_ROOT }
    if (-not $EngineRoot) {
        $common = Join-Path $env:ProgramFiles 'Epic Games\UE_4.27'
        if (Test-Path -LiteralPath $common) { $EngineRoot = $common }
    }
    if (-not $EngineRoot -and (-not $SkipBuild -or -not $SkipEditorTests)) {
        throw 'UE4.27 root not found. Pass -EngineRoot or set UE427_ROOT.'
    }
    if ($EngineRoot) { $EngineRoot = (Resolve-Path -LiteralPath $EngineRoot).Path }

    $stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
    $bundleRoot = Join-Path $project "Saved\NodeToCode\TestBundles\N2C_Automation_$stamp"
    $bundleZip = "$bundleRoot.zip"
    $logRoot = Join-Path $bundleRoot 'Logs'
    $reportRoot = Join-Path $bundleRoot 'Reports'
    $crashRoot = Join-Path $bundleRoot 'Crashes'
    New-Item -ItemType Directory -Path $logRoot, $reportRoot, $crashRoot -Force | Out-Null
    $summary.result_bundle = $bundleZip
    $summary.result_directory = $bundleRoot

    $windowsPowerShell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    if (-not (Test-Path -LiteralPath $windowsPowerShell -PathType Leaf)) {
        $powerShellCommand = Get-Command powershell.exe -ErrorAction SilentlyContinue
        if (-not $powerShellCommand) { throw 'Windows PowerShell 5.1 was not found.' }
        $windowsPowerShell = $powerShellCommand.Source
    }

    $descriptor = Get-Content -LiteralPath (Join-Path $pluginRoot 'NodeToCode.uplugin') -Raw | ConvertFrom-Json
    $summary.plugin_version = [int]$descriptor.Version
    $summary.plugin_version_name = [string]$descriptor.VersionName

    Write-Stage '1/11' 'Static validation'
    $currentStage = 'process runner self-test'
    $currentLog = Join-Path $logRoot 'RunnerExit0.log'
    $summary.process_runner = 'FAIL'
    $expectedRunnerLogs = @(
        (Join-Path $logRoot 'RunnerExit0.log'), (Join-Path $logRoot 'RunnerExit0.stderr.log'),
        (Join-Path $logRoot 'RunnerStreams.log'), (Join-Path $logRoot 'RunnerStreams.stderr.log'),
        (Join-Path $logRoot 'RunnerArgumentProbe.log'), (Join-Path $logRoot 'RunnerArgumentProbe.stderr.log'),
        (Join-Path $logRoot 'RunnerExit7.log'), (Join-Path $logRoot 'RunnerExit7.stderr.log'),
        (Join-Path $logRoot 'RunnerTimeout.log'), (Join-Path $logRoot 'RunnerTimeout.stderr.log')
    )
    foreach ($runnerLog in $expectedRunnerLogs) { Add-Log $runnerLog }
    Write-Host 'Checking child exit codes, timeout handling, log capture, progress parsing, and ZIP cleanup...' -ForegroundColor Gray
    $null = Test-ProcessRunner -PowerShellPath $windowsPowerShell -LogDirectory $logRoot
    $summary.process_runner = 'PASS'
    Write-Host 'Process runner self-test: PASS' -ForegroundColor Green

    $currentStage = 'static validation'
    $currentLog = Join-Path $logRoot 'StaticValidation.log'
    $validateScript = Join-Path $PSScriptRoot 'Validate-N2CFiles.ps1'
    $staticArgs = @(
        '-NoLogo', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $validateScript,
        '-PluginRoot', $pluginRoot, '-AllowBuildProducts'
    )
    $staticRun = Invoke-ProcessWithTimeout -FilePath $windowsPowerShell -Arguments $staticArgs -StdoutPath $currentLog -TimeoutSeconds 600
    Add-Log $currentLog
    Add-Log $staticRun.Stderr
    if ($staticRun.ExitCode -ne 0 -or $staticRun.TimedOut) {
        $summary.static_validation = 'FAIL'
        $staticDetail = Get-ProcessFailureDetail -ExitCode $staticRun.ExitCode -TimedOut $staticRun.TimedOut -StdoutPath $currentLog -StderrPath $staticRun.Stderr
        Add-Failure 'static validation' $staticDetail $currentLog
        throw 'Static validation failed.'
    }
    $summary.static_validation = 'PASS'
    Write-Host 'Static validation: PASS' -ForegroundColor Green

    Write-Stage '2/11' 'UE4 Editor build'
    $currentStage = 'build'
    if (-not $SkipBuild) {
        $targetFiles = @(Get-ChildItem -LiteralPath (Join-Path $project 'Source') -Filter '*Editor.Target.cs' -File -ErrorAction SilentlyContinue)
        $target = if ($targetFiles.Count) { $targetFiles[0].BaseName -replace '\.Target$', '' } else { $uproject.BaseName + 'Editor' }
        $buildScript = Join-Path $PSScriptRoot 'Build-N2CEditor.ps1'
        if (-not (Test-Path -LiteralPath $buildScript -PathType Leaf)) { throw "Build helper not found: $buildScript" }
        $currentLog = Join-Path $logRoot 'Build.log'
        $buildArgs = @(
            '-NoLogo', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $buildScript,
            '-ProjectRoot', $project, '-EngineRoot', $EngineRoot, '-Configuration', $Configuration
        )
        Write-Host "Building target '$target'..." -ForegroundColor Gray
        $buildRun = Invoke-ProcessWithTimeout -FilePath $windowsPowerShell -Arguments $buildArgs -StdoutPath $currentLog -TimeoutSeconds $BuildTimeoutSeconds
        Add-Log $currentLog
        Add-Log $buildRun.Stderr
        $buildPass = $buildRun.ExitCode -eq 0 -and -not $buildRun.TimedOut
        $summary.build = if ($buildPass) { 'PASS' } else { 'FAIL' }
        if (-not $buildPass) {
            $buildDetail = Get-ProcessFailureDetail -ExitCode $buildRun.ExitCode -TimedOut $buildRun.TimedOut -StdoutPath $currentLog -StderrPath $buildRun.Stderr
            Add-Failure 'build' $buildDetail $currentLog
        }
        Write-Host "Build: $($summary.build)" -ForegroundColor $(if ($buildPass) { 'Green' } else { 'Red' })
        Write-Host "N2C_BUILD|result=$($summary.build)|exit_code=$($buildRun.ExitCode)|timeout=$($buildRun.TimedOut)|target=$target|log=$currentLog"
    }
    else {
        Write-Host 'Build: SKIPPED' -ForegroundColor Yellow
    }

    $currentStage = 'automation restore hygiene'
    $quarantinedAutomationRestores = Clear-StaleN2CAutomationRestoreQueue -ProjectDirectory $project
    $summary.automation_restore_hygiene = 'PASS'

    $contractManifestPath = Join-Path $pluginRoot 'Source\Tests\Fixtures\N2C_IMPORT_CONTRACT_MATRIX_V1.json'
    $contractCaseIds = @()
    if (-not $SkipEditorTests -and $summary.build -ne 'FAIL') {
        if (-not (Test-Path -LiteralPath $contractManifestPath -PathType Leaf)) {
            throw "Import contract manifest not found: $contractManifestPath"
        }
        $contractManifest = Get-Content -LiteralPath $contractManifestPath -Raw | ConvertFrom-Json
        $contractCaseIds = @($contractManifest.cases | ForEach-Object { [string]$_.id })
        if ($contractCaseIds.Count -ne [int]$contractManifest.case_count -or $contractCaseIds.Count -eq 0) {
            throw "Import contract manifest case count mismatch: declared=$($contractManifest.case_count); actual=$($contractCaseIds.Count)"
        }
    }

    Write-Stage '3/11' 'Import contract matrix apply'
    $currentStage = 'contract matrix apply'
    if (-not $SkipEditorTests -and $summary.build -ne 'FAIL') {
        $summary.contract_apply = if (Invoke-AutomationStage 'ContractApply' 'NodeToCode.ContractMatrix.Apply' $contractCaseIds 'Contract apply') { 'PASS' } else { 'FAIL' }
    }
    elseif ($SkipEditorTests) { Write-Host 'Contract apply: SKIPPED' -ForegroundColor Yellow }
    else { Write-Host 'Contract apply: NOT RUN because build failed.' -ForegroundColor Yellow }

    Write-Stage '4/11' 'Import contract fresh-process verification'
    $currentStage = 'contract matrix fresh verification'
    if (-not $SkipEditorTests -and $summary.contract_apply -eq 'PASS') {
        $summary.contract_verify_fresh = if (Invoke-AutomationStage 'ContractVerifyFresh' 'NodeToCode.ContractMatrix.VerifyFreshFirst' $contractCaseIds 'Contract fresh verify') { 'PASS' } else { 'FAIL' }
    }
    elseif ($SkipEditorTests) { Write-Host 'Contract fresh verify: SKIPPED' -ForegroundColor Yellow }
    else { Write-Host 'Contract fresh verify: NOT RUN because contract apply did not pass.' -ForegroundColor Yellow }

    Write-Stage '5/11' 'Import contract idempotent reapply'
    $currentStage = 'contract matrix reapply'
    if (-not $SkipEditorTests -and $summary.contract_verify_fresh -eq 'PASS') {
        $summary.contract_reapply = if (Invoke-AutomationStage 'ContractReapply' 'NodeToCode.ContractMatrix.Reapply' $contractCaseIds 'Contract reapply') { 'PASS' } else { 'FAIL' }
    }
    elseif ($SkipEditorTests) { Write-Host 'Contract reapply: SKIPPED' -ForegroundColor Yellow }
    else { Write-Host 'Contract reapply: NOT RUN because fresh verification did not pass.' -ForegroundColor Yellow }

    Write-Stage '6/11' 'Import contract second fresh-process verification'
    $currentStage = 'contract matrix second fresh verification'
    if (-not $SkipEditorTests -and $summary.contract_reapply -eq 'PASS') {
        $summary.contract_verify_fresh_second = if (Invoke-AutomationStage 'ContractVerifyFreshSecond' 'NodeToCode.ContractMatrix.VerifyFreshSecond' $contractCaseIds 'Contract second fresh verify') { 'PASS' } else { 'FAIL' }
    }
    elseif ($SkipEditorTests) { Write-Host 'Contract second fresh verify: SKIPPED' -ForegroundColor Yellow }
    else { Write-Host 'Contract second fresh verify: NOT RUN because reapply did not pass.' -ForegroundColor Yellow }

    Write-Stage '7/11' 'Import contract fixture cleanup'
    $currentStage = 'contract matrix cleanup'
    if (-not $SkipEditorTests -and $summary.contract_apply -ne 'NOT_RUN') {
        $summary.contract_cleanup = if (Invoke-AutomationStage 'ContractCleanup' 'NodeToCode.ContractMatrix.Cleanup' @('ContractMatrixCleanup') 'Contract cleanup') { 'PASS' } else { 'FAIL' }
    }
    elseif ($SkipEditorTests) { Write-Host 'Contract cleanup: SKIPPED' -ForegroundColor Yellow }
    else { Write-Host 'Contract cleanup: NOT RUN because contract matrix was not started.' -ForegroundColor Yellow }

    Write-Stage '8/11' 'Legacy ManualReplay regression suite'
    $currentStage = 'main automation'
    if (-not $SkipEditorTests -and $summary.build -ne 'FAIL' -and $summary.contract_cleanup -eq 'PASS' -and $summary.contract_verify_fresh_second -eq 'PASS') {
        $mainCases = @(
            'FlowAndArrays', 'StructAndDataTable', 'Enum', 'ContextualEventGraph', 'Delegates',
            'FunctionBoundaries', 'StandardMacros', 'GraphBoundaries', 'Widget', 'AIController',
            'BTTask', 'BTService', 'BTDecorator', 'PreflightRejectsWithoutMutation',
            'SandboxPinPreflight', 'RollbackAfterMutation', 'RollbackStructuralEquality', 'DialogDiagnostics',
            'ToolbarCommands', 'RawByteDefaultReopenExport', 'MissingEnumReject'
        )
        $summary.main_automation = if (Invoke-AutomationStage 'MainManualReplay' 'NodeToCode.ManualReplay' $mainCases 'Main automation') { 'PASS' } else { 'FAIL' }
    }
    elseif ($SkipEditorTests) { Write-Host 'Main automation: SKIPPED' -ForegroundColor Yellow }
    else { Write-Host 'Main automation: NOT RUN because the contract matrix did not pass.' -ForegroundColor Yellow }

    Write-Stage '9/11' 'Deferred restore first pass'
    $currentStage = 'restore first pass'
    if (-not $SkipEditorTests -and $summary.main_automation -eq 'PASS') {
        $summary.restore_first_pass = if (Invoke-AutomationStage 'RestoreFirstPass' 'NodeToCode.RestoreFirstPass.ManualReplayPendingRestore' @('DiskRestoreFirstPass') 'Restore first pass') { 'PASS' } else { 'FAIL' }
    }
    elseif ($SkipEditorTests) { Write-Host 'Restore first pass: SKIPPED' -ForegroundColor Yellow }
    else { Write-Host 'Restore first pass: NOT RUN because main automation did not pass.' -ForegroundColor Yellow }

    Write-Stage '10/11' 'Deferred restore second pass'
    $currentStage = 'restore second pass'
    if (-not $SkipEditorTests -and $summary.restore_first_pass -eq 'PASS') {
        $summary.restore_second_pass = if (Invoke-AutomationStage 'RestoreSecondPass' 'NodeToCode.RestoreSecondPass.ManualReplayPendingRestore' @('DiskRestoreSecondPass') 'Restore second pass') { 'PASS' } else { 'FAIL' }
    }
    elseif ($SkipEditorTests) { Write-Host 'Restore second pass: SKIPPED' -ForegroundColor Yellow }
    else { Write-Host 'Restore second pass: NOT RUN because first pass did not pass.' -ForegroundColor Yellow }

    if ($SkipEditorTests) {
        $summary.automation_report_validation = 'SKIPPED'
    }
    elseif (
        $summary.contract_apply -eq 'PASS' -and
        $summary.contract_verify_fresh -eq 'PASS' -and
        $summary.contract_reapply -eq 'PASS' -and
        $summary.contract_verify_fresh_second -eq 'PASS' -and
        $summary.contract_cleanup -eq 'PASS' -and
        $summary.main_automation -eq 'PASS' -and
        $summary.restore_first_pass -eq 'PASS' -and
        $summary.restore_second_pass -eq 'PASS'
    ) {
        $summary.automation_report_validation = 'PASS'
    }
    else {
        $summary.automation_report_validation = 'FAIL'
    }

    Write-Stage '11/11' 'Source package'
    $currentStage = 'package'
    if (-not $SkipPackage) {
        $currentLog = Join-Path $logRoot 'Package.log'
        $packageScript = Join-Path $PSScriptRoot 'Package-N2CPlugin.ps1'
        $packageOutput = Join-Path $project 'Saved\NodeToCode\Releases'
        $packageArgs = @(
            '-NoLogo', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $packageScript,
            '-PluginRoot', $pluginRoot, '-OutputDirectory', $packageOutput
        )
        Write-Host 'Creating clean source-only plugin ZIP...' -ForegroundColor Gray
        $packageRun = Invoke-ProcessWithTimeout -FilePath $windowsPowerShell -Arguments $packageArgs -StdoutPath $currentLog -TimeoutSeconds 900
        Add-Log $currentLog
        Add-Log $packageRun.Stderr
        $packagePass = $packageRun.ExitCode -eq 0 -and -not $packageRun.TimedOut
        $summary.package = if ($packagePass) { 'PASS' } else { 'FAIL' }
        if (-not $packagePass) {
            $packageDetail = Get-ProcessFailureDetail -ExitCode $packageRun.ExitCode -TimedOut $packageRun.TimedOut -StdoutPath $currentLog -StderrPath $packageRun.Stderr
            Add-Failure 'package' $packageDetail $currentLog
        }
        Write-Host "Package: $($summary.package)" -ForegroundColor $(if ($packagePass) { 'Green' } else { 'Red' })
    }
    else {
        Write-Host 'Package: SKIPPED' -ForegroundColor Yellow
    }
}
catch {
    $message = $_.Exception.Message
    $alreadyRecorded = @($summary.failures | Where-Object stage -eq $currentStage).Count -gt 0
    if (-not $alreadyRecorded) { Add-Failure $currentStage $message $currentLog }
    Write-Host "Stage error: $message" -ForegroundColor Red
}
finally {
    if ($project -and $crashRoot) {
        $projectCrash = Join-Path $project 'Saved\Crashes'
        if (Test-Path -LiteralPath $projectCrash) {
            $currentRunCrashDirectories = @(Get-ChildItem -LiteralPath $projectCrash -Directory -ErrorAction SilentlyContinue |
                Where-Object { $_.LastWriteTimeUtc -ge $runStartedUtc.AddSeconds(-5) })
            foreach ($crashDirectory in $currentRunCrashDirectories) {
                Copy-Item -LiteralPath $crashDirectory.FullName -Destination $crashRoot -Recurse -Force -ErrorAction SilentlyContinue
            }
            $summary.crash_reports_collected = $currentRunCrashDirectories.Count
        }
    }

    $summary.completed_utc = (Get-Date).ToUniversalTime().ToString('o')
    $summary.result_directory_retained = [bool]$KeepResultDirectory
    $summary.overall = if (
        $summary.failures.Count -eq 0 -and
        $summary.process_runner -eq 'PASS' -and
        $summary.static_validation -eq 'PASS' -and
        ($SkipBuild -or $summary.build -eq 'PASS') -and
        ($SkipEditorTests -or ($summary.contract_apply -eq 'PASS' -and $summary.contract_verify_fresh -eq 'PASS' -and $summary.contract_reapply -eq 'PASS' -and $summary.contract_verify_fresh_second -eq 'PASS' -and $summary.contract_cleanup -eq 'PASS' -and $summary.main_automation -eq 'PASS' -and $summary.restore_first_pass -eq 'PASS' -and $summary.restore_second_pass -eq 'PASS' -and $summary.automation_report_validation -eq 'PASS')) -and
        ($SkipPackage -or $summary.package -eq 'PASS')
    ) { 'PASS' } else { 'FAIL' }

    $bundleHash = $null
    $resultPath = $bundleZip
    if ($bundleRoot) {
        try {
            $summaryPath = Join-Path $bundleRoot 'N2C_Automation_Summary.json'
            $summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $summaryPath -Encoding UTF8
            New-N2CZipFromDirectory -SourceDirectory $bundleRoot -DestinationPath $bundleZip
            $bundleHash = Get-N2CFileSha256 -LiteralPath $bundleZip
            Set-Content -LiteralPath "$bundleZip.sha256" -Value "$bundleHash  $([IO.Path]::GetFileName($bundleZip))" -Encoding ASCII
            if ($KeepResultDirectory) {
                Write-Host "N2C_BUNDLE_CLEANUP|result=SKIPPED|kept_directory=$bundleRoot"
            }
            else {
                Remove-N2CBundleDirectoryAfterVerifiedZip -SourceDirectory $bundleRoot -VerifiedZipPath $bundleZip
                Write-Host "N2C_BUNDLE_CLEANUP|result=PASS|removed_directory=$bundleRoot"
            }
            Write-Host "N2C_AUTOMATION_RESULT|result=$($summary.overall)|bundle=$bundleZip|directory=$bundleRoot|directory_retained=$($summary.result_directory_retained)|sha256=$bundleHash|failures=$($summary.failures.Count)"
        }
        catch {
            $bundleError = $_.Exception.Message
            $summary.overall = 'FAIL'
            Add-Failure 'result bundle' $bundleError $null
            $resultPath = $bundleRoot
            Remove-Item -LiteralPath $bundleZip, "$bundleZip.sha256" -Force -ErrorAction SilentlyContinue
            try {
                $summary.completed_utc = (Get-Date).ToUniversalTime().ToString('o')
                $summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $bundleRoot 'N2C_Automation_Summary.json') -Encoding UTF8
            }
            catch {}
            Write-Host "N2C_AUTOMATION_RESULT|result=FAIL|bundle=|bundle_directory=$bundleRoot|sha256=|failures=$($summary.failures.Count)"
        }
    }

    Write-FriendlyResult -Result $summary.overall -ResultPath $resultPath -Hash $bundleHash
    Open-ResultInExplorer -Path $resultPath
}

exit $(if ($summary.overall -eq 'PASS') { 0 } else { 1 })
