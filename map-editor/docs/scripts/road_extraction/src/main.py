#!/usr/bin/env python3
"""
main.py — Road network extraction CLI.

Extract road networks from WoW 3.3.5a ADT terrain files.

Usage:
  # Single tile (Goldshire area)
  python -m src.main --client "Z:\\Games\\wow 3.3.5a client" --map Azeroth --tiles 31,48

  # Multiple tiles (Elwynn Forest roads)
  python -m src.main --client "Z:\\Games\\wow 3.3.5a client" --map Azeroth --tiles 31,48 31,49 32,48 32,49

  # Just parse and dump info (no graph extraction)
  python -m src.main --client "Z:\\Games\\wow 3.3.5a client" --map Azeroth --tiles 31,48 --dump-info

  # Adjust threshold
  python -m src.main --client "Z:\\Games\\wow 3.3.5a client" --map Azeroth --tiles 31,48 --threshold 48

  # Full-map extraction (all tiles)
  python -m src.main --client "Z:\\Games\\wow 3.3.5a client" --map Azeroth --all
"""

import argparse
import os
import sys

import numpy as np

# WoW DBC map IDs for known continents
MAP_IDS = {
    "Azeroth": 0,       # Eastern Kingdoms
    "Kalimdor": 1,
    "Expansion01": 530,  # Outland
    "Northrend": 571,
}


def parse_tile_arg(s: str) -> tuple[int, int]:
    """Parse 'X,Y' into (tileX, tileY)."""
    parts = s.split(",")
    if len(parts) != 2:
        raise argparse.ArgumentTypeError(f"Expected X,Y format, got '{s}'")
    return int(parts[0]), int(parts[1])


