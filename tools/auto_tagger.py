#!/usr/bin/env python
"""
auto_tagger.py -- Processes combatfs Blueprint JSON library files and adds
searchable tags, descriptions, and entry_points metadata.

Reads the manifest (tools/combatfs_manifest.json) to process files in
dependency order (parents before children), propagating inherited tags.

Usage:
    python tools/auto_tagger.py

Output: Updated JSON files in-place under Content/Templates/library/combatfs/.
"""

import json
import os
import re
import sys
import time
from collections import Counter, OrderedDict


# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

PLUGIN_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # up from tools/
MANIFEST_PATH = os.path.join(PLUGIN_ROOT, "tools", "combatfs_manifest.json")
LIBRARY_DIR = os.path.join(PLUGIN_ROOT, "Content", "Templates", "library", "combatfs")

# Tag limits
MIN_BLUEPRINT_TAGS = 3
MAX_BLUEPRINT_TAGS = 15
MIN_FUNCTION_TAGS = 3
MAX_FUNCTION_TAGS = 8

# Minimum node count for a function to get tags/description (skip stubs)
MIN_FUNCTION_NODES = 3

# Stop words to filter from tags
STOP_WORDS = frozenset([
    "bp", "the", "a", "an", "is", "it", "to", "of", "for", "and", "or",
    "in", "on", "at", "be", "do", "has", "get", "set", "new", "k2", "node",
    "pin", "exec", "return", "self", "then", "else", "bool", "int", "float",
    "string", "byte", "text", "name", "class", "struct", "enum", "array",
    "map", "object", "default", "none", "true", "false", "temp", "local",
    "index", "count", "value", "result", "output", "input",
    # Additional noise words from UE internals
    "c", "execute", "retval", "returnvalue", "wildcard",
    # UE structural noise that adds no search value
    "ref", "construction", "script", "root", "scene",
    "data", "type", "info", "blueprint", "graph", "variable", "property",
    "with", "from", "this", "that", "not", "can", "will",
    "num", "all", "any", "each", "per", "use", "used",
    "double", "vector", "rotator", "transform",
    "override", "enabled", "disabled", "received",
    # Single-letter noise and UE boilerplate identifiers
    "ii", "ue", "ga", "bt",
    # Words that appear everywhere and carry minimal search signal
    "user", "parent", "base",
])

# Boolean prefixes to strip
BOOL_PREFIXES = ("bis", "bhas", "bcan", "bshould", "bwants", "bneeds", "ballow", "benable")

