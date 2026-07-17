# ManualReplay and import-contract automation — Version 195

Run:

```bat
Plugins\node2code\Scripts\RUN_N2C_AUTOMATION_AND_PACK.cmd -KeepResultDirectory
```

The console now reports 11 stages. The first four Editor stages execute the 117-case import-contract matrix across separate UE processes. Expected progress resembles:

```text
Completed 42/117: graph_spawn_actor [Passed]
Progress: [#######-------------] Completed 42/117 | Running 43/117: graph_create_widget | Elapsed 00:07:31
```

The matrix produces 468 per-case phase executions plus cleanup. Version 195 then runs the legacy 21-case ManualReplay and two restore tests. For the next release, reduce the mandatory legacy subset according to `N2C_LEGACY_CONTRACT_COVERAGE_V195.md`: Keep unique historical/end-to-end scenarios, retain only unique assertions from Update rows, and retire full duplicates.

Each automation stage passes only when:

- child process exits 0 and does not time out;
- queue completion marker is present;
- every required `N2C_MANUAL_REPLAY_CASE|...|result=PASS` marker exists;
- the UE JSON report exists;
- report test count matches the manifest;
- failed/not-run/in-process counts are zero;
- every test state is `Success` and every test error count is zero.

Warnings are counted and surfaced separately. They are not silently converted to success when report errors exist.

Results are stored in:

```text
<Project>\Saved\NodeToCode\TestBundles\N2C_Automation_<timestamp>.zip
```

Use `-KeepResultDirectory` to retain the unpacked logs/reports for direct inspection or Codex processing.

The contract manifest is reusable: when a new importer feature is added, add cases and coverage mapping there instead of creating an unrelated one-off runner.
