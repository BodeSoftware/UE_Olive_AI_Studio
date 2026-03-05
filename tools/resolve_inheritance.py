"""
resolve_inheritance.py - Build a topologically-sorted inheritance manifest
for extracted Blueprint JSON files.

Scans all JSON files in the combatfs blueprints directory, resolves
parent-child relationships, performs a topological sort (parents before
children), and outputs a manifest JSON file.

Usage:
    python resolve_inheritance.py

Output:
    tools/combatfs_manifest.json
"""

import json
import os
import re
import sys
from collections import defaultdict, deque
from pathlib import Path

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------

SCRIPT_DIR = Path(__file__).resolve().parent
PLUGIN_DIR = SCRIPT_DIR.parent
BLUEPRINTS_DIR = PLUGIN_DIR / "sample_project_templates" / "combatfs" / "blueprints"
OUTPUT_PATH = SCRIPT_DIR / "combatfs_manifest.json"


# ---------------------------------------------------------------------------
# Template ID generation
# ---------------------------------------------------------------------------

def make_template_id(name: str) -> str:
    """
    Convert a Blueprint name to a template_id.

    Rules:
      1. Strip 'FCS_' prefix (case-insensitive)
      2. Convert CamelCase to snake_case
      3. Replace hyphens and spaces with underscores
      4. Collapse multiple underscores
      5. Lowercase everything
      6. Keep existing prefixes like 'bp_', 'cr_', etc.

    Examples:
      BP_AbilityParent        -> bp_ability_parent
      BP_FCS_MeleeComponent   -> bp_melee_component
      CR_BoneTransformUE5     -> cr_bone_transform_ue5
      BP_Spawner-Random       -> bp_spawner_random
    """
    cleaned = name
    # Strip FCS_ prefix (handles BP_FCS_Foo -> BP_Foo)
    if cleaned.upper().startswith("FCS_"):
        cleaned = cleaned[4:]
    # Also handle BP_FCS_Foo pattern where FCS_ appears after BP_
    cleaned = re.sub(r"^(BP_|bp_)FCS_", r"\1", cleaned, flags=re.IGNORECASE)

    # Insert underscore between lowercase/digit and uppercase: camelCase -> camel_Case
    result = re.sub(r"(?<=[a-z0-9])(?=[A-Z])", "_", cleaned)
    # Insert underscore between consecutive uppercase and uppercase+lowercase: ABCDef -> ABC_Def
    result = re.sub(r"(?<=[A-Z])(?=[A-Z][a-z])", "_", result)
    # Replace hyphens and spaces
    result = result.replace("-", "_").replace(" ", "_")
    # Collapse multiple underscores
    result = re.sub(r"_+", "_", result)

    return result.lower()


# ---------------------------------------------------------------------------
# Blueprint entry
# ---------------------------------------------------------------------------

class BlueprintEntry:
    """Holds parsed metadata for a single Blueprint JSON file."""

    __slots__ = (
        "name",
        "file_name",
        "template_id",
        "parent_name",
        "parent_source",
        "parent_display_name",
        "bp_type",
    )

    def __init__(
        self,
        name: str,
        file_name: str,
        parent_name: str,
        parent_source: str,
        parent_display_name: str,
        bp_type: str,
    ):
        self.name = name
        self.file_name = file_name
        self.template_id = make_template_id(name)
        self.parent_name = parent_name          # Resolved BP name (no _C)
        self.parent_source = parent_source      # "blueprint" or "cpp"
        self.parent_display_name = parent_display_name  # Raw name from JSON
        self.bp_type = bp_type


# ---------------------------------------------------------------------------
# Scanning
# ---------------------------------------------------------------------------

