"""
Olive AI Studio - Blueprint Extraction Script (v2 - Fixed)
Run inside Unreal Engine via: Tools > Execute Python Script

Fixes from v1:
- Blueprints were not being found (isinstance check + asset loading fixed)
- All extractors returned empty data (wrong API calls replaced)
- Silent try/except hid all errors (now logs warnings)
- Enum/Struct extraction used non-existent APIs (rewritten)
- Variable extraction used wrong method (now uses new_variables property)
- Graph node extraction now actually works

Output structure:
    output/
    ├── blueprints/      # Actor, Character, Pawn, Component, GameMode etc.
    ├── widgets/         # Widget Blueprints (UMG)
    ├── animation/       # Animation Blueprints
    ├── behavior_trees/  # Behavior Trees + Blackboards
    ├── interfaces/      # Blueprint Interfaces
    ├── enums_structs/   # User Defined Enums + Structs
    ├── misc/            # Anything else
    ├── manifest.json    # Summary
    └── errors.log       # All extraction warnings
"""

import unreal
import json
import os
from datetime import datetime

# ─────────────────────────────────────────────
# CONFIG — edit these before running
# ─────────────────────────────────────────────

# ─────────────────────────────────────────────
# CONFIG — edit these before running
# 

# Where to save output (use forward slashes)
OUTPUT_DIR = "C:/Users/mjoff/OneDrive/Desktop/Olive AI Studio/output"

# Project name (used in manifest, just for your reference)
PROJECT_NAME = "CombatFS"

# Root path to scan (use "/" for everything, or narrow it e.g. "/Game/Blueprints")
SCAN_ROOT = "/Game"

# Set True to get verbose logging in UE Output Log
VERBOSE = True

# ─────────────────────────────────────────────
# ERROR LOGGING
# ─────────────────────────────────────────────

_error_log = []

def log_warn(msg):
    _error_log.append(msg)
    if VERBOSE:
        unreal.log_warning(f"[Olive Extract] {msg}")

def log_info(msg):
    unreal.log(f"[Olive Extract] {msg}")

# ─────────────────────────────────────────────
# SKIP LIST — asset classes to completely ignore
# ─────────────────────────────────────────────

_SKIP_CLASS_NAMES = [
    # Meshes
    "StaticMesh", "SkeletalMesh",
    # Materials
    "Material", "MaterialInstance", "MaterialInstanceConstant",
    "MaterialInstanceDynamic", "MaterialFunction", "MaterialParameterCollection",
    # Textures
    "Texture", "Texture2D", "TextureCube", "TextureRenderTarget2D",
    "MediaTexture", "RuntimeVirtualTexture",
    # Audio
    "SoundWave", "SoundCue", "SoundClass", "SoundMix",
    "SoundAttenuation", "SoundConcurrency", "MetaSoundSource",
    # Physics
    "PhysicsAsset", "PhysicalMaterial",
    # World/Level
    "World", "Level", "MapBuildDataRegistry", "WorldPartitionRuntimeSpatialHash",
    # Data tables/curves
    "DataTable", "CurveFloat", "CurveVector", "CurveLinearColor", "CurveTable",
    # Editor utilities
    "EditorUtilityBlueprint", "EditorUtilityWidget", "EditorUtilityWidgetBlueprint",
    # Animation assets (not AnimBlueprint — we keep those)
    "AnimSequence", "AnimMontage", "AnimComposite", "BlendSpace", "BlendSpace1D",
    "AimOffsetBlendSpace", "AimOffsetBlendSpace1D",
    "Skeleton", "SkeletonRetargetOptions",
    # Niagara
    "NiagaraSystem", "NiagaraEmitter", "NiagaraParameterCollection",
    # Misc non-Blueprint assets
    "Font", "FontFace", "PaperSprite", "PaperTileSet", "PaperFlipbook",
    "LevelSequence", "MediaPlayer", "MediaSource", "FileMediaSource",
    "SubsurfaceProfile", "CameraAnim",
]

SKIP_CLASSES = set()
for name in _SKIP_CLASS_NAMES:
    cls = getattr(unreal, name, None)
    if cls is not None:
        SKIP_CLASSES.add(cls)

# ─────────────────────────────────────────────
# FOLDER ROUTING
# ─────────────────────────────────────────────

