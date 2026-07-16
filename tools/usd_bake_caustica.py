#!/usr/bin/env python3
"""Bake OpenUSD (.usd/.usda/.usdc) stages into Caustica's .caususd cache.

Requires the pxr (OpenUSD) Python package.

Example:
  python tools/usd_bake_caustica.py d:/EMbdoy/fd_4stage_animation_v3_full.usdc
  # writes d:/EMbdoy/fd_4stage_animation_v3_full.caususd
"""

from __future__ import annotations

import argparse
import array
import math
import os
import struct
import sys
import time
from typing import Iterable, List, Optional, Sequence, Tuple

try:
    from pxr import Gf, Usd, UsdGeom
except ImportError as exc:  # pragma: no cover
    print("error: pxr (OpenUSD Python) is required:", exc, file=sys.stderr)
    sys.exit(2)

MAGIC = b"CAUSUSD\0"
VERSION = 2
FLAG_CONVERT_Z_UP_TO_Y_UP = 1 << 0
MESH_FLAG_XFORM_ANIM = 1 << 0
MESH_FLAG_POINT_ANIM = 1 << 1
MESH_FLAG_IS_CAMERA = 1 << 2  # unused for meshes; cameras are a separate table


def z_up_to_y_up_point(p: Gf.Vec3d) -> Gf.Vec3d:
    return Gf.Vec3d(p[0], p[2], -p[1])


def z_up_to_y_up_matrix(m: Gf.Matrix4d) -> Gf.Matrix4d:
    # Row-vector convention (USD): p_y = p_z * B, M_y = Binv * M_z * B
    b = Gf.Matrix4d(1, 0, 0, 0, 0, 0, -1, 0, 0, 1, 0, 0, 0, 0, 0, 1)
    return b.GetInverse() * m * b


def convert_point(p: Gf.Vec3f | Gf.Vec3d, convert: bool) -> Tuple[float, float, float]:
    v = Gf.Vec3d(float(p[0]), float(p[1]), float(p[2]))
    if convert:
        v = z_up_to_y_up_point(v)
    return (float(v[0]), float(v[1]), float(v[2]))


def convert_matrix(m: Gf.Matrix4d, convert: bool) -> Gf.Matrix4d:
    return z_up_to_y_up_matrix(m) if convert else Gf.Matrix4d(m)


def decompose_trs(m: Gf.Matrix4d) -> Tuple[Tuple[float, float, float], Tuple[float, float, float, float], Tuple[float, float, float]]:
    xf = Gf.Transform()
    xf.SetMatrix(m)
    t = xf.GetTranslation()
    q = xf.GetRotation().GetQuat()
    imag = q.GetImaginary()
    s = xf.GetScale()
    # xyzw
    return (
        (float(t[0]), float(t[1]), float(t[2])),
        (float(imag[0]), float(imag[1]), float(imag[2]), float(q.GetReal())),
        (float(s[0]), float(s[1]), float(s[2])),
    )




def dot4(a: Sequence[float], b: Sequence[float]) -> float:
    return sum(x * y for x, y in zip(a, b))


def normalized_quat(q: Sequence[float]) -> Tuple[float, float, float, float]:
    length = math.sqrt(max(dot4(q, q), 1e-30))
    return tuple(float(v / length) for v in q)  # type: ignore[return-value]


def quat_angle(a: Sequence[float], b: Sequence[float]) -> float:
    return 2.0 * math.acos(min(1.0, max(-1.0, abs(dot4(a, b)))))


def smooth_trs_keys(
    trs: List[float],
    translation_threshold: float,
    rotation_threshold_radians: float,
) -> List[float]:
    """Suppress small frame-to-frame simulation noise without blurring fast motion."""
    count = len(trs) // 10
    if count < 3:
        return trs

    out = list(trs)
    quats = [normalized_quat(trs[i * 10 + 3 : i * 10 + 7]) for i in range(count)]
    # Keep quaternion signs continuous. q and -q represent the same rotation, but
    # continuity also makes the adaptive smoothing below deterministic.
    for i in range(1, count):
        if dot4(quats[i - 1], quats[i]) < 0.0:
            quats[i] = tuple(-v for v in quats[i])
    for i, q in enumerate(quats):
        out[i * 10 + 3 : i * 10 + 7] = q

    for i in range(1, count - 1):
        prev = trs[(i - 1) * 10 : (i - 1) * 10 + 3]
        curr = trs[i * 10 : i * 10 + 3]
        nxt = trs[(i + 1) * 10 : (i + 1) * 10 + 3]
        d0 = math.dist(prev, curr)
        d1 = math.dist(curr, nxt)
        if max(d0, d1) <= translation_threshold:
            for c in range(3):
                out[i * 10 + c] = 0.25 * prev[c] + 0.5 * curr[c] + 0.25 * nxt[c]

        if max(quat_angle(quats[i - 1], quats[i]), quat_angle(quats[i], quats[i + 1])) <= rotation_threshold_radians:
            q = normalized_quat(
                tuple(
                    0.25 * quats[i - 1][c] + 0.5 * quats[i][c] + 0.25 * quats[i + 1][c]
                    for c in range(4)
                )
            )
            out[i * 10 + 3 : i * 10 + 7] = q

    # Always emit hemisphere-continuous, normalized endpoint quaternions too.
    out[3:7] = quats[0]
    out[(count - 1) * 10 + 3 : (count - 1) * 10 + 7] = quats[-1]
    return out