def main():
    parser = argparse.ArgumentParser(
        description="Extract road networks from WoW 3.3.5a ADT terrain files"
    )
    parser.add_argument(
        "--client", required=True,
        help="Path to WoW 3.3.5a client directory"
    )
    parser.add_argument(
        "--map", default="Azeroth",
        help="Map internal name (default: Azeroth)"
    )
    tile_group = parser.add_mutually_exclusive_group(required=True)
    tile_group.add_argument(
        "--tiles", nargs="+", type=parse_tile_arg,
        help="Tile coordinates as X,Y (e.g., 31,48 31,49)"
    )
    tile_group.add_argument(
        "--all", action="store_true",
        help="Process ALL tiles in the map (full-map extraction)"
    )
    parser.add_argument(
        "--threshold", type=int, default=64,
        help="Alpha threshold for road detection (0-255, default: 64)"
    )
    parser.add_argument(
        "--output-dir", default=None,
        help="Output directory (default: ./output)"
    )
    parser.add_argument(
        "--config", default=None,
        help="Path to road_textures.json config file"
    )
    parser.add_argument(
        "--dump-info", action="store_true",
        help="Only parse and dump tile info (no mask/graph extraction)"
    )
    parser.add_argument(
        "--no-graph", action="store_true",
        help="Generate masks and images but skip graph extraction"
    )
    parser.add_argument(
        "--prune-iterations", type=int, default=3,
        help="Spur pruning iterations (default: 3)"
    )
    parser.add_argument(
        "--dp-epsilon", type=float, default=2.0,
        help="Douglas-Peucker epsilon in yards (default: 2.0)"
    )

    args = parser.parse_args()

    # Resolve paths
    script_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    output_dir = args.output_dir or os.path.join(script_dir, "output")
    os.makedirs(output_dir, exist_ok=True)

    config_path = args.config
    if not config_path:
        default_config = os.path.join(script_dir, "config", "road_textures.json")
        if os.path.exists(default_config):
            config_path = default_config

    # ---------------------------------------------------------------------------
    # Step 1: Open MPQ archives
    # ---------------------------------------------------------------------------
    from .mpq_reader import MpqArchiveSet

    print(f"Opening MPQ archives from: {args.client}")
    mpq = MpqArchiveSet()
    if not mpq.open(args.client):
        print("ERROR: Failed to open any MPQ archives", file=sys.stderr)
        sys.exit(1)
    print(f"  Opened {len(mpq._archives)} archives")

    # ---------------------------------------------------------------------------
    # Step 2: Parse WDT
    # ---------------------------------------------------------------------------
    from .wdt_parser import parse_wdt

    wdt_path = f"World\\Maps\\{args.map}\\{args.map}.wdt"
    print(f"Reading WDT: {wdt_path}")
    wdt_data = mpq.read_file(wdt_path)
    if wdt_data is None:
        print(f"ERROR: WDT not found: {wdt_path}", file=sys.stderr)
        sys.exit(1)

    wdt = parse_wdt(wdt_data)
    print(f"  MPHD flags: 0x{wdt.mphd_flags:08X}")
    print(f"  Big alpha: {wdt.big_alpha}")
    tile_count = int(wdt.tile_exists.sum())
    print(f"  Tiles with data: {tile_count}")

    # --all mode: process every tile in the map
    if args.all:
        _process_all_tiles(mpq, wdt, args, output_dir, config_path)
        mpq.close()
        return

    # Validate requested tiles exist
    for tx, ty in args.tiles:
        if not wdt.tile_exists[ty, tx]:
            print(f"WARNING: Tile ({tx},{ty}) does not exist in WDT", file=sys.stderr)

    # ---------------------------------------------------------------------------
    # Step 3: Parse ADTs
    # ---------------------------------------------------------------------------
    from .adt_parser import parse_adt
    from .road_classifier import RoadConfig

    config = RoadConfig(config_path) if config_path else None

    tile_adts = {}
    for tx, ty in args.tiles:
        adt_path = f"World\\Maps\\{args.map}\\{args.map}_{tx}_{ty}.adt"
        print(f"Reading ADT: {adt_path}")
        adt_data_bytes = mpq.read_file(adt_path)
        if adt_data_bytes is None:
            print(f"  WARNING: ADT not found, skipping", file=sys.stderr)
            continue

        adt = parse_adt(adt_data_bytes, wdt.mphd_flags)
        tile_adts[(tx, ty)] = adt

        print(f"  Textures: {len(adt.mtex_list)}")
        print(f"  Chunks: {len(adt.chunks)}")

        if args.dump_info:
            _dump_tile_info(adt, wdt.mphd_flags, config)

    if args.dump_info:
        mpq.close()
        return

    # ---------------------------------------------------------------------------
    # Step 4: Build road masks
    # ---------------------------------------------------------------------------
    from .road_mask import build_confidence_map, build_road_mask
    from .visualizer import save_tile_outputs
    from .coord_utils import tile_origin

    tile_masks = {}
    for (tx, ty), adt in tile_adts.items():
        print(f"Building road mask for ({tx},{ty}), threshold={args.threshold}")
        mask = build_road_mask(
            adt, wdt.mphd_flags,
            threshold=args.threshold,
            config=config,
        )
        conf = build_confidence_map(
            adt, wdt.mphd_flags,
            threshold=args.threshold,
            config=config,
        )
        tile_masks[(tx, ty)] = mask

        road_pixels = int(mask.sum())
        total_pixels = mask.shape[0] * mask.shape[1]
        pct = road_pixels / total_pixels * 100
        print(f"  Road pixels: {road_pixels}/{total_pixels} ({pct:.1f}%)")

        # Save visualizations
        save_tile_outputs(mask, conf, adt, wdt.mphd_flags, args.map, tx, ty, output_dir)
        print(f"  Saved images to {output_dir}")

    # ---------------------------------------------------------------------------
    # Step 5: Multi-tile stitching
    # ---------------------------------------------------------------------------
    if len(tile_masks) > 1:
        from .stitcher import save_stitched_image, stitch_tiles

        print("Stitching tiles...")
        raster, min_tx, min_ty, max_tx, max_ty = stitch_tiles(tile_masks)
        stitch_path = os.path.join(output_dir, f"{args.map}_stitched.png")
        scale = save_stitched_image(raster, stitch_path)
        print(f"  Saved stitched image (scale 1:{scale}): {stitch_path}")

    if args.no_graph:
        print("Skipping graph extraction (--no-graph)")
        mpq.close()
        return

    # ---------------------------------------------------------------------------
    # Step 6: Extract road graphs
    # ---------------------------------------------------------------------------
    from .graph_extractor import (
        export_graph_json,
        extract_road_graph,
        merge_tile_graphs,
    )

    tile_graphs = []
    for (tx, ty), mask in tile_masks.items():
        print(f"Extracting graph for ({tx},{ty})...")
        ox, oy = tile_origin(tx, ty)
        graph = extract_road_graph(
            mask, ox, oy,
            prune_iterations=args.prune_iterations,
            dp_epsilon=args.dp_epsilon,
        )
        print(f"  Nodes: {graph.number_of_nodes()}, Edges: {graph.number_of_edges()}")
        tile_graphs.append(graph)

    # Merge if multiple tiles
    if len(tile_graphs) > 1:
        print("Merging tile graphs...")
        final_graph = merge_tile_graphs(tile_graphs)
        print(f"  Merged: {final_graph.number_of_nodes()} nodes, {final_graph.number_of_edges()} edges")
    elif tile_graphs:
        final_graph = tile_graphs[0]
    else:
        print("No graphs to export")
        mpq.close()
        return

    # Export JSON (map-editor compatible format)
    map_id = MAP_IDS.get(args.map, 0)
    json_path = os.path.join(output_dir, f"{args.map}_roads.json")
    export_graph_json(
        final_graph, json_path,
        map_id=map_id,
        map_name=args.map,
        tiles=list(tile_masks.keys()),
        threshold=args.threshold,
    )
    print(f"Exported road graph: {json_path}")
    print(f"  {final_graph.number_of_nodes()} nodes, {final_graph.number_of_edges()} edges")

    mpq.close()
    print("Done!")


