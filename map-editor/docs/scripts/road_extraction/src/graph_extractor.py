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
from scipy.ndimage import distance_transform_edt
from scipy.signal import convolve2d
from skimage.morphology import (
    closing,
    dilation,
    disk,
    opening,
    remove_small_objects,
    skeletonize,
)

from .coord_utils import PIXEL_SIZE, TILE_SIZE


# ---------------------------------------------------------------------------
# 1. Morphological cleanup
# ---------------------------------------------------------------------------

def cleanup_mask(mask: np.ndarray, min_size: int = 50, close_radius: int = 3, open_radius: int = 1) -> np.ndarray:
    """Clean binary mask: close gaps, remove noise, drop tiny regions."""
    closed = closing(mask, disk(close_radius))
    cleaned = opening(closed, disk(open_radius))
    cleaned = remove_small_objects(cleaned, max_size=min_size)
    return cleaned


def filter_by_width(mask: np.ndarray, min_width_px: int = 3) -> np.ndarray:
    """Remove regions thinner than min_width_px pixels."""
    dist = distance_transform_edt(mask)
    wide_enough = dist >= (min_width_px / 2.0)
    result = dilation(wide_enough, disk(min_width_px // 2))
    return mask & result


# ---------------------------------------------------------------------------
# 2-3. Skeletonization + spur pruning
# ---------------------------------------------------------------------------

def extract_skeleton(mask: np.ndarray, prune_iterations: int = 8) -> np.ndarray:
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


def merge_close_nodes(graph: nx.Graph, distance_threshold: float = 8.0) -> nx.Graph:
    """Merge graph nodes within distance_threshold yards of each other.

    Collapses noisy intersection clusters into single junction nodes.
    Uses spatial hashing + union-find for efficiency.
    """
    if graph.number_of_nodes() < 2:
        return graph

    cell_size = distance_threshold
    grid: dict[tuple[int, int], list] = {}

    for nid, data in graph.nodes(data=True):
        wx = data.get("world_x", 0.0)
        wy = data.get("world_y", 0.0)
        cx = int(wx // cell_size)
        cy = int(wy // cell_size)
        grid.setdefault((cx, cy), []).append(nid)

    to_merge = []
    visited = set()
    dist_sq = distance_threshold * distance_threshold

    for (cx, cy), cell_nodes in grid.items():
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                neighbor_nodes = grid.get((cx + dx, cy + dy))
                if neighbor_nodes is None:
                    continue
                for nid_a in cell_nodes:
                    da = graph.nodes[nid_a]
                    for nid_b in neighbor_nodes:
                        if nid_b <= nid_a:
                            continue
                        pair = (nid_a, nid_b)
                        if pair in visited:
                            continue
                        visited.add(pair)

                        db = graph.nodes[nid_b]
                        ddx = da.get("world_x", 0) - db.get("world_x", 0)
                        ddy = da.get("world_y", 0) - db.get("world_y", 0)
                        if ddx * ddx + ddy * ddy <= dist_sq:
                            to_merge.append(pair)

    if not to_merge:
        return graph

    # Union-find
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
    result = nx.Graph()
    id_remap = {}

    for nid in graph.nodes():
        root = find(nid)
        if root not in id_remap:
            id_remap[root] = len(id_remap)
        id_remap[nid] = id_remap[root]

    # Average positions
    pos_accum = {}
    for nid in graph.nodes():
        fid = id_remap[find(nid)]
        data = graph.nodes[nid]
        if fid not in pos_accum:
            pos_accum[fid] = [0.0, 0.0, 0]
        pos_accum[fid][0] += data.get("world_x", 0.0)
        pos_accum[fid][1] += data.get("world_y", 0.0)
        pos_accum[fid][2] += 1

    for fid, (sx, sy, cnt) in pos_accum.items():
        result.add_node(fid, world_x=sx / cnt, world_y=sy / cnt)

    for s, e in graph.edges():
        fs = id_remap[find(s)]
        fe = id_remap[find(e)]
        if fs != fe and not result.has_edge(fs, fe):
            edge_data = dict(graph[s][e])
            result.add_edge(fs, fe, **edge_data)

    return result


def remove_short_components(graph: nx.Graph, min_length: float = 30.0) -> nx.Graph:
    """Remove connected components with total edge length < min_length yards."""
    if graph.number_of_nodes() == 0:
        return graph

    nodes_to_remove = set()
    for component in nx.connected_components(graph):
        subgraph = graph.subgraph(component)
        total_length = 0.0
        for s, e, edata in subgraph.edges(data=True):
            cost = edata.get("cost", 0.0)
            if cost == 0.0:
                ds = graph.nodes[s]
                de = graph.nodes[e]
                dx = ds.get("world_x", 0) - de.get("world_x", 0)
                dy = ds.get("world_y", 0) - de.get("world_y", 0)
                cost = (dx * dx + dy * dy) ** 0.5
            total_length += cost

        if total_length < min_length:
            nodes_to_remove.update(component)

    graph = graph.copy()
    graph.remove_nodes_from(nodes_to_remove)
    return graph


def collapse_parallel_paths(graph: nx.Graph, max_separation: float = 10.0) -> nx.Graph:
    """Detect and merge parallel degree-2 chains within max_separation yards."""
    if graph.number_of_nodes() < 4:
        return graph

    # Extract degree-2 chains
    def _extract_chains(g):
        visited_edges = set()
        chains = []
        for node in g.nodes():
            if g.degree(node) != 2:
                # Start chains from non-degree-2 nodes
                for neighbor in g.neighbors(node):
                    edge = (min(node, neighbor), max(node, neighbor))
                    if edge in visited_edges:
                        continue
                    chain = [node]
                    current = neighbor
                    while g.degree(current) == 2:
                        chain.append(current)
                        visited_edges.add((min(chain[-2], current), max(chain[-2], current)))
                        neighbors = list(g.neighbors(current))
                        next_node = neighbors[0] if neighbors[1] == chain[-2] else neighbors[1]
                        current = next_node
                    chain.append(current)
                    visited_edges.add((min(chain[-2], current), max(chain[-2], current)))
                    chains.append(chain)
        return chains

    chains = _extract_chains(graph)
    if len(chains) < 2:
        return graph

    # Find parallel chain pairs
    merged_chains = set()
    nodes_to_remove = set()
    edges_to_add = []  # (node_a, node_b) pairs to reconnect endpoints

    for i in range(len(chains)):
        if i in merged_chains:
            continue
        ci = chains[i]
        si = graph.nodes[ci[0]]
        ei = graph.nodes[ci[-1]]
        si_x, si_y = si.get("world_x", 0), si.get("world_y", 0)
        ei_x, ei_y = ei.get("world_x", 0), ei.get("world_y", 0)

        for j in range(i + 1, len(chains)):
            if j in merged_chains:
                continue
            cj = chains[j]
            sj = graph.nodes[cj[0]]
            ej = graph.nodes[cj[-1]]
            sj_x, sj_y = sj.get("world_x", 0), sj.get("world_y", 0)
            ej_x, ej_y = ej.get("world_x", 0), ej.get("world_y", 0)

            # Check endpoints proximity (either direction)
            d_ss = ((si_x - sj_x)**2 + (si_y - sj_y)**2) ** 0.5
            d_se = ((si_x - ej_x)**2 + (si_y - ej_y)**2) ** 0.5
            d_es = ((ei_x - sj_x)**2 + (ei_y - sj_y)**2) ** 0.5
            d_ee = ((ei_x - ej_x)**2 + (ei_y - ej_y)**2) ** 0.5

            # Determine direction match
            same_dir = d_ss <= max_separation and d_ee <= max_separation
            rev_dir = d_se <= max_separation and d_es <= max_separation
            if not same_dir and not rev_dir:
                continue

            # Check midpoint proximity
            mid_i_idx = len(ci) // 2
            mid_j_idx = len(cj) // 2
            mi = graph.nodes[ci[mid_i_idx]]
            mj = graph.nodes[cj[mid_j_idx]]
            d_mid = ((mi.get("world_x", 0) - mj.get("world_x", 0))**2 +
                     (mi.get("world_y", 0) - mj.get("world_y", 0))**2) ** 0.5

            if d_mid <= max_separation:
                # Remove the shorter chain's interior nodes and reconnect endpoints
                if len(cj) <= len(ci):
                    keep, drop = ci, cj
                    merged_chains.add(j)
                else:
                    keep, drop = cj, ci
                    merged_chains.add(i)

                for node in drop[1:-1]:
                    nodes_to_remove.add(node)

                # Connect dropped chain's endpoints to kept chain's endpoints
                if same_dir:
                    if drop[0] != keep[0]:
                        edges_to_add.append((drop[0], keep[0]))
                    if drop[-1] != keep[-1]:
                        edges_to_add.append((drop[-1], keep[-1]))
                else:
                    if drop[0] != keep[-1]:
                        edges_to_add.append((drop[0], keep[-1]))
                    if drop[-1] != keep[0]:
                        edges_to_add.append((drop[-1], keep[0]))

                if keep is cj:
                    break

    if nodes_to_remove or edges_to_add:
        graph = graph.copy()
        graph.remove_nodes_from(nodes_to_remove)
        for a, b in edges_to_add:
            if a in graph and b in graph and not graph.has_edge(a, b):
                da = graph.nodes[a]
                db = graph.nodes[b]
                dx = da.get("world_x", 0) - db.get("world_x", 0)
                dy = da.get("world_y", 0) - db.get("world_y", 0)
                cost = (dx * dx + dy * dy) ** 0.5
                graph.add_edge(a, b, cost=cost)

    return graph


def expand_edge_polylines(graph: nx.Graph) -> nx.Graph:
    """Convert edge polyline points (world_pts) into actual graph nodes.

    sknw stores intermediate skeleton points along each edge as world_pts.
    This function promotes those points to real graph nodes so that curved
    roads are represented as chains of short edges (A->p1->p2->...->B)
    instead of a single straight edge (A->B).
    """
    result = nx.Graph()
    next_id = max(graph.nodes()) + 1 if graph.number_of_nodes() > 0 else 0

    # Copy all existing nodes
    for nid, data in graph.nodes(data=True):
        result.add_node(nid, **data)

    # Process each edge
    for s, e, edata in graph.edges(data=True):
        pts = edata.get("world_pts", [])

        if len(pts) <= 2:
            # No intermediate points — keep direct edge
            cost = edata.get("cost", 0.0)
            if cost == 0.0:
                ds = result.nodes[s]
                de = result.nodes[e]
                dx = ds.get("world_x", 0) - de.get("world_x", 0)
                dy = ds.get("world_y", 0) - de.get("world_y", 0)
                cost = (dx * dx + dy * dy) ** 0.5
            result.add_edge(s, e, cost=cost)
            continue

        # Build chain: s -> intermediate nodes -> e
        # pts[0] ≈ node s position, pts[-1] ≈ node e position
        # Use pts[1:-1] as new intermediate waypoints
        chain = [s]
        for wx, wy in pts[1:-1]:
            new_node = next_id
            next_id += 1
            result.add_node(new_node, world_x=float(wx), world_y=float(wy))
            chain.append(new_node)
        chain.append(e)

        # Add edges along the chain
        for i in range(len(chain) - 1):
            a, b = chain[i], chain[i + 1]
            da = result.nodes[a]
            db = result.nodes[b]
            dx = da.get("world_x", 0) - db.get("world_x", 0)
            dy = da.get("world_y", 0) - db.get("world_y", 0)
            cost = (dx * dx + dy * dy) ** 0.5
            result.add_edge(a, b, cost=cost)

    return result


def _point_line_distance(bx: float, by: float, ax: float, ay: float, cx: float, cy: float) -> float:
    """Perpendicular distance from point B to line AC."""
    dx = cx - ax
    dy = cy - ay
    len_sq = dx * dx + dy * dy
    if len_sq < 1e-12:
        return ((bx - ax) ** 2 + (by - ay) ** 2) ** 0.5
    return abs(dx * (ay - by) - dy * (ax - bx)) / (len_sq ** 0.5)


def eliminate_degree2_nodes(
    graph: nx.Graph,
    max_deviation: float = 3.0,
    max_edge_length: float = 50.0,
) -> nx.Graph:
    """Remove degree-2 nodes only on straight segments, preserving curves.

    For each degree-2 node B with neighbors A and C:
      - Compute perpendicular distance from B to line AC.
      - Only remove B if deviation < max_deviation AND dist(A,C) < max_edge_length.
    This keeps waypoints at road bends while simplifying straight runs.

    Parameters:
        max_deviation: max perpendicular distance (yards) to allow removal (default 3.0)
        max_edge_length: max resulting edge length (yards) to allow removal (default 50.0)
    """
    graph = graph.copy()
    changed = True
    while changed:
        changed = False
        for node in list(graph.nodes()):
            if node not in graph:
                continue
            if graph.degree(node) != 2:
                continue
            neighbors = list(graph.neighbors(node))
            if len(neighbors) != 2:
                continue
            a, c = neighbors

            # Get world positions
            da = graph.nodes[a]
            db = graph.nodes[node]
            dc = graph.nodes[c]
            ax, ay = da.get("world_x", 0.0), da.get("world_y", 0.0)
            bx, by = db.get("world_x", 0.0), db.get("world_y", 0.0)
            cx, cy = dc.get("world_x", 0.0), dc.get("world_y", 0.0)

            # Check if removing B would lose curvature
            deviation = _point_line_distance(bx, by, ax, ay, cx, cy)
            if deviation >= max_deviation:
                continue  # B is at a curve — keep it

            # Check resulting edge length
            dist_ac = ((ax - cx) ** 2 + (ay - cy) ** 2) ** 0.5
            if dist_ac >= max_edge_length:
                continue  # A-C too far apart — keep B as intermediate waypoint

            # Safe to remove: B is on a straight segment between nearby nodes
            cost_ab = 0.0
            cost_bc = 0.0
            for s, e, edata in graph.edges(node, data=True):
                cost = edata.get("cost", 0.0)
                if cost == 0.0:
                    ds = graph.nodes[s]
                    de = graph.nodes[e]
                    ddx = ds.get("world_x", 0) - de.get("world_x", 0)
                    ddy = ds.get("world_y", 0) - de.get("world_y", 0)
                    cost = (ddx * ddx + ddy * ddy) ** 0.5
                if e == a or s == a:
                    cost_ab = cost
                else:
                    cost_bc = cost

            graph.remove_node(node)
            if a in graph and c in graph and not graph.has_edge(a, c):
                graph.add_edge(a, c, cost=cost_ab + cost_bc)
            changed = True

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
    min_size: int = 50,
    close_radius: int = 3,
    open_radius: int = 1,
    min_road_width: int = 0,
    prune_iterations: int = 8,
    dp_epsilon: float = 2.0,
    merge_node_distance: float = 4.0,
    min_component_length: float = 30.0,
) -> nx.Graph:
    """Full pipeline: mask -> cleaned -> skeleton -> graph -> world coords -> simplified -> post-processed."""
    # 1. Cleanup (improved morphology)
    cleaned = cleanup_mask(mask, min_size=min_size,
                           close_radius=close_radius, open_radius=open_radius)

    # 2. Width filter
    if min_road_width > 1:
        cleaned = filter_by_width(cleaned, min_width_px=min_road_width)

    # 3. Skeletonize + prune
    skeleton = extract_skeleton(cleaned, prune_iterations=prune_iterations)

    if not skeleton.any():
        return nx.Graph()

    # 4. Skeleton -> graph
    graph = skeleton_to_graph(skeleton)
    if graph.number_of_nodes() == 0:
        return graph

    # 5. World coordinates
    graph = assign_world_coords(graph, origin_x, origin_y)

    # 6. Simplify edges
    graph = simplify_edges(graph, epsilon=dp_epsilon)

    # 7. Merge close nodes (intersection cleanup)
    if merge_node_distance > 0:
        graph = merge_close_nodes(graph, distance_threshold=merge_node_distance)

    # 8. Remove short isolated components
    if min_component_length > 0:
        graph = remove_short_components(graph, min_length=min_component_length)

    # 9. Collapse parallel paths
    graph = collapse_parallel_paths(graph)

    # 10. Expand edge polylines into actual waypoint nodes (preserves curves)
    graph = expand_edge_polylines(graph)

    # 11. Clean up only truly straight degree-2 nodes
    graph = eliminate_degree2_nodes(graph, max_deviation=2.0, max_edge_length=30.0)

    return graph
