"""
graph_extractor.py — Extract road network graph from binary road masks.

Pipeline:
  1. Morphological cleanup (close gaps, remove noise)
  2. Zhang-Suen skeletonization (1px-wide centerlines)
  3. Spur pruning (remove short false branches)
  4. sknw graph construction (skeleton → NetworkX graph)
  5. World coordinate assignment
  6. Douglas-Peucker simplification
  7. Multi-tile merge at boundaries
  8. JSON export
"""

import json
from datetime import datetime, timezone

import networkx as nx
import numpy as np
from rdp import rdp
from scipy.signal import convolve2d
from skimage.morphology import (
    closing,
    disk,
    opening,
    remove_small_objects,
    skeletonize,
)

from .coord_utils import PIXEL_SIZE, TILE_SIZE


# ---------------------------------------------------------------------------
# 1. Morphological cleanup
# ---------------------------------------------------------------------------

def cleanup_mask(mask: np.ndarray, min_size: int = 20) -> np.ndarray:
    """Clean binary mask: close gaps, remove noise, drop tiny regions."""
    kernel = disk(1)
    cleaned = closing(mask, kernel)
    cleaned = opening(cleaned, kernel)
    cleaned = remove_small_objects(cleaned, max_size=min_size)
    return cleaned


# ---------------------------------------------------------------------------
# 2-3. Skeletonization + spur pruning
# ---------------------------------------------------------------------------

def extract_skeleton(mask: np.ndarray, prune_iterations: int = 3) -> np.ndarray:
    """Skeletonize a binary mask and prune spurs.

    Uses Zhang-Suen algorithm (fewer spurious branches than medial_axis).
    """
    skeleton = skeletonize(mask)
    if prune_iterations > 0:
        skeleton = prune_spurs(skeleton, iterations=prune_iterations)
    return skeleton


def prune_spurs(skeleton: np.ndarray, iterations: int = 3) -> np.ndarray:
    """Remove short dead-end branches from skeleton.

    Each iteration removes endpoints (pixels with exactly 1 neighbor).
    N iterations removes spurs up to N pixels long.
    """
    pruned = skeleton.copy()
    kern = np.ones((3, 3), dtype=int)

    for _ in range(iterations):
        neighbors = convolve2d(pruned.astype(int), kern, mode="same") - pruned.astype(int)
        endpoints = pruned & (neighbors == 1)
        pruned = pruned & ~endpoints

    return pruned


# ---------------------------------------------------------------------------
# 4. Skeleton → NetworkX graph
# ---------------------------------------------------------------------------

def skeleton_to_graph(skeleton: np.ndarray) -> nx.Graph:
    """Convert pixel skeleton to NetworkX graph using sknw."""
    import sknw

    graph = sknw.build_sknw(skeleton.astype(np.uint16), multi=False)
    return graph


# ---------------------------------------------------------------------------
# 5. World coordinate assignment
# ---------------------------------------------------------------------------

def assign_world_coords(
    graph: nx.Graph,
    origin_x: float,
    origin_y: float,
) -> nx.Graph:
    """Assign WoW world coordinates to all nodes and edge points.

    origin_x, origin_y = NW corner of the raster in world coords.
    Row increases south (decreasing X), col increases east (decreasing Y).
    """
    for node_id in graph.nodes():
        pos = graph.nodes[node_id]["o"]  # centroid (row, col)
        r, c = float(pos[0]), float(pos[1])
        graph.nodes[node_id]["world_x"] = origin_x - r * PIXEL_SIZE
        graph.nodes[node_id]["world_y"] = origin_y - c * PIXEL_SIZE

    for s, e in graph.edges():
        pts = graph[s][e]["pts"]  # Nx2 array of (row, col)
        world_pts = []
        for r, c in pts:
            wx = origin_x - float(r) * PIXEL_SIZE
            wy = origin_y - float(c) * PIXEL_SIZE
            world_pts.append((wx, wy))
        graph[s][e]["world_pts"] = world_pts

    return graph


