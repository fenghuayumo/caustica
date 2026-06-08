#!/usr/bin/env python
"""Render COLMAP camera views of a 3DGS PLY with the caustica Python module.

Default input:
    D:/ProgramCode/Python/demo_gsplat&blender/GS/gaussians.ply
    D:/ProgramCode/Python/demo_gsplat&blender/GS/sparse

The script reads COLMAP cameras/images directly, converts the OpenCV/COLMAP
camera frame to caustica/Donut when the 3DGS loader uses RDF->Donut conversion,
and saves one PNG per selected COLMAP image.
"""

from __future__ import annotations

import argparse
import glob
import json
import math
import os
import re
import struct
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

import numpy as np


REPO_ROOT = Path(__file__).resolve().parents[3]
DEFAULT_GS_DIR = Path(r"D:/ScanVideo/models/example/GS")
LEGACY_GS_DIR = Path(r"D:/ProgramCode/Python/demo_gsplat&blender/GS")


CAMERA_MODELS: dict[int, tuple[str, int]] = {
    0: ("SIMPLE_PINHOLE", 3),
    1: ("PINHOLE", 4),
    2: ("SIMPLE_RADIAL", 4),
    3: ("RADIAL", 5),
    4: ("OPENCV", 8),
    5: ("OPENCV_FISHEYE", 8),
    6: ("FULL_OPENCV", 12),
    7: ("FOV", 5),
    8: ("SIMPLE_RADIAL_FISHEYE", 4),
    9: ("RADIAL_FISHEYE", 5),
    10: ("THIN_PRISM_FISHEYE", 12),
}


@dataclass
class ColmapCamera:
    id: int
    model: str
    width: int
    height: int
    params: np.ndarray


@dataclass
class ColmapImage:
    id: int
    qvec: np.ndarray
    tvec: np.ndarray
    camera_id: int
    name: str


@dataclass
class RenderView:
    image_id: int
    name: str
    width: int
    height: int
    fx: float
    fy: float
    cx: float
    cy: float
    c2w: np.ndarray


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


def read_next_bytes(fid, num_bytes: int, fmt: str, endian: str = "<"):
    data = fid.read(num_bytes)
    if len(data) != num_bytes:
        raise EOFError("Unexpected EOF while reading COLMAP binary file")
    return struct.unpack(endian + fmt, data)


def qvec2rotmat(qvec: np.ndarray) -> np.ndarray:
    q0, q1, q2, q3 = qvec
    return np.array(
        [
            [1 - 2 * q2 * q2 - 2 * q3 * q3, 2 * q1 * q2 - 2 * q0 * q3, 2 * q3 * q1 + 2 * q0 * q2],
            [2 * q1 * q2 + 2 * q0 * q3, 1 - 2 * q1 * q1 - 2 * q3 * q3, 2 * q2 * q3 - 2 * q0 * q1],
            [2 * q3 * q1 - 2 * q0 * q2, 2 * q2 * q3 + 2 * q0 * q1, 1 - 2 * q1 * q1 - 2 * q2 * q2],
        ],
        dtype=np.float64,
    )


def read_intrinsics_binary(path: Path) -> dict[int, ColmapCamera]:
    cameras: dict[int, ColmapCamera] = {}
    with path.open("rb") as fid:
        num_cameras = read_next_bytes(fid, 8, "Q")[0]
        for _ in range(num_cameras):
            camera_id, model_id, width, height = read_next_bytes(fid, 24, "iiQQ")
            if model_id not in CAMERA_MODELS:
                raise ValueError(f"Unsupported COLMAP camera model id {model_id}")
            model_name, num_params = CAMERA_MODELS[model_id]
            params = np.array(read_next_bytes(fid, 8 * num_params, "d" * num_params), dtype=np.float64)
            cameras[camera_id] = ColmapCamera(camera_id, model_name, int(width), int(height), params)
    return cameras


