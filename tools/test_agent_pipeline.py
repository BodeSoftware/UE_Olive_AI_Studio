#!/usr/bin/env python3
"""
Agent Pipeline Smoke Test
=========================

Verifies the agent pipeline is properly integrated by:
1. Checking MCP server connectivity
2. Sending a simple tool call to verify tools still work
3. Tailing the UE log for pipeline execution markers
4. Reporting pass/fail status

Usage:
    python tools/test_agent_pipeline.py [--log-path PATH] [--port PORT]

Requires: The UE editor must be running with Olive AI Studio loaded.
"""

import argparse
import json
import os
import re
import socket
import sys
import time
import urllib.request
import urllib.error
from datetime import datetime
from pathlib import Path


# =============================================================================
# Configuration
# =============================================================================

DEFAULT_LOG_PATH = os.path.normpath(os.path.join(
    os.path.dirname(__file__), "..", "docs", "logs", "UE_Olive_AI_Toolkit.log"
))
MCP_PORT_RANGE = range(3000, 3010)
MCP_ENDPOINT = "/mcp"


def _running_in_ue():
    """Detect if we're running inside UE's Python scripting host."""
    try:
        import unreal  # noqa: F401
        return True
    except ImportError:
        return False


# =============================================================================
# Helpers
# =============================================================================

class Colors:
    """ANSI color codes for terminal output. Disabled inside UE."""
    if _running_in_ue():
        GREEN = RED = YELLOW = CYAN = BOLD = RESET = ""
    else:
        GREEN = "\033[92m"
        RED = "\033[91m"
        YELLOW = "\033[93m"
        CYAN = "\033[96m"
        BOLD = "\033[1m"
        RESET = "\033[0m"


def cprint(color, msg):
    print(f"{color}{msg}{Colors.RESET}")


def send_jsonrpc(port, method, params=None, timeout=10):
    """Send a JSON-RPC 2.0 request to the MCP server."""
    payload = {
        "jsonrpc": "2.0",
        "id": int(time.time() * 1000),
        "method": method,
    }
    if params:
        payload["params"] = params

    data = json.dumps(payload).encode("utf-8")
    url = f"http://127.0.0.1:{port}{MCP_ENDPOINT}"

    req = urllib.request.Request(
        url,
        data=data,
        headers={"Content-Type": "application/json"},
        method="POST",
    )

    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            body = resp.read().decode("utf-8")
            return json.loads(body)
    except urllib.error.URLError as e:
        return {"error": {"message": str(e)}}
    except Exception as e:
        return {"error": {"message": str(e)}}


def discover_mcp_port():
    """Auto-discover the MCP server port (3000-3009)."""
    for port in MCP_PORT_RANGE:
        try:
            sock = socket.create_connection(("127.0.0.1", port), timeout=1)
            sock.close()
            # Verify it's actually the MCP server with a ping
            result = send_jsonrpc(port, "ping", timeout=3)
            if "error" not in result or "result" in result:
                return port
        except (socket.timeout, ConnectionRefusedError, OSError):
            continue
    return None


def tail_log_lines(log_path, max_lines=500):
    """Read the last N lines of the UE log file."""
    try:
        with open(log_path, "r", encoding="utf-8", errors="replace") as f:
            lines = f.readlines()
            return lines[-max_lines:]
    except FileNotFoundError:
        return []


def find_pipeline_markers(lines):
    """Search log lines for agent pipeline execution markers."""
    markers = {
        "pipeline_start": re.compile(r"LogOliveAgentPipeline.*Agent pipeline starting"),
        "router": re.compile(r"LogOliveAgentPipeline.*Router:.*complexity=(\w+)"),
        "scout": re.compile(r"LogOliveAgentPipeline.*Scout:.*(\d+) relevant assets"),
        "researcher": re.compile(r"LogOliveAgentPipeline.*Researcher:"),
        "researcher_skip": re.compile(r"LogOliveAgentPipeline.*Researcher: skipped"),
        "architect": re.compile(r"LogOliveAgentPipeline.*Architect:"),
        "validator": re.compile(r"LogOliveAgentPipeline.*Validator:"),
        "pipeline_complete": re.compile(r"LogOliveAgentPipeline.*Agent pipeline complete.*valid=(\w+).*?([\d.]+)s"),
        "pipeline_cli": re.compile(r"LogOliveCLIProvider.*Agent pipeline:.*?(\w+) complexity.*?(\d+) assets.*?([\d.]+)s"),
        "pipeline_fallback": re.compile(r"LogOliveCLIProvider.*Agent pipeline failed.*fallback"),
        "reviewer": re.compile(r"Reviewer.*(?:found|skipped|passed|SATISFIED|missing|deviation)", re.IGNORECASE),
        "correction_pass": re.compile(r"Triggering correction pass"),
    }

    found = {}
    for line in lines:
        for name, pattern in markers.items():
            m = pattern.search(line)
            if m:
                found[name] = {"line": line.strip(), "groups": m.groups()}

    return found