def smooth_point_frames(
    frames: List[List[Tuple[float, float, float]]],
    times: Sequence[float],
    displacement_threshold: float,
) -> List[List[Tuple[float, float, float]]]:
    """Apply a [1,2,1]/4 filter only where both adjacent vertex steps are small."""
    if len(frames) < 3 or displacement_threshold <= 0.0:
        return frames

    out = [list(frame) for frame in frames]
    threshold_sq = displacement_threshold * displacement_threshold
    for fi in range(1, len(frames) - 1):
        dt0 = times[fi] - times[fi - 1]
        dt1 = times[fi + 1] - times[fi]
        # The fixed kernel is valid only for near-uniform samples. In particular,
        # don't blend across gaps such as this sample's 0 -> 201 time-code jump.
        if dt0 <= 0.0 or dt1 <= 0.0 or abs(dt0 - dt1) > 0.01 * max(dt0, dt1):
            continue
        prev, curr, nxt = frames[fi - 1], frames[fi], frames[fi + 1]
        for vi, (p, c, n) in enumerate(zip(prev, curr, nxt)):
            d0_sq = sum((c[k] - p[k]) ** 2 for k in range(3))
            d1_sq = sum((n[k] - c[k]) ** 2 for k in range(3))
            if max(d0_sq, d1_sq) <= threshold_sq:
                out[fi][vi] = tuple(
                    0.25 * p[k] + 0.5 * c[k] + 0.25 * n[k] for k in range(3)
                )
    return out


def compute_vertex_normals(
    positions: Sequence[Tuple[float, float, float]],
    indices: Sequence[int],
) -> List[Tuple[float, float, float]]:
    normals = [(0.0, 0.0, 0.0) for _ in positions]
    for i in range(0, len(indices), 3):
        i0, i1, i2 = indices[i], indices[i + 1], indices[i + 2]
        p0, p1, p2 = positions[i0], positions[i1], positions[i2]
        e0 = (p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2])
        e1 = (p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2])
        n = (
            e0[1] * e1[2] - e0[2] * e1[1],
            e0[2] * e1[0] - e0[0] * e1[2],
            e0[0] * e1[1] - e0[1] * e1[0],
        )
        for idx in (i0, i1, i2):
            a, b, c = normals[idx]
            normals[idx] = (a + n[0], b + n[1], c + n[2])

    out: List[Tuple[float, float, float]] = []
    for n in normals:
        length = (n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) ** 0.5
        if length > 1e-20:
            out.append((n[0] / length, n[1] / length, n[2] / length))
        else:
            out.append((0.0, 1.0, 0.0))
    return out


def triangulate(
    counts: Sequence[int],
    indices: Sequence[int],
) -> List[int]:
    out: List[int] = []
    cursor = 0
    for count in counts:
        if count < 3:
            cursor += count
            continue
        face = indices[cursor : cursor + count]
        cursor += count
        for i in range(1, count - 1):
            out.extend((face[0], face[i], face[i + 1]))
    return out


def write_string(fh, text: str) -> None:
    data = text.encode("utf-8")
    fh.write(struct.pack("<I", len(data)))
    fh.write(data)


def write_f32_array(fh, values: Iterable[float]) -> None:
    # array.tofile avoids struct.pack(*millions) argument limits on dense meshes.
    buf = array.array("f", values)
    buf.tofile(fh)


def write_u32_array(fh, values: Sequence[int]) -> None:
    buf = array.array("I", values)
    buf.tofile(fh)


def mesh_display_color(prim: Usd.Prim) -> Tuple[float, float, float]:
    pv = UsdGeom.PrimvarsAPI(prim).GetPrimvar("displayColor")
    if pv and pv.HasValue():
        vals = pv.Get()
        if vals and len(vals) > 0:
            c = vals[0]
            return (float(c[0]), float(c[1]), float(c[2]))
    return (0.75, 0.75, 0.75)


