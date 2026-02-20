#!/usr/bin/env python3
"""
generate_world_graph.py — Extract world graph from WoW 3.3.5a DBC files.

Reads DBC files from the game client (via MPQ extraction) and generates a JSON
world graph for use by WorldGraph::LoadFromFile().

Data sources:
  - TaxiNodes.dbc     → flight master nodes
  - TaxiPath.dbc      → flight path edges
  - TaxiPathNode.dbc  → flight path waypoints (for travel time estimation)
  - WorldSafeLocs.dbc → graveyard/safe location waypoints

Also includes hardcoded boat/zeppelin routes.

Usage:
  python generate_world_graph.py [--dbc-dir DIR] [--output FILE]

If --dbc-dir is not specified, extracts DBCs from MPQ archives automatically.
"""

import argparse
import json
import math
import os
import struct
import sys
from collections import defaultdict

# =============================================================================
# Configuration
# =============================================================================

# Maps to include (0=EK, 1=Kalimdor, 530=Outland, 571=Northrend)
MAPS = {0, 1, 530, 571}

# Node ID offsets to avoid collisions
FLIGHT_MASTER_ID_OFFSET = 0       # TaxiNode IDs used as-is (max ~600)
SAFE_LOC_ID_OFFSET      = 10000   # WorldSafeLocs.ID + 10000
BOAT_DOCK_ID_OFFSET     = 20000   # Hardcoded boat/zeppelin docks

# Walk edge generation
WALK_KNN              = 6       # Connect each node to K nearest neighbors
WALK_MAX_DIST         = 2500.0  # Max distance for walk edges (yards)
WALK_SPEED            = 8.5     # Average move speed in yd/s (mix of run/mount)
FLIGHT_SPEED          = 32.0    # Approximate flight path speed in yd/s
BOAT_TRAVEL_TIME      = 60.0    # Estimated boat/zeppelin travel time (seconds)
CROSS_CONTINENT_BOAT  = 120.0   # Cross-continent boat/zeppelin time

# Filter out "test" or "Programmer" nodes
BLACKLIST_NAMES = {"Programmer Isle", "Designer Island", "Development Land"}

# Filter out non-useful WorldSafeLocs entries (corpse catchers, PvP respawns, etc.)
SAFELOC_NAME_BLACKLIST = {
    "Corpse Catcher",   # Netherstorm grid — 100+ useless nodes
    "Reuse",            # Placeholder entries
}

# Minimum distance between safe locations on the same map (yards).
# Filters out clustered entries that add noise to the graph.
SAFELOC_MIN_SPACING = 150.0

# MPQ path (auto-detect from game client)
DEFAULT_CLIENT_PATH = r"Z:\Games\wow 3.3.5a client"

# =============================================================================
# DBC parsing
# =============================================================================

def read_dbc(path):
    """Read a DBC file and return (records, string_block)."""
    with open(path, "rb") as f:
        data = f.read()

    magic = data[:4]
    if magic != b"WDBC":
        raise ValueError(f"Not a valid DBC file: {path} (magic={magic!r})")

    n_records, n_fields, record_size, string_block_size = struct.unpack_from("<IIII", data, 4)
    records_start = 20
    string_block_start = records_start + n_records * record_size
    string_block = data[string_block_start:]

    records = []
    for i in range(n_records):
        offset = records_start + i * record_size
        records.append(data[offset : offset + record_size])

    return records, string_block


def get_string(string_block, offset):
    """Read a null-terminated string from the string block."""
    if offset >= len(string_block):
        return ""
    end = string_block.index(b"\x00", offset)
    return string_block[offset:end].decode("utf-8", errors="replace")


# =============================================================================
# DBC record parsers
# =============================================================================