def scan_blueprints(directory: Path) -> list[BlueprintEntry]:
    """
    Scan all JSON files in the directory and extract name + parent_class
    metadata. Only reads the first ~20 lines worth of data (the header
    fields are always at the top of the file).
    """
    entries = []
    json_files = sorted(f for f in os.listdir(directory) if f.endswith(".json"))

    for file_name in json_files:
        file_path = directory / file_name
        try:
            with open(file_path, "r", encoding="utf-8") as fp:
                data = json.load(fp)
        except (json.JSONDecodeError, IOError) as e:
            print(f"WARNING: Could not parse {file_name}: {e}", file=sys.stderr)
            continue

        name = data.get("name", "")
        bp_type = data.get("type", "Normal")
        parent_class = data.get("parent_class", {})
        parent_raw_name = parent_class.get("name", "Unknown")
        parent_source = parent_class.get("source", "cpp")

        # For blueprint-source parents, strip the _C suffix to get the BP name
        if parent_source == "blueprint":
            parent_bp_name = parent_raw_name
            if parent_bp_name.endswith("_C"):
                parent_bp_name = parent_bp_name[:-2]
        else:
            # For C++ parents, use the raw class name (Actor, Character, etc.)
            parent_bp_name = parent_raw_name

        entry = BlueprintEntry(
            name=name,
            file_name=file_name,
            parent_name=parent_bp_name,
            parent_source=parent_source,
            parent_display_name=parent_raw_name,
            bp_type=bp_type,
        )
        entries.append(entry)

    return entries


# ---------------------------------------------------------------------------
# Topological sort
# ---------------------------------------------------------------------------

def topological_sort(entries: list[BlueprintEntry]) -> list[BlueprintEntry]:
    """
    Kahn's algorithm for topological sort.

    Nodes with no blueprint-source parent (i.e., C++ parents) have zero
    in-degree and are processed first. Within the same depth level, entries
    are sorted alphabetically by name for deterministic output.

    Returns the entries in dependency order (parents before children).
    Raises ValueError if a cycle is detected.
    """
    # Build lookup and adjacency
    by_name: dict[str, BlueprintEntry] = {e.name: e for e in entries}
    children: dict[str, list[str]] = defaultdict(list)  # parent -> [children]
    in_degree: dict[str, int] = {e.name: 0 for e in entries}

    for entry in entries:
        if entry.parent_source == "blueprint" and entry.parent_name in by_name:
            children[entry.parent_name].append(entry.name)
            in_degree[entry.name] += 1

    # Seed with zero in-degree nodes (sorted for determinism)
    queue = deque(
        sorted(
            [name for name, deg in in_degree.items() if deg == 0],
            key=str.lower,
        )
    )

    result: list[BlueprintEntry] = []

    while queue:
        # Process all nodes at this level, then sort the next level
        next_level: list[str] = []

        current_name = queue.popleft()
        result.append(by_name[current_name])

        for child_name in sorted(children[current_name], key=str.lower):
            in_degree[child_name] -= 1
            if in_degree[child_name] == 0:
                # Insert into queue maintaining sorted order
                # Use bisect-like insertion for determinism
                inserted = False
                for i, q_name in enumerate(queue):
                    if child_name.lower() < q_name.lower():
                        queue.insert(i, child_name)
                        inserted = True
                        break
                if not inserted:
                    queue.append(child_name)

    if len(result) != len(entries):
        processed = {e.name for e in result}
        remaining = [e.name for e in entries if e.name not in processed]
        raise ValueError(
            f"Cycle detected! {len(remaining)} nodes could not be sorted: "
            f"{remaining[:10]}..."
        )

    return result


# ---------------------------------------------------------------------------
# Children map
# ---------------------------------------------------------------------------