# UE function name to semantic domain mapping
FUNCTION_DOMAIN_MAP = {
    # Damage
    "applydamage": "damage",
    "applypointdamage": "damage",
    "applyradialdamage": "damage",
    "takedamage": "damage",
    "receiveanydam": "damage",
    "receivepointdam": "damage",
    "receiveradialdam": "damage",
    "damagehealth": "damage",
    "calculatedamage": "damage",
    "dodamage": "damage",
    # Animation
    "playanimmontage": "animation",
    "playanimation": "animation",
    "playslotanimation": "animation",
    "montage": "animation",
    "stopmontage": "animation",
    "getaniminstance": "animation",
    "setanim": "animation",
    # Spawning
    "spawnactor": "spawning",
    "spawnactorfromclass": "spawning",
    "spawnemitter": "spawning",
    "spawneffect": "spawning",
    "spawnsound": "spawning",
    "spawnparticle": "spawning",
    "destroyactor": "spawning",
    "destroycomponent": "spawning",
    # Movement & positioning
    "setactorlocation": "movement",
    "setworldlocation": "movement",
    "setrelativelocation": "movement",
    "setactorrotation": "movement",
    "addmovementinput": "movement",
    "launchcharacter": "movement",
    "setmovementmode": "movement",
    "getvelocity": "movement",
    "getactorlocation": "movement",
    "movetoward": "movement",
    "interpolate": "movement",
    "lerp": "movement",
    "teleport": "movement",
    # Player
    "getplayercharacter": "player",
    "getplayercontroller": "player",
    "getplayerpawn": "player",
    "getplayercameramanager": "player",
    # Timer & scheduling
    "settimer": "timer",
    "cleartimer": "timer",
    "settimerbyfunctionname": "timer",
    "k2_settimer": "timer",
    "k2_cleartimer": "timer",
    "delay": "timer",
    "retriggerabledelay": "timer",
    # Branching & logic
    "branch": "conditional",
    "select": "conditional",
    "switch": "conditional",
    "sequence": "flow",
    "doonce": "flow",
    "flipflop": "flow",
    "gate": "flow",
    # Loops
    "foreachloop": "iteration",
    "forloop": "iteration",
    "whileloop": "iteration",
    # Arrays & collections
    "makearray": "collection",
    "array_add": "collection",
    "array_remove": "collection",
    "array_contains": "collection",
    "array_find": "collection",
    "array_length": "collection",
    "array_get": "collection",
    # Debug
    "printstring": "debug",
    "printtext": "debug",
    "drawdebug": "debug",
    # UI / Widget
    "createwidget": "widget",
    "addtoviewport": "widget",
    "removefromparent": "widget",
    "setvisibility": "visual",
    # Audio
    "playsound": "audio",
    "spawnsound": "audio",
    "playsoundatlocatio": "audio",
    "playsound2d": "audio",
    "stopsound": "audio",
    # Visual / rendering
    "setmaterial": "visual",
    "setstaticmesh": "visual",
    "setskeletalmesh": "visual",
    "setcolor": "visual",
    "setopacity": "visual",
    # Actor queries
    "getactorofclass": "query",
    "getallactorsofclass": "query",
    "getallactorswithtag": "query",
    "getallactorsofclasswithtag": "query",
    # Traces & collision
    "linetrace": "trace",
    "spheretrace": "trace",
    "boxtrace": "trace",
    "capsultrace": "trace",
    "linetracesingle": "trace",
    "linetracebychannel": "trace",
    "sphereoverlap": "trace",
    # Physics
    "setphysicssimulat": "physics",
    "addforce": "physics",
    "addimpulse": "physics",
    "setcollision": "physics",
    "setcollisionenabled": "physics",
    "setsimulate": "physics",
    # Persistence
    "savegame": "persistence",
    "loadgame": "persistence",
    "doesslotexist": "persistence",
    "createsaveobject": "persistence",
    # AI & navigation
    "aimoveto": "navigation",
    "movetolocation": "navigation",
    "movetoactor": "navigation",
    "getblackboard": "ai",
    "setvalueasenum": "ai",
    "setvalueasobject": "ai",
    "getvalueasobject": "ai",
    "runbehaviortree": "ai",
    "useblackboard": "ai",
    # Input
    "getinputaxis": "input",
    "getinputaction": "input",
    "enableinput": "input",
    "disableinput": "input",
    # Math (only tag when it is the dominant activity)
    "clamp": "math",
    "normalize": "math",
    "dotproduct": "math",
    "crossproduct": "math",
    "distance": "math",
    # Camera
    "setviewtarget": "camera",
    "getcamera": "camera",
    "setfieldofview": "camera",
    # Inventory / equipment (combat-framework-specific)
    "equip": "equipment",
    "unequip": "equipment",
    "additem": "inventory",
    "removeitem": "inventory",
    # Combat-framework-specific
    "combo": "combo",
    "block": "blocking",
    "parry": "parry",
    "dodge": "dodge",
    "assassination": "assassination",
    "stealth": "stealth",
    "loot": "loot",
    "interact": "interaction",
    "lock": "targeting",
    "lockon": "targeting",
    "target": "targeting",
    "ability": "ability",
    "spell": "ability",
    "buff": "status",
    "debuff": "status",
    "statuseffect": "status",
    "potion": "consumable",
    "heal": "healing",
    "revive": "healing",
    "stamina": "stamina",
    "mana": "mana",
    "ragdoll": "ragdoll",
    "climb": "climbing",
    "swim": "swimming",
    "crouch": "crouch",
    "sprint": "sprint",
}


# ---------------------------------------------------------------------------
# Utility: CamelCase splitting
# ---------------------------------------------------------------------------

_CAMEL_RE = re.compile(r"""
    (?<=[a-z])(?=[A-Z])        # aB  -> a B
  | (?<=[A-Z])(?=[A-Z][a-z])   # ABc -> A Bc
  | (?<=[A-Za-z])(?=[0-9])     # a1  -> a 1
  | (?<=[0-9])(?=[A-Za-z])     # 1a  -> 1 a
  | [_\-\s]+                   # underscores/hyphens/spaces
""", re.VERBOSE)


def split_camel(text: str) -> list[str]:
    """Split a CamelCase or snake_case identifier into lowercase words."""
    if not text:
        return []
    parts = _CAMEL_RE.split(text)
    words = []
    for part in parts:
        part = part.strip()
        if part:
            words.append(part.lower())
    return words


def strip_bool_prefix(word: str) -> str:
    """Strip boolean prefixes like bIs, bHas, bCan from variable names."""
    lower = word.lower()
    for prefix in BOOL_PREFIXES:
        if lower.startswith(prefix) and len(lower) > len(prefix):
            return word[len(prefix):]
    return word


