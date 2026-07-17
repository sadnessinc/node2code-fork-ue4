# NodeToCode local task report

## Task

Requested behavior and TODO priority/item ID.

## Status

`PASS`, `PARTIAL`, `BLOCKED` or `NO_CHANGE`.

## Changed files/functions

List exact paths and functions.

## Evidence

- static descriptor/JSON/manifest validation:
- Editor build:
- automation filter:
- Apply/compile:
- save result:
- unload/fresh-process reopen/recompile:
- structural comparison:
- manual Unreal Editor/PIE/visual check:

## N2C_NEW_NODE_TEST_GATE

State whether the change adds a node, alias, constructor path, graph/asset family, pin identity rule or coverage promotion. When yes, list:

- matrix entry in `N2C_NODE_TEST_REQUIREMENTS_V1.json`;
- positive mandatory test;
- negative/rollback test when applicable;
- fixture and manifest changes;
- persisted semantics asserted after fresh reopen.

A new node/capability without these items cannot be reported as verified.

## Coverage effect

State whether the affected constructor/class moves between `verified`, `supported_untested`, `guarded`, `partial`, `unsupported`, `cosmetic_only` or `dependency_only`.

## Remaining risk

State UE4.27 API/link/runtime risk and whether it is proven or inferred.

## Backup and rollback

Record backup path and rollback behavior. For expected rejects, state how no-mutation was proven.

## Verification boundary

Distinguish source implementation, build verification, Editor automation, persistence verification and manual/gameplay/visual verification.
