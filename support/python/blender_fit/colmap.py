"""COLMAP sparse models; preserve world axes, explicitly convert camera frames."""
from __future__ import annotations

import struct
from pathlib import Path

import numpy as np


MODELS = {0: ("SIMPLE_PINHOLE", 3), 1: ("PINHOLE", 4), 2: ("SIMPLE_RADIAL", 4),
          3: ("RADIAL", 5), 4: ("OPENCV", 8), 5: ("OPENCV_FISHEYE", 8),
          6: ("FULL_OPENCV", 12), 7: ("FOV", 5), 8: ("SIMPLE_RADIAL_FISHEYE", 4),
          9: ("RADIAL_FISHEYE", 5), 10: ("THIN_PRISM_FISHEYE", 12),
          11: ("RAD_TAN_THIN_PRISM_FISHEYE", 16)}


def unpack(stream, fmt):
    fmt = "<" + fmt
    data = stream.read(struct.calcsize(fmt))
    if len(data) != struct.calcsize(fmt):
        raise ValueError("Truncated COLMAP binary model")
    return struct.unpack(fmt, data)


def rotation(q):
    q = np.asarray(q, dtype=float)
    if not np.isfinite(q).all() or abs(np.linalg.norm(q) - 1) > 1e-3:
        raise ValueError("Invalid COLMAP quaternion")
    w, x, y, z = q / np.linalg.norm(q)
    return np.array([[1-2*y*y-2*z*z, 2*x*y-2*w*z, 2*x*z+2*w*y],
                     [2*x*y+2*w*z, 1-2*x*x-2*z*z, 2*y*z-2*w*x],
                     [2*x*z-2*w*y, 2*y*z+2*w*x, 1-2*x*x-2*y*y]])


def load(directory):
    root = Path(directory).resolve()
    if not any((root / ("cameras" + ext)).exists() for ext in (".bin", ".txt")):
        root = root / "0"
    cameras, images, points = {}, [], []
    binary = (root / "cameras.bin").exists()
    if binary:
        with (root / "cameras.bin").open("rb") as f:
            for _ in range(unpack(f, "Q")[0]):
                cid, mid, width, height = unpack(f, "iiQQ")
                if mid not in MODELS:
                    raise ValueError(f"Unsupported camera model id {mid}")
                model, count = MODELS[mid]
                cameras[cid] = (model, width, height, unpack(f, "d" * count))
        with (root / "images.bin").open("rb") as f:
            for _ in range(unpack(f, "Q")[0]):
                row = unpack(f, "idddddddi")
                name = bytearray()
                while True:
                    ch = unpack(f, "c")[0]
                    if ch == b"\0":
                        break
                    name.extend(ch)
                    if len(name) > 4096:
                        raise ValueError("COLMAP image name too long")
                count = unpack(f, "Q")[0]
                for _ in range(count):
                    unpack(f, "ddq")
                images.append((row[0], row[1:5], row[5:8], row[8], name.decode("utf-8")))
        with (root / "points3D.bin").open("rb") as f:
            for _ in range(unpack(f, "Q")[0]):
                row = unpack(f, "QdddBBBd")
                points.append(row[1:4])
                for _ in range(unpack(f, "Q")[0]):
                    unpack(f, "ii")
    else:
        def lines(filename):
            return [line.strip() for line in (root / filename).read_text(encoding="utf-8").splitlines()
                    if not line.lstrip().startswith("#")]
        for line in lines("cameras.txt"):
            if line:
                row = line.split()
                cameras[int(row[0])] = (row[1], int(row[2]), int(row[3]), list(map(float, row[4:])))
        rows = lines("images.txt")
        i = 0
        while i < len(rows):
            if not rows[i]:
                i += 1
                continue
            row = rows[i].split(maxsplit=9)
            images.append((int(row[0]), list(map(float, row[1:5])), list(map(float, row[5:8])), int(row[8]), row[9]))
            i += 2  # The second line may legitimately be empty.
        for line in lines("points3D.txt"):
            if line:
                points.append(list(map(float, line.split()[1:4])))
    views = []
    for iid, q, t, cid, name in sorted(images, key=lambda row: row[4]):
        model, width, height, p = cameras[cid]
        if model not in {"PINHOLE", "SIMPLE_PINHOLE"}:
            raise ValueError(f"{model}: use COLMAP image_undistorter first; distorted images cannot be compared to pinhole renders")
        expected = 4 if model == "PINHOLE" else 3
        if len(p) != expected or not np.isfinite(p).all() or min(width, height, p[0]) <= 0:
            raise ValueError("Invalid camera intrinsics")
        fx, fy, cx, cy = p if expected == 4 else (p[0], p[0], p[1], p[2])
        if fy <= 0:
            raise ValueError("Invalid focal length")
        r = rotation(q)
        t = np.asarray(t, dtype=float)
        if not np.isfinite(t).all():
            raise ValueError("Invalid camera translation")
        views.append({"id": f"view_{iid}", "source_name": name, "width": width, "height": height,
                      "camera": {"position": (-r.T @ t).tolist(), "direction": r.T[:, 2].tolist(),
                                 "up": (-r.T[:, 1]).tolist(),
                                 "intrinsics": dict(zip(("fx", "fy", "cx", "cy"), (fx, fy, cx, cy)))}})
    points = np.asarray(points, dtype=float).reshape((-1, 3))
    if len(views) < 3 or not len(points) or not np.isfinite(points).all():
        raise ValueError("Need at least three views and a finite nonempty sparse point cloud")
    return views, points
