"""
prepare_library.py

Reads the CombatFS manifest and prepares library-ready template JSON files
by injecting metadata fields (template_id, display_name, source_project,
depends_on) at the top of each source JSON object.

Output directory: Content/Templates/library/combatfs/
"""

import json
import os
import sys
import time
from collections import OrderedDict
from pathlib import Path

# ----- Paths -----
PLUGIN_ROOT = Path(r"B:\Unreal Projects\UE_Olive_AI_Toolkit\Plugins\UE_Olive_AI_Studio")
MANIFEST_PATH = PLUGIN_ROOT / "tools" / "combatfs_manifest.json"
SOURCE_DIR = PLUGIN_ROOT / "sample_project_templates" / "combatfs" / "blueprints"
OUTPUT_DIR = PLUGIN_ROOT / "Content" / "Templates" / "library" / "combatfs"

SOURCE_PROJECT = "CombatFS"


def main():
    start_time = time.time()

    # --- Load manifest ---
    if not MANIFEST_PATH.exists():
        print(f"ERROR: Manifest not found at {MANIFEST_PATH}")
        sys.exit(1)

    with open(MANIFEST_PATH, "r", encoding="utf-8") as f:
        manifest = json.load(f)

    processing_order = manifest.get("processing_order", [])
    if not processing_order:
        print("ERROR: processing_order is empty or missing in manifest")
        sys.exit(1)

    print(f"Manifest loaded: {len(processing_order)} entries in processing_order")

    # --- Create output directory ---
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    print(f"Output directory: {OUTPUT_DIR}")

    # --- Process each entry ---
    processed = 0
    errors = []
    total_bytes = 0

    for entry in processing_order:
        template_id = entry["template_id"]
        display_name = entry["name"]
        depends_on = entry["depends_on"]  # null or string
        source_file = entry["file"]

        source_path = SOURCE_DIR / source_file
        output_path = OUTPUT_DIR / f"{template_id}.json"

        # Read source JSON
        if not source_path.exists():
            msg = f"Source file not found: {source_file}"
            errors.append(msg)
            print(f"  SKIP [{template_id}]: {msg}")
            continue

        try:
            with open(source_path, "r", encoding="utf-8") as f:
                source_data = json.load(f, object_pairs_hook=OrderedDict)
        except json.JSONDecodeError as e:
            msg = f"JSON parse error in {source_file}: {e}"
            errors.append(msg)
            print(f"  SKIP [{template_id}]: {msg}")
            continue

        # Build output object with injected fields at the top
        output_data = OrderedDict()
        output_data["template_id"] = template_id
        output_data["display_name"] = display_name
        output_data["source_project"] = SOURCE_PROJECT
        output_data["depends_on"] = depends_on

        # Merge all existing fields (preserving order)
        for key, value in source_data.items():
            output_data[key] = value

        # Write output JSON
        try:
            output_json = json.dumps(output_data, indent=2, ensure_ascii=False)
            with open(output_path, "w", encoding="utf-8", newline="\n") as f:
                f.write(output_json)
                f.write("\n")

            file_size = len(output_json.encode("utf-8"))
            total_bytes += file_size
            processed += 1
        except OSError as e:
            msg = f"Write error for {template_id}: {e}"
            errors.append(msg)
            print(f"  FAIL [{template_id}]: {msg}")
            continue

    # --- Report ---
    elapsed = time.time() - start_time
    total_mb = total_bytes / (1024 * 1024)

    print()
    print("=" * 60)
    print(f"  Files processed: {processed} / {len(processing_order)}")
    print(f"  Total output size: {total_bytes:,} bytes ({total_mb:.2f} MB)")
    print(f"  Errors: {len(errors)}")
    print(f"  Elapsed: {elapsed:.2f}s")
    print("=" * 60)

    if errors:
        print()
        print("Errors:")
        for err in errors:
            print(f"  - {err}")

    # Exit with error code if any failures
    if errors:
        sys.exit(1)


if __name__ == "__main__":
    main()