def build_children_map(entries: list[BlueprintEntry]) -> dict[str, list[str]]:
    """
    Build a map from each entry's template_id to a list of direct children
    template_ids (only blueprint-source children).
    """
    name_to_tid: dict[str, str] = {e.name: e.template_id for e in entries}
    children: dict[str, list[str]] = defaultdict(list)

    for entry in entries:
        if entry.parent_source == "blueprint" and entry.parent_name in name_to_tid:
            parent_tid = name_to_tid[entry.parent_name]
            children[parent_tid].append(entry.template_id)

    # Sort children lists for determinism
    for tid in children:
        children[tid].sort()

    return children


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> None:
    if not BLUEPRINTS_DIR.is_dir():
        print(f"ERROR: Blueprints directory not found: {BLUEPRINTS_DIR}", file=sys.stderr)
        sys.exit(1)

    print(f"Scanning {BLUEPRINTS_DIR}...")
    entries = scan_blueprints(BLUEPRINTS_DIR)
    print(f"  Found {len(entries)} blueprint files")

    # Count by parent source
    bp_parent_count = sum(1 for e in entries if e.parent_source == "blueprint")
    cpp_parent_count = sum(1 for e in entries if e.parent_source != "blueprint")
    print(f"  Blueprint parents: {bp_parent_count}")
    print(f"  C++ parents: {cpp_parent_count}")

    print("Performing topological sort...")
    sorted_entries = topological_sort(entries)

    children_map = build_children_map(sorted_entries)

    # Compute some stats
    max_depth = 0
    depth_map: dict[str, int] = {}
    name_lookup = {e.name: e for e in sorted_entries}

    def get_depth(name: str) -> int:
        if name in depth_map:
            return depth_map[name]
        entry = name_lookup.get(name)
        if entry is None or entry.parent_source != "blueprint":
            depth_map[name] = 0
            return 0
        d = 1 + get_depth(entry.parent_name)
        depth_map[name] = d
        return d

    for entry in sorted_entries:
        d = get_depth(entry.name)
        if d > max_depth:
            max_depth = d

    print(f"  Max inheritance depth: {max_depth}")
    for d in range(max_depth + 1):
        count = sum(1 for v in depth_map.values() if v == d)
        print(f"    Depth {d}: {count} blueprints")

    # Build output manifest
    processing_order = []
    for entry in sorted_entries:
        # For the parent_class display: use the raw C++ class name for cpp parents,
        # or the BP name (without _C) for blueprint parents
        if entry.parent_source == "blueprint":
            depends_on = name_lookup[entry.parent_name].template_id
        else:
            depends_on = None

        item = {
            "template_id": entry.template_id,
            "name": entry.name,
            "file": entry.file_name,
            "parent_class": entry.parent_name,
            "parent_source": entry.parent_source,
            "depends_on": depends_on,
            "depth": depth_map[entry.name],
            "children": children_map.get(entry.template_id, []) or None,
        }
        processing_order.append(item)

    manifest = {
        "project": "combatfs",
        "total_files": len(sorted_entries),
        "max_inheritance_depth": max_depth,
        "depth_distribution": {
            str(d): sum(1 for v in depth_map.values() if v == d)
            for d in range(max_depth + 1)
        },
        "blueprint_parent_count": bp_parent_count,
        "native_parent_count": cpp_parent_count,
        "processing_order": processing_order,
    }

    # Write output
    OUTPUT_PATH.parent.mkdir(parents=True, exist_ok=True)
    with open(OUTPUT_PATH, "w", encoding="utf-8") as fp:
        json.dump(manifest, fp, indent=2, ensure_ascii=False)

    print(f"\nManifest written to {OUTPUT_PATH}")
    print(f"  {len(processing_order)} entries in processing order")

    # Print first few and last few entries as a sanity check
    print("\nFirst 5 entries (roots):")
    for item in processing_order[:5]:
        children_str = f" -> children: {item['children']}" if item["children"] else ""
        print(f"  [{item['depth']}] {item['template_id']:40s} (parent: {item['parent_class']}){children_str}")

    print("\nLast 5 entries (deepest leaves):")
    for item in processing_order[-5:]:
        print(f"  [{item['depth']}] {item['template_id']:40s} depends_on: {item['depends_on']}")


if __name__ == "__main__":
    main()
