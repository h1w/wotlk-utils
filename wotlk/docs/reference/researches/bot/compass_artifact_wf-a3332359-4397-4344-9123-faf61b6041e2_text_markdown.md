# TrinityCore mmaps break stock 32-bit Detour completely

**TrinityCore-generated .mmap files are fundamentally incompatible with stock recastnavigation's 32-bit `dtPolyRef`.** The root cause is that TC's mmap generator writes `maxPolys = 0x80000000` (2³¹) into the `dtNavMeshParams` stored in the `.mmap` file, which requires 31 poly bits — far exceeding what fits in a 32-bit reference alongside tile and salt bits. The result is that `dtNavMesh::init()` silently succeeds despite the overflow, `addTile()` loads tiles without error, the BV-tree spatial search finds polygons correctly — but every constructed poly ref is garbage, so `findNearestPoly` always returns 0. Every known project that successfully loads TC mmaps does so by using TC's own 64-bit-modified Detour library, not stock recastnavigation.

---

## The unsigned arithmetic bug that lets init() silently succeed

TrinityCore's mmap generator (`MapBuilder.cpp`) sets `maxPolys = 1 << DT_POLY_BITS` where TC defines `DT_POLY_BITS` as **31** via their `STATIC_POLY_BITS` constant. This value of `0x80000000` gets written as the `maxPolys` field of the raw `dtNavMeshParams` struct in the `.mmap` file. When stock Detour (32-bit `dtPolyRef`) loads this, the bit allocation in `dtNavMesh::init()` proceeds as follows:

```
m_polyBits = dtIlog2(dtNextPow2(0x80000000)) = 31
m_tileBits = dtIlog2(dtNextPow2(maxTiles))   = 10  (for ~1024 tiles)
m_saltBits = dtMin(31u, 32u - 10u - 31u)
```

The expression `32u - 10u - 31u` performs **unsigned arithmetic**, wrapping to `0xFFFFFFF7` (~4.29 billion). `dtMin(31u, 4294967287u)` yields **31**. The subsequent validation `if (m_saltBits < 10) return DT_FAILURE` passes because 31 ≥ 10. **Init returns `DT_SUCCESS` despite needing 31+10+31 = 72 bits in a 32-bit field.** A recent debug-only `dtAssert(m_tileBits + m_polyBits <= 31)` was added in upstream PR #763, but asserts compile out in release builds and don't prevent the function from succeeding.

`addTile()` also succeeds: it checks `m_polyBits < dtIlog2(dtNextPow2(header->polyCount))`, and since `m_polyBits = 31` easily exceeds any realistic tile's polygon count (typically a few hundred per tile), this check always passes. The tile is stored with `salt = 1`.

## Why findNearestPoly returns exactly zero

The call chain `findNearestPoly → queryPolygons → queryPolygonsInTile` first performs a **BV-tree spatial search** that works perfectly — it uses quantized AABB overlap testing that is completely independent of poly refs. The BV tree correctly identifies which polygons overlap the query box. The failure occurs immediately after, when constructing the poly ref for each found polygon.

`queryPolygonsInTile` calls `getPolyRefBase(tile)`, which invokes `encodePolyId(salt=1, tileIndex=0, polyIndex=0)`:

```cpp
return ((dtPolyRef)1 << (31+10)) | ((dtPolyRef)0 << 31) | (dtPolyRef)0;
//      ^^^^^^^^^^^^^^^^^^^^^^^^
//      (uint32_t)1 << 41  →  UNDEFINED BEHAVIOR (shift >= type width)
```

On x86 processors, the shift count is masked to 5 bits: **41 & 31 = 9**, so the result is `1 << 9 = 512`. The base ref becomes **512** instead of a properly encoded poly ref. Each polygon ref is then `512 | polyIndex`.

When `findNearestPoly`'s internal callback calls `closestPointOnPoly(ref)`, which calls the **safe** `getTileAndPolyByRef(ref)`, decoding produces garbage:

```
polyMask = (1<<31)-1 = 0x7FFFFFFF
ip = 512 & 0x7FFFFFFF = 512        // Should be 0!
```

The validation `if (ip >= tile->header->polyCount)` fires — no WoW tile has 512+ polygons — and returns `DT_FAILURE`. **Every single polygon fails this check.** The nearest-poly callback skips all candidates, `nearestRef` stays at its initialized value of **0**, and `findNearestPoly` returns a null reference.

## Coordinate conversion uses {wowY, wowZ, wowX} without negation

TrinityCore's `PathGenerator::BuildPolyPath` performs a straightforward axis swap with **no negation**:

```cpp
float startPoint[VERTEX_SIZE] = {startPos.y, startPos.z, startPos.x};
```

The mapping is: **Detour X = WoW Y, Detour Y = WoW Z (height), Detour Z = WoW X**. The inverse in `BuildPointPath` confirms this: `Vector3(pathPoints[i*3+2], pathPoints[i*3+0], pathPoints[i*3+1])`, recovering WoW X from Detour Z, WoW Y from Detour X, WoW Z from Detour Y.

This contradicts some commonly cited references that claim negation (`{-wowY, wowZ, -wowX}`). The confusion likely stems from older MaNGOS forks or from conflating the coordinate swap with WoW's axis orientation conventions. **The binary tile data confirms no negation**: `bmin`/`bmax` values in actual `.mmtile` headers show positive coordinate ranges consistent with raw WoW Y/X values passed through as Detour X/Z. The `dtNavMeshParams.orig` field is copied from the minimum bounds of all tile geometry, and `tileWidth = tileHeight = GRID_SIZE` (**533.3333f**, WoW's standard grid cell size).

## TrinityCore's Detour modifications make dtPolyRef always 64-bit

TC's copy of recastnavigation (`dep/recastnavigation/`) completely removes the `#ifdef DT_POLYREF64` conditional and **unconditionally** typedefs `dtPolyRef` and `dtTileRef` as `uint64_d` (a cross-platform 64-bit unsigned integer). Lines 26–53 of TC's `DetourNavMesh.h` are marked `// Edited by TC` and define:

| Constant | TC Value | Stock DT_POLYREF64 Value |
|---|---|---|
| `STATIC_SALT_BITS` | 12 | `DT_SALT_BITS` = 16 |
| `STATIC_TILE_BITS` | 21 | `DT_TILE_BITS` = 28 |
| `STATIC_POLY_BITS` | 31 | `DT_POLY_BITS` = 20 |
| **Total bits** | **64** | **64** |
| `dtPolyRef` type | `uint64_d` (always) | `uint64_t` (opt-in) |

TC's version keeps the runtime `m_polyBits`/`m_tileBits` member variable path for `encodePolyId`, but because `dtPolyRef` is 64-bit, the shifts `(salt << 41)` are well-defined and produce correct results. The `DT_NAVMESH_VERSION` remains **7** in both TC and stock, so the binary tile data format is identical — the incompatibility is purely in the `dtNavMeshParams` metadata (specifically the `maxPolys` value) and the poly ref encoding width.

The `.mmtile` file format consists of a `MmapTileHeader` (magic `0x4d4d4150` = "MMAP", `dtVersion`, `mmapVersion=15`, `size`, `usesLiquids`, 3 bytes padding) followed by raw Detour tile data. The `.mmap` file is simply a raw `dtNavMeshParams` struct (24 bytes: `orig[3]`, `tileWidth`, `tileHeight`, `maxTiles`, `maxPolys`).

## Three fix strategies, ranked by reliability

**Option 1 — Use TC's modified Detour (recommended).** Copy TC's `dep/recastnavigation/Detour/` sources and compile them into your project. This is what every known working project does. AmeisenNavigation (a TCP-based navigation server for WoW bots, ~80 GitHub stars) bundles TC's modified recastnavigation directly. BloogBot's navigation DLL uses MaNGOS's equivalent modified Detour source. Both load `.mmap` params verbatim and call `init()` without modification.