def read_extrinsics_binary(path: Path) -> dict[int, ColmapImage]:
    images: dict[int, ColmapImage] = {}
    with path.open("rb") as fid:
        num_images = read_next_bytes(fid, 8, "Q")[0]
        for _ in range(num_images):
            props = read_next_bytes(fid, 64, "idddddddi")
            image_id = int(props[0])
            qvec = np.array(props[1:5], dtype=np.float64)
            tvec = np.array(props[5:8], dtype=np.float64)
            camera_id = int(props[8])
            name_bytes = bytearray()
            while True:
                ch = read_next_bytes(fid, 1, "c")[0]
                if ch == b"\x00":
                    break
                name_bytes.extend(ch)
            name = name_bytes.decode("utf-8", errors="replace")
            num_points2d = read_next_bytes(fid, 8, "Q")[0]
            fid.seek(24 * num_points2d, os.SEEK_CUR)
            images[image_id] = ColmapImage(image_id, qvec, tvec, camera_id, name)
    return images


def read_intrinsics_text(path: Path) -> dict[int, ColmapCamera]:
    cameras: dict[int, ColmapCamera] = {}
    with path.open("r", encoding="utf-8") as fid:
        for line in fid:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            elems = line.split()
            camera_id = int(elems[0])
            cameras[camera_id] = ColmapCamera(
                camera_id,
                elems[1],
                int(elems[2]),
                int(elems[3]),
                np.array([float(v) for v in elems[4:]], dtype=np.float64),
            )
    return cameras


def read_extrinsics_text(path: Path) -> dict[int, ColmapImage]:
    images: dict[int, ColmapImage] = {}
    with path.open("r", encoding="utf-8") as fid:
        while True:
            line = fid.readline()
            if not line:
                break
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            elems = line.split()
            image_id = int(elems[0])
            qvec = np.array([float(v) for v in elems[1:5]], dtype=np.float64)
            tvec = np.array([float(v) for v in elems[5:8]], dtype=np.float64)
            camera_id = int(elems[8])
            name = elems[9]
            fid.readline()
            images[image_id] = ColmapImage(image_id, qvec, tvec, camera_id, name)
    return images


def resolve_colmap_dir(path: Path) -> Path:
    if (path / "cameras.bin").exists() or (path / "cameras.txt").exists():
        return path
    nested = path / "0"
    if (nested / "cameras.bin").exists() or (nested / "cameras.txt").exists():
        return nested
    return path


def camera_params(camera: ColmapCamera) -> tuple[float, float, float, float]:
    if camera.model == "PINHOLE":
        fx, fy, cx, cy = camera.params[:4]
    elif camera.model in {"SIMPLE_PINHOLE", "SIMPLE_RADIAL", "RADIAL"}:
        f, cx, cy = camera.params[:3]
        fx = fy = f
    elif camera.model == "OPENCV":
        fx, fy, cx, cy = camera.params[:4]
    else:
        raise ValueError(f"Unsupported camera model for caustica test: {camera.model}")
    return float(fx), float(fy), float(cx), float(cy)


def load_colmap_views(colmap_dir: Path, name_prefix: str | None, name_contains: str | None) -> list[RenderView]:
    colmap_dir = resolve_colmap_dir(colmap_dir)
    cameras_bin = colmap_dir / "cameras.bin"
    images_bin = colmap_dir / "images.bin"
    cameras_txt = colmap_dir / "cameras.txt"
    images_txt = colmap_dir / "images.txt"

    if cameras_bin.exists() and images_bin.exists():
        cameras = read_intrinsics_binary(cameras_bin)
        images = read_extrinsics_binary(images_bin)
        print(f"[colmap] loaded binary model: {colmap_dir}")
    elif cameras_txt.exists() and images_txt.exists():
        cameras = read_intrinsics_text(cameras_txt)
        images = read_extrinsics_text(images_txt)
        print(f"[colmap] loaded text model: {colmap_dir}")
    else:
        raise FileNotFoundError(f"Expected cameras/images .bin or .txt in {colmap_dir}")

    views: list[RenderView] = []
    for image in sorted(images.values(), key=lambda item: item.name):
        if name_prefix and not image.name.startswith(name_prefix):
            continue
        if name_contains and name_contains not in image.name:
            continue

        camera = cameras[image.camera_id]
        fx, fy, cx, cy = camera_params(camera)
        c2w = colmap_c2w_from_image(image)
        views.append(RenderView(image.id, image.name, camera.width, camera.height, fx, fy, cx, cy, c2w))

    if not views:
        raise RuntimeError("No COLMAP views matched the selected filters")
    return views


