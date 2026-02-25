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
    tile_group = parser.add_mutually_exclusive_group(required=False)
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
        "--prune-iterations", type=int, default=8,
        help="Spur pruning iterations (default: 8)"
    )
    parser.add_argument(
        "--dp-epsilon", type=float, default=2.0,
        help="Douglas-Peucker epsilon in yards (default: 2.0)"
    )
    parser.add_argument(
        "--min-confidence", type=float, default=0.5,
        help="Minimum classification confidence to include (default: 0.5)"
    )
    parser.add_argument(
        "--close-radius", type=int, default=3,
        help="Morphological closing radius in pixels (default: 3)"
    )
    parser.add_argument(
        "--open-radius", type=int, default=1,
        help="Morphological opening radius in pixels (default: 1)"
    )
    parser.add_argument(
        "--min-road-size", type=int, default=50,
        help="Minimum connected component size in pixels (default: 50)"
    )
    parser.add_argument(
        "--min-road-width", type=int, default=0,
        help="Minimum road width in pixels, 0=disabled (default: 0)"
    )
    parser.add_argument(
        "--water-buffer", type=int, default=5,
        help="Water zone erosion buffer in pixels (default: 5)"
    )
    parser.add_argument(
        "--dump-textures", action="store_true",
        help="Dump all unique textures with classification across all tiles"
    )
    parser.add_argument(
        "--diagnose", action="store_true",
        help="Run detailed per-tile diagnostics (pixel counts at each pipeline stage)"
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

    # --dump-textures mode: list all unique textures with classification
    if args.dump_textures:
        if not args.tiles and not args.all:
            args.all = True  # default to all tiles for dump modes
        _dump_all_textures(mpq, wdt, args, config_path)
        mpq.close()
        return

    # --diagnose mode: detailed per-tile pipeline diagnostics
    if args.diagnose:
        _diagnose_tiles(mpq, wdt, args, config_path)
        mpq.close()
        return

    # Require --tiles or --all for processing modes
    if not args.tiles and not args.all:
        print("ERROR: --tiles or --all is required", file=sys.stderr)
        sys.exit(1)

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
    from .road_mask import build_confidence_map, build_road_mask, build_water_mask
    from .visualizer import save_tile_outputs
    from .coord_utils import tile_origin

    tile_masks = {}
    for (tx, ty), adt in tile_adts.items():
        print(f"Building road mask for ({tx},{ty}), threshold={args.threshold}, min_confidence={args.min_confidence}")
        mask = build_road_mask(
            adt, wdt.mphd_flags,
            threshold=args.threshold,
            config=config,
            min_confidence=args.min_confidence,
        )

        # Subtract water zones
        water_mask = build_water_mask(adt, water_buffer=args.water_buffer)
        if water_mask.any():
            water_pixels = int(water_mask.sum())
            mask = mask & ~water_mask
            print(f"  Water mask: {water_pixels} pixels subtracted")

        conf = build_confidence_map(
            adt, wdt.mphd_flags,
            threshold=args.threshold,
            config=config,
            min_confidence=args.min_confidence,
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
            min_size=args.min_road_size,
            close_radius=args.close_radius,
            open_radius=args.open_radius,
            min_road_width=args.min_road_width,
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
    from .road_mask import build_road_mask, build_water_mask

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
                min_confidence=args.min_confidence,
            )

            # Subtract water zones
            water_mask = build_water_mask(adt, water_buffer=args.water_buffer)
            if water_mask.any():
                mask = mask & ~water_mask

            # Check if any road pixels exist
            if not mask.any():
                print(f"  [{i}/{total}] ({tx},{ty}) no roads")
                continue

            # Extract graph
            ox, oy = tile_origin(tx, ty)
            graph = extract_road_graph(
                mask, ox, oy,
                min_size=args.min_road_size,
                close_radius=args.close_radius,
                open_radius=args.open_radius,
                min_road_width=args.min_road_width,
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
        final_graph = merge_tile_graphs_spatial(tile_graphs, merge_distance=5.0)
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


def _diagnose_tiles(mpq, wdt, args, config_path):
    """Run detailed per-tile diagnostics to identify where road signal is lost."""
    from .adt_parser import parse_adt
    from .coord_utils import tile_origin
    from .mcal_decoder import decode_alpha, get_alpha_format
    from .road_classifier import RoadConfig, is_road_layer
    from .road_mask import build_road_mask, build_water_mask

    config = RoadConfig(config_path) if config_path else None

    # Collect tiles to diagnose
    tiles = []
    if args.tiles:
        tiles = args.tiles
    else:
        for y in range(64):
            for x in range(64):
                if wdt.tile_exists[y, x]:
                    tiles.append((x, y))

    print(f"=== DIAGNOSTIC MODE ===")
    print(f"MPHD flags: 0x{wdt.mphd_flags:08X}")
    print(f"Big alpha (8-bit): {wdt.big_alpha}")
    print(f"Alpha threshold: {args.threshold}")
    print(f"Min confidence: {args.min_confidence}")
    print(f"Tiles to diagnose: {len(tiles)}")
    print()

    summary = {"total": 0, "with_road_tex": 0, "with_raw_pixels": 0,
               "with_post_water": 0, "with_post_cleanup": 0}

    for i, (tx, ty) in enumerate(tiles, 1):
        adt_path = f"World\\Maps\\{args.map}\\{args.map}_{tx}_{ty}.adt"
        adt_data_bytes = mpq.read_file(adt_path)
        if adt_data_bytes is None:
            continue

        try:
            adt = parse_adt(adt_data_bytes, wdt.mphd_flags)
        except Exception as e:
            print(f"[{i}/{len(tiles)}] ({tx},{ty}) PARSE ERROR: {e}")
            continue

        summary["total"] += 1

        # --- Step 1: Classify all textures in this tile ---
        road_textures = {}  # tex_path -> {chunks, total_alpha_pixels, alpha_stats}
        total_road_layers = 0
        total_alpha_above = 0

        for chunk in adt.chunks:
            for layer_idx in range(1, len(chunk.layers)):
                layer = chunk.layers[layer_idx]
                if not (layer.flags & 0x100):
                    continue
                if layer.texture_id >= len(adt.mtex_list):
                    continue

                tex_path = adt.mtex_list[layer.texture_id]
                is_road, conf = is_road_layer(
                    tex_path, effect_id=layer.effect_id,
                    config=config, zone_hint="",
                )
                if not is_road or conf < args.min_confidence:
                    continue

                total_road_layers += 1

                # Decode alpha to check values
                next_offset = None
                if layer_idx + 1 < len(chunk.layers):
                    next_offset = chunk.layers[layer_idx + 1].offset_in_mcal
                else:
                    next_offset = chunk.mcal_size

                try:
                    alpha = decode_alpha(
                        chunk.mcal_data, layer.offset_in_mcal,
                        wdt.mphd_flags, layer.flags,
                        next_offset=next_offset,
                    )
                except (ValueError, IndexError):
                    continue

                fmt = get_alpha_format(wdt.mphd_flags, layer.flags)
                above = int((alpha > args.threshold).sum())
                total_alpha_above += above

                if tex_path not in road_textures:
                    road_textures[tex_path] = {
                        "conf": conf, "chunks": 0, "above": 0,
                        "fmt": fmt, "alpha_min": 255, "alpha_max": 0,
                        "alpha_sum": 0, "alpha_count": 0,
                    }
                rt = road_textures[tex_path]
                rt["chunks"] += 1
                rt["above"] += above
                rt["alpha_min"] = min(rt["alpha_min"], int(alpha.min()))
                rt["alpha_max"] = max(rt["alpha_max"], int(alpha.max()))
                rt["alpha_sum"] += int(alpha.sum())
                rt["alpha_count"] += alpha.size

        if not road_textures:
            if (i % 100) == 0:
                print(f"[{i}/{len(tiles)}] ({tx},{ty}) no road textures")
            continue

        summary["with_road_tex"] += 1

        # --- Step 2: Build raw road mask ---
        raw_mask = build_road_mask(
            adt, wdt.mphd_flags, threshold=args.threshold,
            config=config, min_confidence=args.min_confidence,
        )
        raw_pixels = int(raw_mask.sum())

        if raw_pixels == 0:
            print(f"[{i}/{len(tiles)}] ({tx},{ty}) "
                  f"road textures={len(road_textures)} layers={total_road_layers} "
                  f"alpha_above_threshold=0 RAW_MASK=0")
            for tp, rt in road_textures.items():
                basename = os.path.basename(tp)
                avg_alpha = rt["alpha_sum"] / rt["alpha_count"] if rt["alpha_count"] > 0 else 0
                print(f"    {basename}: conf={rt['conf']:.1f} fmt={rt['fmt']} "
                      f"chunks={rt['chunks']} above={rt['above']} "
                      f"alpha=[{rt['alpha_min']},{rt['alpha_max']}] "
                      f"avg={avg_alpha:.0f}")
            continue

        summary["with_raw_pixels"] += 1

        # --- Step 3: Water mask ---
        water_mask = build_water_mask(adt, water_buffer=args.water_buffer)
        post_water = raw_mask & ~water_mask if water_mask.any() else raw_mask
        post_water_pixels = int(post_water.sum())
        water_removed = raw_pixels - post_water_pixels

        if post_water_pixels > 0:
            summary["with_post_water"] += 1

        # --- Step 4: Morphological cleanup ---
        from .graph_extractor import cleanup_mask
        cleaned = cleanup_mask(
            post_water,
            min_size=args.min_road_size,
            close_radius=args.close_radius,
            open_radius=args.open_radius,
        )
        cleaned_pixels = int(cleaned.sum())

        if cleaned_pixels > 0:
            summary["with_post_cleanup"] += 1

        # --- Print results ---
        print(f"[{i}/{len(tiles)}] ({tx},{ty}) "
              f"tex={len(road_textures)} layers={total_road_layers} "
              f"raw={raw_pixels} water-={water_removed} "
              f"post_water={post_water_pixels} cleanup={cleaned_pixels}")

        # Show per-texture details for tiles with many road pixels
        if raw_pixels > 100 or len(tiles) <= 10:
            for tp, rt in sorted(road_textures.items(),
                                  key=lambda kv: -kv[1]["above"]):
                basename = os.path.basename(tp)
                avg_alpha = rt["alpha_sum"] / rt["alpha_count"] if rt["alpha_count"] > 0 else 0
                print(f"    {basename}: conf={rt['conf']:.1f} fmt={rt['fmt']} "
                      f"chunks={rt['chunks']} above={rt['above']} "
                      f"alpha=[{rt['alpha_min']},{rt['alpha_max']}] "
                      f"avg={avg_alpha:.0f}")

    # --- Summary ---
    print(f"\n=== SUMMARY ===")
    print(f"Total tiles parsed: {summary['total']}")
    print(f"Tiles with road textures: {summary['with_road_tex']}")
    print(f"Tiles with raw road pixels (alpha > {args.threshold}): {summary['with_raw_pixels']}")
    print(f"Tiles with road pixels after water subtraction: {summary['with_post_water']}")
    print(f"Tiles with road pixels after morphological cleanup: {summary['with_post_cleanup']}")


def _dump_all_textures(mpq, wdt, args, config_path):
    """Dump all unique textures across all tiles with classification."""
    from .adt_parser import parse_adt
    from .road_classifier import RoadConfig, is_road_layer

    config = RoadConfig(config_path) if config_path else None

    # Collect all valid tiles
    tiles = []
    if args.tiles:
        tiles = args.tiles
    else:
        for y in range(64):
            for x in range(64):
                if wdt.tile_exists[y, x]:
                    tiles.append((x, y))

    # texture_path -> {classification, chunk_count}
    tex_stats: dict[str, dict] = {}

    total = len(tiles)
    for i, (tx, ty) in enumerate(tiles, 1):
        adt_path = f"World\\Maps\\{args.map}\\{args.map}_{tx}_{ty}.adt"
        adt_data_bytes = mpq.read_file(adt_path)
        if adt_data_bytes is None:
            continue

        try:
            adt = parse_adt(adt_data_bytes, wdt.mphd_flags)
        except Exception:
            continue

        if (i % 50) == 0 or i == total:
            print(f"  Scanning textures... [{i}/{total}]", file=sys.stderr)

        for chunk in adt.chunks:
            for li, layer in enumerate(chunk.layers):
                if layer.texture_id >= len(adt.mtex_list):
                    continue
                tex = adt.mtex_list[layer.texture_id]

                if tex not in tex_stats:
                    is_road, conf = is_road_layer(
                        tex, effect_id=layer.effect_id,
                        config=config,
                    )
                    if is_road and conf >= 1.0:
                        cls = "road_high"
                    elif is_road and conf >= 0.6:
                        cls = "road_medium"
                    elif is_road:
                        cls = "road_low"
                    else:
                        cls = "not_road"
                    tex_stats[tex] = {"cls": cls, "conf": conf, "chunks": 0}

                tex_stats[tex]["chunks"] += 1

    # Sort: road_high first, then road_medium, then not_road, then by chunk count
    cls_order = {"road_high": 0, "road_medium": 1, "road_low": 2, "not_road": 3}
    sorted_textures = sorted(
        tex_stats.items(),
        key=lambda kv: (cls_order.get(kv[1]["cls"], 9), -kv[1]["chunks"]),
    )

    print(f"\n{'Classification':<14} {'Chunks':>8}  Texture Path")
    print("-" * 80)
    for tex_path, info in sorted_textures:
        print(f"{info['cls']:<14} {info['chunks']:>8}  {tex_path}")

    # Summary
    road_count = sum(1 for _, v in tex_stats.items() if v["cls"].startswith("road"))
    total_tex = len(tex_stats)
    print(f"\nTotal: {total_tex} unique textures, {road_count} classified as road")


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
