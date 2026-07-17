# NodeToCode Version 183 release notes

Version: **183**  
VersionName: **1.2.83-ue427-sandbox-preflight-restore-notice-candidate**

- Added transient duplicate-Blueprint sandbox validation to every ordinary dry run. Real UE4.27 node allocation, pin resolution and schema connection attempts now happen before target mutation.
- Added canonical and compatibility handling for `UK2Node_Knot` `InputPin`/`OutputPin`.
- Added mandatory `NodeToCode.ManualReplay.SandboxPinPreflight` regression.
- Deferred restore now attempts at queue-processing startup and again during clean UE shutdown. It writes `N2C_LAST_PENDING_RESTORE_STATUS.txt`; the next interactive startup shows a modal result even when shutdown already consumed the queue. Commandlets log instead of opening UI.
- Added the mandatory top-level AI authoring contract `N2C_AI_JSON_IMPORT_AUTHORING_RULES.md`.
- Added project-export evidence showing Version 182 failed restore left earlier actions on disk.
- Added corrected final manual project patch with one entry per Blueprint and canonical pin names.
- Added P1/P4 TODO items for project Struct/Enum import tests and Project Settings mappings.

Status: source-only candidate. UE4.27 build and automation must be run on the target machine.
