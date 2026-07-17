# NodeToCode Version 181 release notes

**Version:** 181  
**VersionName:** `1.2.81-ue427-progress-selftest-cleanup-hotfix-candidate`

## Fixed

- Complete trailing automation log lines no longer remain stuck in `PendingText` solely because the newline has not yet been flushed.
- The runtime progress parser probe now passes while still testing the no-final-newline case.
- Diagnostic result directories are deleted after a verified ZIP is created.
- Diagnostic ZIPs are verified by entry count, path, size and SHA-256 before source deletion.
- A failed result ZIP/cleanup no longer leaves a stale ZIP that could be mistaken for a valid bundle.

## Not changed

Blueprint importer/exporter C++ behavior is unchanged from Version 180.

## Status

Candidate. UE4.27 Editor automation has not been executed in the packaging environment.
