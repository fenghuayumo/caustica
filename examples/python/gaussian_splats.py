#!/usr/bin/env python
"""3D Gaussian splat example: interactive preview or headless Ref/RT batch.

Modes:
  interactive  Open a window (default)
  reference    Headless reference accumulation + OIDN
  realtime     Headless realtime frames (DLSS-RR when available)
  batch        Run reference then realtime

Usage:
    python examples/python/gaussian_splats.py --ply path/to/splat.ply
    python examples/python/gaussian_splats.py --mode batch --out-dir ./3dgs_out
    python examples/python/gaussian_splats.py --mode reference --headless --out ref.png
"""

from __future__ import annotations

import argparse
import json
import math
import struct
import tempfile
from pathlib import Path

from _common import ASSETS_DIR, resolve_output_path, run_window_loop


DEFAULT_PLY = ASSETS_DIR / "Models" / "Gingy" / "splat_crop.ply"
if not DEFAULT_PLY.exists():
    DEFAULT_PLY = Path(r"D:/ScanVideo/Gingy/splat_crop.ply")


def normalize(v: tuple[float, float, float] | list[float]) -> tuple[float, float, float]:
    length = math.sqrt(sum(x * x for x in v))
    if length <= 1e-8:
        return (0.0, 0.0, 1.0)
    return (v[0] / length, v[1] / length, v[2] / length)