def normalize(v: np.ndarray) -> tuple[float, float, float]:
    length = float(np.linalg.norm(v))
    if length <= 1e-10:
        return (0.0, 0.0, 1.0)
    v = v / length
    return (float(v[0]), float(v[1]), float(v[2]))


def colmap_w2c_from_image(image: ColmapImage) -> np.ndarray:
    """COLMAP world-to-camera: p_cam = R @ p_world + t."""
    w2c = np.eye(4, dtype=np.float64)
    w2c[:3, :3] = qvec2rotmat(image.qvec)
    w2c[:3, 3] = image.tvec
    return w2c


def colmap_c2w_from_image(image: ColmapImage) -> np.ndarray:
    return np.linalg.inv(colmap_w2c_from_image(image))


def caustica_camera_from_colmap(view: RenderView, convert_rdf_to_donut: bool):
    """Pose from COLMAP c2w: camera +Z forward, +Y down in OpenCV convention."""
    c2w = view.c2w
    pos = c2w[:3, 3].copy()
    direction = c2w[:3, 2].copy()
    up = -c2w[:3, 1].copy()

    if convert_rdf_to_donut:
        rdf_to_donut = np.array([1.0, -1.0, -1.0], dtype=np.float64)
        pos = pos * rdf_to_donut
        direction = direction * rdf_to_donut
        up = up * rdf_to_donut

    return (
        (float(pos[0]), float(pos[1]), float(pos[2])),
        normalize(direction),
        normalize(up),
    )


def scaled_colmap_intrinsics(view: RenderView, width: int, height: int) -> tuple[float, float, float, float]:
    sx = width / view.width
    sy = height / view.height
    return (
        float(view.fx * sx),
        float(view.fy * sy),
        float(view.cx * sx),
        float(view.cy * sy),
    )


def set_camera_intrinsics(renderer, fx: float, fy: float, cx: float, cy: float, width: float, height: float) -> None:
    if hasattr(renderer, "set_camera_intrinsics"):
        renderer.set_camera_intrinsics(fx, fy, cx, cy, width, height)
        return
    if hasattr(renderer, "app") and hasattr(renderer.app, "set_camera_intrinsics"):
        renderer.app.set_camera_intrinsics(fx, fy, cx, cy, width, height)
        return
    raise RuntimeError("This caustica build has no set_camera_intrinsics; rebuild caustica Python bindings.")


def apply_colmap_view_to_renderer(
    renderer,
    view: RenderView,
    width: int,
    height: int,
    convert_rdf_to_donut: bool,
    symmetric_fov: bool,
) -> dict:
    """Apply COLMAP extrinsics (c2w) + intrinsics (K) to caustica camera."""
    cam_pos, cam_dir, cam_up = caustica_camera_from_colmap(view, convert_rdf_to_donut)
    renderer.set_camera(cam_pos, cam_dir, cam_up)

    record: dict = {
        "caustica_position": cam_pos,
        "caustica_direction": cam_dir,
        "caustica_up": cam_up,
        "c2w": view.c2w.tolist(),
    }

    if symmetric_fov:
        fov_y = vertical_fov_degrees(view)
        renderer.set_camera_fov(fov_y)
        record["projection"] = "symmetric_vertical_fov"
        record["vertical_fov_degrees"] = fov_y
    else:
        fx, fy, cx, cy = scaled_colmap_intrinsics(view, width, height)
        set_camera_intrinsics(renderer, fx, fy, cx, cy, float(width), float(height))
        record["projection"] = "pinhole_intrinsics"
        record["fx"] = fx
        record["fy"] = fy
        record["cx"] = cx
        record["cy"] = cy
        record["width"] = width
        record["height"] = height

    return record


def vertical_fov_degrees(view: RenderView) -> float:
    return math.degrees(2.0 * math.atan(view.height / (2.0 * view.fy)))


def safe_stem(name: str) -> str:
    stem = Path(name).stem
    return re.sub(r"[^A-Za-z0-9_.-]+", "_", stem)[:120] or "view"


