# Version 181 — progress self-test and result cleanup hotfix

## Trigger

Version 180 passed PowerShell syntax parsing but stopped in the process-runner self-test with:

```text
Automation progress parser self-test failed: started=1; completed=1; current=Finishing Alpha...
```

The probe intentionally ended with a complete `Test Started. Name={Beta}` line without a trailing newline. The incremental parser buffered that complete line as if it were partial.

## Fix

The parser now recognizes a trailing line as complete when it contains a fully closed `Test Started`, `Test Completed`, or `N2C_MANUAL_REPLAY_CASE` marker. Truly incomplete lines remain buffered. The same no-final-newline probe remains in the runtime self-test.

## Result directory cleanup

After writing the diagnostic result ZIP, the orchestrator now reopens it and verifies every source file by entry name, size and SHA-256. Only after this verification succeeds does it remove:

```text
Saved/NodeToCode/TestBundles/N2C_Automation_<stamp>/
```

The `.zip` and `.zip.sha256` files remain. If creation, verification or deletion fails, the diagnostic directory is preserved and the result-bundle stage reports an error.

## Verification status

Cross-platform static validation is performed for this candidate. Windows PowerShell 5.1 runtime, UE4.27 build, Main ManualReplay and both restore stages remain NOT RUN in the packaging environment.
