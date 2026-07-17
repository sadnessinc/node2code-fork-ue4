#!/usr/bin/env python3
"""Cross-platform static release validator for the NodeToCode UE4.27 source package.

This script does not compile Unreal code and never reports Editor automation as passed.
It validates package hygiene, JSON/manifests, registered test contracts, basic C++
lexical/preprocessor balance, duplicate patch node ids/edge references, secrets and
machine-specific paths. It writes a deterministic JSON report when --report is used.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from collections import Counter
from pathlib import Path
from typing import Any

FORBIDDEN_DIRS = {"Binaries", "Intermediate", "Saved", ".vs", "DerivedDataCache", "__pycache__"}
FORBIDDEN_EXTS = {".dll", ".pdb", ".obj", ".lib", ".exp", ".log", ".dmp", ".pyc", ".pyo"}
TEXT_EXTS = {".md", ".json", ".cpp", ".h", ".cs", ".ps1", ".cmd", ".ini", ".uplugin"}
SECRET_PATTERNS = {
    "private_key": re.compile(r"BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY"),
    "github_token": re.compile(r"\bgh[pousr]_[A-Za-z0-9]{20,}\b"),
    "openai_key": re.compile(r"\bsk-[A-Za-z0-9_-]{20,}\b"),
    "bearer_token": re.compile(r"\bBearer\s+[A-Za-z0-9._-]{20,}\b", re.I),
}
ABSOLUTE_WINDOWS_PATH = re.compile(r"(?<![A-Za-z0-9_])[A-Za-z]:\\")
REQUIRED_TESTS = [
    "NodeToCode.ManualReplay.FlowAndArrays",
    "NodeToCode.ManualReplay.StructAndDataTable",
    "NodeToCode.ManualReplay.Enum",
    "NodeToCode.ManualReplay.ContextualEventGraph",
    "NodeToCode.ManualReplay.Delegates",
    "NodeToCode.ManualReplay.FunctionBoundaries",
    "NodeToCode.ManualReplay.StandardMacros",
    "NodeToCode.ManualReplay.GraphBoundaries",
    "NodeToCode.ManualReplay.Widget",
    "NodeToCode.ManualReplay.AIController",
    "NodeToCode.ManualReplay.BTTask",
    "NodeToCode.ManualReplay.BTService",
    "NodeToCode.ManualReplay.BTDecorator",
    "NodeToCode.ManualReplay.PreflightRejectsWithoutMutation",
    "NodeToCode.ManualReplay.SandboxPinPreflight",
    "NodeToCode.ManualReplay.RollbackAfterMutation",
    "NodeToCode.ManualReplay.RollbackStructuralEquality",
    "NodeToCode.RestoreFirstPass.ManualReplayPendingRestore",
    "NodeToCode.RestoreSecondPass.ManualReplayPendingRestore",
    "NodeToCode.ManualReplay.DialogDiagnostics",
    "NodeToCode.ManualReplay.ToolbarCommands",
    "NodeToCode.ManualReplay.RawByteDefaultReopenExport",
    "NodeToCode.ManualReplay.MissingEnumReject",
]

REQUIRED_FILES = [
    "NodeToCode.uplugin",
    "README.md",
    "CODEX_PLUGIN_START_HERE.md",
    "N2C_AI_JSON_IMPORT_AUTHORING_RULES.md",
    "Source/NodeToCode.Build.cs",
    "Source/Private/Core/N2CPatchImporter.cpp",
    "Source/Private/Core/N2CEditorIntegration.cpp",
    "Source/Private/N2CVerificationTests.cpp",
    "Source/Tests/Fixtures/N2C_P0_FIXTURE_MANIFEST_V1.json",
    "Source/Tests/Fixtures/ManualReplay/N2C_MANUAL_REPLAY_MANIFEST_V1.json",
    "Source/Tests/Fixtures/N2C_NODE_TEST_REQUIREMENTS_V1.json",
    "Source/Tests/Fixtures/N2C_IMPORT_CONTRACT_MATRIX_V1.json",
    "Source/Documentation/N2C_CURRENT_STATE.md",
    "Source/Documentation/N2C_AUTOMATION_FAILURE_AUDIT_20260716_143553.md",
    "Source/Documentation/N2C_AUTOMATION_REPORT_20260716_092715.md",
    "Source/Documentation/N2C_MANUAL_DRY_RUN_STRUCT_PIN_AUDIT_20260716.md",
    "Source/Documentation/N2C_AUTOMATION_REPORT_20260716_090558.md",
    "Source/Documentation/N2C_FAILED_IMPORT_PROJECT_EXPORT_AUDIT_20260716_031216.md",
    "Source/Documentation/N2C_AUTOMATION_REPORT_20260716_014238.md",
    "Source/Documentation/N2C_RELEASE_GATE_LOG_AUDIT_182.md",
    "Source/Documentation/N2C_RELEASE_NOTES_182.md",
    "Source/Documentation/N2C_STATIC_AUDIT_182.json",
    "Source/Documentation/N2C_PROGRESS_SELFTEST_CLEANUP_HOTFIX_181.md",
    "Source/Documentation/N2C_RELEASE_NOTES_181.md",
    "Source/Documentation/N2C_STATIC_AUDIT_181.json",
    "Source/Documentation/N2C_POWERSHELL_PROGRESS_PARSER_HOTFIX_180.md",
    "Source/Documentation/N2C_RELEASE_NOTES_180.md",
    "Source/Documentation/N2C_STATIC_AUDIT_180.json",
    "Source/Documentation/N2C_MANUAL_REPLAY_FIXES_PROGRESS_179.md",
    "Source/Documentation/N2C_RELEASE_NOTES_179.md",
    "Source/Documentation/N2C_STATIC_AUDIT_179.json",
    "Source/Documentation/N2C_POWERSHELL_PROCESS_AUDIT_178.md",
    "Source/Documentation/N2C_RELEASE_NOTES_178.md",
    "Source/Documentation/N2C_POWERSHELL_RUNTIME_HOTFIX_177.md",
    "Source/Documentation/N2C_RELEASE_NOTES_177.md",
    "Source/Documentation/N2C_POWERSHELL_PARSER_HOTFIX_176.md",
    "Source/Documentation/N2C_RELEASE_NOTES_176.md",
    "Source/Documentation/N2C_FULL_REVIEW_173.md",
    "Source/Documentation/N2C_AUTOMATION_RUNNER_UX_FIX_175.md",
    "Source/Documentation/N2C_RELEASE_NOTES_175.md",
    "Source/Documentation/N2C_AUTOMATION_RUNNER_HOTFIX_174.md",
    "Source/Documentation/N2C_RELEASE_NOTES_174.md",
    "Source/Documentation/N2C_TODO.md",
    "Scripts/RUN_N2C_AUTOMATION_AND_PACK.cmd",
    "Scripts/Codex/Audit-N2CProjectExport.ps1",
    "Scripts/Codex/Build-N2CEditor.ps1",
    "Scripts/Codex/Invoke-N2CFullValidation.ps1",
    "Scripts/Codex/Package-N2CPlugin.ps1",
    "Scripts/Codex/Run-N2CProjectExport.ps1",
    "Scripts/Codex/Run-N2CVerification.ps1",
    "Scripts/Codex/Search-UE427Source.ps1",
    "Scripts/Codex/Test-N2CPowerShellSyntax.ps1",
    "Scripts/Codex/Validate-N2CFiles.ps1",
    "Scripts/Codex/Validate-N2CImportContractMatrix.py",
    "Scripts/Codex/validate_n2c_release.py",
]

REQUIRED_CASE_MARKERS = [
    "FlowAndArrays", "StructAndDataTable", "Enum", "ContextualEventGraph",
    "Delegates", "FunctionBoundaries", "StandardMacros", "GraphBoundaries",
    "Widget", "AIController", "BTTask", "BTService", "BTDecorator",
    "PreflightRejectsWithoutMutation", "SandboxPinPreflight", "RollbackAfterMutation",
    "RollbackStructuralEquality", "DialogDiagnostics", "ToolbarCommands",
    "RawByteDefaultReopenExport", "MissingEnumReject", "DiskRestoreFirstPass",
    "DiskRestoreSecondPass",
]


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def strip_cpp(text: str) -> str:
    """Replace comments and literals with spaces while preserving newlines."""
    out: list[str] = []
    i = 0
    n = len(text)
    state = "code"
    raw_end = ""
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if state == "code":
            if c == "/" and nxt == "/":
                out.extend("  "); i += 2; state = "line_comment"; continue
            if c == "/" and nxt == "*":
                out.extend("  "); i += 2; state = "block_comment"; continue
            if c == 'R' and nxt == '"':
                # C++ raw literal: R"delimiter(contents)delimiter"
                j = text.find("(", i + 2)
                if j != -1 and j - (i + 2) <= 16:
                    delim = text[i + 2:j]
                    raw_end = ")" + delim + '"'
                    out.extend(" " * (j + 1 - i)); i = j + 1; state = "raw"; continue
            if c == '"': out.append(" "); i += 1; state = "string"; continue
            if c == "'": out.append(" "); i += 1; state = "char"; continue
            out.append(c); i += 1; continue
        if state == "line_comment":
            if c == "\n": out.append("\n"); state = "code"
            else: out.append(" ")
            i += 1; continue
        if state == "block_comment":
            if c == "*" and nxt == "/": out.extend("  "); i += 2; state = "code"; continue
            out.append("\n" if c == "\n" else " "); i += 1; continue
        if state in {"string", "char"}:
            if c == "\\":
                out.append(" "); i += 1
                if i < n: out.append("\n" if text[i] == "\n" else " "); i += 1
                continue
            end = '"' if state == "string" else "'"
            if c == end: out.append(" "); i += 1; state = "code"; continue
            out.append("\n" if c == "\n" else " "); i += 1; continue
        if state == "raw":
            if raw_end and text.startswith(raw_end, i):
                out.extend(" " * len(raw_end)); i += len(raw_end); state = "code"; raw_end = ""; continue
            out.append("\n" if c == "\n" else " "); i += 1; continue
    return "".join(out)


def balance_cpp(path: Path) -> list[str]:
    text = path.read_text(encoding="utf-8-sig", errors="replace")
    clean = strip_cpp(text)
    errors: list[str] = []
    pairs = {"{": "}", "(": ")", "[": "]"}
    closing = {v: k for k, v in pairs.items()}
    stack: list[tuple[str, int]] = []
    line = 1
    for c in clean:
        if c == "\n": line += 1; continue
        if c in pairs: stack.append((c, line))
        elif c in closing:
            if not stack or stack[-1][0] != closing[c]:
                errors.append(f"unexpected {c} at line {line}")
            else: stack.pop()
    for c, ln in stack[-20:]: errors.append(f"unclosed {c} from line {ln}")

    pp_stack: list[tuple[str, int]] = []
    for ln, raw in enumerate(clean.splitlines(), 1):
        m = re.match(r"\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b", raw)
        if not m: continue
        token = m.group(1)
        if token in {"if", "ifdef", "ifndef"}: pp_stack.append((token, ln))
        elif token in {"elif", "else"}:
            if not pp_stack: errors.append(f"#{token} without #if at line {ln}")
        elif token == "endif":
            if not pp_stack: errors.append(f"#endif without #if at line {ln}")
            else: pp_stack.pop()
    for token, ln in pp_stack: errors.append(f"unclosed #{token} from line {ln}")
    return errors


def strip_powershell(text: str) -> str:
    """Remove ordinary PowerShell comments/strings for delimiter checks.

    Release scripts intentionally do not use here-strings, so a compact scanner is
    sufficient and avoids claiming a full PowerShell parse.
    """
    out: list[str] = []
    i = 0
    state = "code"
    quote = ""
    while i < len(text):
        c = text[i]
        nxt = text[i + 1] if i + 1 < len(text) else ""
        if state == "code":
            if c == "#": out.append(" "); i += 1; state = "comment"; continue
            if c in {"'", '"'}: out.append(" "); quote = c; i += 1; state = "string"; continue
            out.append(c); i += 1; continue
        if state == "comment":
            if c == "\n": out.append("\n"); state = "code"
            else: out.append(" ")
            i += 1; continue
        if state == "string":
            if quote == '"' and c == "`":
                out.append(" "); i += 1
                if i < len(text): out.append("\n" if text[i] == "\n" else " "); i += 1
                continue
            if c == quote:
                if quote == "'" and nxt == "'": out.extend("  "); i += 2; continue
                out.append(" "); i += 1; state = "code"; continue
            out.append("\n" if c == "\n" else " "); i += 1; continue
    return "".join(out)


def balance_powershell(path: Path) -> list[str]:
    clean = strip_powershell(path.read_text(encoding="utf-8-sig", errors="replace"))
    pairs = {"{": "}", "(": ")", "[": "]"}
    closing = {v: k for k, v in pairs.items()}
    stack: list[tuple[str, int]] = []
    errors: list[str] = []
    line = 1
    for c in clean:
        if c == "\n": line += 1; continue
        if c in pairs: stack.append((c, line))
        elif c in closing:
            if not stack or stack[-1][0] != closing[c]: errors.append(f"unexpected {c} at line {line}")
            else: stack.pop()
    for c, ln in stack[-20:]: errors.append(f"unclosed {c} from line {ln}")
    return errors


def validate_patch_object(obj: dict[str, Any], label: str) -> list[str]:
    errors: list[str] = []
    if obj.get("schema") != "N2C_PATCH_V1": errors.append(f"{label}: schema is not N2C_PATCH_V1")
    actions = obj.get("actions")
    if not isinstance(actions, list) or not actions:
        errors.append(f"{label}: actions[] missing/empty")
        return errors
    for ai, action in enumerate(actions):
        where = f"{label}:actions[{ai}]"
        if not isinstance(action, dict): errors.append(f"{where}: not an object"); continue
        if not isinstance(action.get("type"), str) or not action["type"]: errors.append(f"{where}: type missing")
        nodes = action.get("nodes", [])
        if nodes is None: nodes = []
        if not isinstance(nodes, list): errors.append(f"{where}: nodes is not an array"); continue
        ids: list[str] = []
        for ni, node in enumerate(nodes):
            if not isinstance(node, dict): errors.append(f"{where}:nodes[{ni}] not an object"); continue
            nid = node.get("id")
            if not isinstance(nid, str) or not nid: errors.append(f"{where}:nodes[{ni}] id missing")
            else: ids.append(nid)
            if not isinstance(node.get("type"), str) or not node["type"]: errors.append(f"{where}:nodes[{ni}] type missing")
        dup = sorted(k for k, v in Counter(ids).items() if v > 1)
        if dup: errors.append(f"{where}: duplicate node ids: {', '.join(dup)}")
        idset = set(ids)
        for edge_field in ("exec_edges", "data_edges", "edges"):
            edges = action.get(edge_field, [])
            if edges is None: continue
            if not isinstance(edges, list): errors.append(f"{where}:{edge_field} is not an array"); continue
            for ei, edge in enumerate(edges):
                if not isinstance(edge, dict): errors.append(f"{where}:{edge_field}[{ei}] not an object"); continue
                has_node_id_form = "from_node_id" in edge or "to_node_id" in edge
                has_compact_form = "from" in edge or "to" in edge
                if has_node_id_form and has_compact_form:
                    errors.append(f"{where}:{edge_field}[{ei}] mixes from_node_id/to_node_id with from/to")
                src = edge.get("from_node_id", edge.get("from"))
                dst = edge.get("to_node_id", edge.get("to"))
                if src is None: errors.append(f"{where}:{edge_field}[{ei}] source property missing")
                elif src not in idset: errors.append(f"{where}:{edge_field}[{ei}] unknown source {src!r}")
                if dst is None: errors.append(f"{where}:{edge_field}[{ei}] target property missing")
                elif dst not in idset: errors.append(f"{where}:{edge_field}[{ei}] unknown target {dst!r}")
                if not edge.get("from_pin"): errors.append(f"{where}:{edge_field}[{ei}] from_pin missing")
                if not edge.get("to_pin"): errors.append(f"{where}:{edge_field}[{ei}] to_pin missing")
    return errors


def validate_final_manual_pin_contract(obj: dict[str, Any], label: str) -> tuple[list[str], dict[str, Any]]:
    """Reject friendly aliases in the authoritative final manual fixture.

    Runtime compatibility aliases remain supported for older patches. The final
    fixture intentionally uses only UE4.27 internal PinName values so it catches
    drift in AI-authored JSON before a user reaches Editor dry-run.
    """
    errors: list[str] = []
    counters = Counter()
    patch_scopes: set[tuple[str, str, str]] = set()

    def struct_pin_name(node: dict[str, Any]) -> str:
        path = str(node.get("struct_path") or node.get("type_path") or "")
        object_part = path.rsplit(".", 1)[-1]
        return object_part.rsplit("/", 1)[-1]

    for asset_index, asset in enumerate(obj.get("assets", [])):
        if not isinstance(asset, dict):
            continue
        for action_index, action in enumerate(asset.get("actions", [])):
            if not isinstance(action, dict):
                continue
            if action.get("type") == "patch_graph":
                stable_scope = action.get("import_scope") or action.get("action_id")
                if not isinstance(stable_scope, str) or not stable_scope.strip():
                    errors.append(f"{label}:assets[{asset_index}].actions[{action_index}]: maintained patch_graph requires stable import_scope/action_id")
                else:
                    graph_name = str(action.get("graph_name") or "")
                    asset_path = str(asset.get("blueprint_path") or "")
                    scope_key = (asset_path, graph_name, stable_scope.strip())
                    if scope_key in patch_scopes:
                        errors.append(f"{label}: duplicate maintained patch_graph scope {scope_key!r}")
                    patch_scopes.add(scope_key)
                    counters["stable_patch_graph_scopes"] += 1
            nodes = {n.get("id"): n for n in action.get("nodes", []) if isinstance(n, dict)}

            incoming_spawn_transforms: set[str] = set()
            for edge_field in ("data_edges", "edges"):
                for edge in action.get(edge_field, []) or []:
                    if not isinstance(edge, dict):
                        continue
                    target_id = edge.get("to_node_id", edge.get("to"))
                    target_pin = str(edge.get("to_pin", ""))
                    if target_id in nodes and nodes[target_id].get("type") == "K2Node_SpawnActorFromClass" and re.sub(r"[^a-z0-9]", "", target_pin.lower()) == "spawntransform":
                        incoming_spawn_transforms.add(str(target_id))

            for node_id, node in nodes.items():
                if node.get("type") != "K2Node_SpawnActorFromClass":
                    continue
                counters["spawn_actor_nodes"] += 1
                pin_defaults = node.get("pin_defaults") if isinstance(node.get("pin_defaults"), dict) else {}
                if any(re.sub(r"[^a-z0-9]", "", str(k).lower()) == "spawntransform" for k in pin_defaults):
                    errors.append(f"{label}:assets[{asset_index}].actions[{action_index}]: SpawnActorFromClass {node_id!r} must not use a literal SpawnTransform default")
                if str(node_id) not in incoming_spawn_transforms:
                    errors.append(f"{label}:assets[{asset_index}].actions[{action_index}]: SpawnActorFromClass {node_id!r} requires an incoming SpawnTransform edge")
                class_default = pin_defaults.get("Class")
                if not isinstance(class_default, str) or not class_default.startswith(("/Script/", "/Game/")):
                    errors.append(f"{label}:assets[{asset_index}].actions[{action_index}]: SpawnActorFromClass {node_id!r} requires a canonical Class object path")
                else:
                    counters["spawn_actor_class_defaults"] += 1

            for edge_index, edge in enumerate(action.get("data_edges", [])):
                if not isinstance(edge, dict):
                    continue
                has_node_id_form = "from_node_id" in edge or "to_node_id" in edge
                has_compact_form = "from" in edge or "to" in edge
                if has_node_id_form and has_compact_form:
                    errors.append(f"{label}:assets[{asset_index}].actions[{action_index}].data_edges[{edge_index}]: mixed edge schema")
                counters["edge_schema_node_id" if has_node_id_form else "edge_schema_compact"] += 1
                source = nodes.get(edge.get("from_node_id", edge.get("from")))
                target = nodes.get(edge.get("to_node_id", edge.get("to")))
                from_pin = str(edge.get("from_pin", ""))
                to_pin = str(edge.get("to_pin", ""))
                where = f"{label}:assets[{asset_index}].actions[{action_index}].data_edges[{edge_index}]"

                if source and source.get("type") == "K2Node_MakeStruct":
                    expected = struct_pin_name(source)
                    counters["make_struct_edges"] += 1
                    if not expected or from_pin != expected:
                        errors.append(f"{where}: MakeStruct output must be canonical {expected!r}, got {from_pin!r}")
                if target and target.get("type") == "K2Node_BreakStruct":
                    expected = struct_pin_name(target)
                    counters["break_struct_edges"] += 1
                    if not expected or to_pin != expected:
                        errors.append(f"{where}: BreakStruct input must be canonical {expected!r}, got {to_pin!r}")
                if source and source.get("type") == "K2Node_SetFieldsInStruct":
                    counters["set_fields_out_edges"] += 1
                    if from_pin != "StructOut":
                        errors.append(f"{where}: SetFields output must be 'StructOut', got {from_pin!r}")
                if target and target.get("type") == "K2Node_SetFieldsInStruct":
                    counters["set_fields_in_edges"] += 1
                    if to_pin != "StructRef":
                        errors.append(f"{where}: SetFields input must be 'StructRef', got {to_pin!r}")
                if target and target.get("type") == "K2Node_Select" and to_pin in {"A", "B", "False", "True", "false", "true"}:
                    errors.append(f"{where}: Select option uses compatibility alias {to_pin!r}; use 'Option 0'/'Option 1' or exported enum PinName")
                if source and source.get("type") == "K2Node_Knot" and from_pin != "OutputPin":
                    errors.append(f"{where}: Knot output must be 'OutputPin', got {from_pin!r}")
                if target and target.get("type") == "K2Node_Knot" and to_pin != "InputPin":
                    errors.append(f"{where}: Knot input must be 'InputPin', got {to_pin!r}")
                if source and source.get("type") == "K2Node_GetDataTableRow" and from_pin in {"RowFound", "Out Row", "OutRow"}:
                    errors.append(f"{where}: GetDataTableRow uses friendly output alias {from_pin!r}")
                if source and source.get("type") == "K2Node_SwitchEnum" and from_pin == "Default":
                    errors.append(f"{where}: SwitchEnum 'Default' is not a canonical fully-enumerated output")
                if source and source.get("type") == "K2Node_CreateDelegate" and from_pin != "OutputDelegate":
                    errors.append(f"{where}: CreateDelegate output must be canonical 'OutputDelegate', got {from_pin!r}")

    for required_counter in ("make_struct_edges", "break_struct_edges", "set_fields_in_edges", "set_fields_out_edges"):
        if counters[required_counter] < 1:
            errors.append(f"{label}: missing required connected struct pipeline counter {required_counter}")
    if counters["spawn_actor_nodes"] < 1:
        errors.append(f"{label}: missing compile-safe SpawnActorFromClass fixture")
    if counters["spawn_actor_class_defaults"] < 1:
        errors.append(f"{label}: missing canonical SpawnActor Class default fixture")
    for required_schema in ("edge_schema_node_id", "edge_schema_compact"):
        if counters[required_schema] < 1:
            errors.append(f"{label}: final manual fixture does not exercise {required_schema}")
    return errors, dict(counters)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--plugin-root", type=Path, default=Path(__file__).resolve().parents[2])
    ap.add_argument("--reference-root", type=Path)
    ap.add_argument("--report", type=Path)
    args = ap.parse_args()
    root = args.plugin_root.resolve()
    failures: list[str] = []
    warnings: list[str] = []
    checks: dict[str, Any] = {}

    missing_required_files = [rel for rel in REQUIRED_FILES if not (root / rel).is_file()]
    if missing_required_files:
        failures.append("missing required files: " + ", ".join(missing_required_files))
    checks["required_files"] = {"required": len(REQUIRED_FILES), "missing": missing_required_files}

    descriptor_path = root / "NodeToCode.uplugin"
    try:
        descriptor = json.loads(descriptor_path.read_text(encoding="utf-8-sig"))
    except Exception as e:
        descriptor = {}; failures.append(f"descriptor invalid: {e}")
    version = descriptor.get("Version")
    version_name = descriptor.get("VersionName")
    if not isinstance(version, int) or version < 101:
        failures.append(f"invalid plugin Version {version!r}")
    version_match = re.fullmatch(r"1\.2\.(?P<patch>[0-9]+)-ue427-[A-Za-z0-9._-]+", str(version_name or ""))
    if not version_match:
        failures.append(f"VersionName does not match 1.2.<patch>-ue427-<label>: {version_name!r}")
    elif isinstance(version, int) and int(version_match.group("patch")) != version - 100:
        failures.append(f"Version/VersionName mismatch: Version {version} requires 1.2.{version - 100}, got {version_name!r}")
    final_manual_name = f"N2C_FINAL_MANUAL_ALL_STRICT_SUPPORTED_V{version}.json"
    dynamic_required = [
        final_manual_name,
        f"Source/Tests/Fixtures/ManualReplay/{final_manual_name}",
        f"Source/Documentation/N2C_RELEASE_NOTES_{version}.md",
        f"Source/Documentation/N2C_STATIC_AUDIT_{version}.json",
        f"Source/Documentation/N2C_IMPORT_CONTRACT_MATRIX_CATALOG_{version}.md",
    ]
    missing_dynamic = [rel for rel in dynamic_required if not (root / rel).is_file()]
    if missing_dynamic:
        failures.append("missing current-release files: " + ", ".join(missing_dynamic))
    checks["release_metadata"] = {"version": version, "version_name": version_name, "dynamic_required": dynamic_required, "missing": missing_dynamic}

    report_path = args.report.resolve() if args.report else None
    package_manifest_path = (root / "PACKAGE_MANIFEST.json").resolve()
    all_paths = sorted(root.rglob("*"))
    # PACKAGE_MANIFEST.json contains the static-audit hash, while the static audit
    # validates PACKAGE_MANIFEST.json. Keep the manifest out of the audit inventory
    # to avoid a report<->manifest hash cycle; validate it explicitly below instead.
    files = [
        p for p in all_paths
        if p.is_file()
        and p.resolve() != package_manifest_path
        and (report_path is None or p.resolve() != report_path)
    ]
    dirs = [p for p in all_paths if p.is_dir()]
    bad_dirs = [p.relative_to(root).as_posix() for p in dirs if p.name in FORBIDDEN_DIRS]
    bad_files = [p.relative_to(root).as_posix() for p in files if p.suffix.lower() in FORBIDDEN_EXTS]
    if bad_dirs: failures.append("forbidden directories: " + ", ".join(bad_dirs[:20]))
    if bad_files: failures.append("forbidden files: " + ", ".join(bad_files[:20]))
    checks["package_hygiene"] = {"forbidden_directories": bad_dirs, "forbidden_files": bad_files}

    json_results: list[dict[str, Any]] = []
    patch_errors: list[str] = []
    for p in sorted(root.rglob("*.json")):
        if report_path is not None and p.resolve() == report_path:
            continue
        rel = p.relative_to(root).as_posix()
        try:
            obj = json.loads(p.read_text(encoding="utf-8-sig"))
            json_results.append({"path": rel, "result": "PASS"})
            if isinstance(obj, dict) and obj.get("schema") == "N2C_PATCH_V1":
                patch_errors.extend(validate_patch_object(obj, rel))
        except Exception as e:
            json_results.append({"path": rel, "result": "FAIL", "error": str(e)})
            failures.append(f"invalid JSON {rel}: {e}")
    if patch_errors: failures.extend(patch_errors)
    checks["json"] = {"count": len(json_results), "results": json_results, "patch_contract_errors": patch_errors}

    def load_json(rel: str) -> dict[str, Any]:
        return json.loads((root / rel).read_text(encoding="utf-8-sig"))
    try:
        p0 = load_json("Source/Tests/Fixtures/N2C_P0_FIXTURE_MANIFEST_V1.json")
        p0_ids = [x.get("fixture_id") for x in p0.get("fixtures", []) if isinstance(x, dict)]
        if p0.get("fixture_count") != len(p0_ids): failures.append("P0 fixture_count mismatch")
        if len(p0_ids) != len(set(p0_ids)): failures.append("duplicate P0 fixture_id")
        if p0.get("historical_record") is not True or p0.get("current_release_pass_evidence") is not False:
            failures.append("P0 manifest must be explicitly marked historical/non-current evidence")
        checks["p0_manifest"] = {"declared": p0.get("fixture_count"), "actual": len(p0_ids), "unique": len(set(p0_ids)), "historical": p0.get("historical_record")}
    except Exception as e: failures.append(f"P0 manifest validation failed: {e}")
    try:
        man = load_json("Source/Tests/Fixtures/ManualReplay/N2C_MANUAL_REPLAY_MANIFEST_V1.json")
        if man.get("version") != version:
            failures.append(f"ManualReplay manifest version mismatch: expected {version}, got {man.get('version')!r}")
        if man.get("plugin_version") != version:
            failures.append(f"ManualReplay manifest plugin_version mismatch: expected {version}, got {man.get('plugin_version')!r}")
        if man.get("version_name") != version_name:
            failures.append(f"ManualReplay manifest version_name mismatch: expected {version_name!r}, got {man.get('version_name')!r}")
        cases = [x for x in man.get("cases", []) if isinstance(x, dict)]
        case_ids = [x.get("id") for x in cases]
        case_tests = [x.get("test") for x in cases]
        if man.get("case_count") != len(cases): failures.append("ManualReplay case_count mismatch")
        if len(case_ids) != len(set(case_ids)): failures.append("duplicate ManualReplay case id")
        if set(case_tests) != set(REQUIRED_TESTS) or len(case_tests) != len(REQUIRED_TESTS):
            failures.append("ManualReplay manifest test membership mismatch")
        process_counts = Counter(x.get("process") for x in cases)
        if process_counts != Counter({"main": 21, "restore_first": 1, "restore_second": 1}):
            failures.append(f"ManualReplay process counts mismatch: {dict(process_counts)}")
        fixture_names = man.get("fixtures", [])
        missing_fixture_files = [name for name in fixture_names if not (root / "Source/Tests/Fixtures/ManualReplay" / str(name)).is_file()]
        if missing_fixture_files: failures.append("ManualReplay fixture files missing: " + ", ".join(missing_fixture_files))
        checks["manual_replay_manifest"] = {"declared": man.get("case_count"), "actual": len(cases), "unique": len(set(case_ids)), "process_counts": dict(process_counts), "tests": case_tests, "missing_fixture_files": missing_fixture_files}
    except Exception as e: failures.append(f"ManualReplay manifest validation failed: {e}")

    try:
        authoring = (root / "N2C_AI_JSON_IMPORT_AUTHORING_RULES.md").read_text(encoding="utf-8-sig")
        for required_text in (
            "mandatory first document", "N2C_PROJECT_PATCH_V1", "InputPin", "OutputPin",
            "unsupported", "Alternative", "N2C_NEW_NODE_TEST_GATE",
            "K2Node_Composite_<n>", "canonical identity", "StructType->GetFName()",
            "K2Node_MakeStruct", "K2Node_BreakStruct", "Option 0", "Option 1"
        ):
            if required_text.lower() not in authoring.lower():
                failures.append(f"AI JSON authoring rules missing required contract text: {required_text}")
        checks["ai_json_authoring_rules"] = {"path": "N2C_AI_JSON_IMPORT_AUTHORING_RULES.md", "chars": len(authoring)}
    except Exception as e:
        failures.append(f"AI JSON authoring rules validation failed: {e}")

    try:
        manual_patch = load_json(final_manual_name)
        final_pin_errors, final_pin_counters = validate_final_manual_pin_contract(
            manual_patch, final_manual_name)
        if manual_patch.get("target_plugin_version") != version:
            failures.append(f"final manual target_plugin_version mismatch: expected {version}, got {manual_patch.get('target_plugin_version')!r}")
        if manual_patch.get("target_plugin_version_name") != version_name:
            failures.append(f"final manual target_plugin_version_name mismatch: expected {version_name!r}, got {manual_patch.get('target_plugin_version_name')!r}")
        fixture_manual_path = root / "Source/Tests/Fixtures/ManualReplay" / final_manual_name
        if fixture_manual_path.is_file() and (root / final_manual_name).read_bytes() != fixture_manual_path.read_bytes():
            failures.append("root and ManualReplay final manual JSON copies differ")
        if final_pin_errors:
            failures.extend(final_pin_errors)
        checks["final_manual_pin_contract"] = {
            "result": "PASS" if not final_pin_errors else "FAIL",
            "errors": final_pin_errors,
            "counters": final_pin_counters,
        }
        collapsed_actions = [
            action
            for asset in manual_patch.get("assets", []) if isinstance(asset, dict)
            for action in asset.get("actions", []) if isinstance(action, dict) and action.get("type") in {"create_collapsed_graph", "replace_collapsed_graph"}
        ]
        if len(collapsed_actions) != 1:
            failures.append(f"final manual JSON must contain exactly one collapsed graph action, got {len(collapsed_actions)}")
        else:
            bound_identity = str(collapsed_actions[0].get("bound_graph_identity", ""))
            if ".K2Node_Composite_" in bound_identity:
                failures.append("final manual JSON hardcodes unstable generated Composite object identity")
            if not bound_identity.endswith(":EventGraph.N2C_FinalCollapsed"):
                failures.append("final manual JSON canonical collapsed graph identity is missing or unexpected")
        checks["final_manual_composite_identity"] = {"actions": len(collapsed_actions), "canonical": bool(collapsed_actions) and ".K2Node_Composite_" not in str(collapsed_actions[0].get("bound_graph_identity", ""))}
    except Exception as e:
        failures.append(f"final manual Composite identity validation failed: {e}")

    capabilities: list[dict[str, Any]] = []
    try:
        matrix = load_json("Source/Tests/Fixtures/N2C_NODE_TEST_REQUIREMENTS_V1.json")
        capabilities = [x for x in matrix.get("capabilities", []) if isinstance(x, dict)]
        ids = [x.get("id") for x in capabilities]
        if matrix.get("schema") != "N2C_NODE_TEST_REQUIREMENTS_V1" or matrix.get("policy") != "N2C_NEW_NODE_TEST_GATE":
            failures.append("unsupported new-node test requirement matrix")
        if matrix.get("version") != version: failures.append(f"new-node test requirement matrix version mismatch: expected {version}, got {matrix.get('version')!r}")
        if len(capabilities) < 10: failures.append("new-node test requirement matrix is unexpectedly small")
        if len(ids) != len(set(ids)): failures.append("duplicate capability id in new-node test requirement matrix")
        for capability in capabilities:
            cid = capability.get("id")
            status = capability.get("status")
            if status not in {"verified", "guarded", "deferred"}:
                failures.append(f"invalid capability status: {cid}={status}")
                continue
            mapped = list(capability.get("positive_tests") or []) + list(capability.get("negative_tests") or [])
            if status == "verified":
                if not capability.get("positive_tests"): failures.append(f"verified capability lacks positive regression: {cid}")
                if not capability.get("fresh_process_required") and cid != "editor_ui_contract":
                    failures.append(f"verified runtime capability lacks fresh-process requirement: {cid}")
                for test_name in mapped:
                    is_contract_test = str(test_name).startswith("NodeToCode.ContractMatrix.")
                    if test_name not in REQUIRED_TESTS and not is_contract_test:
                        failures.append(f"capability references non-mandatory test: {cid} -> {test_name}")
            elif capability.get("release_claim_allowed"):
                failures.append(f"deferred/guarded capability allows verified claim: {cid}")
        animation = [x for x in capabilities if x.get("id") == "animation_graph"]
        if len(animation) != 1 or animation[0].get("status") != "deferred" or animation[0].get("release_claim_allowed"):
            failures.append("Animation/AnimGraph deferred gate missing or claimable")
        checks["new_node_test_gate"] = {"capabilities": len(capabilities), "verified": sum(x.get("status") == "verified" for x in capabilities), "deferred": sum(x.get("status") == "deferred" for x in capabilities)}
    except Exception as e:
        failures.append(f"new-node test requirement validation failed: {e}")

    test_source = (root / "Source/Private/N2CVerificationTests.cpp").read_text(encoding="utf-8-sig", errors="replace")
    missing_tests = [t for t in REQUIRED_TESTS if t not in test_source]
    if missing_tests: failures.append("missing registered tests: " + ", ".join(missing_tests))
    supplemental_boundary_test = "NodeToCode.Verification.P0GraphBoundaries.CompositeCanonicalIdentityRepeat"
    if supplemental_boundary_test not in test_source:
        failures.append("missing Composite canonical identity repeat regression")
    headless_harness_markers = [
        'ApplyPatchToBlueprint(Blueprint, Patch, FirstReport, false, false)',
        'ApplyPatchToBlueprint(Reloaded, Patch, SecondReport, false, false)',
        'ApplyPatchToBlueprint(Blueprint, GoodPatch, GoodApplyReport, false, false)',
        'DryRunPatch(Blueprint, GoodPatch, GoodDryRunReport, false)',
        'CountPendingRestoreManifests()',
        'bPersistedLinks',
        'bSecondSaved',
    ]
    headless_harness_markers.extend([
        "Struct_ConnectedSetFields",
        "Struct_LegacyMakeBreakAliases",
        "StructType FName",
        "struct_pin=%s",
    ])
    missing_headless_harness_markers = [m for m in headless_harness_markers if m not in test_source]
    if missing_headless_harness_markers:
        failures.append("headless regression harness markers missing: " + ", ".join(missing_headless_harness_markers))
    importer_text = (root / "Source/Private/Core/N2CPatchImporter.cpp").read_text(encoding="utf-8-sig", errors="replace")
    if "CanonicalizeCompositeBoundGraphIdentity" not in importer_text:
        failures.append("Composite canonical identity importer helper is missing")
    if "LogicalBlueprintPath" not in importer_text or "Blueprint->GetPathName());" not in importer_text:
        failures.append("transient sandbox logical Blueprint identity forwarding is missing")


    try:
        contract = load_json("Source/Tests/Fixtures/N2C_IMPORT_CONTRACT_MATRIX_V1.json")
        contract_cases = [x for x in contract.get("cases", []) if isinstance(x, dict)]
        contract_ids = [str(x.get("id", "")) for x in contract_cases]
        if contract.get("schema") != "N2C_IMPORT_CONTRACT_MATRIX_V1": failures.append("unsupported import contract matrix")
        if contract.get("version") != version: failures.append("import contract matrix version mismatch")
        if contract.get("case_count") != len(contract_cases) or len(contract_cases) < 100:
            failures.append(f"import contract matrix count mismatch/too small: declared={contract.get('case_count')} actual={len(contract_cases)}")
        if not all(contract_ids) or len(contract_ids) != len(set(contract_ids)):
            failures.append("import contract matrix contains missing/duplicate ids")
        for row in contract_cases:
            cid = str(row.get("id", ""))
            if row.get("fresh_session_required") is not True or row.get("reapply_required") is not True:
                failures.append(f"contract case lacks fresh/reapply gate: {cid}")
            if row.get("expected_apply") is True and not row.get("expected"):
                failures.append(f"positive contract case lacks semantic assertion: {cid}")
        action_start = importer_text.find("static bool IsEventGraphPatchAction")
        action_end = importer_text.find("static bool IsSupportedNodeType", action_start)
        supported_actions = set(re.findall(r'ActionType\s*==\s*TEXT\("([^"]+)"\)', importer_text[action_start:action_end]))
        coverage = contract.get("coverage") or {}
        action_rows = [x for x in coverage.get("actions", []) if isinstance(x, dict)]
        mapped_actions = {str(x.get("token", "")) for x in action_rows}
        if mapped_actions != supported_actions:
            failures.append("import action contract coverage mismatch")
        for group in ("actions", "nodes", "guards"):
            for coverage_row in coverage.get(group, []):
                token = str(coverage_row.get("token", "")); refs = list(coverage_row.get("cases") or [])
                if not token or not refs: failures.append(f"empty import contract coverage row: {group}/{token}")
                for ref in refs:
                    if str(ref).startswith("NodeToCode."):
                        if str(ref) not in test_source: failures.append(f"contract coverage references unregistered test: {token} -> {ref}")
                    elif str(ref) not in contract_ids:
                        failures.append(f"contract coverage references unknown case: {token} -> {ref}")
        for guard_row in coverage.get("guards", []):
            if str(guard_row.get("token", "")) not in importer_text:
                failures.append(f"contract guard absent from importer: {guard_row.get('token')}")
        contract_markers = [
            "NodeToCode.ContractMatrix.Apply", "NodeToCode.ContractMatrix.VerifyFreshFirst",
            "NodeToCode.ContractMatrix.Reapply", "NodeToCode.ContractMatrix.VerifyFreshSecond",
            "NodeToCode.ContractMatrix.Cleanup", "N2C_IMPORT_CONTRACT|phase=",
        ]
        missing_contract_markers = [m for m in contract_markers if m not in test_source]
        if missing_contract_markers: failures.append("contract runtime markers missing: " + ", ".join(missing_contract_markers))
        checks["import_contract_matrix"] = {
            "cases": len(contract_cases), "actions": len(action_rows),
            "nodes": len(coverage.get("nodes", [])), "guards": len(coverage.get("guards", [])),
            "negative": sum(x.get("expected_apply") is False for x in contract_cases),
        }
    except Exception as e:
        failures.append(f"import contract matrix validation failed: {e}")
    for marker in ("UK2Node_MakeStruct", "UK2Node_BreakStruct", "bStructValueAlias", "StructType->GetFName()"):
        if marker not in importer_text:
            failures.append(f"struct pin compatibility marker missing: {marker}")
    try:
        for capability in capabilities:
            if capability.get("status") != "verified":
                continue
            for test_name in list(capability.get("positive_tests") or []) + list(capability.get("negative_tests") or []):
                if test_name not in test_source:
                    failures.append(f"capability references unregistered test: {capability.get('id')} -> {test_name}")
        for rel in ["CODEX_PLUGIN_START_HERE.md", "Source/Documentation/AGENTS.md", "Source/Documentation/N2C_VERIFICATION_WORKFLOW.md", "Source/Documentation/N2C_UE427_SOURCE_WORKFLOW.md"]:
            if "N2C_NEW_NODE_TEST_GATE" not in (root / rel).read_text(encoding="utf-8-sig", errors="replace"):
                failures.append(f"mandatory new-node test gate marker missing: {rel}")
    except Exception as e:
        failures.append(f"new-node gate source/doc validation failed: {e}")
    missing_case_markers = [name for name in REQUIRED_CASE_MARKERS if f"case={name}" not in test_source and f'TEXT("{name}")' not in test_source and f'"{name}"' not in test_source]
    if missing_case_markers: failures.append("missing ManualReplay case markers: " + ", ".join(missing_case_markers))
    checks["required_tests"] = {"required": len(REQUIRED_TESTS), "missing": missing_tests, "required_case_markers": len(REQUIRED_CASE_MARKERS), "missing_case_markers": missing_case_markers, "headless_harness_missing": missing_headless_harness_markers}

    importer = (root / "Source/Private/Core/N2CPatchImporter.cpp").read_text(encoding="utf-8-sig", errors="replace")
    required_markers = [
        "IsExplicitEnumBackedByteDeclaration",
        "enum_member_type_unresolved",
        "automation_force_failure_after_mutation",
        "N2C_ROLLBACK_RESULT|result=PASS",
        "N2C_APPLY_BLOCKED_BEFORE_MUTATION",
    ]
    missing_markers = [m for m in required_markers if m not in importer]
    if missing_markers: failures.append("missing importer markers: " + ", ".join(missing_markers))
    real_cancel = re.findall(r"(?m)^\s*Transaction\.Cancel\(\)\s*;", importer)
    if real_cancel: failures.append("real Transaction.Cancel() rollback call found")
    checks["importer_contract"] = {"missing_markers": missing_markers, "transaction_cancel_calls": len(real_cancel)}
    delegate_importer_markers = [
        "UK2Node_CreateDelegate", "GetDelegateOutPin()", "OutputDelegate", "output_delegate",
        "selection deferred until delegate links exist", "create_delegate_signature_missing",
        "create_delegate_function_selection_lost",
    ]
    missing_delegate_markers = [m for m in delegate_importer_markers if m not in importer]
    if missing_delegate_markers: failures.append("missing CreateDelegate pin compatibility markers: " + ", ".join(missing_delegate_markers))
    if '"from_pin":"Delegate"' not in test_source or '"from_pin":"OutputDelegate"' not in test_source:
        failures.append("delegate regression must exercise legacy Delegate and canonical OutputDelegate source pins")
    delegate_test_markers = [
        '"function_name":"N2C_P0_DelegateHandler"',
        '"type":"add_or_replace_function","function_name":"N2C_P0_DelegateHandler"',
    ]
    missing_delegate_test_markers = [m for m in delegate_test_markers if m not in test_source]
    if missing_delegate_test_markers:
        failures.append("same-patch CreateDelegate compile regression markers missing: " + ", ".join(missing_delegate_test_markers))
    checks["delegate_pin_contract"] = {
        "missing_importer_markers": missing_delegate_markers,
        "missing_same_patch_test_markers": missing_delegate_test_markers,
    }

    spawn_contract_markers = [
        "ValidateSpawnActorTransformContract", "spawn_transform_link_missing",
        "KismetMathLibrary.MakeTransform", '"to_pin":"SpawnTransform"',
        "SpawnActorTransformLinkReject",
    ]
    member_default_markers = [
        "CanonicalizeStructMemberVariableDefaultValue",
        "TryFormatK2CustomStructDefault",
        "FDefaultValueHelper::IsStringValidRotator",
        "member_default_import_text_invalid",
        "pin_default_invalid",
        "PPF_SerializedAsImportText",
        "N2C_RotatorDefault",
        "RunInvalidStructMemberDefaultReject",
        "RotatorPinDefaultReject",
        "GetPinDefaultValuesFromString",
        "ApplySchemaPinDefaultValue",
        "SpawnActorClassDefaultReject",
        "schema_default_storage",
        "BoundaryDefault",
        "cdo_rotator",
        "ContainerPtrToValuePtr<FRotator>",
    ]
    missing_member_default_markers = [
        marker for marker in member_default_markers
        if marker not in importer and marker not in test_source
    ]
    if missing_member_default_markers:
        failures.append("member struct-default regression markers missing: " + ", ".join(missing_member_default_markers))
    forbidden_raw_default_writes = [
        "DataTablePin->DefaultValue = DataTable->GetPathName()",
        "Pin->DefaultValue = GetStringFieldSafe(PinObj, TEXT(\"default_value\"))",
    ]
    found_raw_default_writes = [marker for marker in forbidden_raw_default_writes if marker in importer]
    if found_raw_default_writes:
        failures.append("raw Blueprint pin-default writes bypass schema conversion: " + ", ".join(found_raw_default_writes))
    missing_spawn_markers = [m for m in spawn_contract_markers if m not in importer and m not in test_source]
    if missing_spawn_markers:
        failures.append("SpawnActor compile-parity regression markers missing: " + ", ".join(missing_spawn_markers))
    editor_integration = (root / "Source/Private/Core/N2CEditorIntegration.cpp").read_text(encoding="utf-8-sig", errors="replace")
    if "FCoreDelegates::OnPreExit" in editor_integration or "ProcessPendingBackupRestoresOnPreExit" in editor_integration:
        failures.append("pending restore must not execute from OnPreExit; restore is startup-only")
    checks["manual_apply_compile_parity"] = {
        "missing_spawn_markers": missing_spawn_markers,
        "missing_member_default_markers": missing_member_default_markers,
        "raw_schema_bypass_writes": found_raw_default_writes,
        "startup_only_restore": "FCoreDelegates::OnPreExit" not in editor_integration,
    }

    exporter = (root / "Source/Private/Core/N2CAIExport.cpp").read_text(encoding="utf-8-sig", errors="replace")
    exporter_markers = ["BlueprintVariableDefaultToString", "PPF_SerializedAsImportText"]
    missing_exporter_markers = [m for m in exporter_markers if m not in exporter]
    if missing_exporter_markers: failures.append("missing exporter default markers: " + ", ".join(missing_exporter_markers))
    contextual_flags_marker = '"shift":false,"ctrl":false,"alt":false,"cmd":false'
    if contextual_flags_marker not in test_source:
        failures.append("Contextual InputKey fixture is missing persisted modifier flags")
    if "category=%s|subtype=%s|default=%s" not in test_source:
        failures.append("Raw Byte export diagnostics marker is missing")
    checks["manual_replay_fixes"] = {
        "missing_exporter_markers": missing_exporter_markers,
        "contextual_input_flags": contextual_flags_marker in test_source,
        "raw_byte_diagnostics": "category=%s|subtype=%s|default=%s" in test_source,
    }

    orchestrator = (root / "Scripts/Codex/Invoke-N2CFullValidation.ps1").read_text(encoding="utf-8-sig", errors="replace")
    package_script = (root / "Scripts/Codex/Package-N2CPlugin.ps1").read_text(encoding="utf-8-sig", errors="replace")
    release_tool_sources = {
        "Validate-N2CFiles.ps1": (root / "Scripts/Codex/Validate-N2CFiles.ps1").read_text(encoding="utf-8-sig", errors="replace"),
        "Package-N2CPlugin.ps1": package_script,
        "validate_n2c_release.py": (root / "Scripts/Codex/validate_n2c_release.py").read_text(encoding="utf-8-sig", errors="replace"),
    }
    release_tool_errors = []
    for tool_name, tool_source in release_tool_sources.items():
        if re.search(r"(?i)(?:descriptor\.Version|\bversion)\s*(?:-ne|!=)\s*[0-9]{3}", tool_source):
            release_tool_errors.append(f"{tool_name}: hard-coded current release number")
        if re.search(r"N2C_FINAL_MANUAL_ALL_STRICT_SUPPORTED_V[0-9]+\.json", tool_source):
            release_tool_errors.append(f"{tool_name}: hard-coded versioned final manual filename")
        if tool_name.endswith(".ps1") and "Get-FileHash" in tool_source:
            release_tool_errors.append(f"{tool_name}: Get-FileHash dependency is not allowed; use Get-N2CFileSha256")
    if release_tool_errors:
        failures.extend(release_tool_errors)
    checks["release_tool_single_source"] = {"errors": release_tool_errors}
    search_script = (root / "Scripts/Codex/Search-UE427Source.ps1").read_text(encoding="utf-8-sig", errors="replace")
    verification_script = (root / "Scripts/Codex/Run-N2CVerification.ps1").read_text(encoding="utf-8-sig", errors="replace")
    project_export_script = (root / "Scripts/Codex/Run-N2CProjectExport.ps1").read_text(encoding="utf-8-sig", errors="replace")
    cmd_script = (root / "Scripts/RUN_N2C_AUTOMATION_AND_PACK.cmd").read_text(encoding="utf-8-sig", errors="replace")
    script_markers = [
        "BuildTimeoutSeconds", "AutomationTimeoutSeconds", "taskkill.exe /PID",
        "NodeToCode.ManualReplay", "NodeToCode.ContractMatrix.Apply", "NodeToCode.ContractMatrix.VerifyFreshFirst",
        "NodeToCode.ContractMatrix.Reapply", "NodeToCode.ContractMatrix.VerifyFreshSecond", "NodeToCode.ContractMatrix.Cleanup",
        "N2C_IMPORT_CONTRACT_MATRIX_V1.json", "NodeToCode.RestoreFirstPass.ManualReplayPendingRestore",
        "NodeToCode.RestoreSecondPass.ManualReplayPendingRestore", "RequiredCases",
        "N2C_MANUAL_REPLAY_CASE|case=", "New-N2CZipFromDirectory", "CreateEntryFromFile",
        "Get-N2CFileSha256 -LiteralPath $bundleZip", "SHA-256 helper self-test failed", "-AllowBuildProducts",
        "System.Diagnostics.ProcessStartInfo", "ReadToEndAsync()", "WaitForExit($waitMilliseconds)",
        "Test-ProcessRunner", "RunnerExit0.log", "RunnerStreams.log", "RunnerArgumentProbe.log", "RunnerExit7.log", "RunnerTimeout.log",
        "ConvertTo-NativeCommandLineArgument", "${DisplayName}: $resultText", "Stopped at:",
        "Success.", "explorer.exe", "NoOpenResult", "KeepResultDirectory", "NoPause", "Get-ProcessFailureDetail",
        "Clear-StaleN2CAutomationRestoreQueue", "N2C_AUTOMATION_RESTORE_HYGIENE|result=PASS|quarantined=",
        "/Game/N2C_Test/Generated/", "PendingRestoreCancelled\\AutomationHarness",
        "$runStartedUtc = [DateTime]::UtcNow", "LastWriteTimeUtc -ge $runStartedUtc.AddSeconds(-5)", "crash_reports_collected",
        "New-AutomationProgressState", "'Progress: [{0}] Completed {1}/{2} | {3} | Elapsed {4}' -f", "Running {0}/{1}: {2}", "Finalizing UE report...", "failed_cases:", "Automation progress parser self-test failed",
        "$processId = $process.Id", "pendingLooksComplete", "Result ZIP hash mismatch:", "Remove-N2CBundleDirectoryAfterVerifiedZip", "N2C_BUNDLE_CLEANUP|result=PASS", "N2C_BUNDLE_CLEANUP|result=SKIPPED|kept_directory=", "Test-AutomationReport", "automation_report_test_count_mismatch", "result bundle", "bundle_directory=",
    ]
    missing_script_markers = [m for m in script_markers if m not in orchestrator]
    package_markers = ["N2C_PACKAGE_MANIFEST_V2", "CreateEntryFromFile", "node2code/", "Package hash mismatch", ".Replace(\'\\\', \'/\')", "N2C_AI_JSON_IMPORT_AUTHORING_RULES.md", "$finalManualFileName", "Get-N2CFileSha256"]
    missing_package_markers = [m for m in package_markers if m not in package_script]
    if '"$DisplayName: $resultText"' in orchestrator: failures.append("unsafe PowerShell interpolation remains: $DisplayName:")
    if "Start-Process @startArgs" in orchestrator: failures.append("legacy Start-Process child runner remains")
    if "AddHours(-8)" in orchestrator: failures.append("stale eight-hour crash collection window remains")
    if missing_script_markers: failures.append("orchestrator contract markers missing: " + ", ".join(missing_script_markers))
    if missing_package_markers: failures.append("package contract markers missing: " + ", ".join(missing_package_markers))
    search_missing = [m for m in ["$sourceMatches.Count", "$searchExitCode"] if m not in search_script]
    if search_missing or re.search(r"(?i)\$matches\b", strip_powershell(search_script)):
        failures.append("UE source-search contract invalid")
    verification_missing = [m for m in ["queue_completion_marker_missing", "required_case_markers_missing", "N2C_MANUAL_REPLAY_CASE|case="] if m not in verification_script]
    if verification_missing: failures.append("standalone verification guards missing: " + ", ".join(verification_missing))
    export_missing = [m for m in ["fresh_export_archive_missing", "LastWriteTimeUtc", "queue_completion_marker_missing"] if m not in project_export_script]
    if export_missing: failures.append("project-export guards missing: " + ", ".join(export_missing))
    cmd_markers = ["setlocal EnableExtensions DisableDelayedExpansion", ":scan_args", "shift", "%*", "Windows PowerShell 5.1 was not found.", "[0/11] PowerShell syntax preflight", "Test-N2CPowerShellSyntax.ps1", "Press any key to exit.", "exit /b %EXIT_CODE%"]
    cmd_missing = [m for m in cmd_markers if m not in cmd_script]
    if cmd_missing: failures.append("CMD contract markers missing: " + ", ".join(cmd_missing))
    if "for %%A in (%*)" in cmd_script: failures.append("unsafe FOR-based CMD argument scan remains")
    checks["script_contract"] = {
        "orchestrator_missing": missing_script_markers,
        "package_missing": missing_package_markers,
        "search_missing": search_missing,
        "verification_missing": verification_missing,
        "project_export_missing": export_missing,
        "cmd_missing": cmd_missing,
    }

    cpp_results: list[dict[str, Any]] = []
    for p in sorted([x for x in files if x.suffix.lower() in {".cpp", ".h", ".cs"}]):
        errs = balance_cpp(p)
        rel = p.relative_to(root).as_posix()
        cpp_results.append({"path": rel, "result": "PASS" if not errs else "FAIL", "errors": errs})
        for e in errs: failures.append(f"lexical balance {rel}: {e}")
    checks["lexical_balance"] = {"count": len(cpp_results), "results": cpp_results}

    protected_names = (
        "PID|Host|PSHOME|PSScriptRoot|PSCommandPath|MyInvocation|Args|Error|Matches|Input|"
        "ExecutionContext|PSBoundParameters|LASTEXITCODE|Home|Profile|PWD|ShellId|StackTrace|"
        "NestedPromptLevel|PSVersionTable"
    )
    reserved_assignment_pattern = re.compile(
        rf"(?im)\$(?:{protected_names})\s*(?:=|\+=|-=|\*=|/=|%=)"
    )
    forbidden_reference_pattern = re.compile(r"(?i)\$(PID|Args|Matches)\b")
    reserved_assignment_hits: list[dict[str, Any]] = []
    forbidden_reference_hits: list[dict[str, Any]] = []
    for p in sorted([x for x in files if x.suffix.lower() == ".ps1"]):
        rel = p.relative_to(root).as_posix()
        ps_text = p.read_text(encoding="utf-8-sig", errors="replace")
        clean_ps = strip_powershell(ps_text)
        for match in reserved_assignment_pattern.finditer(clean_ps):
            line = clean_ps.count("\n", 0, match.start()) + 1
            reserved_assignment_hits.append({"path": rel, "line": line, "text": match.group(0)})
        for match in forbidden_reference_pattern.finditer(clean_ps):
            line = clean_ps.count("\n", 0, match.start()) + 1
            forbidden_reference_hits.append({"path": rel, "line": line, "variable": match.group(1)})
    if reserved_assignment_hits:
        failures.append("PowerShell automatic variable assignments found: " + ", ".join(
            f"{hit['path']}:{hit['line']}:{hit['text']}" for hit in reserved_assignment_hits
        ))
    if forbidden_reference_hits:
        failures.append("PowerShell forbidden automatic variable references found: " + ", ".join(
            f"{hit['path']}:{hit['line']}:${hit['variable']}" for hit in forbidden_reference_hits
        ))
    checks["powershell_reserved_variables"] = {
        "assignment_hits": reserved_assignment_hits,
        "forbidden_reference_hits": forbidden_reference_hits,
    }

    ps_results: list[dict[str, Any]] = []
    unsafe_interpolation_hits: list[dict[str, Any]] = []
    valid_scopes = {"env", "global", "script", "local", "private", "using", "variable", "function", "alias"}
    unsafe_interpolation_pattern = re.compile(r'(?<![`$\{])\$(?P<name>[A-Za-z_][A-Za-z0-9_]*):')
    for p in sorted([x for x in files if x.suffix.lower() == ".ps1"]):
        ps_source = p.read_text(encoding="utf-8-sig", errors="replace")
        rel = p.relative_to(root).as_posix()
        for line_number, source_line in enumerate(ps_source.splitlines(), start=1):
            if '"' not in source_line:
                continue
            for match in unsafe_interpolation_pattern.finditer(source_line):
                if match.group("name").lower() in valid_scopes:
                    continue
                unsafe_interpolation_hits.append({"path": rel, "line": line_number, "variable": match.group("name")})
        if "$ErrorActionPreference = 'Stop'" not in ps_source:
            failures.append(f"PowerShell strict error policy missing: {rel}")
        if "Set-StrictMode -Version 2.0" not in ps_source:
            failures.append(f"PowerShell StrictMode missing: {rel}")
        if p.name == "Validate-N2CFiles.ps1":
            for marker in ("Get-N2CCollectionCount", "Assert-N2CCollectionCount", "Get-N2CJsonPropertyValue", "Test-N2CValidatorHelpers", "System.Collections.IEnumerable", "regex MatchCollection", "N2C_STATIC_VALIDATION_EXCEPTION", "must use Get-N2CCollectionCount instead of direct .Count", "must use Get-N2CJsonPropertyValue instead of direct JSON member access"):
                if marker not in ps_source:
                    failures.append(f"validator StrictMode hardening marker missing: {marker}")
            # Direct member access to .Count on an accidentally scalar pipeline result is
            # a terminating PropertyNotFoundStrict error in Windows PowerShell 5.1.
            scrubbed = strip_powershell(ps_source)
            direct_count_lines = [i for i, line in enumerate(scrubbed.splitlines(), 1) if re.search(r"\.Count\b", line)]
            if direct_count_lines:
                failures.append(f"Validate-N2CFiles.ps1 contains direct .Count member access at lines {direct_count_lines[:20]}")
            if "@($Value).Length" in ps_source:
                failures.append("Validate-N2CFiles.ps1 still contains the Windows PowerShell 5.1-unsafe null collection wrapper")
            json_root_names = ("descriptor", "p0", "manual", "finalManual", "nodeMatrix", "contractMatrix", "capability", "nodeCoverageRow", "guardCoverageRow")
            direct_json_member_pattern = re.compile(r"\$(?:" + "|".join(map(re.escape, json_root_names)) + r")\.[A-Za-z_][A-Za-z0-9_]*")
            direct_json_member_lines = [i for i, line in enumerate(scrubbed.splitlines(), 1) if direct_json_member_pattern.search(line)]
            if direct_json_member_lines:
                failures.append(f"Validate-N2CFiles.ps1 contains direct JSON member access at lines {direct_json_member_lines[:20]}")
        errs = balance_powershell(p)
        rel = p.relative_to(root).as_posix()
        ps_results.append({"path": rel, "result": "PASS" if not errs else "FAIL", "errors": errs})
        for e in errs: failures.append(f"PowerShell delimiter balance {rel}: {e}")
    if unsafe_interpolation_hits:
        failures.append("unsafe PowerShell variable-colon interpolation found: " + ", ".join(
            f"{hit['path']}:{hit['line']}:${hit['variable']}:" for hit in unsafe_interpolation_hits
        ))
    checks["powershell_interpolation"] = {"unsafe_hits": unsafe_interpolation_hits}
    checks["powershell_delimiter_balance"] = {"scope": "basic delimiter check, not execution/parser proof", "count": len(ps_results), "results": ps_results}

    machine_paths: list[dict[str, Any]] = []
    secret_hits: list[dict[str, Any]] = []
    for p in files:
        if p.suffix.lower() not in TEXT_EXTS: continue
        rel = p.relative_to(root).as_posix()
        text = p.read_text(encoding="utf-8-sig", errors="replace")
        if p.suffix.lower() in {".md", ".json", ".ps1", ".cmd", ".ini", ".uplugin"}:
            for m in ABSOLUTE_WINDOWS_PATH.finditer(text):
                line = text.count("\n", 0, m.start()) + 1
                machine_paths.append({"path": rel, "line": line, "match": m.group(0)})
        for name, pattern in SECRET_PATTERNS.items():
            for m in pattern.finditer(text):
                line = text.count("\n", 0, m.start()) + 1
                secret_hits.append({"path": rel, "line": line, "kind": name})
    if machine_paths: failures.append(f"machine-specific absolute paths found: {len(machine_paths)}")
    if secret_hits: failures.append(f"potential secrets found: {len(secret_hits)}")
    checks["text_hygiene"] = {"machine_paths": machine_paths, "potential_secrets": secret_hits}

    # UE4.27 reference-source evidence: every included K2Node_*.h must exist in supplied refs when refs are passed.
    ref_result: dict[str, Any] = {"status": "NOT_RUN"}
    if args.reference_root:
        ref = args.reference_root.resolve()
        headers = {p.name: p for p in ref.rglob("*.h")}
        k2_includes = sorted(set(re.findall(r'#include\s+"(K2Node_[^"]+\.h)"', "\n".join(
            p.read_text(encoding="utf-8-sig", errors="replace") for p in files if p.suffix.lower() in {".cpp", ".h"}
        ))))
        missing = [h for h in k2_includes if Path(h).name not in headers]
        if missing:
            warnings.append("K2 headers not found in supplied reference root: " + ", ".join(missing))
        ref_result = {"status": "PASS" if not missing else "PARTIAL", "checked_headers": len(k2_includes), "missing": missing}
    checks["ue427_reference_headers"] = ref_result

    package_manifest_path = root / "PACKAGE_MANIFEST.json"
    package_manifest_result: dict[str, Any] = {"status": "NOT_PRESENT"}
    if package_manifest_path.is_file():
        try:
            package_manifest = json.loads(package_manifest_path.read_text(encoding="utf-8-sig"))
            rows = package_manifest.get("files", [])
            manifest_errors: list[str] = []
            if package_manifest.get("schema") != "N2C_PACKAGE_MANIFEST_V2": manifest_errors.append("wrong schema")
            if package_manifest.get("version") != version or package_manifest.get("version_name") != version_name: manifest_errors.append("version mismatch")
            if package_manifest.get("top_level") != "node2code" or package_manifest.get("source_only") is not True: manifest_errors.append("package identity mismatch")
            if package_manifest.get("file_count") != len(rows): manifest_errors.append("file_count mismatch")
            row_paths = [row.get("path") for row in rows if isinstance(row, dict)]
            if len(row_paths) != len(set(row_paths)): manifest_errors.append("duplicate manifest paths")
            expected_paths = sorted(p.relative_to(root).as_posix() for p in root.rglob("*") if p.is_file() and p.name != "PACKAGE_MANIFEST.json")
            if sorted(row_paths) != expected_paths: manifest_errors.append("manifest membership mismatch")
            for row in rows:
                if not isinstance(row, dict): manifest_errors.append("non-object manifest row"); continue
                rel = row.get("path")
                target = root / str(rel)
                if not target.is_file(): manifest_errors.append(f"missing file {rel}"); continue
                if row.get("size") != target.stat().st_size: manifest_errors.append(f"size mismatch {rel}")
                if row.get("sha256") != sha256(target): manifest_errors.append(f"hash mismatch {rel}")
            if manifest_errors:
                failures.extend("package manifest: " + e for e in manifest_errors)
            package_manifest_result = {"status": "PASS" if not manifest_errors else "FAIL", "rows": len(rows), "errors": manifest_errors}
        except Exception as e:
            failures.append(f"package manifest validation failed: {e}")
            package_manifest_result = {"status": "FAIL", "error": str(e)}
    checks["package_manifest"] = package_manifest_result

    line_counts = []
    for p in files:
        if p.suffix.lower() in TEXT_EXTS:
            try: count = p.read_text(encoding="utf-8-sig", errors="replace").count("\n") + 1
            except Exception: continue
            line_counts.append({"path": p.relative_to(root).as_posix(), "lines": count})
    line_counts.sort(key=lambda x: (-x["lines"], x["path"]))
    checks["inventory"] = {
        "file_count": len(files),
        "total_bytes": sum(p.stat().st_size for p in files),
        "top_text_files_by_lines": line_counts[:20],
        "files": [{"path": p.relative_to(root).as_posix(), "size": p.stat().st_size, "sha256": sha256(p)} for p in files],
    }

    report = {
        "schema": "N2C_STATIC_AUDIT_V1",
        "version": version,
        "version_name": version_name,
        "scope": "static source/package validation only; no UE4 C++ build or Editor automation",
        "result": "PASS" if not failures else "FAIL",
        "failures": failures,
        "warnings": warnings,
        "checks": checks,
    }
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"N2C_STATIC_AUDIT|result={report['result']}|files={len(files)}|json={len(json_results)}|cpp_balance={len(cpp_results)}|failures={len(failures)}|warnings={len(warnings)}")
    for item in failures: print("FAIL:", item)
    for item in warnings: print("WARN:", item)
    return 0 if not failures else 1


if __name__ == "__main__":
    raise SystemExit(main())