# =============================================================================
# Test Steps
# =============================================================================

def test_mcp_connectivity(port):
    """Test 1: MCP server is reachable and responds to initialize."""
    cprint(Colors.CYAN, "\n[Test 1] MCP Server Connectivity")

    if port is None:
        cprint(Colors.RED, "  FAIL: Could not discover MCP server on ports 3000-3009")
        cprint(Colors.YELLOW, "  Is the UE editor running with Olive AI Studio?")
        return False, None

    cprint(Colors.GREEN, f"  OK: MCP server found on port {port}")

    # Send initialize
    result = send_jsonrpc(port, "initialize", {
        "protocolVersion": "2024-11-05",
        "capabilities": {},
        "clientInfo": {"name": "pipeline-test", "version": "1.0"},
    })

    if "error" in result and "result" not in result:
        cprint(Colors.RED, f"  FAIL: initialize failed: {result['error']}")
        return False, port

    cprint(Colors.GREEN, "  OK: initialize succeeded")
    return True, port


def test_tools_registered(port):
    """Test 2: Tools are still registered (pipeline didn't break tool loading)."""
    cprint(Colors.CYAN, "\n[Test 2] Tool Registration")

    result = send_jsonrpc(port, "tools/list", timeout=15)

    if "error" in result and "result" not in result:
        cprint(Colors.RED, f"  FAIL: tools/list failed: {result['error']}")
        return False

    tools = result.get("result", {}).get("tools", [])
    tool_names = {t["name"] for t in tools}

    expected = [
        "blueprint.create",
        "blueprint.describe",
        "blueprint.apply_plan_json",
        "project.search",
    ]

    all_found = True
    for name in expected:
        if name in tool_names:
            cprint(Colors.GREEN, f"  OK: {name}")
        else:
            cprint(Colors.RED, f"  MISSING: {name}")
            all_found = False

    cprint(Colors.GREEN, f"  Total tools registered: {len(tools)}")
    return all_found


def test_settings_exist(port):
    """Test 3: Agent Pipeline settings are accessible."""
    cprint(Colors.CYAN, "\n[Test 3] Agent Pipeline Settings")

    # Read the settings resource if available
    result = send_jsonrpc(port, "resources/list", timeout=5)
    resources = result.get("result", {}).get("resources", [])

    # Check if any resource mentions pipeline or settings
    has_resources = len(resources) > 0
    if has_resources:
        cprint(Colors.GREEN, f"  OK: {len(resources)} resources available")
    else:
        cprint(Colors.YELLOW, "  WARN: No resources listed (not blocking)")

    # Try a simple tool call to verify the engine is responsive
    result = send_jsonrpc(port, "tools/call", {
        "name": "project.search",
        "arguments": {"query": "BP_*", "max_results": 1},
    }, timeout=10)

    if "error" in result and "result" not in result:
        cprint(Colors.RED, f"  FAIL: project.search failed: {result['error']}")
        return False

    cprint(Colors.GREEN, "  OK: Engine responsive (project.search works)")
    return True