def clean_class_suffix(name: str) -> str:
    """Remove _C suffix from Blueprint class names."""
    if name.endswith("_C"):
        return name[:-2]
    return name


def words_to_tags(words: list[str]) -> list[str]:
    """Filter a list of words into valid tag candidates."""
    tags = []
    for w in words:
        # Strip non-alphanumeric characters (handles things like "dead?" or "var#")
        w = re.sub(r"[^a-zA-Z0-9]", "", w).lower().strip()
        if not w or len(w) < 2 or w in STOP_WORDS:
            continue
        # Remove numeric-only words
        if w.isdigit():
            continue
        tags.append(w)
    return tags


def dedupe_preserve_order(items: list[str]) -> list[str]:
    """Deduplicate while preserving insertion order."""
    seen = set()
    result = []
    for item in items:
        if item not in seen:
            seen.add(item)
            result.append(item)
    return result


# ---------------------------------------------------------------------------
# Domain detection from function names
# ---------------------------------------------------------------------------

def detect_domains_from_function(func_name: str) -> set[str]:
    """Map a UE function name to semantic domain tags."""
    domains = set()
    lower = func_name.lower()
    # Remove common prefixes for matching
    for prefix in ("k2_", "bp_", "ue_"):
        if lower.startswith(prefix):
            lower = lower[len(prefix):]

    for pattern, domain in FUNCTION_DOMAIN_MAP.items():
        if pattern in lower:
            domains.add(domain)

    return domains


def detect_domains_from_cast(title: str) -> set[str]:
    """Extract domain hints from Cast node titles like 'Cast To Character'."""
    domains = set()
    # Extract the target class name
    match = re.match(r"Cast\s+To\s+(.+)", title, re.IGNORECASE)
    if match:
        target = match.group(1).strip()
        target_words = split_camel(clean_class_suffix(target))
        filtered = words_to_tags(target_words)
        domains.update(filtered)
    return domains


# ---------------------------------------------------------------------------
# Node scanning: extract semantic signals from graph nodes
# ---------------------------------------------------------------------------

def scan_nodes(nodes: list[dict]) -> dict:
    """
    Scan a list of graph nodes and extract semantic information.

    Returns dict with keys:
        called_functions: list[str]  -- functions called
        variables_used: list[str]    -- variables get/set
        domains: set[str]            -- detected semantic domains
        events: list[dict]           -- event entry points (name, type)
        cast_targets: list[str]      -- cast target classes
    """
    called_functions = []
    variables_used = []
    domains = set()
    events = []
    cast_targets = []

    for node in nodes:
        node_type = node.get("type", "")

        if node_type == "CallFunction":
            func = node.get("function", "")
            if func:
                called_functions.append(func)
                domains.update(detect_domains_from_function(func))

        elif node_type in ("VariableGet", "VariableSet"):
            var = node.get("variable", "")
            if var:
                variables_used.append(var)

        elif node_type == "Cast":
            title = node.get("title", "")
            if title:
                domains.update(detect_domains_from_cast(title))
                match = re.match(r"Cast\s+To\s+(.+)", title, re.IGNORECASE)
                if match:
                    cast_targets.append(match.group(1).strip())

        elif node_type in ("Event", "CustomEvent"):
            func = node.get("function", "")
            if func:
                events.append({"name": func, "type": node_type})
                # Also extract domains from event names
                domains.update(detect_domains_from_function(func))

        elif node_type == "K2Node_DynamicCast":
            title = node.get("title", "")
            if title:
                domains.update(detect_domains_from_cast(title))

    return {
        "called_functions": called_functions,
        "variables_used": variables_used,
        "domains": domains,
        "events": events,
        "cast_targets": cast_targets,
    }


# ---------------------------------------------------------------------------
# Tag generation: blueprint-level
# ---------------------------------------------------------------------------

