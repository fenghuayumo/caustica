#!/usr/bin/env python
"""Interactive 3DGS-only rasterization test for the caustica Python module.

The script launches caustica with an empty mesh scene and a single 3DGS .ply file.
By default it opens a window and keeps stepping frames until the window is
closed or Ctrl+C is pressed. Use --headless --out <png> for a quick screenshot.

Examples:
    python test_splat_interactive.py
    python test_splat_interactive.py --ply D:/ScanVideo/Gingy/splat_crop.ply
    python test_splat_interactive.py --headless --out gingy.png
"""

from __future__ import annotations

import argparse
import glob
import json
import math
import os
import struct
import sys
import time
import tempfile
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
DEFAULT_PLY = Path(r"D:/ScanVideo/Gingy/splat_crop.ply")


def configure_import_path() -> None:
    candidates = [
        REPO_ROOT / "bin",
        REPO_ROOT / "build" / "caustica" / "Release",
        Path(__file__).resolve().parent,
    ]
    for candidate in candidates:
        if glob.glob(str(candidate / "caustica*.pyd")) or glob.glob(str(candidate / "caustica*.so")):
            sys.path.insert(0, str(candidate))
            os.environ["PATH"] = str(candidate) + os.pathsep + os.environ.get("PATH", "")
            os.chdir(candidate)
            return

    searched = "\n".join(f"  {p}" for p in candidates)
    raise RuntimeError(f"Could not find caustica Python module. Searched:\n{searched}")


def normalize(v: tuple[float, float, float] | list[float]) -> tuple[float, float, float]:
    length = math.sqrt(sum(x * x for x in v))
    if length <= 1e-8:
        return (0.0, 0.0, 1.0)
    return (v[0] / length, v[1] / length, v[2] / length)


def parse_binary_ply_bounds(ply_path: Path, convert_rdf_to_rub: bool, sample_cap: int) -> tuple[
    tuple[float, float, float],
    tuple[float, float, float],
    int,
]:
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
        raise RuntimeError(f"Only binary_little_endian PLY is supported by this helper: {format_line}")

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
                raise RuntimeError("List properties in the vertex element are not supported by this helper")
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
    mins = [float("inf"), float("inf"), float("inf")]
    maxs = [float("-inf"), float("-inf"), float("-inf")]
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

            x = read_float(row, "x")
            y = read_float(row, "y")
            z = read_float(row, "z")
            point = [x, -y, -z] if convert_rdf_to_rub else [x, y, z]
            for axis in range(3):
                mins[axis] = min(mins[axis], point[axis])
                maxs[axis] = max(maxs[axis], point[axis])
            sampled += 1

    if sampled == 0:
        raise RuntimeError("PLY has no sampled vertices")

    center = tuple((mins[i] + maxs[i]) * 0.5 for i in range(3))
    extents = tuple(maxs[i] - mins[i] for i in range(3))
    return center, extents, vertex_count


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
    model_path = REPO_ROOT / "Assets" / "Models" / "ConvergenceTest" / "ConvergenceTest.gltf"
    if not model_path.exists():
        raise FileNotFoundError(f"Hidden dummy model not found: {model_path}")

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


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="caustica 3DGS-only interactive test.")
    parser.add_argument("--ply", type=Path, default=DEFAULT_PLY, help="Path to the 3DGS .ply file.")
    parser.add_argument("--scene", default=None, help="Optional scene file. Defaults to a generated hidden-dummy scene.")
    parser.add_argument("--width", type=int, default=1280)
    parser.add_argument("--height", type=int, default=720)
    parser.add_argument("--headless", action="store_true", help="Render a screenshot and exit instead of opening a window.")
    parser.add_argument("--out", default="splat_test.png", help="Output image path for --headless.")
    parser.add_argument("--frames", type=int, default=16, help="Frames to render in --headless mode.")
    parser.add_argument("--side", choices=["front", "back", "left", "right", "top"], default="front")
    parser.add_argument("--distance-scale", type=float, default=3.0)
    parser.add_argument("--fov", type=float, default=45.0)
    parser.add_argument("--splat-scale", type=float, default=1.0)
    parser.add_argument("--alpha-scale", type=float, default=1.0)
    parser.add_argument("--brightness", type=float, default=1.0)
    parser.add_argument("--alpha-cull", type=float, default=1.0 / 255.0)
    parser.add_argument("--depth-test", dest="depth_test", action="store_true", default=True)
    parser.add_argument("--no-depth-test", dest="depth_test", action="store_false")
    parser.add_argument("--rdf-to-rub", dest="rdf_to_rub", action="store_true", default=True)
    parser.add_argument("--no-rdf-to-rub", dest="rdf_to_rub", action="store_false")
    parser.add_argument("--tonemap", action="store_true", help="Enable tone mapping.")
    parser.add_argument("--bloom", action="store_true", help="Enable bloom.")
    parser.add_argument("--cam-pos", nargs=3, type=float, metavar=("X", "Y", "Z"))
    parser.add_argument("--cam-dir", nargs=3, type=float, metavar=("X", "Y", "Z"))
    parser.add_argument("--cam-up", nargs=3, type=float, metavar=("X", "Y", "Z"))
    parser.add_argument("--sample-cap", type=int, default=200_000, help="PLY vertices sampled for camera bounds.")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    configure_import_path()

    import caustica

    ply_path = args.ply.resolve()
    if not ply_path.exists():
        raise FileNotFoundError(f"PLY not found: {ply_path}")

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

    print(f"[caustica] PLY: {ply_path}")
    print(f"[caustica] vertices: {vertex_count}")
    print(f"[caustica] center: {center}")
    print(f"[caustica] extents: {extents}")
    print(f"[caustica] camera position: {cam_pos}")
    print(f"[caustica] camera direction: {cam_dir}")

    scene = args.scene or str(create_splat_only_scene())
    print(f"[caustica] scene: {scene}")

    renderer = caustica.Renderer(
        width=args.width,
        height=args.height,
        headless=args.headless,
        scene=scene,
        realtime=True,
        accumulation_target=1,
    )

    try:
        settings = renderer.settings
        settings.realtime_mode = True
        settings.enable_gaussian_splats = True
        settings.gaussian_splat_depth_test = args.depth_test
        settings.gaussian_splat_scale = args.splat_scale
        settings.gaussian_splat_alpha_scale = args.alpha_scale
        settings.gaussian_splat_brightness = args.brightness
        settings.gaussian_splat_alpha_cull_threshold = args.alpha_cull
        settings.enable_tone_mapping = args.tonemap
        settings.enable_bloom = args.bloom
        settings.realtime_aa = caustica.RealtimeAA.Off
        if not renderer.load_gaussian_splats(str(ply_path), args.rdf_to_rub):
            raise RuntimeError(f"Failed to load Gaussian splat: {ply_path}")
        renderer.set_camera(cam_pos, cam_dir, cam_up)
        renderer.set_camera_fov(args.fov)

        if args.headless:
            renderer.step_n(max(1, args.frames))
            out_path = Path(args.out).resolve()
            if not renderer.save_screenshot(str(out_path)):
                raise RuntimeError(f"Failed to save screenshot: {out_path}")
            print(f"[caustica] saved: {out_path}")
            return 0

        print("[caustica] interactive: close the window or press Ctrl+C to exit.")
        while renderer.step(-1.0):
            time.sleep(0.001)
    except KeyboardInterrupt:
        print("\n[caustica] interrupted")
    finally:
        renderer.close()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
