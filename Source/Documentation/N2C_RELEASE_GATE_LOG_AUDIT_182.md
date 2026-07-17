# Version 182 release gate and log audit

Version 182 is a source-only candidate based on the verified Version 181 C++ baseline.

## Runner changes

- Progress now separates completed work from the currently running test:

```text
Progress: [####----------------] Completed 4/20 | Running 5/20: ContextualEventGraph | Elapsed 00:00:42
```

- After the final test it displays `Completed 20/20 | Finalizing UE report...` instead of presenting a completed test as still running.
- `-KeepResultDirectory` keeps the verified unpacked result directory beside the ZIP for Codex or manual inspection.
- Every Editor stage validates both the raw UE log contract and `Reports/<Stage>/index.json`: expected test count, zero failed/not-run/in-process tests, zero test errors and successful per-test state.
- `N2C patch apply trace` diagnostics use `Display`, not `Warning`.

## Mandatory new-node regression gate

Marker: `N2C_NEW_NODE_TEST_GATE`.

Any newly supported importer node, alias, constructor path, asset/graph family, pin identity rule or coverage promotion must add or extend a mandatory regression before it can be called verified. The positive contract is strict preflight, real Apply, compile, save, unload, fresh UE process, reopen, persisted semantic assertion and cleanup. Identity-sensitive or failure-prone paths also need a no-mutation rejection or verified rollback test.

The authoritative mapping is `Source/Tests/Fixtures/N2C_NODE_TEST_REQUIREMENTS_V1.json`. Both static validators check the matrix, mandatory test registration and the deferred AnimGraph claim boundary.

## Parallel execution decision

Parallel main-suite execution is intentionally not enabled. Most ManualReplay cases save/unload real project packages and invoke another fresh `UE4Editor-Cmd` child. Multiple outer workers would multiply UE processes and concurrently mutate one project Content/Saved/Asset Registry/DDC environment. A safe implementation requires isolated project copies per worker and a benchmark proving a net gain; this remains deferred rather than risking nondeterministic false PASS/FAIL results.

## Progress output contract

The runner prints a permanent `Completed {N}/{Total}: {Case} [{Result}]` line for every finished test and keeps a separate live `Running {N}/{Total}: {Case}` line. Fast tests therefore remain visible even when several complete between one-second log polls.
