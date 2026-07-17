# Import contract matrix catalog — Version 193

This catalog is generated from `Source/Tests/Fixtures/N2C_IMPORT_CONTRACT_MATRIX_V1.json`. The JSON manifest is authoritative. Every listed case runs through Apply/compile/save, fresh-process verification, identical reapply, second fresh-process verification, and final cleanup.

Totals: **117 cases**, **38 action tokens**, **63 node/alias families**, **29 runtime guards**.

The scope is exhaustive against the current importer-declared support surface. Deferred systems and arbitrary malformed JSON permutations are not claimed as supported.

## aliases (19)

- `action_patch_animation_graph_wrong_asset_reject` — negative/no-mutation
- `action_patch_event_graph` — positive
- `action_patch_widget_graph` — positive
- `alias_add_event_dispatchers` — positive
- `alias_add_event_graph_nodes` — positive
- `alias_add_member_variables` — positive
- `alias_add_nodes_to_graph` — positive
- `alias_add_variables` — positive
- `alias_delete_dispatcher` — positive
- `alias_delete_dispatchers` — positive
- `alias_delete_event_dispatchers` — positive
- `alias_delete_event_graph_nodes` — positive
- `alias_delete_functions` — positive
- `alias_delete_macros` — positive
- `alias_delete_member_variable` — positive
- `alias_delete_member_variables` — positive
- `alias_delete_variables` — positive
- `alias_import_scs_hierarchy` — positive
- `alias_move_function_category` — positive

## delegates (4)

- `delegate_bind_call_remove` — positive
- `delegate_component_bound` — positive
- `delegate_delete_dispatcher` — positive
- `delegate_missing_identity_reject` — negative/no-mutation

## functions (10)

- `fn_add_impure` — positive
- `fn_add_pure` — positive
- `fn_category` — positive
- `fn_delete` — positive
- `fn_invalid_name_reject` — negative/no-mutation
- `fn_missing_replace_reject` — negative/no-mutation
- `fn_multiple_results` — positive
- `fn_params_locals` — positive
- `fn_rename` — positive
- `fn_replace_body` — positive

## graphs (23)

- `collapsed_create` — positive
- `collapsed_replace` — positive
- `graph_array_ops` — positive
- `graph_branch_sequence` — positive
- `graph_call_functions` — positive
- `graph_cast_self` — positive
- `graph_create_widget` — positive
- `graph_delete_node` — positive
- `graph_duplicate_node_id_reject` — negative/no-mutation
- `graph_events_parent` — positive
- `graph_incompatible_edge_reject` — negative/no-mutation
- `graph_input_nodes` — positive
- `graph_interface_message` — positive
- `graph_knot_edge` — positive
- `graph_missing_graph_reject` — negative/no-mutation
- `graph_missing_node_id_reject` — negative/no-mutation
- `graph_missing_pin_reject` — negative/no-mutation
- `graph_rename_custom_event` — positive
- `graph_select` — positive
- `graph_spawn_actor` — positive
- `graph_unknown_node_reject` — negative/no-mutation
- `macro_add` — positive
- `macro_delete` — positive

## guard_failures (14)

- `datatable_missing_identity_reject` — negative/no-mutation
- `delegate_identity_mismatch_reject` — negative/no-mutation
- `enum_case_mismatch_reject` — negative/no-mutation
- `enum_identity_missing_reject` — negative/no-mutation
- `function_unsupported_parameter_flags_reject` — negative/no-mutation
- `graph_edge_node_missing_reject` — negative/no-mutation
- `input_action_identity_missing_reject` — negative/no-mutation
- `input_key_identity_missing_reject` — negative/no-mutation
- `member_default_trailing_data_reject` — negative/no-mutation
- `pin_default_target_missing_reject` — negative/no-mutation
- `struct_identity_missing_reject` — negative/no-mutation
- `struct_member_identity_missing_reject` — negative/no-mutation
- `struct_member_mismatch_reject` — negative/no-mutation
- `widget_missing_class_identity_reject` — negative/no-mutation

## guarded_nodes (6)

- `guard_node_add_component_reject` — negative/no-mutation
- `guard_node_ease_function_reject` — negative/no-mutation
- `guard_node_input_touch_reject` — negative/no-mutation
- `guard_node_local_ref_pose_wrong_asset_reject` — negative/no-mutation
- `guard_node_mpc_function_reject` — negative/no-mutation
- `guard_node_timeline_reject` — negative/no-mutation

## interfaces (1)

- `interface_implement` — positive

## scs (3)

- `scs_add_component` — positive
- `scs_invalid_class_reject` — negative/no-mutation
- `scs_update_component` — positive

## specialized (6)

- `datatable_linked` — positive
- `datatable_linked_untyped_reject` — negative/no-mutation
- `datatable_literal` — positive
- `enum_node_family` — positive
- `struct_make_set_break` — positive
- `struct_missing_reject` — negative/no-mutation

## subclasses (5)

- `ai_controller_blueprint` — positive
- `bt_decorator_blueprint` — positive
- `bt_service_blueprint` — positive
- `bt_task_blueprint` — positive
- `widget_blueprint` — positive

## variables (26)

- `var_array_default` — positive
- `var_bad_struct_default_reject` — negative/no-mutation
- `var_bool_add` — positive
- `var_change_default` — positive
- `var_class_add` — positive
- `var_delete` — positive
- `var_enum_default` — positive
- `var_float_default` — positive
- `var_get_node` — positive
- `var_get_to_function` — positive
- `var_int_default` — positive
- `var_invalid_type_reject` — negative/no-mutation
- `var_map_default` — positive
- `var_missing_enum_reject` — negative/no-mutation
- `var_name_default` — positive
- `var_object_default` — positive
- `var_raw_byte_default` — positive
- `var_rename` — positive
- `var_rotator_default` — positive
- `var_set_default` — positive
- `var_set_node` — positive
- `var_soft_refs` — positive
- `var_string_default` — positive
- `var_text_default` — positive
- `var_transform_add` — positive
- `var_vector_default` — positive

## Reuse rule

When a new importer action, node constructor, alias, default-storage family, graph family, asset subclass, or guard is added, its release change is incomplete until the manifest maps it to at least one positive or negative case and the static coverage audit reports no unmapped source token.