def parse_binary_ply_bounds(
    ply_path: Path, convert_rdf_to_rub: bool, sample_cap: int
) -> tuple[tuple[float, float, float], tuple[float, float, float], int]:
    with ply_path.open("rb") as f:
        header_lines: list[str] = []
        while True:
            line = f.readline()
            if not line:
                raise RuntimeError("Unexpected EOF in PLY header")
            text = line.decode("ascii", errors="replace").strip()
            header_lines.append(text)
            if text == "end_header":
                data_offset = f.tell()
                break

    if header_lines[0] != "ply":
        raise RuntimeError(f"Not a PLY file: {ply_path}")
    format_line = next((line for line in header_lines if line.startswith("format ")), "")
    if "binary_little_endian" not in format_line:
        raise RuntimeError(f"Only binary_little_endian PLY supported: {format_line}")

    type_info = {
        "char": ("b", 1), "int8": ("b", 1),
        "uchar": ("B", 1), "uint8": ("B", 1), "uint8_t": ("B", 1),
        "short": ("h", 2), "int16": ("h", 2),
        "ushort": ("H", 2), "uint16": ("H", 2),
        "int": ("i", 4), "int32": ("i", 4),
        "uint": ("I", 4), "uint32": ("I", 4),
        "float": ("f", 4), "float32": ("f", 4),
        "double": ("d", 8), "float64": ("d", 8),
    }

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
                raise RuntimeError("List vertex properties are not supported")
            if parts[1] not in type_info:
                raise RuntimeError(f"Unsupported PLY property type: {parts[1]}")
            properties.append((parts[2], parts[1], offset))
            offset += type_info[parts[1]][1]

    offsets = {name: (ptype, byte_offset) for name, ptype, byte_offset in properties}
    for axis in ("x", "y", "z"):
        if axis not in offsets:
            raise RuntimeError(f"PLY is missing vertex property '{axis}'")

    stride = offset
    step = max(1, vertex_count // max(1, min(sample_cap, vertex_count)))
    mins = [float("inf")] * 3
    maxs = [float("-inf")] * 3
    sampled = 0

    def read_float(row: bytes, name: str) -> float:
        ptype, byte_offset = offsets[name]
        code, _ = type_info[ptype]
        return float(struct.unpack_from("<" + code, row, byte_offset)[0])

    with ply_path.open("rb") as f:
        f.seek(data_offset)
        for index in range(vertex_count):
            row = f.read(stride)
            if len(row) != stride:
                raise RuntimeError(f"Unexpected EOF in PLY vertex data at row {index}")
            if index % step != 0:
                continue
            x, y, z = read_float(row, "x"), read_float(row, "y"), read_float(row, "z")
            point = [x, -y, -z] if convert_rdf_to_rub else [x, y, z]
            for axis in range(3):
                mins[axis] = min(mins[axis], point[axis])
                maxs[axis] = max(maxs[axis], point[axis])
            sampled += 1

    if sampled == 0:
        raise RuntimeError("PLY has no sampled vertices")
    center = tuple((mins[i] + maxs[i]) * 0.5 for i in range(3))
    extents = tuple(maxs[i] - mins[i] for i in range(3))
    return center, extents, vertex_count  # type: ignore[return-value]


def camera_from_bounds(
    center: tuple[float, float, float],
    extents: tuple[float, float, float],
    side: str,
    distance_scale: float,
) -> tuple[tuple[float, float, float], tuple[float, float, float], tuple[float, float, float]]:
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


def create_splat_only_scene() -> Path:
    """Minimal scene with a far-away dummy mesh so 3DGS can load alone."""
    model_path = ASSETS_DIR / "Models" / "ConvergenceTest" / "ConvergenceTest.gltf"
    if not model_path.exists():
        # Fallback: inline builtin plane tucked away.
        scene_path = Path(tempfile.gettempdir()) / "caustica_splat_only.scene.json"
        scene = {
            "models": ["builtin:plane"],
            "graph": [
                {
                    "name": "HiddenDummyMesh",
                    "model": 0,
                    "translation": [100000.0, 100000.0, 100000.0],
                    "scaling": 0.001,
                },
                {
                    "name": "Cameras",
                    "children": [
                        {
                            "name": "Default",
                            "type": "PerspectiveCameraEx",
                            "translation": [0.0, 0.0, -5.0],
                            "rotation": [0.0, 0.0, 0.0, 1.0],
                            "verticalFov": 0.785398,
                            "zNear": 0.001,
                            "exposureCompensation": 0.0,
                            "enableAutoExposure": False,
                        }
                    ],
                },
            ],
        }
        scene_path.write_text(json.dumps(scene, indent=2), encoding="utf-8")
        return scene_path

    scene_path = Path(tempfile.gettempdir()) / "caustica_splat_only.scene.json"
    scene = {
        "models": [str(model_path).replace("\\", "/")],
        "graph": [
            {
                "name": "HiddenDummyMesh",
                "model": 0,
                "translation": [100000.0, 100000.0, 100000.0],
                "scaling": 0.001,
            },
            {
                "name": "Cameras",
                "children": [
                    {
                        "name": "Default",
                        "type": "PerspectiveCameraEx",
                        "translation": [0.0, 0.0, -5.0],
                        "rotation": [0.0, 0.0, 0.0, 1.0],
                        "verticalFov": 0.785398,
                        "zNear": 0.001,
                        "exposureCompensation": 0.0,
                        "enableAutoExposure": False,
                    }
                ],
            },
        ],
    }
    scene_path.write_text(json.dumps(scene, indent=2), encoding="utf-8")
    return scene_path


def parse_vec3(values: list[float] | None) -> tuple[float, float, float] | None:
    if values is None:
        return None
    return (float(values[0]), float(values[1]), float(values[2]))


def configure_gaussian_splats(caustica, settings, args: argparse.Namespace) -> None:
    settings.enable_gaussian_splats = True
    settings.gaussian_splat_depth_test = args.depth_test
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
    settings.gaussian_splat_scale = args.splat_scale
    settings.gaussian_splat_alpha_scale = args.alpha_scale
    settings.gaussian_splat_brightness = args.brightness
    settings.gaussian_splat_alpha_cull_threshold = args.alpha_cull
    settings.gaussian_splat_mip_antialiasing = args.mip_antialiasing
    settings.gaussian_splat_frustum_culling = int(
        {
            "disabled": caustica.GaussianSplatFrustumCulling.Disabled,
            "distance": caustica.GaussianSplatFrustumCulling.AtDistanceStage,
            "raster": caustica.GaussianSplatFrustumCulling.AtRasterStage,
        }[args.frustum_culling]
    )
    shadow_mode = {
        "disabled": caustica.GaussianSplatShadowMode.Disabled,
        "hard": caustica.GaussianSplatShadowMode.Hard,
        "soft": caustica.GaussianSplatShadowMode.Soft,
    }[args.shadow_mode]
    settings.gaussian_splat_shadows_mode = int(shadow_mode)
    settings.gaussian_splat_shadows = args.shadow_mode != "disabled"
    if args.translation is not None:
        settings.gaussian_splat_translation = parse_vec3(args.translation)
    if args.rotation is not None:
        settings.gaussian_splat_rotation_euler_deg = parse_vec3(args.rotation)
    if args.object_scale is not None:
        settings.gaussian_splat_object_scale = parse_vec3(args.object_scale)


def configure_camera(renderer, args: argparse.Namespace, ply_path: Path) -> None:
    center, extents, vertex_count = parse_binary_ply_bounds(
        ply_path, args.rdf_to_rub, args.sample_cap
    )
    cam_pos, cam_dir, cam_up = camera_from_bounds(
        center, extents, args.side, args.distance_scale
    )
    if args.cam_pos:
        cam_pos = tuple(args.cam_pos)
    if args.cam_dir:
        cam_dir = normalize(args.cam_dir)
    if args.cam_up:
        cam_up = normalize(args.cam_up)
    print(f"[caustica] PLY vertices={vertex_count} center={center} extents={extents}")
    print(f"[caustica] camera pos={cam_pos} dir={cam_dir}")
    renderer.set_camera(cam_pos, cam_dir, cam_up)
    renderer.set_camera_fov(args.fov)


def select_realtime_aa(caustica, settings, requested: str) -> tuple[int, str]:
    dlss = bool(getattr(settings, "is_dlss_supported", False))
    rr = bool(getattr(settings, "is_dlss_rr_supported", False))
    if requested == "dlss-rr":
        if rr:
            return int(caustica.RealtimeAA.DLSS_RR), "dlss_rr"
        if dlss:
            print("[caustica] DLSS-RR unavailable; falling back to DLSS.")
            return int(caustica.RealtimeAA.DLSS), "dlss"
        print("[caustica] DLSS unavailable; falling back to TAA.")
        return int(caustica.RealtimeAA.TAA), "taa"
    if requested == "dlss":
        if dlss:
            return int(caustica.RealtimeAA.DLSS), "dlss"
        return int(caustica.RealtimeAA.TAA), "taa"
    if requested == "taa":
        return int(caustica.RealtimeAA.TAA), "taa"
    return int(caustica.RealtimeAA.Off), "off"


def make_renderer(caustica, args, scene: str, ply_path: Path, *, realtime: bool, headless: bool):
    renderer = caustica.Renderer(
        width=args.width,
        height=args.height,
        headless=headless,
        vulkan=args.vulkan,
        adapter=args.adapter,
        scene=scene,
        realtime=realtime,
        accumulation_target=args.frames,
    )
    if not renderer.load_gaussian_splats(str(ply_path), args.rdf_to_rub):
        renderer.close()
        raise RuntimeError(f"Failed to load Gaussian splat: {ply_path}")
    return renderer


def render_reference(caustica, args, scene: str, ply_path: Path, out_path: Path) -> None:
    print(f"\n[caustica] Reference: {args.frames} spp + OIDN -> {out_path}")
    with make_renderer(caustica, args, scene, ply_path, realtime=False, headless=True) as renderer:
        app = renderer.app
        app.set_reference_mode(
            spp=args.frames,
            oidn=True,
            oidn_quality=int(caustica.OidnQuality.High),
            oidn_passes=int(caustica.OidnPasses.AlbedoNormal),
            oidn_prefilter=int(caustica.OidnPrefilter.Accurate),
        )
        s = renderer.settings
        s.oidn_use_gpu = args.oidn_gpu
        s.enable_tone_mapping = args.tonemap
        s.enable_bloom = args.bloom
        s.bounce_count = args.bounces
        s.use_nee = True
        s.oidn_apply()
        configure_gaussian_splats(caustica, s, args)
        configure_camera(renderer, args, ply_path)
        s.reset_accumulation = True
        frames = renderer.step_until_accumulated(max(args.frames + 128, args.frames * 4))
        if not renderer.save_screenshot(str(out_path)):
            raise RuntimeError(f"Failed to save: {out_path}")
        print(f"[caustica] saved: {out_path} ({frames} frames)")


def render_realtime(caustica, args, scene: str, ply_path: Path, out_path: Path) -> None:
    print(f"\n[caustica] Realtime: {args.frames} frames -> {out_path}")
    with make_renderer(caustica, args, scene, ply_path, realtime=True, headless=True) as renderer:
        s = renderer.settings
        aa_mode, aa_label = select_realtime_aa(caustica, s, args.realtime_aa)
        renderer.app.set_realtime_mode(standalone_denoiser=False, realtime_aa=aa_mode)
        s.enable_tone_mapping = args.tonemap
        s.enable_bloom = args.bloom
        if aa_label in {"dlss", "dlss_rr"}:
            s.dlss_mode = int(caustica.DLSSMode.Balanced)
        if aa_label == "dlss_rr":
            s.dlss_rr_preset = int(caustica.DLSSRRPreset.PresetE)
            s.disable_restirs_with_dlss_rr = True
        configure_gaussian_splats(caustica, s, args)
        configure_camera(renderer, args, ply_path)
        s.reset_accumulation = True
        renderer.step_n(args.frames)
        # Rewrite path when AA label is known for batch mode.
        if out_path.name.startswith("realtime"):
            out_path = out_path.with_name(f"realtime_{aa_label}.png")
        if not renderer.save_screenshot(str(out_path)):
            raise RuntimeError(f"Failed to save: {out_path}")
        print(f"[caustica] saved: {out_path} (aa={aa_label})")


def run_interactive(caustica, args, scene: str, ply_path: Path) -> None:
    with make_renderer(
        caustica, args, scene, ply_path, realtime=True, headless=False
    ) as renderer:
        s = renderer.settings
        renderer.app.set_realtime_mode(
            standalone_denoiser=False, realtime_aa=int(caustica.RealtimeAA.Off)
        )
        s.enable_tone_mapping = args.tonemap
        s.enable_bloom = args.bloom
        configure_gaussian_splats(caustica, s, args)
        configure_camera(renderer, args, ply_path)
        run_window_loop(renderer)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="caustica 3DGS interactive / batch example.")
    parser.add_argument("--mode", choices=["interactive", "reference", "realtime", "batch"], default="interactive")
    parser.add_argument("--ply", type=Path, default=DEFAULT_PLY)
    parser.add_argument("--scene", default=None)
    parser.add_argument("--out-dir", type=Path, default=Path("gaussian_splats_out"))
    parser.add_argument("--out", default="splat.png", help="Screenshot for single reference/realtime mode.")
    parser.add_argument("--width", type=int, default=1280)
    parser.add_argument("--height", type=int, default=720)
    parser.add_argument("--frames", type=int, default=32)
    parser.add_argument("--bounces", type=int, default=8)
    parser.add_argument("--vulkan", action="store_true")
    parser.add_argument(
        "--adapter",
        default="auto",
        help="GPU selector: auto, index:N, name:text, uuid:hex, or luid:hex",
    )
    parser.add_argument("--side", choices=["front", "back", "left", "right", "top"], default="front")
    parser.add_argument("--distance-scale", type=float, default=3.0)
    parser.add_argument("--fov", type=float, default=45.0)
    parser.add_argument("--cam-pos", nargs=3, type=float)
    parser.add_argument("--cam-dir", nargs=3, type=float)
    parser.add_argument("--cam-up", nargs=3, type=float)
    parser.add_argument("--sample-cap", type=int, default=200_000)
    parser.add_argument("--sorting", choices=["gpu", "stochastic"], default="gpu")
    parser.add_argument("--storage-format", choices=["float32", "float16", "uint8"], default="uint8")
    parser.add_argument("--splat-scale", type=float, default=1.0)
    parser.add_argument("--alpha-scale", type=float, default=1.0)
    parser.add_argument("--brightness", type=float, default=1.0)
    parser.add_argument("--alpha-cull", type=float, default=1.0 / 255.0)
    parser.add_argument("--translation", nargs=3, type=float)
    parser.add_argument("--rotation", nargs=3, type=float)
    parser.add_argument("--object-scale", nargs=3, type=float)
    parser.add_argument("--depth-test", dest="depth_test", action="store_true", default=True)
    parser.add_argument("--no-depth-test", dest="depth_test", action="store_false")
    parser.add_argument("--rdf-to-rub", dest="rdf_to_rub", action="store_true", default=True)
    parser.add_argument("--no-rdf-to-rub", dest="rdf_to_rub", action="store_false")
    parser.add_argument("--mip-antialiasing", action="store_true")
    parser.add_argument("--frustum-culling", choices=["disabled", "distance", "raster"], default="raster")
    parser.add_argument("--shadow-mode", choices=["disabled", "hard", "soft"], default="disabled")
    parser.add_argument("--realtime-aa", choices=["dlss-rr", "dlss", "taa", "off"], default="dlss-rr")
    parser.add_argument("--oidn-gpu", dest="oidn_gpu", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--tonemap", action="store_true")
    parser.add_argument("--bloom", action="store_true")
    # Compatibility with older test_splat_interactive.py flag name.
    parser.add_argument("--headless", action="store_true", help="Alias: force --mode reference when interactive.")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    mode = args.mode
    if args.headless and mode == "interactive":
        mode = "reference"

    ply_path = args.ply.resolve()
    if not ply_path.exists():
        raise FileNotFoundError(
            f"PLY not found: {ply_path}\nPass --ply <path> to a local 3DGS .ply file."
        )

    launch_cwd = Path.cwd()
    import caustica

    scene = args.scene or str(create_splat_only_scene())
    print(f"[caustica] mode={mode} ply={ply_path}")
    print(f"[caustica] scene={scene}")

    if mode == "interactive":
        run_interactive(caustica, args, scene, ply_path)
        return 0

    out_dir = resolve_output_path(args.out_dir, launch_cwd)
    out_dir.mkdir(parents=True, exist_ok=True)
    if mode in {"reference", "batch"}:
        out = out_dir / "reference_oidn.png" if mode == "batch" else resolve_output_path(args.out, launch_cwd)
        render_reference(caustica, args, scene, ply_path, out)
    if mode in {"realtime", "batch"}:
        out = out_dir / "realtime.png" if mode == "batch" else resolve_output_path(args.out, launch_cwd)
        render_realtime(caustica, args, scene, ply_path, out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