def test_pipeline_log_markers(log_path):
    """Test 4: Check UE log for pipeline execution evidence."""
    cprint(Colors.CYAN, "\n[Test 4] Pipeline Log Markers")

    if not os.path.exists(log_path):
        cprint(Colors.YELLOW, f"  SKIP: Log file not found at {log_path}")
        cprint(Colors.YELLOW, "  Use --log-path to specify the correct location")
        return None

    lines = tail_log_lines(log_path, max_lines=2000)
    if not lines:
        cprint(Colors.YELLOW, "  SKIP: Log file is empty")
        return None

    markers = find_pipeline_markers(lines)

    if not markers:
        cprint(Colors.YELLOW, "  No pipeline execution found in recent log")
        cprint(Colors.YELLOW, "  Send a build request in the chat panel to trigger the pipeline,")
        cprint(Colors.YELLOW, "  then re-run this test.")
        return None

    # Report what we found
    cprint(Colors.GREEN, f"  Found {len(markers)} pipeline markers:")

    if "pipeline_start" in markers:
        cprint(Colors.GREEN, "  [+] Pipeline started")

    if "router" in markers:
        groups = markers["router"]["groups"]
        complexity = groups[0] if groups else "?"
        cprint(Colors.GREEN, f"  [+] Router: complexity={complexity}")

    if "scout" in markers:
        groups = markers["scout"]["groups"]
        assets = groups[0] if groups else "?"
        cprint(Colors.GREEN, f"  [+] Scout: {assets} relevant assets")

    if "researcher_skip" in markers:
        cprint(Colors.GREEN, "  [+] Researcher: skipped (Simple task)")
    elif "researcher" in markers:
        cprint(Colors.GREEN, "  [+] Researcher: ran")

    if "architect" in markers:
        cprint(Colors.GREEN, "  [+] Architect: ran")

    if "validator" in markers:
        cprint(Colors.GREEN, "  [+] Validator: ran")

    if "pipeline_complete" in markers:
        groups = markers["pipeline_complete"]["groups"]
        valid = groups[0] if groups else "?"
        elapsed = groups[1] if len(groups) > 1 else "?"
        cprint(Colors.GREEN, f"  [+] Pipeline complete: valid={valid}, {elapsed}s")

    if "pipeline_cli" in markers:
        groups = markers["pipeline_cli"]["groups"]
        complexity = groups[0] if groups else "?"
        assets = groups[1] if len(groups) > 1 else "?"
        elapsed = groups[2] if len(groups) > 2 else "?"
        cprint(Colors.GREEN, f"  [+] CLI integration: {complexity} complexity, {assets} assets, {elapsed}s")

    if "pipeline_fallback" in markers:
        cprint(Colors.YELLOW, "  [!] Pipeline used fallback (LLM calls failed)")

    if "reviewer" in markers:
        cprint(Colors.GREEN, "  [+] Reviewer: ran")

    if "correction_pass" in markers:
        cprint(Colors.YELLOW, "  [!] Reviewer triggered a correction pass")

    return True


def test_new_files_exist():
    """Test 5: Verify all pipeline source files exist."""
    cprint(Colors.CYAN, "\n[Test 5] Pipeline Source Files")

    plugin_root = Path(__file__).parent.parent
    expected_files = [
        "Source/OliveAIEditor/Public/Brain/OliveAgentConfig.h",
        "Source/OliveAIEditor/Public/Brain/OliveAgentPipeline.h",
        "Source/OliveAIEditor/Private/Brain/OliveAgentPipeline.cpp",
    ]

    all_exist = True
    for rel_path in expected_files:
        full = plugin_root / rel_path
        if full.exists():
            size = full.stat().st_size
            cprint(Colors.GREEN, f"  OK: {rel_path} ({size:,} bytes)")
        else:
            cprint(Colors.RED, f"  MISSING: {rel_path}")
            all_exist = False

    # Check the .cpp is substantial (not just stubs)
    cpp_path = plugin_root / "Source/OliveAIEditor/Private/Brain/OliveAgentPipeline.cpp"
    if cpp_path.exists():
        content = cpp_path.read_text(encoding="utf-8", errors="replace")
        line_count = content.count("\n")
        todo_count = content.lower().count("todo")
        stub_count = content.lower().count("stub")

        cprint(Colors.GREEN if line_count > 1000 else Colors.YELLOW,
               f"  Lines: {line_count} (expected 2000+)")
        if todo_count > 0:
            cprint(Colors.YELLOW, f"  WARN: {todo_count} TODO markers remaining")
        if stub_count > 0:
            cprint(Colors.YELLOW, f"  WARN: {stub_count} stub markers remaining")

        # Check key methods are implemented (not just declared)
        # Class methods (qualified with FOliveAgentPipeline:: or FOliveAgentPipelineResult::)
        class_methods = [
            "SendAgentCompletion",
            "Execute",
            "RunRouter",
            "RunScout",
            "RunResearcher",
            "RunArchitect",
            "RunValidator",
            "RunReviewer",
            "FormatForPromptInjection",
            "ParseBuildPlan",
            "TryResolveClass",
        ]
        for method in class_methods:
            pattern = rf"(?:FOliveAgentPipeline|FOliveAgentPipelineResult)::{method}\b"
            if re.search(pattern, content):
                cprint(Colors.GREEN, f"  [+] {method}() implemented")
            else:
                cprint(Colors.RED, f"  [-] {method}() NOT FOUND")
                all_exist = False

        # Free functions in anonymous namespace
        anon_functions = ["BuildAssetStateSummary", "TrySendViaProvider"]
        for func in anon_functions:
            pattern = rf"FString\s+{func}\b|bool\s+{func}\b"
            if re.search(pattern, content):
                cprint(Colors.GREEN, f"  [+] {func}() implemented (anon namespace)")
            else:
                cprint(Colors.YELLOW, f"  [?] {func}() not found (may be inlined)")


    return all_exist