def generate_blueprint_tags(data: dict) -> tuple[list[str], set[str]]:
    """
    Generate blueprint-level tags from all available signals.

    Returns (tags_list, domains_set).
    """
    tags = []
    all_domains = set()

    # 1. Blueprint name words
    display_name = data.get("display_name", data.get("name", ""))
    name_words = split_camel(display_name)
    # Strip common prefixes like BP_, AN_, AM_
    prefix_stripped = [w for w in name_words if w not in ("bp", "an", "am", "ga", "cr", "ui", "w", "wbp")]
    tags.extend(words_to_tags(prefix_stripped))

    # 2. Parent class name
    parent_class = data.get("parent_class", {})
    if isinstance(parent_class, dict):
        parent_name = parent_class.get("name", "")
    else:
        parent_name = str(parent_class)
    if parent_name:
        parent_words = split_camel(clean_class_suffix(parent_name))
        tags.extend(words_to_tags(parent_words))

    # 3. Variable names and categories
    variables = data.get("variables", [])
    if isinstance(variables, list):
        var_categories = set()
        for var in variables:
            if isinstance(var, dict):
                var_name = var.get("name", "")
                stripped = strip_bool_prefix(var_name)
                var_words = split_camel(stripped)
                tags.extend(words_to_tags(var_words))

                cat = var.get("category", "")
                if cat and cat.lower() not in ("default", ""):
                    cat_words = split_camel(cat)
                    filtered = words_to_tags(cat_words)
                    var_categories.update(filtered)
        tags.extend(sorted(var_categories))

    # 4. Component names
    components = data.get("components", {})
    if isinstance(components, dict):
        tree = components.get("tree", [])
        if isinstance(tree, list):
            for comp in tree:
                if isinstance(comp, dict):
                    comp_name = comp.get("name", "")
                    comp_class = comp.get("class", "")
                    comp_words = split_camel(clean_class_suffix(comp_name))
                    tags.extend(words_to_tags(comp_words))
                    class_words = split_camel(clean_class_suffix(comp_class))
                    tags.extend(words_to_tags(class_words))

    # 5. Interface names
    interfaces = data.get("interfaces", [])
    if isinstance(interfaces, list):
        for iface in interfaces:
            if isinstance(iface, dict):
                iface_name = iface.get("name", "")
                # Strip I_ prefix and _C suffix
                cleaned = clean_class_suffix(iface_name)
                if cleaned.startswith("I_"):
                    cleaned = cleaned[2:]
                iface_words = split_camel(cleaned)
                tags.extend(words_to_tags(iface_words))

    # 6. Event dispatcher names
    event_dispatchers = data.get("event_dispatchers", [])
    if isinstance(event_dispatchers, list):
        for ed in event_dispatchers:
            if isinstance(ed, dict):
                ed_name = ed.get("name", "")
                ed_words = split_camel(ed_name)
                tags.extend(words_to_tags(ed_words))

    # 7. Function names (richest source)
    graphs = data.get("graphs", {})
    all_graphs = []
    if isinstance(graphs, dict):
        all_graphs.extend(graphs.get("event_graphs", []))
        all_graphs.extend(graphs.get("functions", []))
        all_graphs.extend(graphs.get("macros", []))
    elif isinstance(graphs, list):
        all_graphs = graphs

    for graph in all_graphs:
        if not isinstance(graph, dict):
            continue
        graph_name = graph.get("name", "")
        if graph_name.lower() in ("eventgraph", "executeubergraph", "ubergraph"):
            continue
        fn_words = split_camel(graph_name)
        tags.extend(words_to_tags(fn_words))

    # 8. Scan all nodes for semantic domains
    for graph in all_graphs:
        if not isinstance(graph, dict):
            continue
        nodes = graph.get("nodes", [])
        scan_result = scan_nodes(nodes)
        all_domains.update(scan_result["domains"])

        # Called function names also yield words
        for func in scan_result["called_functions"]:
            func_words = split_camel(func)
            # Only take meaningful function name words (skip math/utility noise)
            filtered = words_to_tags(func_words)
            # Don't add ALL function word fragments -- just domain tags
            pass

        # Variables used
        for var in scan_result["variables_used"]:
            stripped = strip_bool_prefix(var)
            var_words = split_camel(stripped)
            tags.extend(words_to_tags(var_words))

    # Add domain tags
    tags.extend(sorted(all_domains))

    # Filter and deduplicate
    tags = dedupe_preserve_order(tags)

    # Trim to limits
    if len(tags) > MAX_BLUEPRINT_TAGS:
        tags = tags[:MAX_BLUEPRINT_TAGS]

    return tags, all_domains


