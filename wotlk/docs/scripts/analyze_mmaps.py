"""
Analyze mmaps directory — show tile coverage, find gaps, check positions.

Usage:
    python analyze_mmaps.py <mmaps_dir> [--map <id>] [--pos <x> <y>] [--visual]

Examples:
    python analyze_mmaps.py "Z:\Games\wow 3.3.5a client\wotlk\mmaps"
    python analyze_mmaps.py "Z:\Games\wow 3.3.5a client\wotlk\mmaps" --map 0 --visual
    python analyze_mmaps.py "Z:\Games\wow 3.3.5a client\wotlk\mmaps" --pos -8891.55 -82.03
"""

import os
import sys
import struct
import math
import argparse
from collections import defaultdict

TILE_SIZE = 533.33333

# Known map names (WotLK 3.3.5a)
MAP_NAMES = {
    0: "Eastern Kingdoms", 1: "Kalimdor", 30: "Alterac Valley",
    33: "Shadowfang Keep", 34: "Stormwind Stockade", 35: "Stormwind Prison (unused)",
    36: "Deadmines", 37: "Azshara Crater (unused)", 43: "Wailing Caverns",
    44: "Monastery (unused)", 47: "Razorfen Kraul", 48: "Blackfathom Deeps",
    70: "Uldaman", 90: "Gnomeregan", 109: "Sunken Temple",
    129: "Razorfen Downs", 189: "Scarlet Monastery", 209: "Zul'Farrak",
    229: "Blackrock Spire", 230: "Blackrock Depths", 249: "Onyxia's Lair",
    269: "Opening of the Dark Portal", 289: "Scholomance", 309: "Zul'Gurub",
    329: "Stratholme", 349: "Maraudon", 369: "Deeprun Tram",
    389: "Ragefire Chasm", 409: "Molten Core", 429: "Dire Maul",
    449: "Alliance PvP Barracks", 450: "Horde PvP Barracks",
    469: "Blackwing Lair", 489: "Warsong Gulch", 509: "Ruins of Ahn'Qiraj",
    529: "Arathi Basin", 530: "Outland", 531: "Ahn'Qiraj Temple",
    532: "Karazhan", 533: "Naxxramas", 534: "Hyjal Past",
    540: "Hellfire Ramparts", 542: "Blood Furnace", 543: "Hellfire Citadel",
    544: "Magtheridon's Lair", 545: "The Steamvault", 546: "The Underbog",
    547: "The Slave Pens", 548: "Serpentshrine Cavern", 550: "Tempest Keep",
    552: "The Arcatraz", 553: "The Botanica", 554: "The Mechanar",
    555: "Shadow Labyrinth", 556: "Sethekk Halls", 557: "Mana-Tombs",
    558: "Auchenai Crypts", 559: "Nagrand Arena", 560: "Old Hillsbrad Foothills",
    562: "Blade's Edge Arena", 564: "Black Temple", 565: "Gruul's Lair",
    566: "Eye of the Storm", 568: "Zul'Aman", 571: "Northrend",
    572: "Ruins of Lordaeron", 574: "Utgarde Keep", 575: "Utgarde Pinnacle",
    576: "The Nexus", 578: "The Oculus", 580: "Sunwell Plateau",
    585: "Magisters' Terrace", 595: "Culling of Stratholme",
    598: "Sunwell Fix (unused)", 599: "Halls of Stone",
    600: "Drak'Tharon Keep", 601: "Azjol-Nerub", 602: "Halls of Lightning",
    603: "Ulduar", 604: "Gundrak", 607: "Strand of the Ancients",
    608: "Violet Hold", 609: "Ebon Hold", 615: "Obsidian Sanctum",
    616: "Eye of Eternity", 617: "Dalaran Sewers", 618: "Ring of Valor",
    619: "Ahn'kahet", 624: "Vault of Archavon",
    628: "Isle of Conquest", 631: "Icecrown Citadel",
    632: "The Forge of Souls", 649: "Trial of the Crusader",
    650: "Trial of the Champion", 658: "Pit of Saron",
    668: "Halls of Reflection", 723: "Stormwind", 724: "The Ruby Sanctum",
}

# Notable WoW locations for reference
KNOWN_LOCATIONS = {
    "Stormwind": (-8913, -133, 0),
    "Ironforge": (-4981, -881, 0),
    "Darnassus": (9947, 2482, 1),
    "Orgrimmar": (1629, -4373, 1),
    "Thunder Bluff": (-1278, 122, 1),
    "Undercity": (1586, 239, 0),
    "Shattrath": (-1838, 5301, 530),
    "Dalaran": (5804, 624, 571),
    "Goldshire": (-9459, 62, 0),
    "Crossroads": (-441, -2596, 1),
    "Booty Bay": (-14354, 516, 0),
    "Light's Hope Chapel": (2280, -5310, 0),
    "Theramore": (-3727, -4515, 1),
}