def _process_all_tiles(mpq, wdt, args, output_dir, config_path):
    """Process ALL tiles in the map — streaming, memory-efficient."""
    from .adt_parser import parse_adt
    from .coord_utils import tile_origin
    from .graph_extractor import (
        export_graph_json,
        extract_road_graph,
        merge_tile_graphs_spatial,
    )
    from .road_classifier import RoadConfig
    from .road_mask import build_road_mask

    config = RoadConfig(config_path) if config_path else None

    # Collect all valid tiles from WDT
    tiles = []
    for y in range(64):
        for x in range(64):
            if wdt.tile_exists[y, x]:
                tiles.append((x, y))  # (tx, ty) matching existing convention

    total = len(tiles)
    print(f"Processing {total} tiles for map {args.map}...")

    tile_graphs = []
    tiles_with_roads = 0
    tiles_skipped = 0
    total_nodes = 0
    total_edges = 0

    for i, (tx, ty) in enumerate(tiles, 1):
        try:
            # Read ADT
            adt_path = f"World\\Maps\\{args.map}\\{args.map}_{tx}_{ty}.adt"
            adt_data_bytes = mpq.read_file(adt_path)
            if adt_data_bytes is None:
                print(f"  [{i}/{total}] ({tx},{ty}) ADT not found, skipping")
                tiles_skipped += 1
                continue

            adt = parse_adt(adt_data_bytes, wdt.mphd_flags)

            # Build road mask
            mask = build_road_mask(
                adt, wdt.mphd_flags,
                threshold=args.threshold,
                config=config,
            )

            # Check if any road pixels exist
            if not mask.any():
                print(f"  [{i}/{total}] ({tx},{ty}) no roads")
                continue

            # Extract graph
            ox, oy = tile_origin(tx, ty)
            graph = extract_road_graph(
                mask, ox, oy,
                prune_iterations=args.prune_iterations,
                dp_epsilon=args.dp_epsilon,
            )

            if graph.number_of_nodes() > 0:
                tile_graphs.append(graph)
                tiles_with_roads += 1
                n = graph.number_of_nodes()
                e = graph.number_of_edges()
                total_nodes += n
                total_edges += e
                print(f"  [{i}/{total}] ({tx},{ty}) {n} nodes, {e} edges")
            else:
                print(f"  [{i}/{total}] ({tx},{ty}) no roads")

        except Exception as exc:
            print(f"  [{i}/{total}] ({tx},{ty}) ERROR: {exc}", file=sys.stderr)
            tiles_skipped += 1

    # Merge all tile graphs
    if not tile_graphs:
        print("No road graphs extracted from any tile")
        return

    print(f"\nMerging {len(tile_graphs)} tile graphs (spatial merge)...")
    if len(tile_graphs) > 1:
        final_graph = merge_tile_graphs_spatial(tile_graphs)
    else:
        final_graph = tile_graphs[0]

    print(f"  Merged: {final_graph.number_of_nodes()} nodes, {final_graph.number_of_edges()} edges")

    # Export JSON (map-editor compatible format)
    map_id = MAP_IDS.get(args.map, 0)
    json_path = os.path.join(output_dir, f"{args.map}_roads.json")
    export_graph_json(
        final_graph, json_path,
        map_id=map_id,
        map_name=args.map,
        tiles=tiles,
        threshold=args.threshold,
    )
    print(f"\nExported: {json_path}")
    print(f"  {final_graph.number_of_nodes()} nodes, {final_graph.number_of_edges()} edges")
    print(f"  Tiles: {tiles_with_roads}/{total} with roads"
          f" ({tiles_skipped} skipped)")
    print("Done!")


def _dump_tile_info(adt, wdt_mphd_flags: int, config=None):
    """Dump detailed tile info for debugging."""
    from .road_classifier import classify_texture, is_road_layer

    print("\n  --- MTEX texture list ---")
    for i, tex in enumerate(adt.mtex_list):
        cls = classify_texture(tex)
        marker = " *ROAD*" if cls.startswith("road") else ""
        print(f"    [{i:3d}] {tex} -> {cls}{marker}")

    print("\n  --- MCNK chunks with road layers ---")
    for chunk in adt.chunks:
        road_layers = []
        for li, layer in enumerate(chunk.layers):
            if li == 0:
                continue
            if layer.texture_id >= len(adt.mtex_list):
                continue
            tex = adt.mtex_list[layer.texture_id]
            is_road, conf = is_road_layer(tex, config=config)
            if is_road:
                road_layers.append((li, tex, conf))

        if road_layers:
            print(f"    Chunk({chunk.index_x},{chunk.index_y}) "
                  f"pos=({chunk.position_x:.1f}, {chunk.position_y:.1f}) "
                  f"layers={chunk.n_layers}")
            for li, tex, conf in road_layers:
                print(f"      Layer {li}: {os.path.basename(tex)} conf={conf:.1f}")

    print()


if __name__ == "__main__":
    main()