def parse_taxi_nodes(dbc_dir, connected_node_ids=None):
    """Parse TaxiNodes.dbc → list of flight master dicts.

    Record: ID(u32), MapID(u32), X(f), Y(f), Z(f),
            Name_lang[16](u32*16), NameFlags(u32),
            MountCreatureID_Horde(u32), MountCreatureID_Alliance(u32)
    24 fields = 96 bytes

    If connected_node_ids is provided, only include nodes that have at least
    one TaxiPath connection (filters out legacy/unused nodes).
    """
    records, strings = read_dbc(os.path.join(dbc_dir, "TaxiNodes.dbc"))
    nodes = []
    for rec in records:
        node_id, map_id = struct.unpack_from("<II", rec, 0)
        x, y, z = struct.unpack_from("<fff", rec, 8)
        name_off = struct.unpack_from("<I", rec, 20)[0]  # enUS = first locale
        # Mount creature IDs at offset 88 and 92
        mount_h, mount_a = struct.unpack_from("<II", rec, 88)

        name = get_string(strings, name_off)

        if map_id not in MAPS:
            continue
        if name in BLACKLIST_NAMES or name.startswith("Generic,"):
            continue
        if mount_h == 0 and mount_a == 0:
            continue  # Unused/test node
        if connected_node_ids is not None and node_id not in connected_node_ids:
            continue  # No flight paths — legacy node

        faction = "neutral"
        if mount_h != 0 and mount_a == 0:
            faction = "horde"
        elif mount_h == 0 and mount_a != 0:
            faction = "alliance"

        nodes.append({
            "dbc_id": node_id,
            "map": map_id,
            "x": x, "y": y, "z": z,
            "name": name,
            "faction": faction,
        })

    return nodes


def parse_taxi_paths(dbc_dir):
    """Parse TaxiPath.dbc → list of flight path dicts.

    Record: ID(u32), FromNode(u32), ToNode(u32), Cost(u32)
    4 fields = 16 bytes
    """
    records, _ = read_dbc(os.path.join(dbc_dir, "TaxiPath.dbc"))
    paths = []
    for rec in records:
        path_id, from_node, to_node, cost = struct.unpack("<IIII", rec)
        paths.append({
            "path_id": path_id,
            "from": from_node,
            "to": to_node,
            "gold_cost": cost,
        })
    return paths


def parse_taxi_path_nodes(dbc_dir):
    """Parse TaxiPathNode.dbc → dict of path_id -> list of (x,y,z) waypoints.

    Record: ID(u32), PathID(u32), NodeIndex(u32), MapID(u32),
            X(f), Y(f), Z(f), Flags(u32), Delay(u32),
            ArrivalEventID(u32), DepartureEventID(u32)
    11 fields = 44 bytes
    """
    records, _ = read_dbc(os.path.join(dbc_dir, "TaxiPathNode.dbc"))
    path_nodes = defaultdict(list)
    for rec in records:
        _, path_id, node_idx, map_id = struct.unpack_from("<IIII", rec, 0)
        x, y, z = struct.unpack_from("<fff", rec, 16)
        path_nodes[path_id].append((node_idx, x, y, z))

    # Sort by node index within each path
    for pid in path_nodes:
        path_nodes[pid].sort(key=lambda t: t[0])

    return dict(path_nodes)