def wow_to_tile(x, y):
    """Convert WoW world coords to tile coords."""
    tx = 32 - int(math.floor(x / TILE_SIZE))
    ty = 32 - int(math.floor(y / TILE_SIZE))
    return tx, ty


def tile_to_wow_center(tx, ty):
    """Get WoW world coords for tile center."""
    x = (32 - tx) * TILE_SIZE + TILE_SIZE / 2
    y = (32 - ty) * TILE_SIZE + TILE_SIZE / 2
    return x, y


def parse_mmtile_name(name):
    """Parse mmtile filename -> (mapId, tileX, tileY) or None.

    TrinityCore format: {mapId:03d}{gridX:02d}{gridY:02d}.mmtile
    where gridX = 32 - floor(wowX / 533.333), gridY = 32 - floor(wowY / 533.333)
    """
    base = name.replace('.mmtile', '')
    if len(base) != 7 or not base.isdigit():
        return None
    map_id = int(base[0:3])
    tile_x = int(base[3:5])  # gridX from WoW X (north-south)
    tile_y = int(base[5:7])  # gridY from WoW Y (east-west)
    return map_id, tile_x, tile_y


def scan_directory(mmaps_dir):
    """Scan mmaps dir, return {map_id: set((tileX, tileY))} and set of mmap headers."""
    tiles = defaultdict(set)
    headers = set()

    for fname in os.listdir(mmaps_dir):
        if fname.endswith('.mmtile'):
            parsed = parse_mmtile_name(fname)
            if parsed:
                map_id, tx, ty = parsed
                tiles[map_id].add((tx, ty))
        elif fname.endswith('.mmap'):
            base = fname.replace('.mmap', '')
            if base.isdigit():
                headers.add(int(base))

    return tiles, headers


def check_mmap_header(mmaps_dir, map_id):
    """Read and validate .mmap header file."""
    fname = os.path.join(mmaps_dir, f"{map_id:03d}.mmap")
    if not os.path.exists(fname):
        return None

    size = os.path.getsize(fname)
    with open(fname, 'rb') as f:
        data = f.read()

    info = {'file': fname, 'size': size}

    MMAP_MAGIC = 0x4D4D4150

    if size >= 40:
        magic, version = struct.unpack_from('<II', data, 0)
        if magic == MMAP_MAGIC:
            info['format'] = 'new'
            info['magic'] = magic
            info['version'] = version
            orig = struct.unpack_from('<3f', data, 8)
            tw, th = struct.unpack_from('<2f', data, 20)
            mt, mp = struct.unpack_from('<2i', data, 28)
            info['params'] = {
                'orig': orig, 'tileWidth': tw, 'tileHeight': th,
                'maxTiles': mt, 'maxPolys': mp
            }
            return info

    if size == 28:
        orig = struct.unpack_from('<3f', data, 0)
        tw, th = struct.unpack_from('<2f', data, 12)
        mt, mp = struct.unpack_from('<2i', data, 20)
        info['format'] = 'legacy'
        info['params'] = {
            'orig': orig, 'tileWidth': tw, 'tileHeight': th,
            'maxTiles': mt, 'maxPolys': mp
        }
        return info

    info['format'] = 'unknown'
    return info


def print_summary(mmaps_dir, tiles, headers):
    """Print overall summary."""
    total_tiles = sum(len(t) for t in tiles.values())
    print(f"=== MMAPS Analysis: {mmaps_dir} ===\n")
    print(f"Map headers (.mmap): {len(headers)}")
    print(f"Total tiles (.mmtile): {total_tiles}")
    print(f"Maps with tiles: {len(tiles)}\n")

    print(f"{'Map':>5}  {'Name':<30}  {'Header':>6}  {'Tiles':>6}  {'TX range':>12}  {'TY range':>12}")
    print("-" * 85)

    for map_id in sorted(set(list(tiles.keys()) + list(headers))):
        name = MAP_NAMES.get(map_id, "???")
        has_header = "YES" if map_id in headers else "NO"
        tile_set = tiles.get(map_id, set())
        n = len(tile_set)

        if n > 0:
            txs = [t[0] for t in tile_set]
            tys = [t[1] for t in tile_set]
            tx_range = f"{min(txs):2d}-{max(txs):2d}"
            ty_range = f"{min(tys):2d}-{max(tys):2d}"
        else:
            tx_range = "-"
            ty_range = "-"

        print(f"{map_id:>5}  {name:<30}  {has_header:>6}  {n:>6}  {tx_range:>12}  {ty_range:>12}")