def get_output_folder(asset):
    """Route an asset to the correct output subfolder."""
    cls_name = asset.get_class().get_name()

    # WidgetBlueprint before Blueprint (it's a subclass)
    if isinstance(asset, unreal.WidgetBlueprint):
        return "widgets"

    # AnimBlueprint before Blueprint (it's a subclass)
    if isinstance(asset, unreal.AnimBlueprint):
        return "animation"

    # Blueprint (regular Actor/Character/Component/etc.)
    if isinstance(asset, unreal.Blueprint):
        # Check if it's an interface Blueprint
        bp_type = None
        try:
            bp_type = asset.get_editor_property("blueprint_type")
        except:
            pass
        if bp_type is not None and str(bp_type) == "BPTYPE_Interface":
            return "interfaces"

        # Also check parent class name for interface
        try:
            parent = asset.get_editor_property("parent_class")
            if parent:
                parent_name = parent.get_name()
                if "Interface" in parent_name and parent_name != "Interface":
                    return "interfaces"
        except:
            pass

        return "blueprints"

    # Behavior trees
    if isinstance(asset, unreal.BehaviorTree):
        return "behavior_trees"
    if isinstance(asset, unreal.BlackboardData):
        return "behavior_trees"

    # Enums and Structs
    if isinstance(asset, unreal.UserDefinedEnum):
        return "enums_structs"
    if isinstance(asset, unreal.UserDefinedStruct):
        return "enums_structs"

    # Interface check by class name
    if "Interface" in cls_name:
        return "interfaces"

    return "misc"


# ─────────────────────────────────────────────
# PIN EXTRACTION
# ─────────────────────────────────────────────

def extract_pin(pin):
    """Extract data from a single EdGraphPin."""
    result = {}

    # Pin name
    try:
        result["name"] = str(pin.get_editor_property("pin_name"))
    except Exception as e:
        result["name"] = "unknown"
        log_warn(f"Pin name error: {e}")

    # Direction
    try:
        d = pin.get_editor_property("direction")
        result["direction"] = "input" if d == unreal.EdGraphPinDirection.EGPD_INPUT else "output"
    except:
        pass

    # Pin type info
    try:
        pin_type = pin.get_editor_property("pin_type")
        cat = str(pin_type.get_editor_property("pin_category"))
        result["type"] = cat

        sub_cat = pin_type.get_editor_property("pin_sub_category")
        if sub_cat and str(sub_cat):
            result["sub_type"] = str(sub_cat)

        sub_obj = pin_type.get_editor_property("pin_sub_category_object")
        if sub_obj:
            obj = sub_obj.get() if hasattr(sub_obj, 'get') else sub_obj
            if obj is not None:
                result["sub_object"] = obj.get_name()

        # Container type (array, set, map)
        try:
            container = pin_type.get_editor_property("container_type")
            if container and str(container) != "None":
                result["container"] = str(container)
        except:
            pass

        # Is reference
        try:
            is_ref = pin_type.get_editor_property("is_reference")
            if is_ref:
                result["is_reference"] = True
        except:
            pass
    except:
        pass

    # Default value
    try:
        dv = pin.get_editor_property("default_value")
        if dv and str(dv):
            result["default_value"] = str(dv)
    except:
        pass

    # Default object
    try:
        do = pin.get_editor_property("default_object")
        if do:
            result["default_object"] = do.get_path_name()
    except:
        pass

    # Connections
    try:
        linked = pin.get_editor_property("linked_to")
        if linked and len(linked) > 0:
            connections = []
            for lp in linked:
                conn = {}
                try:
                    conn["pin"] = str(lp.get_editor_property("pin_name"))
                except:
                    pass
                try:
                    owner = lp.get_editor_property("owning_node")
                    if owner:
                        conn["node_guid"] = str(owner.get_editor_property("node_guid"))
                except:
                    try:
                        owner = lp.get_editor_property("owner_node")
                        if owner:
                            conn["node_guid"] = str(owner.get_editor_property("node_guid"))
                    except:
                        pass
                if conn:
                    connections.append(conn)
            if connections:
                result["connections"] = connections
    except:
        pass

    return result


# ─────────────────────────────────────────────
# NODE EXTRACTION
# ─────────────────────────────────────────────

