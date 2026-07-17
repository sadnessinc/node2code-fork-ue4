# N2C PowerShell/CMD process audit — Version 178

## Trigger

A real Version 177 run printed:

```text
Details: exit_code=125; N2C_VALIDATE|result=PASS
```

`Validate-N2CFiles.ps1` completed successfully. The parent process wrapper nevertheless treated the child code as unavailable and substituted 125.

## Root cause

The old wrapper used `Start-Process -PassThru`, polling and later reading the returned process object's `ExitCode`. This was not reliable in the observed Windows PowerShell 5.1 run. The implementation also had accumulated separate code paths for build and automation children, making regressions difficult to prove.

## Version 178 implementation

`Invoke-N2CFullValidation.ps1` now owns one native runner based on `System.Diagnostics.ProcessStartInfo`:

- `UseShellExecute = false`;
- redirected stdout and stderr;
- `ReadToEndAsync()` for both streams;
- `WaitForExit(timeout)` followed by parameterless `WaitForExit()` on normal completion;
- `ExitCode` read only after termination;
- timeout exit 124;
- `taskkill.exe /PID <id> /T /F`, with `Process.Kill()` fallback;
- explicit Windows command-line argument quoting, including empty values, quotes and trailing backslashes.

## Mandatory runner self-tests

Before the static validator starts, controlled children verify:

1. exit 0 propagation and stdout capture;
2. independent stdout and stderr capture;
3. invocation of a script whose filename contains spaces;
4. preservation of an argument containing spaces and a trailing backslash;
5. exit 7 propagation;
6. timeout detection and process-tree termination.

Any failure stops at `process runner self-test`, before build or Editor tests.

## Full script audit corrections

- `Search-UE427Source.ps1`: removed the final accidental `$Matches` reference and uses `$sourceMatches`.
- `Run-N2CVerification.ps1`: requires queue completion, rejects fatal/failure markers and requires every main ManualReplay marker.
- `Run-N2CProjectExport.ps1`: accepts only a project-export ZIP created by the current run.
- `RUN_N2C_AUTOMATION_AND_PACK.cmd`: uses a `SHIFT` loop to inspect `-NoPause`, forwards original `%*`, preserves `%ERRORLEVEL%`, and returns it with `exit /b`.
- All PowerShell scripts use terminating error policy and StrictMode 2.0.
- Static validation parses every `.ps1` with the Windows PowerShell parser and inspects its AST for protected automatic-variable writes.
- Source-only hygiene now rejects `__pycache__`, `.pyc` and `.pyo`.
- Diagnostic and source ZIP writers create explicit forward-slash entry names.
- Final bundle failure leaves the result directory and reports `result bundle` instead of hiding the earlier test result.

## Evidence boundary

The cross-platform validator checks structure, manifests, contracts and basic delimiter balance. It cannot prove Windows PowerShell execution. The native parser and runner self-tests execute only on Windows. UE4.27 build and Editor automation remain separate required gates.