def parse_world_safe_locs(dbc_dir):
    """Parse WorldSafeLocs.dbc → list of safe location dicts.

    Record: ID(u32), MapID(u32), X(f), Y(f), Z(f), Name_lang[17](u32*17)
    22 fields = 88 bytes

    Filters out:
    - "Corpse Catcher" entries (Netherstorm grid — 100+ useless nodes)
    - Entries too close to an already-accepted node (< SAFELOC_MIN_SPACING yd)
    """
    records, strings = read_dbc(os.path.join(dbc_dir, "WorldSafeLocs.dbc"))

    # First pass: collect all candidates
    candidates = []
    for rec in records:
        loc_id, map_id = struct.unpack_from("<II", rec, 0)
        x, y, z = struct.unpack_from("<fff", rec, 8)
        name_off = struct.unpack_from("<I", rec, 20)[0]
        name = get_string(strings, name_off)

        if map_id not in MAPS:
            continue

        # Name blacklist
        skip = False
        for bl in SAFELOC_NAME_BLACKLIST:
            if bl.lower() in name.lower():
                skip = True
                break
        if skip:
            continue

        candidates.append({
            "dbc_id": loc_id,
            "map": map_id,
            "x": x, "y": y, "z": z,
            "name": name,
        })

    # Second pass: deduplicate by minimum spacing (per map)
    accepted = []
    accepted_by_map = defaultdict(list)  # map_id -> list of (x, y)

    for c in candidates:
        mid = c["map"]
        too_close = False
        for ax, ay in accepted_by_map[mid]:
            dx, dy = c["x"] - ax, c["y"] - ay
            if math.sqrt(dx * dx + dy * dy) < SAFELOC_MIN_SPACING:
                too_close = True
                break
        if too_close:
            continue
        accepted.append(c)
        accepted_by_map[mid].append((c["x"], c["y"]))

    return accepted


# =============================================================================
# Flight path distance calculation
# =============================================================================

def calc_path_distance(waypoints):
    """Calculate total 3D distance along a flight path from its waypoints."""
    total = 0.0
    for i in range(1, len(waypoints)):
        _, x0, y0, z0 = waypoints[i - 1]
        _, x1, y1, z1 = waypoints[i]
        dx, dy, dz = x1 - x0, y1 - y0, z1 - z0
        total += math.sqrt(dx * dx + dy * dy + dz * dz)
    return total


# =============================================================================
# Boat/Zeppelin routes (hardcoded)
# =============================================================================

# Each entry: (name, map, x, y, z, faction)
BOAT_DOCKS = [
    # Alliance boats
    ("Stormwind Harbor (boat dock)", 0, -8640.0, 1328.0, 5.2, "alliance"),
    ("Auberdine (boat dock)", 1, 6443.0, 599.0, 9.0, "alliance"),
    ("Menethil Harbor (boat dock)", 0, -3764.0, -580.0, 6.5, "alliance"),
    ("Theramore Isle (boat dock)", 1, -5413.0, -2383.0, 6.0, "alliance"),
    ("Valiance Keep (boat dock)", 571, 2236.0, 5134.0, 5.3, "alliance"),
    ("Valgarde (boat dock)", 571, 588.0, -5095.0, 1.5, "alliance"),
    ("Rut'theran Village (boat dock)", 1, 8643.0, 840.0, 13.5, "neutral"),

    # Horde zeppelins
    ("Orgrimmar Zeppelin Tower", 1, 1178.0, -4213.0, 26.0, "horde"),
    ("Undercity Zeppelin Tower", 0, 2060.0, 288.0, 97.0, "horde"),
    ("Grom'gol Zeppelin Tower", 0, -12413.0, 205.0, 32.0, "horde"),
    ("Warsong Hold Zeppelin", 571, 2837.0, 6187.0, 89.0, "horde"),
    ("Vengeance Landing Zeppelin", 571, 1977.0, -6083.0, 12.0, "horde"),

    # Neutral
    ("Ratchet (boat dock)", 1, -999.0, -3826.0, 5.4, "neutral"),
    ("Booty Bay (boat dock)", 0, -14279.0, 557.0, 9.0, "neutral"),
    ("Moa'ki Harbor (boat dock)", 571, 2039.0, -1.0, 11.0, "neutral"),
    ("Kamagua (boat dock)", 571, 936.0, -5518.0, 2.7, "neutral"),
]

