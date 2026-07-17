# Legacy ManualReplay coverage decisions — Version 195

This document classifies the legacy suite against the 117-case contract matrix. Version 195 still runs all 21 legacy cases and both restore processes for its final compatibility proof. For the next release gate, only unique historical and end-to-end scenarios marked **Keep** remain mandatory; **Update** cases retain only their unique integration assertions; **Retire** cases leave the mandatory legacy gate because their behavior is fully covered by named ContractMatrix cases.

| Legacy test | Covering ContractMatrix case(s) | Decision |
|---|---|---|
| `FlowAndArrays` | `graph_branch_sequence`, `graph_array_ops`, `var_array_default` | Update — keep combined executable/persistence path only |
| `StructAndDataTable` | `struct_make_set_break`, `datatable_literal`, `datatable_linked` | Keep — unique combined save/reopen integration |
| `Enum` | `enum_node_family`, `var_enum_default` | Retire — full component duplicate |
| `ContextualEventGraph` | `graph_events_parent`, `graph_spawn_actor`, `var_class_add`, `var_rotator_default`, `var_bad_struct_default_reject`, `graph_incompatible_edge_reject` | Update — keep combined compile/save/reopen integration; accept exact apply-stage guard as well as preflight rejection |
| `Delegates` | `delegate_bind_call_remove`, `delegate_component_bound`, `alias_add_event_dispatchers` | Keep — unique same-patch handler/dispatcher lifecycle |
| `FunctionBoundaries` | `fn_add_impure`, `fn_add_pure`, `fn_multiple_results` | Retire — full duplicate |
| `StandardMacros` | `macro_add`, node-family coverage | Keep — unique standard-macro identity/persistence history |
| `GraphBoundaries` | `collapsed_create`, `collapsed_replace`, `macro_add` | Keep — unique repeat-import and fresh-reopen boundary integration |
| `Widget` | `widget_blueprint`, `graph_create_widget` | Retire — full duplicate |
| `AIController` | `ai_controller_blueprint`, `graph_events_parent` | Retire — full duplicate |
| `BTTask` | `bt_task_blueprint` | Retire — full duplicate |
| `BTService` | `bt_service_blueprint` | Retire — full duplicate |
| `BTDecorator` | `bt_decorator_blueprint` | Retire — full duplicate |
| `PreflightRejectsWithoutMutation` | 36 matrix negative/no-mutation cases | Keep — unique transaction-wide hash equality |
| `SandboxPinPreflight` | `graph_missing_pin_reject`, `graph_knot_edge` | Update — keep bad-then-good transient-sandbox integration |
| `RollbackAfterMutation` | none (test-only post-mutation fault) | Keep — unique real Editor Undo regression |
| `RollbackStructuralEquality` | none (fault-injection snapshot equality) | Keep — unique rollback structural proof |
| `DialogDiagnostics` | none | Keep — unique human-readable diagnostics integration |
| `ToolbarCommands` | none | Keep — unique command registration contract |
| `RawByteDefaultReopenExport` | `var_raw_byte_default`, `var_rotator_default`, `var_bad_struct_default_reject` | Keep — unique combined CDO/reopen/export integration |
| `MissingEnumReject` | `var_missing_enum_reject` | Retire — full duplicate |
| `RestoreFirstPass.ManualReplayPendingRestore` | none | Keep — unique startup-only restore phase 1 |
| `RestoreSecondPass.ManualReplayPendingRestore` | none | Keep — unique fresh-process byte/compile restore phase 2 |

The two previously failing legacy cases were therefore not reasons to weaken the production importer: `ContextualEventGraph` had a stale phase-specific expectation and was updated while preserving its unique integration coverage; `RawByteDefaultReopenExport` was kept because its combined reopen/export proof is not duplicated by any single matrix case.