def build_catalog_description(data: dict, domains: set[str], parent_info: str | None = None) -> str:
    """Build a catalog_description string for a blueprint."""
    display_name = data.get("display_name", data.get("name", ""))

    parent_class = data.get("parent_class", {})
    if isinstance(parent_class, dict):
        parent_name = parent_class.get("name", "")
    else:
        parent_name = str(parent_class)

    # Count functions
    graphs = data.get("graphs", {})
    fn_count = 0
    if isinstance(graphs, dict):
        fn_count = len(graphs.get("functions", []))
    elif isinstance(graphs, list):
        fn_count = len([g for g in graphs if isinstance(g, dict) and g.get("graph_type") == "Function"])

    # Build domain summary
    # Prioritize interesting domains over generic ones
    priority_domains = [
        "damage", "animation", "combat", "movement", "spawning", "equipment",
        "inventory", "ability", "healing", "targeting", "combo", "blocking",
        "dodge", "assassination", "climbing", "navigation", "audio", "widget",
        "timer", "trace", "physics", "persistence", "interaction", "ragdoll",
        "stamina", "mana", "stealth", "loot", "status", "visual", "camera",
        "player", "ai", "input", "sprint", "crouch", "swimming", "parry",
        "consumable",
    ]
    sorted_domains = []
    for d in priority_domains:
        if d in domains:
            sorted_domains.append(d)
    # Add remaining
    for d in sorted(domains):
        if d not in sorted_domains:
            sorted_domains.append(d)

    domain_summary = ""
    if sorted_domains:
        top = sorted_domains[:4]
        if len(top) == 1:
            domain_summary = f"Handles {top[0]}."
        elif len(top) == 2:
            domain_summary = f"Handles {top[0]} and {top[1]}."
        else:
            domain_summary = f"Handles {', '.join(top[:-1])}, and {top[-1]}."

    # Build the description
    parent_part = f"{clean_class_suffix(parent_name)}-based"
    if parent_info:
        parent_part = f"{clean_class_suffix(parent_name)}-based (child of {parent_info})"

    fn_part = f"with {fn_count} function{'s' if fn_count != 1 else ''}" if fn_count > 0 else "with no functions"

    desc = f"{display_name}: {parent_part} blueprint {fn_part}."
    if domain_summary:
        desc += f" {domain_summary}"

    return desc


# ---------------------------------------------------------------------------
# Tag generation: function-level
# ---------------------------------------------------------------------------

def generate_function_tags(graph: dict) -> tuple[list[str], str]:
    """
    Generate tags and description for a single function graph.

    Returns (tags_list, description_string).
    """
    graph_name = graph.get("name", "")
    node_count = graph.get("node_count", len(graph.get("nodes", [])))

    # Skip stubs and ubergraph
    if node_count < MIN_FUNCTION_NODES:
        return [], ""
    if graph_name.lower() in ("executeubergraph",):
        return [], ""

    tags = []
    domains = set()

    # Function name words
    fn_words = split_camel(graph_name)
    tags.extend(words_to_tags(fn_words))

    # Scan nodes
    nodes = graph.get("nodes", [])
    scan_result = scan_nodes(nodes)

    # Called functions -- add domain tags
    domains.update(scan_result["domains"])
    for func in scan_result["called_functions"]:
        func_words = split_camel(func)
        filtered = words_to_tags(func_words)
        # Only add the most meaningful words (skip very common ones)
        for w in filtered:
            if len(w) >= 4:  # Skip very short words
                tags.append(w)

    # Variables used
    for var in scan_result["variables_used"]:
        stripped = strip_bool_prefix(var)
        var_words = split_camel(stripped)
        tags.extend(words_to_tags(var_words))

    # Add domain tags
    tags.extend(sorted(domains))

    # Deduplicate and limit
    tags = dedupe_preserve_order(tags)
    if len(tags) > MAX_FUNCTION_TAGS:
        tags = tags[:MAX_FUNCTION_TAGS]

    # Build description
    desc = build_function_description(graph_name, domains, scan_result)

    return tags, desc