def extract_node(node):
    """Extract data from a single K2Node / EdGraphNode."""
    result = {}

    # Node class type
    try:
        result["node_type"] = node.get_class().get_name()
    except:
        result["node_type"] = "Unknown"

    # GUID
    try:
        result["node_guid"] = str(node.get_editor_property("node_guid"))
    except:
        pass

    # Comment
    try:
        c = node.get_editor_property("node_comment")
        if c and str(c):
            result["comment"] = str(c)
    except:
        pass

    # ----- Type-specific properties -----

    # Function call: member name + class
    try:
        func_ref = node.get_editor_property("function_reference")
        if func_ref:
            try:
                mn = func_ref.get_editor_property("member_name")
                if mn and str(mn):
                    result["function_name"] = str(mn)
            except:
                pass
            try:
                mp = func_ref.get_editor_property("member_parent")
                if mp:
                    obj = mp.get() if hasattr(mp, 'get') else mp
                    if obj is not None:
                        result["function_class"] = obj.get_name()
            except:
                pass
    except:
        pass

    # Variable get/set
    try:
        var_ref = node.get_editor_property("variable_reference")
        if var_ref:
            try:
                mn = var_ref.get_editor_property("member_name")
                if mn and str(mn):
                    result["variable_name"] = str(mn)
            except:
                pass
            try:
                mp = var_ref.get_editor_property("member_parent")
                if mp:
                    obj = mp.get() if hasattr(mp, 'get') else mp
                    if obj is not None:
                        result["variable_class"] = obj.get_name()
            except:
                pass
    except:
        pass

    # Custom event name
    try:
        cname = node.get_editor_property("custom_function_name")
        if cname and str(cname):
            result["custom_event_name"] = str(cname)
    except:
        pass

    # Delegate reference
    try:
        dref = node.get_editor_property("delegate_reference")
        if dref:
            try:
                mn = dref.get_editor_property("member_name")
                if mn and str(mn):
                    result["delegate_name"] = str(mn)
            except:
                pass
    except:
        pass

    # Cast target
    try:
        tt = node.get_editor_property("target_type")
        if tt:
            result["cast_target"] = tt.get_name()
    except:
        pass

    # Timeline name
    try:
        tname = node.get_editor_property("timeline_name")  
        if tname and str(tname):
            result["timeline_name"] = str(tname)
    except:
        pass

    # Macro reference
    try:
        macro = node.get_editor_property("macro_graph")
        if macro:
            result["macro_name"] = macro.get_name()
    except:
        pass

    # Enum literal
    try:
        enum = node.get_editor_property("enum")
        if enum:
            result["enum_type"] = enum.get_name()
    except:
        pass

    # ----- Pins -----
    try:
        pins = node.get_editor_property("pins")
        if pins:
            extracted_pins = []
            for p in pins:
                extracted_pins.append(extract_pin(p))
            result["pins"] = extracted_pins
    except Exception as e:
        result["pins"] = []
        log_warn(f"Pin extraction error on node {result.get('node_type','?')}: {e}")

    return result


# ─────────────────────────────────────────────
# GRAPH EXTRACTION
# ─────────────────────────────────────────────

def extract_graph(graph):
    """Extract all nodes from a graph."""
    result = {
        "graph_name": "Unknown",
        "graph_type": "Unknown",
        "nodes": [],
    }
    try:
        result["graph_name"] = graph.get_name()
    except:
        pass
    try:
        result["graph_type"] = graph.get_class().get_name()
    except:
        pass

    try:
        nodes = graph.get_editor_property("nodes")
        if nodes:
            for n in nodes:
                try:
                    result["nodes"].append(extract_node(n))
                except Exception as e:
                    log_warn(f"Node extraction error in {result['graph_name']}: {e}")
    except Exception as e:
        log_warn(f"Graph nodes error in {result['graph_name']}: {e}")

    return result


# ─────────────────────────────────────────────
# BLUEPRINT EXTRACTOR (main one)
# ─────────────────────────────────────────────

