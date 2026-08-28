"""Minimal COLMAP sparse-model reader used by the ``colmap`` example mode.

Not an executable example. Reads ``cameras``/``images`` in either binary or text
form and converts COLMAP's OpenCV camera frame (right-down-front) to the
caustica convention.

Requires numpy, which is not a dependency of the caustica package itself:

    python -m pip install numpy
"""

from __future__ import annotations

import math
import os
import re
import struct
from dataclasses import dataclass
from pathlib import Path

try:
    import numpy as np
except ImportError as exc:  # pragma: no cover - environment dependent
    raise SystemExit(
        "The COLMAP example mode needs numpy. Install it with:\n"
        "    python -m pip install numpy"
    ) from exc

# COLMAP model id -> (name, parameter count). See colmap/src/base/camera_models.h
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

#: Models whose first parameters are a plain pinhole intrinsic matrix.
SUPPORTED_MODELS = {"PINHOLE", "SIMPLE_PINHOLE", "SIMPLE_RADIAL", "RADIAL", "OPENCV"}


@dataclass
class ColmapCamera:
    id: int
    model: str
    width: int
    height: int
    params: "np.ndarray"


@dataclass
class ColmapImage:
    id: int
    qvec: "np.ndarray"
    tvec: "np.ndarray"
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
    c2w: "np.ndarray"

    @property
    def vertical_fov_degrees(self) -> float:
        return math.degrees(2.0 * math.atan(self.height / (2.0 * self.fy)))


def _read_next_bytes(fid, num_bytes: int, fmt: str, endian: str = "<"):
    data = fid.read(num_bytes)
    if len(data) != num_bytes:
        raise SystemExit("Unexpected EOF while reading COLMAP binary file")
    return struct.unpack(endian + fmt, data)


def qvec2rotmat(qvec: "np.ndarray") -> "np.ndarray":
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
        num_cameras = _read_next_bytes(fid, 8, "Q")[0]
        for _ in range(num_cameras):
            camera_id, model_id, width, height = _read_next_bytes(fid, 24, "iiQQ")
            if model_id not in CAMERA_MODELS:
                raise SystemExit(f"Unsupported COLMAP camera model id {model_id}")
            model_name, num_params = CAMERA_MODELS[model_id]
            params = np.array(
                _read_next_bytes(fid, 8 * num_params, "d" * num_params), dtype=np.float64
            )
            cameras[camera_id] = ColmapCamera(camera_id, model_name, int(width), int(height), params)
    return cameras


def read_extrinsics_binary(path: Path) -> dict[int, ColmapImage]:
    images: dict[int, ColmapImage] = {}
    with path.open("rb") as fid:
        num_images = _read_next_bytes(fid, 8, "Q")[0]
        for _ in range(num_images):
            props = _read_next_bytes(fid, 64, "idddddddi")
            image_id = int(props[0])
            qvec = np.array(props[1:5], dtype=np.float64)
            tvec = np.array(props[5:8], dtype=np.float64)
            camera_id = int(props[8])
            name_bytes = bytearray()
            while True:
                ch = _read_next_bytes(fid, 1, "c")[0]
                if ch == b"\x00":
                    break
                name_bytes.extend(ch)
            name = name_bytes.decode("utf-8", errors="replace")
            num_points2d = _read_next_bytes(fid, 8, "Q")[0]
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
            images[int(elems[0])] = ColmapImage(
                int(elems[0]),
                np.array([float(v) for v in elems[1:5]], dtype=np.float64),
                np.array([float(v) for v in elems[5:8]], dtype=np.float64),
                int(elems[8]),
                elems[9],
            )
            fid.readline()  # Skip the 2D point observations.
    return images


def resolve_colmap_dir(path: Path) -> Path:
    """Accept either a model directory or a parent holding the usual ``0`` model."""
    if (path / "cameras.bin").exists() or (path / "cameras.txt").exists():
        return path
    nested = path / "0"
    if (nested / "cameras.bin").exists() or (nested / "cameras.txt").exists():
        return nested
    return path


def pinhole_params(camera: ColmapCamera) -> tuple[float, float, float, float]:
    if camera.model in {"PINHOLE", "OPENCV"}:
        fx, fy, cx, cy = camera.params[:4]
    elif camera.model in {"SIMPLE_PINHOLE", "SIMPLE_RADIAL", "RADIAL"}:
        f, cx, cy = camera.params[:3]
        fx = fy = f
    else:
        raise SystemExit(
            f"Unsupported COLMAP camera model {camera.model!r}; "
            f"supported: {', '.join(sorted(SUPPORTED_MODELS))}"
        )
    return float(fx), float(fy), float(cx), float(cy)


def load_views(
    colmap_dir: Path, name_prefix: str | None = None, name_contains: str | None = None
) -> list[RenderView]:
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
        raise SystemExit(f"Expected cameras/images .bin or .txt in {colmap_dir}")

    views: list[RenderView] = []
    for image in sorted(images.values(), key=lambda item: item.name):
        if name_prefix and not image.name.startswith(name_prefix):
            continue
        if name_contains and name_contains not in image.name:
            continue
        camera = cameras[image.camera_id]
        fx, fy, cx, cy = pinhole_params(camera)
        views.append(
            RenderView(
                image.id, image.name, camera.width, camera.height, fx, fy, cx, cy, c2w_from_image(image)
            )
        )

    if not views:
        raise SystemExit("No COLMAP views matched the selected filters")
    return views


def c2w_from_image(image: ColmapImage) -> "np.ndarray":
    """Invert COLMAP's world-to-camera transform ``p_cam = R @ p_world + t``."""
    w2c = np.eye(4, dtype=np.float64)
    w2c[:3, :3] = qvec2rotmat(image.qvec)
    w2c[:3, 3] = image.tvec
    return np.linalg.inv(w2c)


def caustica_camera(view: RenderView, convert_rdf_to_rub: bool):
    """Convert a COLMAP pose to a caustica ``(position, direction, up)`` triple.

    COLMAP cameras look down +Z with +Y pointing down. When the splat loader
    applies the RDF->RUB conversion, the pose has to be mirrored to match.
    """
    from _common import normalize

    c2w = view.c2w
    pos = c2w[:3, 3].copy()
    direction = c2w[:3, 2].copy()
    up = -c2w[:3, 1].copy()

    if convert_rdf_to_rub:
        mirror = np.array([1.0, -1.0, -1.0], dtype=np.float64)
        pos = pos * mirror
        direction = direction * mirror
        up = up * mirror

    return (float(pos[0]), float(pos[1]), float(pos[2])), normalize(direction), normalize(up)


def scaled_intrinsics(view: RenderView, width: int, height: int) -> tuple[float, float, float, float]:
    """Rescale ``K`` from the COLMAP image size to the render target size."""
    sx = width / view.width
    sy = height / view.height
    return float(view.fx * sx), float(view.fy * sy), float(view.cx * sx), float(view.cy * sy)


def safe_stem(name: str) -> str:
    stem = Path(name).stem
    return re.sub(r"[^A-Za-z0-9_.-]+", "_", stem)[:120] or "view"