def build_function_description(func_name: str, domains: set[str], scan_result: dict) -> str:
    """Build a one-sentence description for a function."""
    # Determine the primary verb from the function name
    words = split_camel(func_name)
    filtered = [w for w in words if w.lower() not in STOP_WORDS and len(w) > 1]

    # Pick a verb based on the function name or domains
    verb_map = {
        "calculate": "Calculates",
        "compute": "Computes",
        "update": "Updates",
        "apply": "Applies",
        "check": "Checks",
        "validate": "Validates",
        "initialize": "Initializes",
        "init": "Initializes",
        "setup": "Sets up",
        "reset": "Resets",
        "clear": "Clears",
        "remove": "Removes",
        "add": "Adds",
        "create": "Creates",
        "spawn": "Spawns",
        "destroy": "Destroys",
        "play": "Plays",
        "stop": "Stops",
        "start": "Starts",
        "end": "Ends",
        "finish": "Finishes",
        "begin": "Begins",
        "enable": "Enables",
        "disable": "Disables",
        "toggle": "Toggles",
        "fire": "Fires",
        "trigger": "Triggers",
        "handle": "Handles",
        "process": "Processes",
        "execute": "Executes",
        "run": "Runs",
        "load": "Loads",
        "save": "Saves",
        "store": "Stores",
        "restore": "Restores",
        "damage": "Applies damage",
        "heal": "Heals",
        "equip": "Equips",
        "unequip": "Unequips",
        "attack": "Performs attack",
        "block": "Handles blocking",
        "dodge": "Handles dodge",
        "cast": "Casts",
        "activate": "Activates",
        "deactivate": "Deactivates",
        "release": "Releases",
        "grab": "Grabs",
        "drop": "Drops",
        "pick": "Picks up",
        "interact": "Handles interaction",
        "notify": "Notifies",
        "receive": "Receives",
        "send": "Sends",
        "show": "Shows",
        "hide": "Hides",
        "display": "Displays",
        "refresh": "Refreshes",
        "sync": "Synchronizes",
        "freeze": "Freezes",
        "thaw": "Thaws",
        "mount": "Mounts",
        "dismount": "Dismounts",
        "climb": "Handles climbing",
        "jump": "Handles jumping",
        "land": "Handles landing",
        "fall": "Handles falling",
        "swim": "Handles swimming",
        "crouch": "Handles crouching",
        "sprint": "Handles sprinting",
        "aim": "Handles aiming",
        "shoot": "Shoots",
        "reload": "Reloads",
        "lunge": "Performs lunge",
    }

    verb = "Handles"
    if filtered:
        first_word = filtered[0].lower()
        if first_word in verb_map:
            verb = verb_map[first_word]

    # Build subject from remaining words + domains
    subject_words = filtered[1:] if len(filtered) > 1 else filtered
    domain_list = sorted(domains)

    # Combine into description
    subject_parts = []
    for w in subject_words[:3]:
        subject_parts.append(w.lower())
    if domain_list:
        for d in domain_list[:2]:
            if d not in subject_parts:
                subject_parts.append(d)

    if not subject_parts:
        subject_parts = ["logic"]

    subject = " ".join(subject_parts)
    desc = f"{func_name}: {verb} {subject}."

    # Keep it 10-20 words
    word_count = len(desc.split())
    if word_count < 10 and scan_result.get("called_functions"):
        # Add some called function context
        top_calls = []
        for fn in scan_result["called_functions"][:3]:
            fn_simple = fn.split("::")[-1] if "::" in fn else fn
            domains_from_call = detect_domains_from_function(fn_simple)
            if domains_from_call:
                top_calls.extend(domains_from_call)
        if top_calls:
            extras = dedupe_preserve_order(top_calls)[:2]
            extras = [e for e in extras if e not in subject_parts]
            if extras:
                desc = desc[:-1] + f" using {' and '.join(extras)}."

    return desc


# ---------------------------------------------------------------------------
# Event graph entry points
# ---------------------------------------------------------------------------

def extract_entry_points(graph: dict) -> list[dict]:
    """Extract event entry points from an event graph."""
    nodes = graph.get("nodes", [])
    entry_points = []

    for node in nodes:
        node_type = node.get("type", "")
        if node_type in ("Event", "CustomEvent"):
            func = node.get("function", "")
            if not func:
                continue

            # Generate tags for the entry point
            ep_tags = []
            ep_words = split_camel(func)
            ep_tags.extend(words_to_tags(ep_words))

            # Detect domains from the event name
            domains = detect_domains_from_function(func)
            ep_tags.extend(sorted(domains))

            ep_tags = dedupe_preserve_order(ep_tags)
            if len(ep_tags) > MAX_FUNCTION_TAGS:
                ep_tags = ep_tags[:MAX_FUNCTION_TAGS]

            entry_points.append({
                "name": func,
                "type": node_type,
                "tags": " ".join(ep_tags),
            })

    return entry_points


# ---------------------------------------------------------------------------
# Event dispatcher tagging
# ---------------------------------------------------------------------------

def tag_event_dispatchers(dispatchers: list[dict]) -> list[dict]:
    """Add tags to event dispatcher entries."""
    result = []
    for ed in dispatchers:
        if not isinstance(ed, dict):
            result.append(ed)
            continue
        ed_name = ed.get("name", "")
        ed_words = split_camel(ed_name)
        ed_tags = words_to_tags(ed_words)

        # Detect domains
        domains = detect_domains_from_function(ed_name)
        ed_tags.extend(sorted(domains))

        ed_tags = dedupe_preserve_order(ed_tags)

        new_ed = OrderedDict(ed)
        new_ed["tags"] = " ".join(ed_tags)
        result.append(new_ed)

    return result


# ---------------------------------------------------------------------------
# Ordered dict insertion helper
# ---------------------------------------------------------------------------