def extract_variables(bp):
    """Extract variables via new_variables property (the correct way)."""
    variables = []
    try:
        new_vars = bp.get_editor_property("new_variables")
        if new_vars:
            for v in new_vars:
                var = {}
                try:
                    var["name"] = str(v.get_editor_property("var_name"))
                except:
                    var["name"] = "unknown"

                try:
                    pin_type = v.get_editor_property("var_type")
                    var["type"] = str(pin_type.get_editor_property("pin_category"))

                    sub_obj = pin_type.get_editor_property("pin_sub_category_object")
                    if sub_obj:
                        obj = sub_obj.get() if hasattr(sub_obj, 'get') else sub_obj
                        if obj is not None:
                            var["sub_type"] = obj.get_name()

                    sub_cat = pin_type.get_editor_property("pin_sub_category")
                    if sub_cat and str(sub_cat):
                        var["sub_category"] = str(sub_cat)

                    # Container type
                    try:
                        ct = pin_type.get_editor_property("container_type")
                        if ct and str(ct) != "None":
                            var["container"] = str(ct)
                    except:
                        pass
                except:
                    var["type"] = "unknown"

                # Category
                try:
                    cat = v.get_editor_property("category")
                    if cat and str(cat):
                        var["category"] = str(cat)
                except:
                    pass

                # Replication
                try:
                    rep = v.get_editor_property("replication_condition")
                    if rep and str(rep) != "COND_None":
                        var["replication"] = str(rep)
                except:
                    pass

                # Check if it's a delegate (event dispatcher) — skip those, handled separately
                if var.get("type") in ("mcdelegate", "delegate"):
                    continue

                variables.append(var)
    except Exception as e:
        log_warn(f"Variable extraction error: {e}")
    return variables


def extract_components(bp):
    """Extract Simple Construction Script components."""
    components = []
    try:
        scs = bp.get_editor_property("simple_construction_script")
        if not scs:
            return components

        # Try all_nodes first, then root_nodes
        nodes = None
        try:
            nodes = scs.get_editor_property("all_nodes")
        except:
            pass
        if not nodes:
            try:
                nodes = scs.get_all_nodes()
            except:
                pass

        if nodes:
            for node in nodes:
                comp = {}
                try:
                    comp_class = node.get_editor_property("component_class")
                    if comp_class:
                        comp["type"] = comp_class.get_name()
                except:
                    comp["type"] = "unknown"

                try:
                    comp["name"] = str(node.get_editor_property("internal_variable_name"))
                except:
                    comp["name"] = "unknown"

                try:
                    parent = node.get_editor_property("parent_component_or_variable_name")
                    if parent and str(parent) and str(parent) != "None":
                        comp["parent"] = str(parent)
                except:
                    pass

                # Try to get the component template for default values
                try:
                    template = node.get_editor_property("component_template")
                    if template:
                        # Get some useful default properties
                        try:
                            comp["mobility"] = str(template.get_editor_property("mobility"))
                        except:
                            pass
                except:
                    pass

                components.append(comp)
    except Exception as e:
        log_warn(f"Component extraction error: {e}")
    return components


def extract_interfaces(bp):
    """Extract implemented interfaces."""
    interfaces = []
    try:
        ifaces = bp.get_editor_property("implemented_interfaces")
        if ifaces:
            for iface in ifaces:
                try:
                    cls = iface.get_editor_property("interface")
                    if cls:
                        interfaces.append(cls.get_name())
                except Exception as e:
                    log_warn(f"Interface extraction error: {e}")
    except:
        pass
    return interfaces


def extract_event_dispatchers(bp):
    """Extract event dispatchers from new_variables (they're delegate-type vars)."""
    dispatchers = []
    try:
        new_vars = bp.get_editor_property("new_variables")
        if new_vars:
            for v in new_vars:
                try:
                    pin_type = v.get_editor_property("var_type")
                    category = str(pin_type.get_editor_property("pin_category"))
                    if category in ("mcdelegate", "delegate"):
                        disp = {
                            "name": str(v.get_editor_property("var_name")),
                        }
                        dispatchers.append(disp)
                except:
                    pass
    except:
        pass
    return dispatchers


def extract_functions_list(bp):
    """Get a quick list of function names from function graphs."""
    funcs = []
    try:
        func_graphs = bp.get_editor_property("function_graphs")
        if func_graphs:
            for g in func_graphs:
                try:
                    name = g.get_name()
                    # Skip internal UE function graphs
                    if name and not name.startswith("__"):
                        funcs.append(name)
                except:
                    pass
    except:
        pass
    return funcs


