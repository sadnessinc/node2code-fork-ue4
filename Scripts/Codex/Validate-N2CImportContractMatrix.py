#!/usr/bin/env python3
"""Static contract gate for NodeToCode importer coverage.

The runtime truth remains UE4.27.2 automation. This script prevents a supported
action/node/guard from being added without a mapped contract case and validates
all authored matrix graph references before the Editor is launched.
"""
from __future__ import annotations
import argparse, json, re, sys
from pathlib import Path


def load(path: Path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--plugin-root", default=str(Path(__file__).resolve().parents[2]))
    args = ap.parse_args()
    root = Path(args.plugin_root).resolve()
    descriptor = load(root / "NodeToCode.uplugin")
    matrix = load(root / "Source/Tests/Fixtures/N2C_IMPORT_CONTRACT_MATRIX_V1.json")
    importer = (root / "Source/Private/Core/N2CPatchImporter.cpp").read_text(encoding="utf-8-sig")
    tests = (root / "Source/Private/N2CVerificationTests.cpp").read_text(encoding="utf-8-sig")
    errors: list[str] = []

    if matrix.get("schema") != "N2C_IMPORT_CONTRACT_MATRIX_V1": errors.append("schema mismatch")
    if matrix.get("version") != descriptor.get("Version"): errors.append("version mismatch")
    cases = matrix.get("cases") or []
    if matrix.get("case_count") != len(cases): errors.append("case_count mismatch")
    if len(cases) < 100: errors.append(f"matrix too small: {len(cases)}")
    ids = [c.get("id") for c in cases]
    if None in ids or len(ids) != len(set(ids)): errors.append("missing/duplicate case id")
    idset = set(ids)

    a0 = importer.find("static bool IsEventGraphPatchAction")
    a1 = importer.find("static bool IsSupportedNodeType", a0)
    action_block = importer[a0:a1]
    supported_actions = set(re.findall(r'ActionType\s*==\s*TEXT\("([^"]+)"\)', action_block))

    n0 = a1
    n1 = importer.find("static bool IsGuardedSafeSkippedNodeType", n0)
    node_block = importer[n0:n1]
    exact_nodes = set(re.findall(r'Lower\s*==\s*TEXT\("([^"]+)"\)', node_block))
    contains_nodes = set(re.findall(r'Lower\.Contains\(TEXT\("([^"]+)"\)\)', node_block))
    def node_supported(token: str) -> bool:
        lower = token.lower()
        return lower in exact_nodes or any(fragment in lower for fragment in contains_nodes)

    authored_actions: set[str] = set()
    authored_nodes: set[str] = set()
    for case in cases:
        cid = str(case.get("id", ""))
        if case.get("fresh_session_required") is not True or case.get("reapply_required") is not True:
            errors.append(f"{cid}: fresh/reapply gate missing")
        if case.get("expected_apply") is True and not case.get("expected"):
            errors.append(f"{cid}: positive case lacks semantic expected assertion")
        for field in ("setup_patch", "patch"):
            patch = case.get(field)
            if not patch: continue
            if patch.get("schema") != "N2C_PATCH_V1": errors.append(f"{cid}/{field}: schema")
            actions = patch.get("actions") or []
            patch_scopes: set[str] = set()
            for ai, action in enumerate(actions):
                token = action.get("type")
                authored_actions.add(token)
                if token not in supported_actions: errors.append(f"{cid}/{field}/action[{ai}]: unsupported action {token}")
                nodes = action.get("nodes") or []
                if nodes and token in {"patch_graph", "patch_event_graph", "patch_widget_graph", "patch_animation_graph", "add_event_graph_nodes", "add_nodes_to_graph"}:
                    scope = action.get("import_scope") or action.get("action_id")
                    if not isinstance(scope, str) or not scope.strip():
                        errors.append(f"{cid}/{field}/action[{ai}]: node-bearing graph action lacks stable import_scope/action_id")
                    elif scope in patch_scopes:
                        errors.append(f"{cid}/{field}/action[{ai}]: duplicate import scope {scope}")
                    else:
                        patch_scopes.add(scope)
                node_ids = [n.get("id") for n in nodes]
                if case.get("expected_apply") is True and len(node_ids) != len(set(node_ids)):
                    errors.append(f"{cid}/{field}/action[{ai}]: duplicate node id")
                for node in nodes:
                    nt = str(node.get("type", "")); authored_nodes.add(nt)
                    if case.get("expected_apply") is True and not node_supported(nt):
                        errors.append(f"{cid}/{field}: unsupported node {nt}")
                known = set(node_ids)
                for edge_kind in ("exec_edges", "data_edges"):
                    for ei, edge in enumerate(action.get(edge_kind) or []):
                        src = edge.get("from_node_id", edge.get("from")); dst = edge.get("to_node_id", edge.get("to"))
                        if case.get("expected_apply") is True and (src not in known or dst not in known):
                            errors.append(f"{cid}/{field}/{edge_kind}[{ei}]: unknown node {src}->{dst}")
                        if not edge.get("from_pin") or not edge.get("to_pin"):
                            errors.append(f"{cid}/{field}/{edge_kind}[{ei}]: empty pin")

    coverage = matrix.get("coverage") or {}
    action_rows = coverage.get("actions") or []
    mapped_actions = {r.get("token") for r in action_rows}
    if mapped_actions != supported_actions:
        errors.append(f"action coverage mismatch missing={sorted(supported_actions-mapped_actions)} extra={sorted(mapped_actions-supported_actions)}")
    for group in ("actions", "nodes", "guards"):
        for row in coverage.get(group) or []:
            token = row.get("token"); refs = row.get("cases") or []
            if not token or not refs: errors.append(f"coverage {group}: empty token/cases")
            for ref in refs:
                if ref.startswith("NodeToCode."):
                    if ref not in tests: errors.append(f"coverage {group}/{token}: unregistered legacy test {ref}")
                elif ref not in idset:
                    errors.append(f"coverage {group}/{token}: unknown case {ref}")
    for row in coverage.get("nodes") or []:
        if not node_supported(str(row.get("token", ""))): errors.append(f"node coverage token not accepted: {row.get('token')}")
    for row in coverage.get("guards") or []:
        if str(row.get("token")) not in importer: errors.append(f"guard absent from importer: {row.get('token')}")

    required_runtime = [
        "NodeToCode.ContractMatrix.Apply", "NodeToCode.ContractMatrix.VerifyFreshFirst",
        "NodeToCode.ContractMatrix.Reapply", "NodeToCode.ContractMatrix.VerifyFreshSecond",
        "NodeToCode.ContractMatrix.Cleanup", "N2C_IMPORT_CONTRACT|phase=",
    ]
    for marker in required_runtime:
        if marker not in tests: errors.append(f"runtime harness marker missing: {marker}")

    if errors:
        for e in errors: print(f"FAIL: {e}")
        print(f"N2C_IMPORT_CONTRACT_STATIC|result=FAIL|cases={len(cases)}|actions={len(supported_actions)}|nodes={len(coverage.get('nodes') or [])}|guards={len(coverage.get('guards') or [])}|errors={len(errors)}")
        return 1
    print(f"N2C_IMPORT_CONTRACT_STATIC|result=PASS|cases={len(cases)}|actions={len(supported_actions)}|nodes={len(coverage.get('nodes') or [])}|guards={len(coverage.get('guards') or [])}|negative={sum(c.get('expected_apply') is False for c in cases)}")
    return 0

if __name__ == "__main__":
    sys.exit(main())
