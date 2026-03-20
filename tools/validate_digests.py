#!/usr/bin/env python3
"""validate_digests.py -- Check quality of LLM-generated template digests.

Usage:
    python tools/validate_digests.py combatfs
    python tools/validate_digests.py  # validates all projects

Checks:
- digest field exists and is a dict
- purpose exists, 5-20 words
- key_patterns has 2-8 entries
- functions list covers top functions by node count
- variables reference actual template variables
- dispatchers reference actual template dispatchers

Reports pass/fail per template, summary at end.
"""

import json
import os
import sys
from pathlib import Path

PLUGIN_ROOT = Path(__file__).parent.parent
LIBRARY_DIR = PLUGIN_ROOT / "Content" / "Templates" / "library"

MIN_PURPOSE_WORDS = 5
MAX_PURPOSE_WORDS = 20
MIN_KEY_PATTERNS = 2
MAX_KEY_PATTERNS = 8
MIN_FUNCTION_NODES = 5  # functions below this are stubs


def get_top_functions(data, n=3):
    """Get top N function names by node count from the template."""
    functions = []
    if "graphs" in data:
        for fn in data["graphs"].get("functions", []):
            node_count = len(fn.get("nodes", []))
            if node_count >= MIN_FUNCTION_NODES:
                functions.append((fn.get("name", ""), node_count))
    functions.sort(key=lambda x: x[1], reverse=True)
    return [f[0] for f in functions[:n]]


def get_template_variables(data):
    """Get self-defined variable names from the template."""
    variables = set()
    for var in data.get("variables", []):
        if var.get("defined_in") == "self":
            variables.add(var.get("name", ""))
    return variables


def get_template_dispatchers(data):
    """Get event dispatcher names from the template."""
    return {d.get("name", "") for d in data.get("event_dispatchers", [])}


def validate_digest(filepath):
    """Validate a single template's digest. Returns (pass, errors)."""
    errors = []

    with open(filepath, "r", encoding="utf-8") as f:
        data = json.load(f)

    template_id = data.get("template_id", filepath.name)

    # Check digest exists
    digest = data.get("digest")
    if digest is None:
        return False, [f"{template_id}: missing digest field"]
    if not isinstance(digest, dict):
        return False, [f"{template_id}: digest is not an object"]

    # Check purpose
    purpose = digest.get("purpose", "")
    if not purpose:
        errors.append(f"{template_id}: missing purpose")
    else:
        word_count = len(purpose.split())
        if word_count < MIN_PURPOSE_WORDS:
            errors.append(f"{template_id}: purpose too short ({word_count} words, min {MIN_PURPOSE_WORDS})")
        if word_count > MAX_PURPOSE_WORDS:
            errors.append(f"{template_id}: purpose too long ({word_count} words, max {MAX_PURPOSE_WORDS})")

    # Check key_patterns
    patterns = digest.get("key_patterns", [])
    if not isinstance(patterns, list):
        errors.append(f"{template_id}: key_patterns is not an array")
    elif len(patterns) < MIN_KEY_PATTERNS:
        errors.append(f"{template_id}: too few key_patterns ({len(patterns)}, min {MIN_KEY_PATTERNS})")
    elif len(patterns) > MAX_KEY_PATTERNS:
        errors.append(f"{template_id}: too many key_patterns ({len(patterns)}, max {MAX_KEY_PATTERNS})")

    # Check functions cover top functions by node count
    digest_functions = digest.get("functions", [])
    if not isinstance(digest_functions, list):
        errors.append(f"{template_id}: functions is not an array")
    else:
        digest_fn_names = {f.get("name", "").lower() for f in digest_functions if isinstance(f, dict)}
        top_functions = get_top_functions(data, n=3)

        for fn_name in top_functions:
            if fn_name.lower() not in digest_fn_names:
                errors.append(f"{template_id}: top function '{fn_name}' missing from digest")

        # Check each function has a 'does' field
        for fn in digest_functions:
            if isinstance(fn, dict):
                does = fn.get("does", "")
                if not does:
                    errors.append(f"{template_id}: function '{fn.get('name', '?')}' missing 'does'")
                elif len(does.split()) < 3:
                    errors.append(f"{template_id}: function '{fn.get('name', '?')}' does too short")

    # Check variables reference actual template variables
    digest_vars = digest.get("variables", [])
    if isinstance(digest_vars, list):
        template_vars = get_template_variables(data)
        for var_entry in digest_vars:
            if isinstance(var_entry, str):
                var_name = var_entry.split(":")[0]
                if var_name and template_vars and var_name not in template_vars:
                    errors.append(f"{template_id}: digest variable '{var_name}' not in template")

    # Check dispatchers reference actual template dispatchers
    digest_dispatchers = digest.get("dispatchers", [])
    if isinstance(digest_dispatchers, list):
        template_dispatchers = get_template_dispatchers(data)
        for disp in digest_dispatchers:
            if isinstance(disp, str) and template_dispatchers and disp not in template_dispatchers:
                errors.append(f"{template_id}: digest dispatcher '{disp}' not in template")

    return len(errors) == 0, errors


def validate_project(project_dir):
    """Validate all templates in a project directory."""
    passed = 0
    failed = 0
    missing = 0
    all_errors = []

    json_files = sorted(project_dir.glob("*.json"))
    for filepath in json_files:
        try:
            with open(filepath, "r", encoding="utf-8") as f:
                data = json.load(f)

            if "digest" not in data:
                missing += 1
                continue

            ok, errors = validate_digest(filepath)
            if ok:
                passed += 1
            else:
                failed += 1
                all_errors.extend(errors)
        except (json.JSONDecodeError, Exception) as e:
            failed += 1
            all_errors.append(f"{filepath.name}: {e}")

    return passed, failed, missing, all_errors


def main():
    projects = sys.argv[1:] if len(sys.argv) > 1 else []

    if not projects:
        # Validate all projects
        if LIBRARY_DIR.exists():
            projects = [d.name for d in sorted(LIBRARY_DIR.iterdir()) if d.is_dir()]

    if not projects:
        print("No library template projects found.")
        sys.exit(1)

    total_passed = 0
    total_failed = 0
    total_missing = 0

    for project in projects:
        project_dir = LIBRARY_DIR / project
        if not project_dir.exists():
            print(f"Project directory not found: {project_dir}")
            continue

        print(f"\n{'='*60}")
        print(f"Validating: {project}")
        print(f"{'='*60}")

        passed, failed, missing, errors = validate_project(project_dir)
        total_passed += passed
        total_failed += failed
        total_missing += missing

        for err in errors:
            print(f"  FAIL: {err}")

        total = passed + failed
        if total > 0:
            rate = passed / total * 100
            print(f"\n  Results: {passed}/{total} passed ({rate:.1f}%), {missing} missing digest")
        else:
            print(f"\n  No templates with digests found ({missing} missing)")

    print(f"\n{'='*60}")
    print(f"TOTAL: {total_passed} passed, {total_failed} failed, {total_missing} missing")
    if total_passed + total_failed > 0:
        rate = total_passed / (total_passed + total_failed) * 100
        print(f"Pass rate: {rate:.1f}%")
    print(f"{'='*60}")

    sys.exit(1 if total_failed > 0 else 0)


if __name__ == "__main__":
    main()
