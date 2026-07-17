# NodeToCode UE4.27 automation report — run 20260716_014238

## Verdict

**PASS with non-blocking warnings.** Version 181 built successfully and all mandatory automation stages completed. The result proves the Version 181 C++ baseline on UE4.27.2 / Win64 / VS2019 v142. It does not automatically verify later runner or documentation changes.

Evidence bundle SHA-256:

```text
d85e69e7e1584ebef81066717328759165fffdb993ca49d04807ecfe1eec051e
```

## Stage results

| Stage | Result | Evidence |
|---|---:|---|
| PowerShell syntax preflight | PASS | 9 scripts parsed |
| Process runner self-test | PASS | exit 0/7, streams, quoting, timeout, progress and ZIP cleanup |
| Static validation | PASS | Version 181 working tree |
| UE4 Editor build | PASS | `testEditor`, exit code 0 |
| Main ManualReplay | PASS | 20/20 tests, 0 failed, 0 not run |
| Deferred restore first pass | PASS | real mutation and queued restore manifest |
| Deferred restore second pass | PASS | bytes restored, manifest consumed, compile passed, mutation absent |
| Source package | PASS | source-only package created |
| Result bundle verification | PASS | ZIP path/size/SHA-256 verification before cleanup |

Main ManualReplay duration was approximately 256.36 seconds. The full orchestration completed in approximately 5 minutes 21 seconds including build, startup, restore processes and packaging.

## Test result audit

The UE automation JSON report contains:

```text
succeeded: 3
succeeded_with_warnings: 17
failed: 0
not_run: 0
in_process: 0
errors: 0
warnings: 241
```

All 20 `Test Completed` records are `Passed`. All 20 required main case markers are present. Both restore case markers are present. No fatal error, assertion, ensure, unhandled exception, access violation, automation error, Blueprint compiler error, save error or crash callstack was found.

## Warning classification

The 241 test-report warnings are not hidden failures:

- 238 are `N2C patch apply trace` diagnostics emitted with `Warning` severity. Version 182 changes only those trace messages to `Display`, so successful tests are no longer reported as `Success with warnings` for trace output.
- 3 are expected missing-object warnings from `MissingEnumReject`, which intentionally resolves a nonexistent enum and proves rejection before mutation.

The raw UE logs also contain environment/project warnings outside the NodeToCode test report:

- optional PIX, OpenVR and Magic Leap initialization warnings in headless mode;
- optional profiler DLL load messages (`aqProf`/VTune) in process stdout;
- an existing project asset `Content/Test/test3.uasset` reports seven invalid `Make Vector.execute` compatibility links and an unknown `MoveForward` axis on every UE startup.

The `test3.uasset` warnings are real project-content warnings, but they are unrelated to generated NodeToCode fixtures and did not affect the automation results. They should be cleaned separately to keep future logs quiet.

## Negative-test FAIL marker

`MissingEnumReject` contains an internal round-trip stage marker with `result=FAIL` and code `enum_member_type_unresolved`. This is expected: the automation case itself ends with `result=PASS` because it proves the invalid enum is rejected without mutation.

## Promotion boundary

Version 181 has real build, main automation and two-process restore evidence. Version 182 changes runner/report validation, progress text, trace severity and release rules, so Version 182 remains a candidate until the full command is run again.