# Connections: (from_dock_index, to_dock_index, travel_time_seconds)
BOAT_ROUTES = [
    # Alliance boats
    (0, 1, CROSS_CONTINENT_BOAT),     # Stormwind → Auberdine
    (0, 4, CROSS_CONTINENT_BOAT),     # Stormwind → Valiance Keep
    (2, 3, CROSS_CONTINENT_BOAT),     # Menethil → Theramore
    (2, 5, CROSS_CONTINENT_BOAT),     # Menethil → Valgarde
    (1, 6, BOAT_TRAVEL_TIME),         # Auberdine → Rut'theran Village

    # Horde zeppelins
    (7, 8, CROSS_CONTINENT_BOAT),     # Orgrimmar → Undercity
    (7, 9, CROSS_CONTINENT_BOAT),     # Orgrimmar → Grom'gol
    (7, 10, CROSS_CONTINENT_BOAT),    # Orgrimmar → Warsong Hold
    (8, 11, CROSS_CONTINENT_BOAT),    # Undercity → Vengeance Landing

    # Neutral boats
    (12, 13, BOAT_TRAVEL_TIME),       # Ratchet → Booty Bay
    (14, 15, BOAT_TRAVEL_TIME),       # Moa'ki → Kamagua
]


# =============================================================================
# Portal / Teleport routes (hardcoded)
# =============================================================================

PORTAL_ID_OFFSET = 30000

# Each entry: (name, map, x, y, z, faction)
PORTAL_NODES = [
    # Rut'theran ↔ Darnassus portal (instant teleport, both ends on map 1)
    ("Rut'theran Village (portal)", 1, 8705.0, 960.0, 13.2, "alliance"),    # 0
    ("Darnassus (portal)", 1, 9946.0, 2616.0, 1316.0, "alliance"),          # 1

    # Silvermoon ↔ Undercity (Orb of Translocation)
    ("Silvermoon City (orb)", 530, 10034.0, -7000.0, 62.0, "horde"),        # 2
    ("Undercity (orb)", 0, 1808.0, 336.0, -29.0, "horde"),                  # 3

    # Dark Portal (Blasted Lands ↔ Hellfire Peninsula)
    ("Dark Portal, Blasted Lands", 0, -11810.0, -3190.0, -15.0, "neutral"), # 4
    ("Dark Portal, Hellfire", 530, -248.0, 922.0, 84.4, "neutral"),         # 5

    # Deeprun Tram (Stormwind ↔ Ironforge, effectively instant)
    ("Deeprun Tram, Stormwind", 0, -8358.0, 527.0, 91.4, "alliance"),      # 6
    ("Deeprun Tram, Ironforge", 0, -4838.0, -1317.0, 502.0, "alliance"),   # 7

    # Shattrath portals to capital cities
    ("Shattrath (portal hub)", 530, -1822.0, 5417.0, -12.4, "neutral"),    # 8

    # Dalaran portals (Northrend hub city)
    ("Dalaran (portal hub)", 571, 5804.0, 624.0, 647.8, "neutral"),         # 9
]

# Connections: (from_portal_index, to_portal_index, travel_time_seconds)
PORTAL_ROUTES = [
    (0, 1, 5.0),     # Rut'theran ↔ Darnassus (instant portal)
    (2, 3, 10.0),    # Silvermoon ↔ Undercity (orb, cross-map)
    (4, 5, 10.0),    # Dark Portal (cross-map)
    (6, 7, 30.0),    # Deeprun Tram (same map, ~30s ride)
]


# =============================================================================
# Walk edge generation
# =============================================================================

def dist_2d(a, b):
    """2D distance between two nodes."""
    dx = a["x"] - b["x"]
    dy = a["y"] - b["y"]
    return math.sqrt(dx * dx + dy * dy)


