# Documentation and release rules

- Read `N2C_DOCUMENT_AUTHORITY.md` first.
- Keep `N2C_CURRENT_STATE.md` factual and compact.
- Keep `N2C_TODO.md` limited to unfinished work.
- Never claim build, automation, fresh-process or manual evidence that was not run.
- Do not store raw logs, dumps or generated test folders in the source plugin.
- `N2C_NEW_NODE_TEST_GATE`: every new supported node/alias/constructor branch/graph family/asset subclass/pin identity rule or coverage promotion must add or extend a mandatory regression and update `N2C_NODE_TEST_REQUIREMENTS_V1.json` in the same change.
- A runtime node regression must cover Apply, compile, save, unload, fresh-process reopen and persisted semantics. Add no-mutation rejection or verified rollback coverage where failure is possible.
- Deferred Animation/AnimGraph, Niagara, Behavior Tree asset, Blackboard and EQS capabilities may not be described as verified until their dedicated mandatory tests exist and pass.