def print_map_detail(mmaps_dir, map_id, tile_set):
    """Print detailed info for a specific map."""
    name = MAP_NAMES.get(map_id, "???")
    print(f"\n=== Map {map_id}: {name} ===\n")

    # Header info
    header = check_mmap_header(mmaps_dir, map_id)
    if header:
        print(f"Header: {header['file']} ({header['size']} bytes, {header.get('format', '?')} format)")
        if 'params' in header:
            p = header['params']
            print(f"  orig: ({p['orig'][0]:.1f}, {p['orig'][1]:.1f}, {p['orig'][2]:.1f})")
            print(f"  tileWidth: {p['tileWidth']:.3f}  tileHeight: {p['tileHeight']:.3f}")
            print(f"  maxTiles: {p['maxTiles']}  maxPolys: 0x{p['maxPolys'] & 0xFFFFFFFF:08X}")
    else:
        print("Header: MISSING")

    if not tile_set:
        print("\nNo tiles found!")
        return

    txs = sorted(set(t[0] for t in tile_set))
    tys = sorted(set(t[1] for t in tile_set))

    print(f"\nTiles: {len(tile_set)}")
    print(f"tileX range: {min(txs)}-{max(txs)} ({len(txs)} unique)")
    print(f"tileY range: {min(tys)}-{max(tys)} ({len(tys)} unique)")

    # Bounding box in WoW coords
    wx1 = (32 - max(txs)) * TILE_SIZE
    wx2 = (32 - min(txs) + 1) * TILE_SIZE
    wy1 = (32 - max(tys)) * TILE_SIZE
    wy2 = (32 - min(tys) + 1) * TILE_SIZE
    print(f"WoW X coverage: {wx1:.0f} to {wx2:.0f}")
    print(f"WoW Y coverage: {wy1:.0f} to {wy2:.0f}")

    # Check known locations
    print(f"\nKnown locations:")
    for loc_name, (lx, ly, lmap) in sorted(KNOWN_LOCATIONS.items()):
        if lmap != map_id:
            continue
        ltx, lty = wow_to_tile(lx, ly)
        has = (ltx, lty) in tile_set
        # Check 3x3
        count_3x3 = sum(1 for dx in range(-1, 2) for dy in range(-1, 2)
                        if (ltx + dx, lty + dy) in tile_set)
        status = f"OK ({count_3x3}/9 tiles)" if has else f"MISSING (tile {ltx},{lty})"
        print(f"  {loc_name:<25} WoW({lx:>7.0f}, {ly:>7.0f}) -> tile({ltx},{lty}): {status}")

    # Coverage density
    full_rect = (max(txs) - min(txs) + 1) * (max(tys) - min(tys) + 1)
    density = len(tile_set) / full_rect * 100 if full_rect > 0 else 0
    print(f"\nDensity: {len(tile_set)}/{full_rect} = {density:.1f}% of bounding rectangle")


def print_visual(map_id, tile_set):
    """Print ASCII visual map of tile coverage."""
    if not tile_set:
        print("No tiles to visualize.")
        return

    txs = sorted(set(t[0] for t in tile_set))
    tys = sorted(set(t[1] for t in tile_set))

    min_tx, max_tx = min(txs), max(txs)
    min_ty, max_ty = min(tys), max(tys)

    name = MAP_NAMES.get(map_id, "???")
    print(f"\n=== Visual: Map {map_id} ({name}) ===")
    print(f"    X={min_tx} .. {max_tx}  (left=west, right=east)")
    print(f"    Y={min_ty} .. {max_ty}  (top=north, bottom=south)")

    # Mark known locations
    loc_markers = {}
    for loc_name, (lx, ly, lmap) in KNOWN_LOCATIONS.items():
        if lmap != map_id:
            continue
        ltx, lty = wow_to_tile(lx, ly)
        if min_tx <= ltx <= max_tx and min_ty <= lty <= max_ty:
            loc_markers[(ltx, lty)] = loc_name[0]  # first letter

    # Header with X coords (every 5)
    header = "     "
    for tx in range(min_tx, max_tx + 1):
        if tx % 5 == 0:
            header += f"{tx:<2}"[-2:]
        else:
            header += "  "
    print(header)

    for ty in range(min_ty, max_ty + 1):
        row = f"{ty:3d}  "
        for tx in range(min_tx, max_tx + 1):
            if (tx, ty) in tile_set:
                marker = loc_markers.get((tx, ty), None)
                if marker:
                    row += f"\033[92m{marker}\033[0m "  # green letter
                else:
                    row += "\033[90m#\033[0m "  # dim hash
            else:
                marker = loc_markers.get((tx, ty), None)
                if marker:
                    row += f"\033[91m{marker}\033[0m "  # red letter = missing
                else:
                    row += ". "
        print(row)

    # Legend
    print(f"\n  \033[90m#\033[0m = tile present  . = tile missing")
    print(f"  \033[92mX\033[0m = location (has tile)  \033[91mX\033[0m = location (MISSING tile)")
    for loc_name, (lx, ly, lmap) in sorted(KNOWN_LOCATIONS.items()):
        if lmap != map_id:
            continue
        ltx, lty = wow_to_tile(lx, ly)
        if min_tx - 3 <= ltx <= max_tx + 3 and min_ty - 3 <= lty <= max_ty + 3:
            in_set = "present" if (ltx, lty) in tile_set else "MISSING"
            print(f"  {loc_name[0]} = {loc_name} ({in_set})")