def generate_walk_edges(all_nodes):
    """Generate walk edges using K-nearest-neighbors on same map.

    Also ensures boat/zeppelin docks are always connected to their nearest
    flight master and nearest waypoint (regardless of KNN rank).
    """
    # Group nodes by map
    by_map = defaultdict(list)
    for node in all_nodes:
        by_map[node["map"]].append(node)

    edges = []
    seen = set()  # (min_id, max_id) to avoid duplicates

    def add_edge(a_id, b_id, distance):
        pair = (min(a_id, b_id), max(a_id, b_id))
        if pair in seen:
            return
        seen.add(pair)
        cost = distance / WALK_SPEED
        edges.append({
            "from": a_id,
            "to": b_id,
            "type": "walk",
            "cost": round(cost, 1),
            "bidir": True,
        })

    for map_id, nodes in by_map.items():
        # KNN walk edges
        for i, node_a in enumerate(nodes):
            distances = []
            for j, node_b in enumerate(nodes):
                if i == j:
                    continue
                d = dist_2d(node_a, node_b)
                if d <= WALK_MAX_DIST:
                    distances.append((d, node_b))

            distances.sort(key=lambda t: t[0])

            for d, node_b in distances[:WALK_KNN]:
                add_edge(node_a["id"], node_b["id"], d)

        # Ensure transport/portal nodes connect to nearest FM and nearest waypoint
        special = [n for n in nodes if n["type"] in ("boat_zeppelin", "portal")]
        fms = [n for n in nodes if n["type"] == "flight_master"]
        wps = [n for n in nodes if n["type"] == "waypoint"]

        for sp in special:
            # Connect to nearest flight master
            if fms:
                nearest_fm = min(fms, key=lambda f: dist_2d(sp, f))
                d = dist_2d(sp, nearest_fm)
                if d <= WALK_MAX_DIST:
                    add_edge(sp["id"], nearest_fm["id"], d)

            # Connect to nearest waypoint
            if wps:
                nearest_wp = min(wps, key=lambda w: dist_2d(sp, w))
                d = dist_2d(sp, nearest_wp)
                if d <= WALK_MAX_DIST:
                    add_edge(sp["id"], nearest_wp["id"], d)

    return edges


# =============================================================================
# MPQ extraction
# =============================================================================

def extract_dbcs_from_mpq(client_path, out_dir):
    """Extract required DBC files from the game client's MPQ archives."""
    try:
        from mpyq import MPQArchive
    except ImportError:
        print("ERROR: mpyq not installed. Run: pip install mpyq", file=sys.stderr)
        sys.exit(1)

    os.makedirs(out_dir, exist_ok=True)

    # DBC files are in locale-enUS.MPQ (localized names)
    mpq_path = os.path.join(client_path, "Data", "enUS", "locale-enUS.MPQ")
    if not os.path.exists(mpq_path):
        print(f"ERROR: MPQ not found: {mpq_path}", file=sys.stderr)
        sys.exit(1)

    targets = [
        "TaxiNodes.dbc",
        "TaxiPath.dbc",
        "TaxiPathNode.dbc",
        "WorldSafeLocs.dbc",
        "AreaPOI.dbc",
    ]

    archive = MPQArchive(mpq_path)
    for fname in targets:
        mpq_name = f"DBFilesClient\\{fname}".encode()
        data = archive.read_file(mpq_name)
        if not data:
            print(f"WARNING: Could not extract {fname} from MPQ", file=sys.stderr)
            continue
        out_path = os.path.join(out_dir, fname)
        with open(out_path, "wb") as f:
            f.write(data)
        print(f"  Extracted {fname}: {len(data)} bytes")

    return out_dir


# =============================================================================
# Main generation
# =============================================================================