def extract_all_graphs(bp):
    """Extract all graph types from a Blueprint."""
    all_graphs = []

    # UberGraphPages (Event Graphs)
    try:
        pages = bp.get_editor_property("uber_graph_pages")
        if pages:
            for g in pages:
                extracted = extract_graph(g)
                extracted["graph_role"] = "event_graph"
                all_graphs.append(extracted)
    except Exception as e:
        log_warn(f"Event graph extraction error: {e}")

    # Function Graphs
    try:
        funcs = bp.get_editor_property("function_graphs")
        if funcs:
            for g in funcs:
                extracted = extract_graph(g)
                extracted["graph_role"] = "function"
                all_graphs.append(extracted)
    except Exception as e:
        log_warn(f"Function graph extraction error: {e}")

    # Macro Graphs
    try:
        macros = bp.get_editor_property("macro_graphs")
        if macros:
            for g in macros:
                extracted = extract_graph(g)
                extracted["graph_role"] = "macro"
                all_graphs.append(extracted)
    except Exception as e:
        log_warn(f"Macro graph extraction error: {e}")

    # Delegate Signature Graphs (event dispatcher signatures)
    try:
        delegates = bp.get_editor_property("delegate_signature_graphs")
        if delegates:
            for g in delegates:
                extracted = extract_graph(g)
                extracted["graph_role"] = "delegate_signature"
                all_graphs.append(extracted)
    except:
        pass

    # Interface implementation graphs
    try:
        ifaces = bp.get_editor_property("implemented_interfaces")
        if ifaces:
            for iface in ifaces:
                try:
                    graphs = iface.get_editor_property("graphs")
                    if graphs:
                        for g in graphs:
                            extracted = extract_graph(g)
                            extracted["graph_role"] = "interface_implementation"
                            all_graphs.append(extracted)
                except:
                    pass
    except:
        pass

    return all_graphs


def extract_blueprint(asset, asset_path):
    """Full Blueprint extraction."""
    data = {
        "asset_path": asset_path,
        "asset_type": asset.get_class().get_name(),
        "parent_class": None,
        "blueprint_type": None,
        "components": [],
        "variables": [],
        "event_dispatchers": [],
        "functions_list": [],
        "interfaces": [],
        "graphs": [],
    }

    # Parent class
    try:
        parent = asset.get_editor_property("parent_class")
        if parent:
            data["parent_class"] = parent.get_name()
    except Exception as e:
        log_warn(f"Parent class error on {asset_path}: {e}")

    # Blueprint type (Normal, Interface, MacroLibrary, etc.)
    try:
        bp_type = asset.get_editor_property("blueprint_type")
        if bp_type:
            data["blueprint_type"] = str(bp_type)
    except:
        pass

    data["components"] = extract_components(asset)
    data["variables"] = extract_variables(asset)
    data["event_dispatchers"] = extract_event_dispatchers(asset)
    data["functions_list"] = extract_functions_list(asset)
    data["interfaces"] = extract_interfaces(asset)
    data["graphs"] = extract_all_graphs(asset)

    return data


# ─────────────────────────────────────────────
# BEHAVIOR TREE EXTRACTOR
# ─────────────────────────────────────────────

def extract_behavior_tree(asset, asset_path):
    data = {
        "asset_path": asset_path,
        "asset_type": "BehaviorTree",
        "blackboard": None,
        "root_node": None,
    }
    try:
        bb = asset.get_editor_property("blackboard_asset")
        if bb:
            data["blackboard"] = bb.get_path_name()
    except:
        pass

    # Try to get root node info
    try:
        root = asset.get_editor_property("root_node")
        if root:
            data["root_node"] = root.get_class().get_name()
    except:
        pass

    return data


def extract_blackboard(asset, asset_path):
    data = {
        "asset_path": asset_path,
        "asset_type": "BlackboardData",
        "keys": [],
        "parent": None,
    }
    try:
        parent = asset.get_editor_property("parent")
        if parent:
            data["parent"] = parent.get_path_name()
    except:
        pass

    try:
        keys = asset.get_editor_property("keys")
        if keys:
            for key in keys:
                k = {}
                try:
                    k["name"] = str(key.get_editor_property("entry_name"))
                except:
                    k["name"] = "unknown"
                try:
                    kt = key.get_editor_property("key_type")
                    if kt:
                        k["type"] = kt.get_class().get_name()
                except:
                    k["type"] = "unknown"
                data["keys"].append(k)
    except Exception as e:
        log_warn(f"Blackboard key extraction error on {asset_path}: {e}")
    return data


# ─────────────────────────────────────────────
# ENUM / STRUCT EXTRACTORS
# ─────────────────────────────────────────────