**Option 2 — Override maxPolys before calling init().** Read the `dtNavMeshParams` from the `.mmap` file, then replace `maxPolys` with a smaller value before calling `init()`. This works because `addTile()` checks the tile's actual `polyCount` against `m_polyBits` (the log₂ of maxPolys), not against maxPolys directly. The tile binary data stores only the actual polygon count, not the navmesh-level `maxPolys`. The constraint is:

```
m_polyBits + m_tileBits + saltBits(≥10) ≤ 32
```

For a typical WoW continent map with ~4096 tiles (`m_tileBits = 12`), you need `m_polyBits ≤ 32 - 12 - 10 = 10`, so `maxPolys ≤ 1024`. WoW tiles rarely exceed a few hundred polygons, so **`maxPolys = 1 << 10 = 1024`** should work for most tiles. If any tile exceeds 1024 polygons (needing 11 poly bits), `addTile` will fail for that specific tile. For maps with fewer tiles, more poly bits are available. Set `maxPolys` to the smallest power of 2 that covers the largest `polyCount` across all tiles you need to load, then verify the bit budget fits.

**Option 3 — Build stock recastnavigation with DT_POLYREF64.** The stock library supports this via CMake option `RECASTNAVIGATION_DT_POLYREF64=ON`. However, stock `DT_POLYREF64` uses `DT_POLY_BITS = 20` (max 1M polys) while TC generates with 31 poly bits. The stock encoding uses static compile-time constants (`DT_SALT_BITS=16`, `DT_TILE_BITS=28`, `DT_POLY_BITS=20`), so the runtime `maxPolys` value doesn't directly control the bit layout — the tile and poly indices are masked to fixed widths. This means stock `DT_POLYREF64` would allocate 28 tile bits and 20 poly bits regardless of the `.mmap` params. With only 20 poly bits, tiles with `polyCount > 1048576` would have their index truncated, but since no WoW tile approaches this count, it should function correctly. The caveat is that stock `DT_POLYREF64` has a known alignment bug with tile links (referenced in upstream issue/PR #791), which may cause subtle corruption. Using vcpkg, you would need to set the feature flag or override the CMake option.

## No project loads TC mmaps with unmodified stock Detour

Every project found that successfully loads TrinityCore or MaNGOS `.mmap` files uses the emulator's own modified recastnavigation:

- **AmeisenNavigation** — Bundles TC's modified recastnavigation in its repository; loads `.mmap` params directly into `dtNavMesh::init()`; supports multiple mmap format patterns via config
- **BloogBot** — Self-described as "blatantly ripped off from the MaNGOS codebase"; uses MaNGOS's modified Detour with 64-bit refs
- **namigator** (namreeb) — Generates its own navmesh format from WoW client data; does not load TC mmaps at all
- **CapekNav**, **meshReader** — Generate their own data, not TC-compatible

No project was found that uses the `maxPolys` override approach (Option 2), though it is technically viable. The `maxPolys` override is undocumented and requires scanning tile files to determine the maximum polyCount, making it fragile compared to simply using TC's Detour source.

## Conclusion

The incompatibility is a silent, complete failure: init and addTile succeed, spatial queries find the right tiles and polygons, but **every poly ref is corrupted by undefined-behavior bit shifts**, causing all validation to fail and all query results to return null. The most robust fix is to compile against TrinityCore's Detour sources (always-64-bit `dtPolyRef`). The `maxPolys` override is a viable lightweight alternative for projects that cannot change their Detour dependency, provided you verify that `polyBits + tileBits + 10 ≤ 32` and that no tile exceeds your chosen maxPolys. The coordinate system is a simple axis swap `{wowY, wowZ, wowX}` with no negation — match this exactly when converting WoW positions to Detour query coordinates.