def generate(dbc_dir, output_path):
    print(f"Reading DBC files from: {dbc_dir}")

    # 1. Parse DBC data
    # First parse paths to find which taxi nodes are actually connected
    taxi_paths = parse_taxi_paths(dbc_dir)
    connected_taxi_ids = set()
    for tp in taxi_paths:
        connected_taxi_ids.add(tp["from"])
        connected_taxi_ids.add(tp["to"])

    taxi_nodes = parse_taxi_nodes(dbc_dir, connected_node_ids=connected_taxi_ids)
    taxi_path_nodes = parse_taxi_path_nodes(dbc_dir)
    safe_locs = parse_world_safe_locs(dbc_dir)

    print(f"  TaxiNodes: {len(taxi_nodes)} flight masters")
    print(f"  TaxiPaths: {len(taxi_paths)} flight connections")
    print(f"  WorldSafeLocs: {len(safe_locs)} safe locations")

    # 2. Build node list
    all_nodes = []
    taxi_node_ids = set()  # Track valid taxi node IDs

    # Flight masters (ID = TaxiNode.ID)
    for tn in taxi_nodes:
        node = {
            "id": tn["dbc_id"] + FLIGHT_MASTER_ID_OFFSET,
            "name": tn["name"],
            "map": tn["map"],
            "x": round(tn["x"], 1),
            "y": round(tn["y"], 1),
            "z": round(tn["z"], 1),
            "type": "flight_master",
            "faction": tn["faction"],
        }
        all_nodes.append(node)
        taxi_node_ids.add(tn["dbc_id"])

    # Safe locations (ID = 10000 + SafeLoc.ID)
    for sl in safe_locs:
        node = {
            "id": sl["dbc_id"] + SAFE_LOC_ID_OFFSET,
            "name": sl["name"],
            "map": sl["map"],
            "x": round(sl["x"], 1),
            "y": round(sl["y"], 1),
            "z": round(sl["z"], 1),
            "type": "waypoint",
            "faction": "neutral",
        }
        all_nodes.append(node)

    # Boat/Zeppelin docks (ID = 20000+)
    for i, (name, map_id, x, y, z, faction) in enumerate(BOAT_DOCKS):
        node = {
            "id": BOAT_DOCK_ID_OFFSET + i,
            "name": name,
            "map": map_id,
            "x": round(x, 1),
            "y": round(y, 1),
            "z": round(z, 1),
            "type": "boat_zeppelin",
            "faction": faction,
        }
        all_nodes.append(node)

    # Portal / teleport nodes (ID = 30000+)
    for i, (name, map_id, x, y, z, faction) in enumerate(PORTAL_NODES):
        node = {
            "id": PORTAL_ID_OFFSET + i,
            "name": name,
            "map": map_id,
            "x": round(x, 1),
            "y": round(y, 1),
            "z": round(z, 1),
            "type": "portal",
            "faction": faction,
        }
        all_nodes.append(node)

    print(f"  Total nodes: {len(all_nodes)}")

    # 3. Build edge list
    all_edges = []

    # Flight path edges
    flight_count = 0
    for tp in taxi_paths:
        from_id = tp["from"]
        to_id = tp["to"]

        # Skip paths involving nodes not in our set
        if from_id not in taxi_node_ids or to_id not in taxi_node_ids:
            continue

        # Compute travel time from path waypoints
        waypoints = taxi_path_nodes.get(tp["path_id"], [])
        if waypoints:
            distance = calc_path_distance(waypoints)
            cost = distance / FLIGHT_SPEED
        else:
            # Fallback: rough estimate from gold cost
            cost = tp["gold_cost"] * 0.5  # very rough

        all_edges.append({
            "from": from_id + FLIGHT_MASTER_ID_OFFSET,
            "to": to_id + FLIGHT_MASTER_ID_OFFSET,
            "type": "flight",
            "cost": round(cost, 1),
            "bidir": False,  # Flight paths are directional (A→B is not same as B→A)
        })
        flight_count += 1

    print(f"  Flight edges: {flight_count}")

    # Boat/Zeppelin edges
    for from_idx, to_idx, travel_time in BOAT_ROUTES:
        from_id = BOAT_DOCK_ID_OFFSET + from_idx
        to_id = BOAT_DOCK_ID_OFFSET + to_idx
        all_edges.append({
            "from": from_id,
            "to": to_id,
            "type": "boat",
            "cost": round(travel_time, 1),
            "bidir": True,
        })

    # Portal / teleport edges
    for from_idx, to_idx, travel_time in PORTAL_ROUTES:
        from_id = PORTAL_ID_OFFSET + from_idx
        to_id = PORTAL_ID_OFFSET + to_idx
        all_edges.append({
            "from": from_id,
            "to": to_id,
            "type": "teleport",
            "cost": round(travel_time, 1),
            "bidir": True,
        })

    print(f"  Boat/Zeppelin edges: {len(BOAT_ROUTES)}")
    print(f"  Portal edges: {len(PORTAL_ROUTES)}")

    # Walk edges (KNN)
    walk_edges = generate_walk_edges(all_nodes)
    all_edges.extend(walk_edges)
    print(f"  Walk edges: {len(walk_edges)}")

    print(f"  Total edges: {len(all_edges)}")

    # 4. Build JSON output
    json_nodes = []
    for node in all_nodes:
        jn = {
            "id": node["id"],
            "name": node["name"],
            "map": node["map"],
            "x": node["x"],
            "y": node["y"],
            "z": node["z"],
            "type": node["type"],
        }
        # Include faction as metadata (WorldGraph parser ignores unknown fields)
        if node.get("faction") and node["faction"] != "neutral":
            jn["faction"] = node["faction"]
        json_nodes.append(jn)

    json_edges = []
    for edge in all_edges:
        je = {
            "from": edge["from"],
            "to": edge["to"],
            "type": edge["type"],
            "cost": edge["cost"],
        }
        if not edge.get("bidir", True):
            je["bidir"] = False
        json_edges.append(je)

    output = {
        "_comment": "WoW 3.3.5a World Graph — auto-generated from DBC files",
        "_maps": {str(m): name for m, name in {
            0: "Eastern Kingdoms", 1: "Kalimdor",
            530: "Outland", 571: "Northrend"
        }.items() if m in MAPS},
        "nodes": json_nodes,
        "edges": json_edges,
    }

    # Write JSON
    with open(output_path, "w", encoding="utf-8") as f:
        json.dump(output, f, indent=2, ensure_ascii=False)

    print(f"\nGenerated: {output_path}")
    print(f"  {len(json_nodes)} nodes, {len(json_edges)} edges")

    # Print summary by map
    for map_id in sorted(MAPS):
        map_nodes = [n for n in json_nodes if n["map"] == map_id]
        fm = sum(1 for n in map_nodes if n["type"] == "flight_master")
        wp = sum(1 for n in map_nodes if n["type"] == "waypoint")
        bd = sum(1 for n in map_nodes if n["type"] == "boat_zeppelin")
        print(f"  Map {map_id}: {len(map_nodes)} nodes ({fm} FM, {wp} WP, {bd} docks)")


