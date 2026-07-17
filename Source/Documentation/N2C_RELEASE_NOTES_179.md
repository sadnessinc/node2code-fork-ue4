# NodeToCode Version 179 release notes

**Version:** 179  
**VersionName:** `1.2.79-ue427-manual-replay-fixes-progress-candidate`

Changes:

- fixed strict positive InputKey fixture metadata in `ContextualEventGraph`;
- fixed persisted Blueprint member default export by reading the generated-class CDO;
- retained local-variable default serialization behavior;
- added raw Byte export diagnostics for category, subtype and default;
- added live `N/20` progress, current test and elapsed time during Editor automation;
- changed failure summaries to list concrete failed cases;
- fixed final result log selection after the package stage;
- added static checks for both regression fixes and progress-runner markers.

Status: source-only candidate. Version 178 build evidence is real, but Version 179 main and restore automation have not yet been run.