# ---------------------------------------------------------------------------
# 6. Edge simplification (Douglas-Peucker)
# ---------------------------------------------------------------------------

def simplify_edges(graph: nx.Graph, epsilon: float = 2.0) -> nx.Graph:
    """Simplify edge polylines using Douglas-Peucker algorithm.

    epsilon: tolerance in yards. 2.0 = 2-yard tolerance.
    """
    for s, e in graph.edges():
        pts = graph[s][e].get("world_pts", [])
        if len(pts) > 2:
            simplified = rdp(pts, epsilon=epsilon)
            graph[s][e]["world_pts"] = [tuple(p) for p in simplified]
    return graph


# ---------------------------------------------------------------------------
# 7. Multi-tile merge
# ---------------------------------------------------------------------------

def merge_tile_graphs(
    graphs: list[nx.Graph],
    merge_distance: float = 2.0,
) -> nx.Graph:
    """Merge multiple tile graphs, connecting nodes within merge_distance yards.

    Steps:
      1. Combine all graphs with globally unique node IDs
      2. Find pairs of boundary nodes from different tiles within merge_distance
      3. Merge them into single nodes (average position)
    """
    if not graphs:
        return nx.Graph()
    if len(graphs) == 1:
        return graphs[0]

    merged = nx.Graph()
    node_offset = 0
    remap = {}  # (graph_idx, old_id) → new_id

    # 1. Combine graphs
    for gi, g in enumerate(graphs):
        for nid in g.nodes():
            new_id = node_offset
            remap[(gi, nid)] = new_id
            merged.add_node(new_id, **g.nodes[nid])
            node_offset += 1

        for s, e in g.edges():
            new_s = remap[(gi, s)]
            new_e = remap[(gi, e)]
            merged.add_edge(new_s, new_e, **g[s][e])

    # 2. Find merge candidates (nodes from different source graphs that are close)
    nodes_list = list(merged.nodes(data=True))
    to_merge = []  # list of (id_a, id_b) pairs

    for i in range(len(nodes_list)):
        nid_a, data_a = nodes_list[i]
        if "world_x" not in data_a:
            continue
        for j in range(i + 1, len(nodes_list)):
            nid_b, data_b = nodes_list[j]
            if "world_x" not in data_b:
                continue

            dx = data_a["world_x"] - data_b["world_x"]
            dy = data_a["world_y"] - data_b["world_y"]
            dist = (dx * dx + dy * dy) ** 0.5

            if dist <= merge_distance:
                to_merge.append((nid_a, nid_b))

    # 3. Merge nodes (union-find approach)
    parent = {}

    def find(x):
        while parent.get(x, x) != x:
            parent[x] = parent.get(parent[x], parent[x])
            x = parent[x]
        return x

    def union(a, b):
        ra, rb = find(a), find(b)
        if ra != rb:
            parent[rb] = ra

    for a, b in to_merge:
        union(a, b)

    # Build remapped graph
    final = nx.Graph()
    id_remap = {}

    for nid in merged.nodes():
        root = find(nid)
        if root not in id_remap:
            id_remap[root] = len(id_remap)
        id_remap[nid] = id_remap[root]

    # Average positions for merged nodes
    pos_accum = {}  # final_id → (sum_x, sum_y, count)
    for nid in merged.nodes():
        fid = id_remap[find(nid)]
        data = merged.nodes[nid]
        if "world_x" in data:
            if fid not in pos_accum:
                pos_accum[fid] = [0.0, 0.0, 0]
            pos_accum[fid][0] += data["world_x"]
            pos_accum[fid][1] += data["world_y"]
            pos_accum[fid][2] += 1

    for fid, (sx, sy, cnt) in pos_accum.items():
        final.add_node(fid, world_x=sx / cnt, world_y=sy / cnt)

    # Add edges (skip self-loops after merging)
    for s, e in merged.edges():
        fs = id_remap[find(s)]
        fe = id_remap[find(e)]
        if fs != fe and not final.has_edge(fs, fe):
            edge_data = dict(merged[s][e])
            final.add_edge(fs, fe, **edge_data)

    return final


