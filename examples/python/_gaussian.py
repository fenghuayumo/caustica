"""3D Gaussian splat helpers shared by the caustica examples.

Not an executable example. ``gaussian_splats.py`` and ``mesh_deformation.py``
both configure 3DGS rendering, so the settings mapping, the splat-only scene
template and the PLY bounds reader live here instead of being duplicated.
"""

from __future__ import annotations

import argparse
import json
import struct
import tempfile
from pathlib import Path

# A standalone .ply needs a scene to load into, so the splat-only template
# parks a tiny builtin plane far outside the view frustum.
_DUMMY_MESH_DISTANCE = 100000.0

_PLY_TYPES: dict[str, tuple[str, int]] = {
    "char": ("b", 1), "int8": ("b", 1),
    "uchar": ("B", 1), "uint8": ("B", 1), "uint8_t": ("B", 1),
    "short": ("h", 2), "int16": ("h", 2),
    "ushort": ("H", 2), "uint16": ("H", 2),
    "int": ("i", 4), "int32": ("i", 4),
    "uint": ("I", 4), "uint32": ("I", 4),
    "float": ("f", 4), "float32": ("f", 4),
    "double": ("d", 8), "float64": ("d", 8),
}


# --------------------------------------------------------------------------
# Splat-only scene
# --------------------------------------------------------------------------


def create_splat_only_scene(name: str = "caustica_splat_only") -> str:
    """Write a scene containing only a hidden dummy mesh and a default camera.

    3DGS content is appended afterwards with ``engine.load_gaussian_splat_file``.
    """
    scene = {
        "entities": [
            {
                "id": "HiddenDummyMesh",
                "name": "HiddenDummyMesh",
                "components": {
                    "Transform": {
                        "translation": [_DUMMY_MESH_DISTANCE] * 3,
                        "scale": 0.001,
                    },
                    "PrefabInstance": {"source": "builtin:plane"},
                },
            },
            {"id": "Cameras", "name": "Cameras"},
            {
                "id": "Default",
                "name": "Default",
                "parent": "Cameras",
                "components": {
                    "Transform": {
                        "translation": [0.0, 0.0, -5.0],
                        "rotation": [0.0, 0.0, 0.0, 1.0],
                    },
                    "PerspectiveCameraEx": {
                        "verticalFov": 0.785398,
                        "zNear": 0.001,
                        "exposureCompensation": 0.0,
                        "enableAutoExposure": False,
                    },
                },
            },
        ],
    }
    scene_path = Path(tempfile.gettempdir()) / f"{name}.scene.json"
    scene_path.write_text(json.dumps(scene, indent=2), encoding="utf-8")
    return str(scene_path)


# --------------------------------------------------------------------------
# PLY bounds
# --------------------------------------------------------------------------