def ordered_insert(d: dict, after_key: str | None, key: str, value) -> OrderedDict:
    """
    Return a new OrderedDict with `key: value` inserted after `after_key`.
    If after_key is None or not found, append at end.
    """
    result = OrderedDict()
    inserted = False
    for k, v in d.items():
        if k == key:
            continue  # Skip existing key at old position; we will reinsert
        result[k] = v
        if k == after_key and not inserted:
            result[key] = value
            inserted = True
    if not inserted:
        result[key] = value
    return result


def insert_multiple(d: dict, insertions: list[tuple[str | None, str, object]]) -> OrderedDict:
    """
    Insert multiple key-value pairs into an OrderedDict.
    Each insertion is (after_key, key, value).
    """
    result = OrderedDict(d)
    for after_key, key, value in insertions:
        result = ordered_insert(result, after_key, key, value)
    return result


# ---------------------------------------------------------------------------
# Main processing
# ---------------------------------------------------------------------------

def process_file(filepath: str, manifest_entry: dict, tag_cache: dict[str, list[str]]) -> dict:
    """
    Process a single Blueprint JSON file, adding tags and descriptions.

    Returns a summary dict with stats.
    """
    with open(filepath, "r", encoding="utf-8") as f:
        data = json.load(f)

    template_id = data.get("template_id", manifest_entry.get("template_id", ""))
    depends_on = manifest_entry.get("depends_on")

    # ------------------------------------------------------------------
    # 1. Blueprint-level tags and catalog_description
    # ------------------------------------------------------------------
    bp_tags, domains = generate_blueprint_tags(data)

    # Handle inheritance
    inherited_tags_str = ""
    parent_display = None
    if depends_on and depends_on in tag_cache:
        parent_tags = tag_cache[depends_on]
        parent_tags_set = set(parent_tags)

        # Only keep tags that are NOT in parent
        own_tags = [t for t in bp_tags if t not in parent_tags_set]

        # Ensure minimum tags even if parent covers most
        if len(own_tags) < MIN_BLUEPRINT_TAGS and bp_tags:
            # Add back some that are most relevant to this specific blueprint
            for t in bp_tags:
                if t not in own_tags:
                    own_tags.append(t)
                if len(own_tags) >= MIN_BLUEPRINT_TAGS:
                    break

        inherited_tags_str = " ".join(parent_tags)
        bp_tags = own_tags
        parent_display = manifest_entry.get("parent_class", depends_on)

    # Ensure minimum tags
    if len(bp_tags) < MIN_BLUEPRINT_TAGS:
        # Pull from display name if we are short
        display_name = data.get("display_name", data.get("name", ""))
        extra = words_to_tags(split_camel(display_name))
        for t in extra:
            if t not in bp_tags:
                bp_tags.append(t)
            if len(bp_tags) >= MIN_BLUEPRINT_TAGS:
                break

    bp_tags = dedupe_preserve_order(bp_tags)
    if len(bp_tags) > MAX_BLUEPRINT_TAGS:
        bp_tags = bp_tags[:MAX_BLUEPRINT_TAGS]

    catalog_desc = build_catalog_description(data, domains, parent_display)

    # Store complete tags for children (own + inherited)
    complete_tags = list(bp_tags)
    if inherited_tags_str:
        for t in inherited_tags_str.split():
            if t not in complete_tags:
                complete_tags.append(t)
    tag_cache[template_id] = complete_tags

    # ------------------------------------------------------------------
    # 2. Function-level tags and description
    # ------------------------------------------------------------------
    graphs = data.get("graphs", {})
    all_function_graphs = []
    all_event_graphs = []

    if isinstance(graphs, dict):
        all_event_graphs = graphs.get("event_graphs", [])
        all_function_graphs = graphs.get("functions", [])
    elif isinstance(graphs, list):
        for g in graphs:
            if isinstance(g, dict):
                if g.get("graph_type") == "EventGraph":
                    all_event_graphs.append(g)
                else:
                    all_function_graphs.append(g)

    fn_tags_count = 0
    for graph in all_function_graphs:
        if not isinstance(graph, dict):
            continue
        graph_name = graph.get("name", "")
        node_count = graph.get("node_count", len(graph.get("nodes", [])))

        if node_count < MIN_FUNCTION_NODES or graph_name.lower() == "executeubergraph":
            continue

        fn_tags, fn_desc = generate_function_tags(graph)
        if fn_tags:
            graph["tags"] = " ".join(fn_tags)
            fn_tags_count += len(fn_tags)
        if fn_desc:
            graph["description"] = fn_desc

    # ------------------------------------------------------------------
    # 3. Event graph entry_points
    # ------------------------------------------------------------------
    for eg in all_event_graphs:
        if not isinstance(eg, dict):
            continue
        entry_points = extract_entry_points(eg)
        if entry_points:
            eg["entry_points"] = entry_points
            # Event graph tags = union of all entry point tags
            all_ep_tags = []
            for ep in entry_points:
                ep_tag_str = ep.get("tags", "")
                all_ep_tags.extend(ep_tag_str.split())
            all_ep_tags = dedupe_preserve_order(all_ep_tags)
            if len(all_ep_tags) > MAX_FUNCTION_TAGS:
                all_ep_tags = all_ep_tags[:MAX_FUNCTION_TAGS]
            eg["tags"] = " ".join(all_ep_tags)

    # ------------------------------------------------------------------
    # 4. Event dispatcher tagging
    # ------------------------------------------------------------------
    event_dispatchers = data.get("event_dispatchers", [])
    if isinstance(event_dispatchers, list) and event_dispatchers:
        data["event_dispatchers"] = tag_event_dispatchers(event_dispatchers)

    # ------------------------------------------------------------------
    # 5. Insert new fields into the data in proper order
    # ------------------------------------------------------------------
    tags_str = " ".join(bp_tags)

    # Insert tags after display_name, catalog_description after tags,
    # inherited_tags after catalog_description (if applicable)
    insertions = [
        ("display_name", "tags", tags_str),
        ("tags", "catalog_description", catalog_desc),
    ]
    if inherited_tags_str:
        insertions.append(("catalog_description", "inherited_tags", inherited_tags_str))

    data = insert_multiple(data, insertions)

    # ------------------------------------------------------------------
    # Write back
    # ------------------------------------------------------------------
    with open(filepath, "w", encoding="utf-8", newline="\n") as f:
        json.dump(data, f, indent=2, ensure_ascii=False)
        f.write("\n")

    return {
        "template_id": template_id,
        "tag_count": len(bp_tags),
        "fn_tags_count": fn_tags_count,
        "has_inherited": bool(inherited_tags_str),
    }