def xform_has_samples(prim: Usd.Prim) -> bool:
    xf = UsdGeom.Xformable(prim)
    for op in xf.GetOrderedXformOps():
        if op.GetAttr().GetNumTimeSamples() > 0:
            return True
    return False


def gather_xform_keys(
    prim: Usd.Prim,
    fps: float,
    convert: bool,
    translation_smoothing: float,
    rotation_smoothing_degrees: float,
) -> Tuple[List[float], List[float]]:
    """Returns (times_seconds, interleaved tx ty tz qx qy qz qw sx sy sz)."""
    xf = UsdGeom.Xformable(prim)
    times: List[float] = []
    for op in xf.GetOrderedXformOps():
        times.extend(op.GetAttr().GetTimeSamples())
    times = sorted(set(times))
    if not times:
        m = convert_matrix(xf.ComputeLocalToWorldTransform(Usd.TimeCode.Default()), convert)
        t, q, s = decompose_trs(m)
        return [0.0], list(t) + list(q) + list(s)

    out_times: List[float] = []
    out_trs: List[float] = []
    for tc in times:
        m = convert_matrix(xf.ComputeLocalToWorldTransform(tc), convert)
        t, q, s = decompose_trs(m)
        out_times.append(float(tc) / fps)
        out_trs.extend(t)
        out_trs.extend(q)
        out_trs.extend(s)
    out_trs = smooth_trs_keys(
        out_trs,
        translation_smoothing,
        math.radians(rotation_smoothing_degrees),
    )
    return out_times, out_trs