def read_ply_bounds(
    ply_path: Path, *, convert_rdf_to_rub: bool = True, sample_cap: int = 200_000
) -> tuple[tuple[float, float, float], tuple[float, float, float], int]:
    """Return ``(center, extents, vertex_count)`` for a binary little-endian PLY.

    The engine exposes no bounds query for loaded splats, so the positions are
    sampled directly from the file to frame the camera. At most ``sample_cap``
    vertices are read.
    """
    with ply_path.open("rb") as f:
        header_lines: list[str] = []
        while True:
            line = f.readline()
            if not line:
                raise SystemExit(f"Unexpected EOF in PLY header: {ply_path}")
            text = line.decode("ascii", errors="replace").strip()
            header_lines.append(text)
            if text == "end_header":
                data_offset = f.tell()
                break

    if header_lines[0] != "ply":
        raise SystemExit(f"Not a PLY file: {ply_path}")
    format_line = next((line for line in header_lines if line.startswith("format ")), "")
    if "binary_little_endian" not in format_line:
        raise SystemExit(
            f"Only binary_little_endian PLY files are supported, got: {format_line or '<none>'}"
        )

    vertex_count = 0
    properties: list[tuple[str, str, int]] = []
    in_vertex = False
    offset = 0
    for line in header_lines:
        parts = line.split()
        if len(parts) >= 3 and parts[0] == "element":
            in_vertex = parts[1] == "vertex"
            if in_vertex:
                vertex_count = int(parts[2])
                properties.clear()
                offset = 0
        elif in_vertex and len(parts) >= 3 and parts[0] == "property":
            if parts[1] == "list":
                raise SystemExit("List vertex properties are not supported")
            if parts[1] not in _PLY_TYPES:
                raise SystemExit(f"Unsupported PLY property type: {parts[1]}")
            properties.append((parts[2], parts[1], offset))
            offset += _PLY_TYPES[parts[1]][1]

    offsets = {name: (ptype, byte_offset) for name, ptype, byte_offset in properties}
    for axis in ("x", "y", "z"):
        if axis not in offsets:
            raise SystemExit(f"PLY is missing vertex property '{axis}': {ply_path}")

    stride = offset
    step = max(1, vertex_count // max(1, min(sample_cap, vertex_count)))
    mins = [float("inf")] * 3
    maxs = [float("-inf")] * 3
    sampled = 0

    def read_float(row: bytes, name: str) -> float:
        ptype, byte_offset = offsets[name]
        code, _ = _PLY_TYPES[ptype]
        return float(struct.unpack_from("<" + code, row, byte_offset)[0])

    with ply_path.open("rb") as f:
        f.seek(data_offset)
        for index in range(vertex_count):
            row = f.read(stride)
            if len(row) != stride:
                raise SystemExit(f"Unexpected EOF in PLY vertex data at row {index}")
            if index % step != 0:
                continue
            x, y, z = read_float(row, "x"), read_float(row, "y"), read_float(row, "z")
            point = [x, -y, -z] if convert_rdf_to_rub else [x, y, z]
            for axis in range(3):
                mins[axis] = min(mins[axis], point[axis])
                maxs[axis] = max(maxs[axis], point[axis])
            sampled += 1

    if sampled == 0:
        raise SystemExit(f"PLY has no sampled vertices: {ply_path}")
    center = tuple((mins[i] + maxs[i]) * 0.5 for i in range(3))
    extents = tuple(maxs[i] - mins[i] for i in range(3))
    return center, extents, vertex_count  # type: ignore[return-value]


def camera_from_bounds(
    center: tuple[float, float, float],
    extents: tuple[float, float, float],
    side: str = "front",
    distance_scale: float = 3.0,
):
    """Build a ``(position, direction, up)`` triple looking at ``center``."""
    from _common import normalize

    offset_dirs = {
        "front": (0.0, 0.0, -1.0),
        "back": (0.0, 0.0, 1.0),
        "left": (-1.0, 0.0, 0.0),
        "right": (1.0, 0.0, 0.0),
        "top": (0.0, 1.0, 0.0),
    }
    radius = max(extents) * 0.5
    distance = max(radius * distance_scale, 0.5)
    offset = offset_dirs[side]
    position = tuple(center[i] + offset[i] * distance for i in range(3))
    direction = normalize([center[i] - position[i] for i in range(3)])
    up = (0.0, 0.0, -1.0) if side == "top" else (0.0, 1.0, 0.0)
    return position, direction, up


# --------------------------------------------------------------------------
# Settings
# --------------------------------------------------------------------------


def add_gaussian_args(parser: argparse.ArgumentParser) -> None:
    """Appearance and performance flags for 3DGS rendering."""
    group = parser.add_argument_group("3DGS appearance")
    group.add_argument("--splat-scale", type=float, default=1.0)
    group.add_argument("--alpha-scale", type=float, default=1.0)
    group.add_argument("--brightness", type=float, default=1.0)
    group.add_argument("--alpha-cull", type=float, default=1.0 / 255.0)
    group.add_argument(
        "--depth-test", dest="depth_test", action=argparse.BooleanOptionalAction, default=True
    )
    group.add_argument("--sorting", choices=["gpu", "stochastic"], default="gpu")
    group.add_argument("--storage-format", choices=["float32", "float16", "uint8"], default="uint8")
    group.add_argument(
        "--frustum-culling", choices=["disabled", "distance", "raster"], default="raster"
    )
    group.add_argument(
        "--mip-antialiasing", action=argparse.BooleanOptionalAction, default=False
    )
    group.add_argument("--translation", nargs=3, type=float, metavar=("X", "Y", "Z"))
    group.add_argument("--rotation", nargs=3, type=float, metavar=("X", "Y", "Z"))
    group.add_argument("--object-scale", nargs=3, type=float, metavar=("X", "Y", "Z"))


def add_gaussian_shadow_args(parser: argparse.ArgumentParser) -> None:
    """Ray-traced shadow and emissive-proxy flags for the hybrid 3DGS path."""
    group = parser.add_argument_group("3DGS shadows and emission")
    group.add_argument("--shadow-mode", choices=["disabled", "hard", "soft"], default="soft")
    group.add_argument("--shadow-strength", type=float, default=0.75)
    group.add_argument("--shadow-soft-radius", type=float, default=0.08)
    group.add_argument("--shadow-soft-samples", type=int, default=1)
    group.add_argument(
        "--shadow-kernel-degree",
        type=int,
        default=0,
        help="Gaussian shadow particle kernel degree (0=Linear .. 5=Quintic).",
    )
    group.add_argument(
        "--shadow-adaptive-clamp", action=argparse.BooleanOptionalAction, default=True
    )
    group.add_argument("--shadow-ray-offset", type=float, default=0.01)
    group.add_argument(
        "--emission-intensity",
        type=float,
        default=1.0,
        help="Treat splat radiance as emissive proxy lights at this intensity. 0 disables.",
    )
    group.add_argument("--emission-max-proxies", type=int, default=8192)


def apply_gaussian_settings(caustica, settings, args: argparse.Namespace) -> None:
    """Push the 3DGS flags onto ``settings``.

    Flags added by :func:`add_gaussian_shadow_args` are optional: when a script
    only calls :func:`add_gaussian_args`, the shadow and emission settings are
    left at their engine defaults.
    """
    settings.enable_gaussian_splats = True
    settings.gaussian_splat_depth_test = args.depth_test
    settings.gaussian_splat_scale = args.splat_scale
    settings.gaussian_splat_alpha_scale = args.alpha_scale
    settings.gaussian_splat_brightness = args.brightness
    settings.gaussian_splat_alpha_cull_threshold = args.alpha_cull
    settings.gaussian_splat_mip_antialiasing = args.mip_antialiasing

    settings.gaussian_splat_sorting_mode = int(
        caustica.GaussianSplatSortMode.StochasticSplats
        if args.sorting == "stochastic"
        else caustica.GaussianSplatSortMode.GpuSort
    )
    storage = {
        "float32": caustica.GaussianSplatStorageFormat.Float32,
        "float16": caustica.GaussianSplatStorageFormat.Float16,
        "uint8": caustica.GaussianSplatStorageFormat.Uint8,
    }[args.storage_format]
    settings.gaussian_splat_sh_format = int(storage)
    settings.gaussian_splat_rgba_format = int(storage)
    settings.gaussian_splat_frustum_culling = int(
        {
            "disabled": caustica.GaussianSplatFrustumCulling.Disabled,
            "distance": caustica.GaussianSplatFrustumCulling.AtDistanceStage,
            "raster": caustica.GaussianSplatFrustumCulling.AtRasterStage,
        }[args.frustum_culling]
    )

    if getattr(args, "translation", None) is not None:
        settings.gaussian_splat_translation = tuple(args.translation)
    if getattr(args, "rotation", None) is not None:
        settings.gaussian_splat_rotation_euler_deg = tuple(args.rotation)
    if getattr(args, "object_scale", None) is not None:
        settings.gaussian_splat_object_scale = tuple(args.object_scale)

    shadow_mode = getattr(args, "shadow_mode", None)
    if shadow_mode is not None:
        _apply_shadow_settings(caustica, settings, args, shadow_mode)


def _apply_shadow_settings(caustica, settings, args: argparse.Namespace, shadow_mode: str) -> None:
    enabled = shadow_mode != "disabled"
    settings.gaussian_splat_shadows = enabled
    settings.gaussian_splat_shadows_mode = int(
        {
            "disabled": caustica.GaussianSplatShadowMode.Disabled,
            "hard": caustica.GaussianSplatShadowMode.Hard,
            "soft": caustica.GaussianSplatShadowMode.Soft,
        }[shadow_mode]
    )
    if enabled:
        settings.gaussian_splat_shadow_strength = args.shadow_strength
        settings.gaussian_splat_shadow_soft_radius = args.shadow_soft_radius
        settings.gaussian_splat_shadow_soft_sample_count = args.shadow_soft_samples
        settings.gaussian_splat_shadow_kernel_degree = args.shadow_kernel_degree
        settings.gaussian_splat_shadow_adaptive_clamp = args.shadow_adaptive_clamp
        settings.gaussian_splat_shadow_ray_offset = args.shadow_ray_offset
        # Ray-traced Gaussian shadows need splat acceleration structures.
        settings.gaussian_splat_use_tlas_instances = True
        settings.gaussian_splat_blas_compaction = True

    intensity = getattr(args, "emission_intensity", 0.0)
    settings.gaussian_splat_as_emitter = intensity > 0.0
    if intensity > 0.0:
        settings.gaussian_splat_emission_intensity = intensity
        settings.gaussian_splat_emission_max_proxy_count = args.emission_max_proxies


def rebuild_acceleration_structures(engine, warmup_frames: int = 8) -> None:
    """Rebuild splat acceleration structures and let the rebuild settle."""
    engine.request_full_accel_rebuild()
    warmup = max(warmup_frames, 1)
    print(f"[caustica] Rebuilding 3DGS acceleration structures ({warmup} warmup frames) ...")
    engine.step_n(warmup)
