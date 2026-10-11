from __future__ import annotations

import json
import math
import colorsys
from pathlib import Path


def read(path):
    def reject(value):
        raise ValueError(f"Non-finite JSON: {value}")
    return json.loads(Path(path).read_text(encoding="utf-8"), parse_constant=reject)


def write(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False), encoding="utf-8")
    tmp.replace(path)


def finite(value):
    if isinstance(value, bool) or not isinstance(value, (float, int)) or not math.isfinite(value):
        raise ValueError("Expected a finite number")
    return float(value)


def vector(value, n=3):
    if not isinstance(value, list) or len(value) != n:
        raise ValueError(f"Expected vector of length {n}")
    return [finite(x) for x in value]


def diagnostic_colors(geometry):
    """Stable part legend in linear RGB, shared by renderer and reviewer."""
    parts = geometry["parts"]
    return {p["name"]: list(colorsys.hsv_to_rgb(i / len(parts), 0.35, 0.65)) for i, p in enumerate(parts)}


def validate_geometry(value):
    """The model supplies data, never executable Python or file paths."""
    if not isinstance(value, dict) or set(value) != {"parts"}:
        raise ValueError("Geometry must contain only parts")
    parts = value["parts"]
    if not isinstance(parts, list) or not 1 <= len(parts) <= 32:
        raise ValueError("Expected 1..32 parts")
    names = set()
    for part in parts:
        if set(part) - {"name", "kind", "position", "rotation", "size", "profile", "thickness"}:
            raise ValueError("Unknown part property")
        name = part.get("name")
        if not isinstance(name, str) or not name.isascii() or not name.replace("_", "").isalnum() or len(name) > 64 or name in names:
            raise ValueError("Part names must be unique ASCII identifiers")
        names.add(name)
        if part.get("kind") not in {"box", "ellipsoid", "cylinder", "lathe"}:
            raise ValueError("Unsupported geometry operation")
        for field in ("position", "rotation", "size"):
            values = vector(part[field])
            if max(abs(x) for x in values) > 1e6:
                raise ValueError("Geometry value too large")
        if min(part["size"]) <= 0:
            raise ValueError("Part dimensions must be positive")
        if part["kind"] == "lathe":
            profile = part.get("profile", [])
            if not isinstance(profile, list) or not 2 <= len(profile) <= 64:
                raise ValueError("Lathe needs 2..64 [height, radius] samples")
            previous = -math.inf
            for sample in profile:
                height, radius = vector(sample, 2)
                if height <= previous or radius <= 0 or max(abs(height), radius) > 1e6:
                    raise ValueError("Lathe heights must increase and radii must be positive")
                previous = height
            thickness = finite(part.get("thickness", 0))
            if thickness < 0 or thickness >= min(s[1] for s in profile):
                raise ValueError("Shell thickness must be smaller than every radius")
    return value