def merge_tile_graphs_spatial(
    graphs: list[nx.Graph],
    merge_distance: float = 2.0,
) -> nx.Graph:
    """Merge multiple tile graphs using spatial hashing — O(N) amortized.

    Same result as merge_tile_graphs() but scales to hundreds of tiles
    by only comparing nodes within neighboring grid cells instead of
    doing all-pairs comparison.
    """
    if not graphs:
        return nx.Graph()
    if len(graphs) == 1:
        return graphs[0]

    merged = nx.Graph()
    node_offset = 0
    remap = {}       # (graph_idx, old_id) → new_id
    tile_tag = {}    # new_id → graph_idx (source tile)

    # 1. Combine graphs with globally unique node IDs
    for gi, g in enumerate(graphs):
        for nid in g.nodes():
            new_id = node_offset
            remap[(gi, nid)] = new_id
            merged.add_node(new_id, **g.nodes[nid])
            tile_tag[new_id] = gi
            node_offset += 1

        for s, e in g.edges():
            new_s = remap[(gi, s)]
            new_e = remap[(gi, e)]
            merged.add_edge(new_s, new_e, **g[s][e])

    # 2. Build spatial grid (cell_size = merge_distance)
    cell_size = merge_distance
    grid: dict[tuple[int, int], list[int]] = {}

    for nid, data in merged.nodes(data=True):
        if "world_x" not in data:
            continue
        cx = int(data["world_x"] // cell_size)
        cy = int(data["world_y"] // cell_size)
        grid.setdefault((cx, cy), []).append(nid)

    # 3. Find merge pairs — only nodes from DIFFERENT source tiles
    to_merge = []
    visited = set()

    for (cx, cy), cell_nodes in grid.items():
        # Check same cell + 8 neighbors
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                neighbor_nodes = grid.get((cx + dx, cy + dy))
                if neighbor_nodes is None:
                    continue
                for nid_a in cell_nodes:
                    data_a = merged.nodes[nid_a]
                    for nid_b in neighbor_nodes:
                        if nid_b <= nid_a:
                            continue  # avoid duplicates
                        if tile_tag[nid_a] == tile_tag[nid_b]:
                            continue  # same tile
                        pair = (min(nid_a, nid_b), max(nid_a, nid_b))
                        if pair in visited:
                            continue
                        visited.add(pair)

                        data_b = merged.nodes[nid_b]
                        dx2 = data_a["world_x"] - data_b["world_x"]
                        dy2 = data_a["world_y"] - data_b["world_y"]
                        if dx2 * dx2 + dy2 * dy2 <= merge_distance * merge_distance:
                            to_merge.append(pair)

    # 4. Union-find + remap (same as merge_tile_graphs)
    parent = {}

    def find(x):
        while parent.get(x, x) != x:
            parent[x] = parent.get(parent[x], parent[x])
            x = parent[x]
        return x

    def union(a, b):
        ra, rb = find(a), find(b)
        if ra != rb:
            parent[rb] = ra

    for a, b in to_merge:
        union(a, b)

    # Build remapped graph
    final = nx.Graph()
    id_remap = {}

    for nid in merged.nodes():
        root = find(nid)
        if root not in id_remap:
            id_remap[root] = len(id_remap)
        id_remap[nid] = id_remap[root]

    # Average positions for merged nodes
    pos_accum = {}
    for nid in merged.nodes():
        fid = id_remap[find(nid)]
        data = merged.nodes[nid]
        if "world_x" in data:
            if fid not in pos_accum:
                pos_accum[fid] = [0.0, 0.0, 0]
            pos_accum[fid][0] += data["world_x"]
            pos_accum[fid][1] += data["world_y"]
            pos_accum[fid][2] += 1

    for fid, (sx, sy, cnt) in pos_accum.items():
        final.add_node(fid, world_x=sx / cnt, world_y=sy / cnt)

    # Add edges (skip self-loops after merging)
    for s, e in merged.edges():
        fs = id_remap[find(s)]
        fe = id_remap[find(e)]
        if fs != fe and not final.has_edge(fs, fe):
            edge_data = dict(merged[s][e])
            final.add_edge(fs, fe, **edge_data)

    return final


# ---------------------------------------------------------------------------
# 8. JSON export
# ---------------------------------------------------------------------------

def export_graph_json(
    graph: nx.Graph,
    output_path: str,
    map_id: int = 0,
    map_name: str = "",
    tiles: list[tuple[int, int]] | None = None,
    threshold: int = 64,
):
    """Export road graph as JSON in map-editor WorldGraphData format.

    Output is directly loadable by map-editor via Graph > Open Graph JSON.

    Format:
    {
        "nodes": [{"id": 1, "name": "", "map": 0, "x": ..., "y": ..., "z": 0.0, "type": "waypoint"}, ...],
        "edges": [{"from": 1, "to": 2, "type": "walk", "cost": 5.23, "bidir": true}, ...],
        "metadata": { ... }
    }
    """
    # Assign sequential IDs starting from 1
    node_id_map = {}
    nodes_json = []

    for i, nid in enumerate(sorted(graph.nodes()), start=1):
        node_id_map[nid] = i
        data = graph.nodes[nid]
        nodes_json.append({
            "id": i,
            "name": "",
            "map": map_id,
            "x": round(data.get("world_x", 0.0), 2),
            "y": round(data.get("world_y", 0.0), 2),
            "z": 0.0,
            "type": "waypoint",
        })

    # Build node position lookup for cost computation
    node_pos = {}
    for n in nodes_json:
        node_pos[n["id"]] = (n["x"], n["y"])

    edges_json = []
    for s, e in graph.edges():
        from_id = node_id_map[s]
        to_id = node_id_map[e]

        # Cost = Euclidean distance between endpoints (yards)
        fx, fy = node_pos[from_id]
        tx, ty = node_pos[to_id]
        cost = round(((fx - tx) ** 2 + (fy - ty) ** 2) ** 0.5, 2)

        edges_json.append({
            "from": from_id,
            "to": to_id,
            "type": "walk",
            "cost": cost,
            "bidir": True,
        })

    output = {
        "nodes": nodes_json,
        "edges": edges_json,
        "metadata": {
            "map": map_name,
            "map_id": map_id,
            "tiles": [list(t) for t in (tiles or [])],
            "threshold": threshold,
            "generated": datetime.now(timezone.utc).isoformat(),
            "node_count": len(nodes_json),
            "edge_count": len(edges_json),
        },
    }

    with open(output_path, "w", encoding="utf-8") as f:
        json.dump(output, f, indent=2, ensure_ascii=False)

    return output


# ---------------------------------------------------------------------------
# Full pipeline
# ---------------------------------------------------------------------------

def extract_road_graph(
    mask: np.ndarray,
    origin_x: float,
    origin_y: float,
    min_size: int = 20,
    prune_iterations: int = 3,
    dp_epsilon: float = 2.0,
) -> nx.Graph:
    """Full pipeline: mask → cleaned → skeleton → graph → world coords → simplified."""
    # 1. Cleanup
    cleaned = cleanup_mask(mask, min_size=min_size)

    # 2-3. Skeletonize + prune
    skeleton = extract_skeleton(cleaned, prune_iterations=prune_iterations)

    # Check if skeleton has any pixels
    if not skeleton.any():
        return nx.Graph()

    # 4. Skeleton → graph
    graph = skeleton_to_graph(skeleton)

    if graph.number_of_nodes() == 0:
        return graph

    # 5. World coordinates
    graph = assign_world_coords(graph, origin_x, origin_y)

    # 6. Simplify
    graph = simplify_edges(graph, epsilon=dp_epsilon)

    return graph