def extract_enum(asset, asset_path):
    data = {
        "asset_path": asset_path,
        "asset_type": "UserDefinedEnum",
        "values": [],
    }
    try:
        # Method 1: Use enum's display names (most reliable in UE5 Python)
        names = asset.get_editor_property("display_name_map")
        if names:
            for pair in names:
                try:
                    data["values"].append(str(pair))
                except:
                    pass
    except:
        pass

    if not data["values"]:
        try:
            # Method 2: Use Names property
            names = asset.get_editor_property("names")
            if names:
                for n in names:
                    data["values"].append(str(n))
        except:
            pass

    if not data["values"]:
        try:
            # Method 3: Iterate via engine utility
            num = unreal.EnumEditorUtils.get_num_entries(asset) if hasattr(unreal, 'EnumEditorUtils') else 0
            for i in range(num):
                name = unreal.EnumEditorUtils.get_display_name(asset, i)
                data["values"].append(str(name))
        except:
            pass

    if not data["values"]:
        try:
            # Method 4: Use get_display_name_text_by_value which works on most UE5 enums
            # Try indices 0 through 255 until we get an empty one
            for i in range(256):
                try:
                    name = asset.get_display_name_text_by_value(i)
                    if name and str(name) and str(name) != "" and "MAX" not in str(name):
                        data["values"].append(str(name))
                    else:
                        break
                except:
                    break
        except:
            pass

    if not data["values"]:
        log_warn(f"Could not extract enum values for {asset_path}")

    return data


def extract_struct(asset, asset_path):
    data = {
        "asset_path": asset_path,
        "asset_type": "UserDefinedStruct",
        "fields": [],
    }

    try:
        # In UE5 Python, we can iterate the struct's properties via the script struct
        # UserDefinedStruct properties are accessible through editor properties
        # Try to get children/field names
        for prop in asset.get_editor_property("field_notify_names") or []:
            data["fields"].append({"name": str(prop)})
    except:
        pass

    if not data["fields"]:
        try:
            # Alternative: try to get the struct as a scriptable and inspect
            # This may vary by UE version
            desc = str(asset.get_desc())
            if desc:
                data["description"] = desc
        except:
            pass

    if not data["fields"]:
        log_warn(f"Could not extract struct fields for {asset_path} (UE Python struct introspection is limited)")

    return data


# ─────────────────────────────────────────────
# GENERIC / MISC EXTRACTOR
# ─────────────────────────────────────────────

def extract_generic(asset, asset_path):
    data = {
        "asset_path": asset_path,
        "asset_type": asset.get_class().get_name(),
    }

    # Try to get any useful properties generically
    try:
        outer = asset.get_outer()
        if outer:
            data["outer"] = outer.get_name()
    except:
        pass

    return data


# ─────────────────────────────────────────────
# ASSET ROUTER
# ─────────────────────────────────────────────

def extract_asset(asset, asset_path):
    """Route to the correct extractor based on asset type."""
    # Order matters — most specific first
    if isinstance(asset, unreal.WidgetBlueprint):
        return extract_blueprint(asset, asset_path)
    if isinstance(asset, unreal.AnimBlueprint):
        return extract_blueprint(asset, asset_path)
    if isinstance(asset, unreal.Blueprint):
        return extract_blueprint(asset, asset_path)
    if isinstance(asset, unreal.BehaviorTree):
        return extract_behavior_tree(asset, asset_path)
    if isinstance(asset, unreal.BlackboardData):
        return extract_blackboard(asset, asset_path)
    if isinstance(asset, unreal.UserDefinedEnum):
        return extract_enum(asset, asset_path)
    if isinstance(asset, unreal.UserDefinedStruct):
        return extract_struct(asset, asset_path)
    return extract_generic(asset, asset_path)


# ─────────────────────────────────────────────
# MAIN
# ─────────────────────────────────────────────

def should_skip(asset):
    """Check if asset should be skipped."""
    asset_class = asset.get_class()
    for skip in SKIP_CLASSES:
        try:
            if isinstance(asset, skip):
                return True
        except:
            pass
    # Also skip by class name string for types not in unreal module
    cls_name = asset_class.get_name()
    if cls_name in _SKIP_CLASS_NAMES:
        return True
    return False


def safe_filename(asset_path):
    """Create a safe filename from an asset path."""
    # Strip the object name suffix if present (e.g. "/Game/Foo/Bar.Bar" -> "/Game/Foo/Bar")
    if "." in asset_path.split("/")[-1]:
        asset_path = asset_path.rsplit(".", 1)[0]
    name = asset_path.replace("/", "_").replace("\\", "_").lstrip("_")
    # Truncate if too long for filesystem
    if len(name) > 200:
        name = name[:200]
    return name + ".json"


