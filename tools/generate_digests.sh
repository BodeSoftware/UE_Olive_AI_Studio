#!/usr/bin/env bash
# generate_digests.sh -- Batch-generate digests for library templates using the tagger agent.
#
# Usage:
#   bash tools/generate_digests.sh combatfs
#   bash tools/generate_digests.sh combatfs action_rpg fps_shooter
#   bash tools/generate_digests.sh  # all projects
#
# This script prints the claude command to spawn tagger agents for each template.
# The tagger agent reads the template, generates a digest field, and writes it back.
#
# Prerequisites:
#   - claude CLI installed and authenticated
#   - .claude/agents/tagger.md updated with digest generation instructions

set -euo pipefail

PLUGIN_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LIBRARY_DIR="$PLUGIN_ROOT/Content/Templates/library"

if [ ! -d "$LIBRARY_DIR" ]; then
    echo "Error: Library directory not found at $LIBRARY_DIR"
    exit 1
fi

# Determine which projects to process
if [ $# -gt 0 ]; then
    PROJECTS=("$@")
else
    PROJECTS=()
    for dir in "$LIBRARY_DIR"/*/; do
        [ -d "$dir" ] && PROJECTS+=("$(basename "$dir")")
    done
fi

if [ ${#PROJECTS[@]} -eq 0 ]; then
    echo "No projects found in $LIBRARY_DIR"
    exit 1
fi

TOTAL=0
PROCESSED=0

for project in "${PROJECTS[@]}"; do
    PROJECT_DIR="$LIBRARY_DIR/$project"
    if [ ! -d "$PROJECT_DIR" ]; then
        echo "Warning: Project directory not found: $PROJECT_DIR"
        continue
    fi

    echo "=========================================="
    echo "Project: $project"
    echo "=========================================="

    for template in "$PROJECT_DIR"/*.json; do
        [ -f "$template" ] || continue
        TOTAL=$((TOTAL + 1))

        # Check if digest already exists
        if python3 -c "import json; d=json.load(open('$template')); exit(0 if 'digest' in d else 1)" 2>/dev/null; then
            echo "  SKIP (has digest): $(basename "$template")"
            continue
        fi

        echo "  QUEUE: $(basename "$template")"
        PROCESSED=$((PROCESSED + 1))

        # Print the command to run (user runs these manually or pipes to parallel)
        echo "    claude --agent tagger --print \"Generate a digest for this already-tagged template: $template
Read the template JSON. Generate ONLY the 'digest' field (see section 6 of your instructions). Write the updated JSON back. Do NOT modify any existing fields.\""
    done
done

echo ""
echo "=========================================="
echo "Total templates: $TOTAL"
echo "Need digests: $PROCESSED"
echo "Already have digests: $((TOTAL - PROCESSED))"
echo "=========================================="
echo ""
echo "To run all digest generation, pipe the commands above to your shell."
echo "For parallel execution, consider using GNU parallel or xargs."