# =============================================================================
# CLI
# =============================================================================

def main():
    parser = argparse.ArgumentParser(description="Generate WoW world graph from DBC files")
    parser.add_argument("--dbc-dir", help="Directory containing extracted DBC files")
    parser.add_argument("--client", default=DEFAULT_CLIENT_PATH,
                        help="WoW 3.3.5a client path (for MPQ extraction)")
    parser.add_argument("--output", "-o",
                        default=os.path.join(os.path.dirname(__file__), "..", "..", "data", "world_graph.json"),
                        help="Output JSON file path")
    args = parser.parse_args()

    # Resolve output path
    output_path = os.path.abspath(args.output)
    os.makedirs(os.path.dirname(output_path), exist_ok=True)

    # Get DBC directory
    if args.dbc_dir:
        dbc_dir = args.dbc_dir
    else:
        # Extract from MPQ
        dbc_dir = os.path.join(os.path.dirname(__file__), "..", "dbc_extracted")
        dbc_dir = os.path.abspath(dbc_dir)

        # Check if already extracted
        needed = ["TaxiNodes.dbc", "TaxiPath.dbc", "TaxiPathNode.dbc", "WorldSafeLocs.dbc"]
        if all(os.path.exists(os.path.join(dbc_dir, f)) for f in needed):
            print(f"Using previously extracted DBCs from: {dbc_dir}")
        else:
            print(f"Extracting DBCs from MPQ: {args.client}")
            extract_dbcs_from_mpq(args.client, dbc_dir)

    generate(dbc_dir, output_path)


if __name__ == "__main__":
    main()