def bake(
    stage_path: str,
    out_path: str,
    convert_z_up: bool,
    translation_smoothing: float,
    rotation_smoothing_degrees: float,
    point_smoothing: float,
) -> None:
    stage = Usd.Stage.Open(stage_path)
    if not stage:
        raise RuntimeError(f"failed to open stage: {stage_path}")

    fps = float(stage.GetTimeCodesPerSecond() or stage.GetFramesPerSecond() or 24.0)
    if fps <= 0:
        fps = 24.0
    start_tc = float(stage.GetStartTimeCode())
    end_tc = float(stage.GetEndTimeCode())
    up_axis = UsdGeom.GetStageUpAxis(stage)
    do_convert = convert_z_up and (up_axis == UsdGeom.Tokens.z)

    meshes = [p for p in stage.Traverse() if p.IsA(UsdGeom.Mesh)]
    cameras = [p for p in stage.Traverse() if p.IsA(UsdGeom.Camera)]

    print(f"stage: {stage_path}")
    print(f"  upAxis={up_axis} fps={fps} time=[{start_tc}, {end_tc}] convertYUp={do_convert}")
    print(f"  meshes={len(meshes)} cameras={len(cameras)}")

    t0 = time.time()
    with open(out_path, "wb") as fh:
        fh.write(MAGIC)
        flags = FLAG_CONVERT_Z_UP_TO_Y_UP if do_convert else 0
        fh.write(struct.pack("<I", VERSION))
        fh.write(struct.pack("<I", flags))
        fh.write(struct.pack("<fff", fps, start_tc / fps, end_tc / fps))
        fh.write(struct.pack("<II", len(meshes), len(cameras)))

        for mi, prim in enumerate(meshes):
            mesh = UsdGeom.Mesh(prim)
            pts_attr = mesh.GetPointsAttr()
            counts_attr = mesh.GetFaceVertexCountsAttr()
            indices_attr = mesh.GetFaceVertexIndicesAttr()

            point_times = pts_attr.GetTimeSamples() if pts_attr else []
            has_point_anim = len(point_times) > 1
            has_xform_anim = xform_has_samples(prim)

            # Topology / rest pose at first available sample (or default)
            sample_tc = point_times[0] if point_times else start_tc
            points = pts_attr.Get(sample_tc) if pts_attr else None
            counts = counts_attr.Get() if counts_attr else None
            indices = indices_attr.Get() if indices_attr else None
            if not points or not counts or not indices:
                print(f"  skip empty mesh {prim.GetPath()}")
                write_string(fh, str(prim.GetPath()))
                fh.write(struct.pack("<I", 0))
                color = mesh_display_color(prim)
                fh.write(struct.pack("<fff", *color))
                fh.write(struct.pack("<II", 0, 0))
                # Keep stream aligned with importer expectations.
                fh.write(struct.pack("<I", 0))  # xformKeyCount
                continue

            positions = [convert_point(p, do_convert) for p in points]
            tri = triangulate(list(counts), list(indices))
            normals = compute_vertex_normals(positions, tri)

            flags_m = 0
            if has_xform_anim:
                flags_m |= MESH_FLAG_XFORM_ANIM
            if has_point_anim:
                flags_m |= MESH_FLAG_POINT_ANIM

            write_string(fh, str(prim.GetPath()))
            fh.write(struct.pack("<I", flags_m))
            color = mesh_display_color(prim)
            fh.write(struct.pack("<fff", *color))
            fh.write(struct.pack("<II", len(positions), len(tri)))
            write_f32_array(fh, (c for p in positions for c in p))
            write_f32_array(fh, (c for n in normals for c in n))
            write_u32_array(fh, tri)

            # Rest pose and optional xform animation (always at least one key).
            times_s, trs = gather_xform_keys(
                prim,
                fps,
                do_convert,
                translation_smoothing,
                rotation_smoothing_degrees,
            )
            fh.write(struct.pack("<I", len(times_s)))
            write_f32_array(fh, times_s)
            write_f32_array(fh, trs)

            if has_point_anim:
                fh.write(struct.pack("<I", len(point_times)))
                write_f32_array(fh, (float(t) / fps for t in point_times))
                point_frames = [
                    [convert_point(p, do_convert) for p in pts_attr.Get(tc)]
                    for tc in point_times
                ]
                point_frames = smooth_point_frames(
                    point_frames,
                    point_times,
                    point_smoothing,
                )
                for tc, frame_pos in zip(point_times, point_frames):
                    if len(frame_pos) != len(positions):
                        raise RuntimeError(
                            f"{prim.GetPath()}: point count changed at t={tc} "
                            f"({len(frame_pos)} vs {len(positions)})"
                        )
                    write_f32_array(fh, (c for p in frame_pos for c in p))

            print(
                f"  [{mi + 1}/{len(meshes)}] {prim.GetPath()} "
                f"v={len(positions)} t={len(tri) // 3} "
                f"xformKeys={len(times_s) if has_xform_anim or True else 0} "
                f"pointFrames={len(point_times) if has_point_anim else 0}"
            )

        for prim in cameras:
            cam = UsdGeom.Camera(prim)
            write_string(fh, str(prim.GetPath()))
            focal = float(cam.GetFocalLengthAttr().Get() or 18.5)
            hap = float(cam.GetHorizontalApertureAttr().Get() or 20.955)
            vap = float(cam.GetVerticalApertureAttr().Get() or (hap * 9.0 / 16.0))
            near, far = cam.GetClippingRangeAttr().Get() or (0.1, 100000.0)
            # vertical FOV (radians)
            vfov = 2.0 * math.atan((0.5 * vap) / max(focal, 1e-6))
            fh.write(struct.pack("<ffff", vfov, float(near), float(far), focal))
            times_s, trs = gather_xform_keys(
                prim,
                fps,
                do_convert,
                translation_smoothing,
                rotation_smoothing_degrees,
            )
            fh.write(struct.pack("<I", len(times_s)))
            write_f32_array(fh, times_s)
            write_f32_array(fh, trs)
            print(f"  camera {prim.GetPath()} keys={len(times_s)} vfov_deg={vfov * 180.0 / math.pi:.2f}")

    print(f"wrote {out_path} ({os.path.getsize(out_path) / (1024 * 1024):.1f} MB) in {time.time() - t0:.1f}s")


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", help="Path to .usd/.usda/.usdc")
    parser.add_argument(
        "-o",
        "--output",
        help="Output .caususd path (default: input with .caususd extension)",
    )
    parser.add_argument(
        "--no-y-up",
        action="store_true",
        help="Do not convert Z-up stages to Y-up",
    )
    parser.add_argument(
        "--translation-smoothing",
        type=float,
        default=0.002,
        help="Smooth adjacent transform translations only below this distance (default: 0.002 m)",
    )
    parser.add_argument(
        "--rotation-smoothing",
        type=float,
        default=0.25,
        help="Smooth adjacent rotations only below this angle (default: 0.25 degrees)",
    )
    parser.add_argument(
        "--point-smoothing",
        type=float,
        default=0.002,
        help="Smooth adjacent vertex samples only below this distance (default: 0.002 m)",
    )
    args = parser.parse_args(argv)
    inp = os.path.abspath(args.input)
    out = os.path.abspath(args.output) if args.output else os.path.splitext(inp)[0] + ".caususd"
    bake(
        inp,
        out,
        convert_z_up=not args.no_y_up,
        translation_smoothing=max(0.0, args.translation_smoothing),
        rotation_smoothing_degrees=max(0.0, args.rotation_smoothing),
        point_smoothing=max(0.0, args.point_smoothing),
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