def create_splat_only_scene() -> Path:
    model_path = REPO_ROOT / "Assets" / "Models" / "ConvergenceTest" / "ConvergenceTest.gltf"
    if not model_path.exists():
        raise FileNotFoundError(f"Hidden dummy model not found: {model_path}")

    scene_path = Path(tempfile.gettempdir()) / "caustica_colmap_3dgs.scene.json"
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


def warn_projection_limits(view: RenderView) -> None:
    cx_center = view.width * 0.5
    cy_center = view.height * 0.5
    fx_fy_delta = abs(view.fx - view.fy) / max(abs(view.fy), 1e-6)
    cx_delta = abs(view.cx - cx_center)
    cy_delta = abs(view.cy - cy_center)
    if fx_fy_delta > 1e-3 or cx_delta > 0.5 or cy_delta > 0.5:
        print(
            "[warn] caustica Python camera currently uses symmetric vertical FOV only; "
            f"COLMAP K has fx={view.fx:.3f}, fy={view.fy:.3f}, "
            f"cx={view.cx:.3f}, cy={view.cy:.3f}. Pixel-perfect matching may differ."
        )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Render COLMAP 3DGS views with caustica.")
    parser.add_argument("--gs-dir", type=Path, default=DEFAULT_GS_DIR)
    parser.add_argument("--ply", type=Path, default=None)
    parser.add_argument("--colmap-dir", type=Path, default=None)
    parser.add_argument("--out-dir", type=Path, default=None)
    parser.add_argument("--width", type=int, default=0, help="Override output width. Default: first COLMAP camera width.")
    parser.add_argument("--height", type=int, default=0, help="Override output height. Default: first COLMAP camera height.")
    parser.add_argument("--max-views", type=int, default=8, help="0 means render all selected views.")
    parser.add_argument("--skip", type=int, default=0)
    parser.add_argument("--name-prefix", default=None)
    parser.add_argument("--name-contains", default=None)
    parser.add_argument("--frames-per-view", type=int, default=8)
    parser.add_argument("--warmup-frames", type=int, default=4)
    parser.add_argument("--vulkan", action="store_true")
    parser.add_argument("--adapter-index", type=int, default=-1)
    parser.add_argument("--windowed", action="store_true")
    parser.add_argument("--convert-rdf-to-donut", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--depth-test", action=argparse.BooleanOptionalAction, default=False)
    parser.add_argument("--splat-scale", type=float, default=1.0)
    parser.add_argument("--alpha-scale", type=float, default=1.0)
    parser.add_argument("--brightness", type=float, default=1.0)
    parser.add_argument("--alpha-cull", type=float, default=1.0 / 255.0)
    parser.add_argument("--tonemap", action="store_true")
    parser.add_argument("--bloom", action="store_true")
    parser.add_argument("--storage-format", choices=["float32", "float16", "uint8"], default="float32")
    parser.add_argument("--mip-antialiasing", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--symmetric-fov", action="store_true", help="Ignore cx/cy and use the old vertical-FOV-only projection.")
    return parser.parse_args()


def resolve_gs_dir(gs_dir: Path) -> Path:
    gs_dir = gs_dir.resolve()
    if (gs_dir / "gaussians.ply").exists() or (gs_dir / "sparse").exists():
        return gs_dir
    if LEGACY_GS_DIR.exists():
        print(f"[caustica] GS dir fallback: {LEGACY_GS_DIR}")
        return LEGACY_GS_DIR.resolve()
    return gs_dir


def main() -> int:
    args = parse_args()

    gs_dir = resolve_gs_dir(args.gs_dir)
    ply_path = (args.ply or (gs_dir / "gaussians.ply")).resolve()
    colmap_dir = (args.colmap_dir or (gs_dir / "sparse")).resolve()
    out_dir = (args.out_dir or (gs_dir / "caustica_rendered")).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    if not ply_path.exists():
        raise FileNotFoundError(f"PLY not found: {ply_path}")

    views = load_colmap_views(colmap_dir, args.name_prefix, args.name_contains)
    views = views[max(0, args.skip):]
    if args.max_views > 0:
        views = views[: args.max_views]
    if not views:
        raise RuntimeError("No views left after --skip/--max-views")

    first = views[0]
    width = args.width or first.width
    height = args.height or first.height

    print(f"[caustica] repo       : {REPO_ROOT}")
    print(f"[caustica] ply        : {ply_path}")
    print(f"[caustica] output     : {out_dir}")
    print(f"[caustica] views      : {len(views)}")
    print(f"[caustica] resolution : {width}x{height}")
    print(f"[caustica] RDF->Donut : {args.convert_rdf_to_donut}")
    print(f"[caustica] mip AA     : {args.mip_antialiasing}")
    if args.symmetric_fov:
        warn_projection_limits(first)
        print("[caustica] projection : symmetric vertical FOV only (--symmetric-fov)")
    else:
        fx0, fy0, cx0, cy0 = scaled_colmap_intrinsics(first, width, height)
        print(
            f"[caustica] projection : COLMAP K scaled to output "
            f"(fx={fx0:.3f}, fy={fy0:.3f}, cx={cx0:.3f}, cy={cy0:.3f})"
        )

    configure_import_path()
    import caustica

    scene = str(create_splat_only_scene())
    renderer = caustica.Renderer(
        width=width,
        height=height,
        headless=not args.windowed,
        vulkan=args.vulkan,
        adapter_index=args.adapter_index,
        scene=scene,
        realtime=True,
        accumulation_target=1,
    )

    camera_records = []
    try:
        settings = renderer.settings
        settings.realtime_mode = True
        settings.enable_gaussian_splats = True
        settings.gaussian_splat_depth_test = args.depth_test
        settings.gaussian_splat_scale = args.splat_scale
        settings.gaussian_splat_alpha_scale = args.alpha_scale
        settings.gaussian_splat_brightness = args.brightness
        settings.gaussian_splat_alpha_cull_threshold = args.alpha_cull
        if not renderer.load_gaussian_splats(str(ply_path), args.convert_rdf_to_donut):
            raise RuntimeError(f"Failed to load Gaussian splat: {ply_path}")
        settings.enable_tone_mapping = args.tonemap
        settings.enable_bloom = args.bloom
        settings.realtime_aa = int(caustica.RealtimeAA.Off)
        settings.gaussian_splat_sorting_mode = int(caustica.GaussianSplatSortMode.GpuSort)
        settings.gaussian_splat_frustum_culling = int(caustica.GaussianSplatFrustumCulling.AtRasterStage)
        settings.gaussian_splat_mip_antialiasing = args.mip_antialiasing

        storage = {
            "float32": caustica.GaussianSplatStorageFormat.Float32,
            "float16": caustica.GaussianSplatStorageFormat.Float16,
            "uint8": caustica.GaussianSplatStorageFormat.Uint8,
        }[args.storage_format]
        settings.gaussian_splat_sh_format = int(storage)
        settings.gaussian_splat_rgba_format = int(storage)

        if args.warmup_frames > 0:
            renderer.step_n(args.warmup_frames)

        for index, view in enumerate(views):
            if view.width != first.width or view.height != first.height:
                print(f"[warn] {view.name}: camera size {view.width}x{view.height}, rendering at {width}x{height}")

            cam_record = apply_colmap_view_to_renderer(
                renderer,
                view,
                width,
                height,
                args.convert_rdf_to_donut,
                args.symmetric_fov,
            )
            renderer.step_n(max(1, args.frames_per_view))

            out_path = out_dir / f"{index:04d}_{safe_stem(view.name)}.png"
            if not renderer.save_screenshot(str(out_path)):
                raise RuntimeError(f"Failed to save screenshot: {out_path}")

            print(f"[caustica] saved {index + 1}/{len(views)}: {out_path.name}")
            camera_records.append(
                {
                    "output": str(out_path),
                    "image_name": view.name,
                    "image_id": view.image_id,
                    "colmap_width": view.width,
                    "colmap_height": view.height,
                    "colmap_fx": view.fx,
                    "colmap_fy": view.fy,
                    "colmap_cx": view.cx,
                    "colmap_cy": view.cy,
                    **cam_record,
                }
            )
    finally:
        renderer.close()

    metadata_path = out_dir / "cameras_used.json"
    metadata_path.write_text(json.dumps(camera_records, indent=2), encoding="utf-8")
    print(f"[caustica] wrote metadata: {metadata_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