def check_position(tile_set, x, y, map_id):
    """Check if a specific WoW position has tile coverage."""
    tx, ty = wow_to_tile(x, y)
    print(f"\n=== Position Check ===")
    print(f"WoW coords: ({x:.2f}, {y:.2f}) on map {map_id} ({MAP_NAMES.get(map_id, '???')})")
    print(f"Tile coords: ({tx}, {ty})")

    if not tile_set:
        print(f"No tiles loaded for map {map_id}!")
        return

    has_center = (tx, ty) in tile_set
    print(f"Center tile ({tx},{ty}): {'PRESENT' if has_center else 'MISSING'}")

    print(f"3x3 neighborhood:")
    for dy in range(-1, 2):
        row = "  "
        for dx in range(-1, 2):
            ntx, nty = tx + dx, ty + dy
            has = (ntx, nty) in tile_set
            marker = "OK" if has else "--"
            if dx == 0 and dy == 0:
                marker = f"[{marker}]"
            else:
                marker = f" {marker} "
            row += marker + " "
        print(row)

    # Find nearest tile that exists
    if not has_center and tile_set:
        min_dist = float('inf')
        nearest = None
        for (ntx, nty) in tile_set:
            d = math.sqrt((ntx - tx) ** 2 + (nty - ty) ** 2)
            if d < min_dist:
                min_dist = d
                nearest = (ntx, nty)
        if nearest:
            nwx, nwy = tile_to_wow_center(*nearest)
            dist_yards = math.sqrt((nwx - x) ** 2 + (nwy - y) ** 2)
            print(f"\nNearest available tile: ({nearest[0]},{nearest[1]}) "
                  f"= WoW({nwx:.0f}, {nwy:.0f}), ~{dist_yards:.0f} yards away")
            print(f"Teleport command: .tele {nwx:.0f} {nwy:.0f} 100")


def main():
    parser = argparse.ArgumentParser(description="Analyze TrinityCore mmaps directory")
    parser.add_argument("mmaps_dir", help="Path to mmaps directory")
    parser.add_argument("--map", type=int, default=None, help="Show detail for specific map ID")
    parser.add_argument("--pos", nargs=2, type=float, metavar=('X', 'Y'),
                        help="Check if WoW position has tiles (uses --map or 0)")
    parser.add_argument("--visual", action="store_true", help="Show ASCII tile map")
    args = parser.parse_args()

    if not os.path.isdir(args.mmaps_dir):
        print(f"Error: {args.mmaps_dir} is not a directory")
        sys.exit(1)

    tiles, headers = scan_directory(args.mmaps_dir)

    # Always show summary
    print_summary(args.mmaps_dir, tiles, headers)

    target_map = args.map if args.map is not None else 0

    if args.pos:
        check_position(tiles.get(target_map, set()), args.pos[0], args.pos[1], target_map)

    if args.map is not None:
        tile_set = tiles.get(args.map, set())
        print_map_detail(args.mmaps_dir, args.map, tile_set)
        if args.visual:
            print_visual(args.map, tile_set)

    # If no specific map requested, show detail for the big maps
    if args.map is None and not args.pos:
        for mid in [0, 1, 530, 571]:
            if mid in tiles:
                print_map_detail(args.mmaps_dir, mid, tiles[mid])


if __name__ == '__main__':
    main()