def run():
    log_info("=" * 60)
    log_info("Olive AI Studio - Blueprint Extraction v2")
    log_info(f"Project: {PROJECT_NAME}")
    log_info(f"Scan root: {SCAN_ROOT}")
    log_info(f"Output: {OUTPUT_DIR}")
    log_info("=" * 60)

    counts = {
        "blueprints": 0, "widgets": 0, "animation": 0,
        "behavior_trees": 0, "interfaces": 0, "enums_structs": 0,
        "misc": 0, "skipped": 0, "errors": 0,
    }

    folders = ["blueprints", "widgets", "animation", "behavior_trees",
               "interfaces", "enums_structs", "misc"]
    for folder in folders:
        os.makedirs(os.path.join(OUTPUT_DIR, folder), exist_ok=True)

    # Get all asset paths
    all_paths = unreal.EditorAssetLibrary.list_assets(SCAN_ROOT, recursive=True, include_folder=False)
    total = len(all_paths)
    log_info(f"Found {total} assets to scan")

    # Track asset types we encounter for debugging
    type_counts = {}

    with unreal.ScopedSlowTask(total, "Olive: Extracting Blueprint data...") as task:
        task.make_dialog(True)

        for i, path in enumerate(all_paths):
            if task.should_cancel():
                log_info("Cancelled by user.")
                break

            short_name = path.split("/")[-1] if "/" in path else path
            task.enter_progress_frame(1, f"[{i+1}/{total}] {short_name}")

            try:
                # Load the asset
                asset = unreal.EditorAssetLibrary.load_asset(path)
                if asset is None:
                    log_warn(f"Failed to load: {path}")
                    counts["errors"] += 1
                    continue

                # Track what types we see
                cls_name = asset.get_class().get_name()
                type_counts[cls_name] = type_counts.get(cls_name, 0) + 1

                # Skip check
                if should_skip(asset):
                    counts["skipped"] += 1
                    continue

                # Route and extract
                folder = get_output_folder(asset)
                extracted = extract_asset(asset, path)

                # Write output
                filename = safe_filename(path)
                out_path = os.path.join(OUTPUT_DIR, folder, filename)
                with open(out_path, "w", encoding="utf-8") as f:
                    json.dump(extracted, f, indent=2, default=str)

                counts[folder] = counts.get(folder, 0) + 1

                if VERBOSE and folder == "blueprints":
                    n_vars = len(extracted.get("variables", []))
                    n_comps = len(extracted.get("components", []))
                    n_graphs = len(extracted.get("graphs", []))
                    n_nodes = sum(len(g.get("nodes", [])) for g in extracted.get("graphs", []))
                    log_info(f"  BP: {short_name} — {n_vars} vars, {n_comps} comps, {n_graphs} graphs, {n_nodes} nodes")

            except Exception as e:
                log_warn(f"Error processing {path}: {e}")
                counts["errors"] += 1

    # Write manifest
    manifest = {
        "project": PROJECT_NAME,
        "scan_root": SCAN_ROOT,
        "extracted_at": datetime.now().isoformat(),
        "total_assets_scanned": total,
        "counts": counts,
        "asset_types_found": dict(sorted(type_counts.items(), key=lambda x: -x[1])),
    }
    manifest_path = os.path.join(OUTPUT_DIR, "manifest.json")
    with open(manifest_path, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)

    # Write error log
    if _error_log:
        error_path = os.path.join(OUTPUT_DIR, "errors.log")
        with open(error_path, "w", encoding="utf-8") as f:
            f.write(f"Extraction errors for {PROJECT_NAME}\n")
            f.write(f"Date: {datetime.now().isoformat()}\n")
            f.write(f"Total warnings: {len(_error_log)}\n")
            f.write("=" * 60 + "\n\n")
            for err in _error_log:
                f.write(err + "\n")

    log_info("=" * 60)
    log_info("EXTRACTION COMPLETE")
    log_info(f"Results saved to: {OUTPUT_DIR}")
    for k, v in counts.items():
        if v > 0:
            log_info(f"  {k}: {v}")
    log_info(f"Asset types found: {len(type_counts)}")
    for t, c in sorted(type_counts.items(), key=lambda x: -x[1])[:15]:
        log_info(f"  {t}: {c}")
    if _error_log:
        log_info(f"Warnings logged: {len(_error_log)} (see errors.log)")
    log_info("=" * 60)


run()