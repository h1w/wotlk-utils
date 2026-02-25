"""
road_classifier.py — Classify terrain textures as road or non-road.

Two complementary approaches:
  A) Filename pattern matching (primary): check MTEX path for road keywords
  B) DBC chain (supplementary): effectId → GroundEffectTexture.dbc → all doodadId == -1

Combined confidence scoring: 0.0 (not road) to 1.0 (definite road).
"""

import json
import os

# ---------------------------------------------------------------------------
# Default classification patterns
# ---------------------------------------------------------------------------

ROAD_PATTERNS_HIGH = [
    "road", "dirtroad", "cobble", "cobblestone",
    "flagstone", "paving", "pavement",
]

ROAD_PATTERNS_MEDIUM = [
    "path", "gravel", "trail",
]

EXCLUDE_PATTERNS = [
    # terrain
    "grass", "forest", "leaves", "rock", "cliff",
    "snow", "sand", "moss", "water", "lava", "root",
    "fern", "weed", "mud", "swamp", "marsh",
    "base", "scrub", "brush", "flower", "field",
    # water/coast
    "beach", "shore", "river", "lake", "ocean", "sea",
    "coral", "kelp", "algae", "underwater",
    # natural terrain
    "ground", "floor", "terrain", "earth",
    "farmland", "crop", "farm",
    "cave", "cavern", "mine",
    "rubble", "debris", "ruin",
]


def classify_texture(mtex_path: str) -> str:
    """Classify a texture path by filename patterns.

    Returns: 'road_high', 'road_medium', or 'not_road'
    """
    name = mtex_path.lower()

    if any(p in name for p in ROAD_PATTERNS_HIGH):
        return "road_high"
    if any(p in name for p in ROAD_PATTERNS_MEDIUM):
        if not any(e in name for e in EXCLUDE_PATTERNS):
            return "road_medium"
    return "not_road"


# ---------------------------------------------------------------------------
# Zone-specific overrides
# ---------------------------------------------------------------------------

class RoadConfig:
    """Loads road_textures.json for zone-specific overrides."""

    def __init__(self, config_path: str | None = None):
        self.patterns_high = list(ROAD_PATTERNS_HIGH)
        self.patterns_medium = list(ROAD_PATTERNS_MEDIUM)
        self.exclude = list(EXCLUDE_PATTERNS)
        self.zone_include: dict[str, list[str]] = {}
        self.zone_exclude: dict[str, list[str]] = {}

        if config_path and os.path.exists(config_path):
            with open(config_path, "r") as f:
                cfg = json.load(f)
            if "global_patterns_high" in cfg:
                self.patterns_high = cfg["global_patterns_high"]
            if "global_patterns_medium" in cfg:
                self.patterns_medium = cfg["global_patterns_medium"]
            if "global_exclude" in cfg:
                self.exclude = cfg["global_exclude"]
            for zone, overrides in cfg.get("zone_overrides", {}).items():
                if "include" in overrides:
                    self.zone_include[zone.lower()] = [s.lower() for s in overrides["include"]]
                if "exclude" in overrides:
                    self.zone_exclude[zone.lower()] = [s.lower() for s in overrides["exclude"]]

    def classify(self, mtex_path: str, zone_hint: str = "") -> str:
        """Classify with zone overrides applied."""
        name = mtex_path.lower()
        basename = os.path.basename(name)
        zone = zone_hint.lower()

        # Zone-specific forced exclude
        if zone in self.zone_exclude:
            if basename in self.zone_exclude[zone]:
                return "not_road"

        # Zone-specific forced include
        if zone in self.zone_include:
            if basename in self.zone_include[zone]:
                return "road_high"

        # Global patterns
        if any(p in name for p in self.patterns_high):
            return "road_high"
        if any(p in name for p in self.patterns_medium):
            if not any(e in name for e in self.exclude):
                return "road_medium"
        return "not_road"


# ---------------------------------------------------------------------------
# Combined classification (filename + DBC)
# ---------------------------------------------------------------------------

def is_road_layer(
    mtex_path: str,
    effect_id: int = -1,
    dbc_data: dict | None = None,
    config: RoadConfig | None = None,
    zone_hint: str = "",
) -> tuple[bool, float]:
    """Determine if a texture layer is a road.

    Returns (is_road, confidence) where confidence is 0.0 to 1.0.

    Parameters:
        mtex_path: texture path from MTEX
        effect_id: effectId from MCLY (-1 if not available)
        dbc_data: optional dict mapping effectId → doodad_ids list
        config: optional RoadConfig for zone overrides
        zone_hint: zone name for overrides
    """
    if config:
        fname_class = config.classify(mtex_path, zone_hint)
    else:
        fname_class = classify_texture(mtex_path)

    # DBC check: all doodadId == -1 means no vegetation → likely road
    dbc_is_barren = False
    if dbc_data and effect_id > 0 and effect_id in dbc_data:
        doodad_ids = dbc_data[effect_id]
        dbc_is_barren = all(d == -1 or d == 0xFFFFFFFF for d in doodad_ids)

    if fname_class == "road_high":
        return True, 1.0
    if fname_class == "road_medium" and dbc_is_barren:
        return True, 0.9
    if fname_class == "road_medium":
        return True, 0.6
    if dbc_is_barren and fname_class != "not_road":
        return True, 0.5
    return False, 0.0