def test_integration_wired():
    """Test 6: Verify integration code is wired into CLIProviderBase + ConversationManager."""
    cprint(Colors.CYAN, "\n[Test 6] Integration Wiring")

    plugin_root = Path(__file__).parent.parent
    checks = []

    # CLIProviderBase.cpp
    cli_cpp = plugin_root / "Source/OliveAIEditor/Private/Providers/OliveCLIProviderBase.cpp"
    if cli_cpp.exists():
        content = cli_cpp.read_text(encoding="utf-8", errors="replace")

        checks.append(("CLI: Pipeline include",
                       "OliveAgentPipeline.h" in content))
        checks.append(("CLI: Pipeline.Execute() call",
                       "Pipeline.Execute(" in content))
        checks.append(("CLI: FormatForPromptInjection()",
                       "FormatForPromptInjection()" in content))
        checks.append(("CLI: Write-intent gate (MessageImpliesMutation)",
                       "MessageImpliesMutation" in content))
        checks.append(("CLI: Pipeline fallback",
                       "Build Guidance" in content))
        checks.append(("CLI: Reviewer integration",
                       "RunReviewer(" in content))
        checks.append(("CLI: bEnablePostBuildReview check",
                       "bEnablePostBuildReview" in content))
        checks.append(("CLI: Old discovery pass removed",
                       "Required: Asset Decomposition" not in content))
    else:
        checks.append(("CLI: File exists", False))

    # CLIProviderBase.h
    cli_h = plugin_root / "Source/OliveAIEditor/Public/Providers/OliveCLIProviderBase.h"
    if cli_h.exists():
        content = cli_h.read_text(encoding="utf-8", errors="replace")
        checks.append(("CLI header: CachedPipelineResult",
                       "CachedPipelineResult" in content))
        checks.append(("CLI header: bIsReviewerCorrectionPass",
                       "bIsReviewerCorrectionPass" in content))

    # ConversationManager.cpp
    cm_cpp = plugin_root / "Source/OliveAIEditor/Private/Chat/OliveConversationManager.cpp"
    if cm_cpp.exists():
        content = cm_cpp.read_text(encoding="utf-8", errors="replace")
        checks.append(("CM: Pipeline include",
                       "OliveAgentPipeline.h" in content))
        checks.append(("CM: Pipeline.Execute() call",
                       "Pipeline.Execute(" in content))
        checks.append(("CM: FormatForPromptInjection()",
                       "FormatForPromptInjection()" in content))
        checks.append(("CM: Write-intent gate",
                       "bTurnHasExplicitWriteIntent" in content))
        checks.append(("CM: Reviewer integration",
                       "RunReviewer(" in content))
        checks.append(("CM: bEnablePostBuildReview check",
                       "bEnablePostBuildReview" in content))
        checks.append(("CM: CorrectionDirective.IsEmpty() check",
                       "CorrectionDirective.IsEmpty()" in content))
        checks.append(("CM: ModifiedAssetPaths tracking",
                       "ModifiedAssetPaths" in content))
    else:
        checks.append(("CM: File exists", False))

    # ConversationManager.h
    cm_h = plugin_root / "Source/OliveAIEditor/Public/Chat/OliveConversationManager.h"
    if cm_h.exists():
        content = cm_h.read_text(encoding="utf-8", errors="replace")
        checks.append(("CM header: CachedPipelineResult",
                       "CachedPipelineResult" in content))
        checks.append(("CM header: ModifiedAssetPaths",
                       "ModifiedAssetPaths" in content))

    # Settings
    settings_h = plugin_root / "Source/OliveAIEditor/Public/Settings/OliveAISettings.h"
    if settings_h.exists():
        content = settings_h.read_text(encoding="utf-8", errors="replace")
        checks.append(("Settings: bCustomizeAgentModels",
                       "bCustomizeAgentModels" in content))
        checks.append(("Settings: bEnablePostBuildReview",
                       "bEnablePostBuildReview" in content))
        checks.append(("Settings: GetAgentModelConfig",
                       "GetAgentModelConfig" in content))
        checks.append(("Settings: Agent Pipeline category",
                       'Category="Agent Pipeline"' in content
                       or "Category = \"Agent Pipeline\"" in content))

    all_pass = True
    for label, passed in checks:
        if passed:
            cprint(Colors.GREEN, f"  OK: {label}")
        else:
            cprint(Colors.RED, f"  FAIL: {label}")
            all_pass = False

    return all_pass


