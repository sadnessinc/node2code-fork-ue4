# NodeToCode Version 178 release notes

VersionName: `1.2.78-ue427-powershell-process-audit-candidate`

## Fixed

- Replaced unreliable `Start-Process -PassThru` child exit handling that produced false exit code 125 after a child PASS.
- Added one `ProcessStartInfo` runner for static validation, build, Editor tests and packaging.
- Added exit-code, stream, quoting and timeout self-tests before static validation.
- Fixed the remaining `$Matches` automatic-variable collision in UE source search.
- Prevented standalone verification from passing on exit code alone.
- Prevented project-export verification from reusing an old archive.
- Removed and banned Python bytecode caches from source releases.
- Standardized diagnostic ZIP entries to forward slashes.
- Preserved friendly error/success output, key pause and automatic Explorer opening.
- Preserved a result directory when final diagnostic ZIP creation fails.

## Unchanged

Importer/exporter C++ behavior, rollback semantics, raw Byte support and ManualReplay fixtures are unchanged from Version 177.

## Verification status

- Cross-platform static/package validation: PASS at packaging time.
- Windows PowerShell parser/process self-test: NOT RUN in the packaging environment.
- UE4.27 C++ build: NOT RUN for Version 178.
- Main ManualReplay: NOT RUN.
- Restore first/second pass: NOT RUN.

This release remains a candidate.