def main():
    start_time = time.time()

    # Load manifest
    print(f"Loading manifest from {MANIFEST_PATH}")
    with open(MANIFEST_PATH, "r", encoding="utf-8") as f:
        manifest = json.load(f)

    processing_order = manifest.get("processing_order", [])
    total_expected = manifest.get("total_files", len(processing_order))

    print(f"Found {len(processing_order)} entries in manifest (expected {total_expected})")
    print(f"Library directory: {LIBRARY_DIR}")
    print()

    # Process files
    tag_cache: dict[str, list[str]] = {}
    all_tags: list[str] = []
    total_processed = 0
    total_skipped = 0
    total_fn_tags = 0
    errors: list[str] = []

    for i, entry in enumerate(processing_order):
        template_id = entry.get("template_id", "")
        filename = f"{template_id}.json"
        filepath = os.path.join(LIBRARY_DIR, filename)

        if not os.path.exists(filepath):
            errors.append(f"File not found: {filepath}")
            total_skipped += 1
            continue

        try:
            result = process_file(filepath, entry, tag_cache)
            total_processed += 1
            all_tags.extend(tag_cache.get(template_id, []))
            total_fn_tags += result["fn_tags_count"]

            # Progress indicator every 50 files
            if (i + 1) % 50 == 0:
                print(f"  Processed {i + 1}/{len(processing_order)} files...")

        except Exception as e:
            errors.append(f"Error processing {template_id}: {e}")
            total_skipped += 1

    elapsed = time.time() - start_time

    # ------------------------------------------------------------------
    # Summary
    # ------------------------------------------------------------------
    print()
    print("=" * 60)
    print("  AUTO-TAGGER SUMMARY")
    print("=" * 60)
    print(f"  Total files processed:    {total_processed}")
    print(f"  Total files skipped:      {total_skipped}")
    print(f"  Total blueprint tags:     {len(all_tags)}")
    print(f"  Total function tags:      {total_fn_tags}")
    if total_processed > 0:
        print(f"  Avg tags per blueprint:   {len(all_tags) / total_processed:.1f}")
    print(f"  Processing time:          {elapsed:.2f}s")
    print()

    # Top 20 most common tags
    tag_counter = Counter(all_tags)
    print("  Top 20 most common tags:")
    for tag, count in tag_counter.most_common(20):
        print(f"    {tag:<25s} {count:>4d}")
    print()

    # Errors
    if errors:
        print(f"  ERRORS ({len(errors)}):")
        for err in errors:
            print(f"    - {err}")
    else:
        print("  No errors.")

    print()
    print("=" * 60)

    return 0 if not errors else 1


if __name__ == "__main__":
    sys.exit(main())