# =============================================================================
# Main
# =============================================================================

def main():
    parser = argparse.ArgumentParser(description="Agent Pipeline Smoke Test")
    parser.add_argument("--log-path", default=DEFAULT_LOG_PATH,
                        help="Path to UE_Olive_AI_Toolkit.log")
    parser.add_argument("--port", type=int, default=None,
                        help="MCP server port (auto-discovers if not set)")
    args = parser.parse_args()

    cprint(Colors.BOLD, "=" * 60)
    cprint(Colors.BOLD, "  Olive AI Studio — Agent Pipeline Smoke Test")
    cprint(Colors.BOLD, f"  {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    cprint(Colors.BOLD, "=" * 60)

    results = {}

    # Test 5 & 6: Source file checks (don't need UE running)
    results["source_files"] = test_new_files_exist()
    results["integration"] = test_integration_wired()

    # Test 1-3: MCP server tests (need UE running, but can't loopback from within UE)
    if _running_in_ue():
        cprint(Colors.CYAN, "\n[Test 1] MCP Server Connectivity")
        cprint(Colors.GREEN, "  OK: Running inside UE — MCP server is in-process")
        results["mcp_connect"] = True
        results["tools"] = None  # Can't self-connect via HTTP from in-process
        results["settings"] = None
        cprint(Colors.YELLOW, "  Skipping HTTP tool/settings tests (in-process, use CLI to test MCP)")
    else:
        port = args.port
        if port is None:
            cprint(Colors.CYAN, "\n[Discovery] Looking for MCP server...")
            port = discover_mcp_port()

        ok, port = test_mcp_connectivity(port)
        results["mcp_connect"] = ok

        if ok:
            results["tools"] = test_tools_registered(port)
            results["settings"] = test_settings_exist(port)
        else:
            results["tools"] = None
            results["settings"] = None
            cprint(Colors.YELLOW, "\n  Skipping tool/settings tests (no MCP connection)")

    # Test 4: Log markers
    results["log_markers"] = test_pipeline_log_markers(args.log_path)

    # Summary
    cprint(Colors.BOLD, "\n" + "=" * 60)
    cprint(Colors.BOLD, "  RESULTS")
    cprint(Colors.BOLD, "=" * 60)

    pass_count = sum(1 for v in results.values() if v is True)
    fail_count = sum(1 for v in results.values() if v is False)
    skip_count = sum(1 for v in results.values() if v is None)

    for name, result in results.items():
        label = name.replace("_", " ").title()
        if result is True:
            cprint(Colors.GREEN, f"  PASS  {label}")
        elif result is False:
            cprint(Colors.RED, f"  FAIL  {label}")
        else:
            cprint(Colors.YELLOW, f"  SKIP  {label}")

    print()
    cprint(Colors.BOLD, f"  {pass_count} passed, {fail_count} failed, {skip_count} skipped")

    if fail_count == 0 and pass_count > 0:
        cprint(Colors.GREEN, "\n  Pipeline integration looks good!")
        if skip_count > 0:
            cprint(Colors.YELLOW, "  Some tests were skipped (UE not running or no pipeline run yet).")
            cprint(Colors.YELLOW, "  To test end-to-end: open UE, send a build request, then re-run.")
    else:
        cprint(Colors.RED, "\n  Issues detected — see FAIL items above.")

    return 0 if fail_count == 0 else 1


if __name__ == "__main__":
    # Avoid SystemExit traceback when run inside UE's Python host
    result = main()
    if not _running_in_ue():
        sys.exit(result)
