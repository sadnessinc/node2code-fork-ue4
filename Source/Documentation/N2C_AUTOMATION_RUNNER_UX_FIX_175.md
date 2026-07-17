# N2C automation runner and UX fix — Version 175

## Evidence from the Version 174 user run

The diagnostic bundle showed:

- static validation: PASS;
- `Build.log`: UnrealHeaderTool completed, all nine build actions completed, `UE4Editor-NodeToCode.dll` and `testEditor.target` were produced;
- summary build field: false FAIL with an empty `exit_code`;
- packaging: false rejection of `node2code\CHANGELOG.md` as an unexpected archive root.

The C++ build itself therefore completed. The two failures were in orchestration and packaging logic, before Editor automation could start.

## Exit-code correction

`Invoke-ProcessWithTimeout` now calls `WaitForExit()` and `Refresh()` before reading `System.Diagnostics.Process.ExitCode`. This is required for reliable PowerShell 5.1 behavior after polling `HasExited`. The exit code is cast to an integer and can no longer silently become an empty value.

## ZIP correction

`Package-N2CPlugin.ps1` no longer relies on `ZipFile.CreateFromDirectory`. It creates each ZIP entry explicitly and normalizes the entry name to `/`. Verification also normalizes names before root and forbidden-path checks.

Required archive root:

```text
node2code/
```

## Console UX

The default CMD shows:

1. static validation;
2. UE4 Editor build;
3. main ManualReplay automation;
4. deferred restore first pass;
5. deferred restore second pass;
6. source package.

On failure it prints the stopped stage, details, primary log and diagnostic result ZIP. On success it prints the diagnostic result ZIP and SHA-256. The CMD waits for a key before closing. The PowerShell runner selects the result ZIP in a new Windows Explorer window unless `-NoOpenResult` is passed. `-NoPause` disables the CMD key prompt for headless use.

## Evidence boundary

Version 175 is still a candidate. The corrected full runner must complete main automation plus both restore passes before promotion to verified.
