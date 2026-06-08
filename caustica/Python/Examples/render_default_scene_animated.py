#!/usr/bin/env python
"""Animated mesh deformation test on top of render_default_scene.py.

Loads Assets/default.json with Hybrid 3DGS + 3DGRT soft shadows and emitter
lighting, then animates the Antman mesh via Python vertex deformation.

Usage:
    cd <repo>

    # Interactive window with live deformation:
    python caustica/Python/Examples/render_default_scene_animated.py --window

    # Headless animation sequence:
    python caustica/Python/Examples/render_default_scene_animated.py \
        --headless --frames 48 --out-dir default_scene_anim

All options from render_default_scene.py are also available (hybrid mode,
shadow/emission knobs, etc.).
"""

from __future__ import annotations

import argparse
import math
import sys
import time
from pathlib import Path

EXAMPLES_DIR = Path(__file__).resolve().parent
if str(EXAMPLES_DIR) not in sys.path:
    sys.path.insert(0, str(EXAMPLES_DIR))

from render_default_scene import (
    DEFAULT_SCENE,
    apply_gaussian_settings_and_rebuild,
    build_arg_parser,
    configure_import_path,
    resolve_scene_arg,
)


def parse_args() -> argparse.Namespace:
    parser = build_arg_parser()
    parser.description = "Render default.json with animated mesh deformation."
    parser.add_argument("--mesh-name", default="antman_merged",
                        help="Target mesh name (default: antman_merged from default.json).")
    parser.add_argument("--deform-mode", choices=["wave", "breathe", "sway"], default="wave",
                        help="Procedural deformation preset applied each frame.")
    parser.add_argument("--amplitude", type=float, default=0.04,
                        help="Deformation strength in object-space units.")
    parser.add_argument("--speed", type=float, default=1.5,
                        help="Animation speed multiplier.")
    parser.add_argument("--frames", type=int, default=48,
                        help="Number of animation frames for headless sequence mode.")
    parser.add_argument("--out-dir", type=Path, default=Path("default_scene_anim"),
                        help="Output directory for headless frame sequence.")
    parser.add_argument("--spp-per-frame", type=int, default=32,
                        help="Realtime accumulation steps per animation frame.")
    parser.add_argument("--recompute-normals", dest="recompute_normals",
                        action=argparse.BooleanOptionalAction, default=False)
    parser.add_argument("--rebuild-accel", dest="rebuild_accel",
                        action=argparse.BooleanOptionalAction, default=True,
                        help="Rebuild ray tracing AS after each deformation.")
    return parser.parse_args()


def find_target_mesh(app, mesh_name: str):
    if mesh_name:
        mesh = app.find_mesh(mesh_name)
        if mesh is not None:
            return mesh

    for mesh in app.get_meshes():
        name = mesh.name.lower()
        if name in {"plane", "builtin_plane"} or name.startswith("builtin:"):
            continue
        return mesh

    meshes = app.get_meshes()
    if meshes:
        return meshes[-1]
    raise RuntimeError("No deformable mesh found in the loaded scene.")


def compute_mesh_center(vertices: list[tuple[float, float, float]]) -> tuple[float, float, float]:
    if not vertices:
        return (0.0, 0.0, 0.0)
    sx = sy = sz = 0.0
    for x, y, z in vertices:
        sx += x
        sy += y
        sz += z
    count = float(len(vertices))
    return (sx / count, sy / count, sz / count)


def make_deform_callback(
    base_vertices: list[tuple[float, float, float]],
    center: tuple[float, float, float],
    mode: str,
    amplitude: float,
    time_value: float,
):
    cx, cy, cz = center

    def callback(index: int, pos: tuple[float, float, float]):
        x, y, z = base_vertices[index]
        if mode == "wave":
            lift = amplitude * math.sin(time_value * 2.0 + x * 4.0 + z * 3.0)
            return (x, y + lift, z)
        if mode == "breathe":
            scale = 1.0 + amplitude * 3.0 * math.sin(time_value)
            return (
                cx + (x - cx) * scale,
                cy + (y - cy) * scale,
                cz + (z - cz) * scale,
            )
        if mode == "sway":
            angle = amplitude * 2.5 * math.sin(time_value)
            cos_a = math.cos(angle)
            sin_a = math.sin(angle)
            dx = x - cx
            dz = z - cz
            return (cx + dx * cos_a - dz * sin_a, y, cz + dx * sin_a + dz * cos_a)
        return None

    return callback


def apply_deformation(
    app,
    mesh,
    base_vertices: list[tuple[float, float, float]],
    center: tuple[float, float, float],
    args: argparse.Namespace,
    time_value: float,
) -> None:
    callback = make_deform_callback(
        base_vertices, center, args.deform_mode, args.amplitude, time_value
    )
    app.deform_mesh(
        mesh,
        callback,
        recompute_normals=args.recompute_normals,
        rebuild_acceleration_structure=args.rebuild_accel,
    )


def configure_renderer(renderer, caustica, args: argparse.Namespace) -> tuple[object, list[tuple[float, float, float]], tuple[float, float, float]]:
    settings = renderer.settings
    settings.realtime_mode = True
    settings.accumulation_target = max(args.spp_per_frame, 1)
    settings.accumulation_prewarm_realtime_caches = False
    settings.bounce_count = args.bounces
    settings.use_nee = True
    settings.enable_tone_mapping = True
    settings.realtime_aa = int(caustica.RealtimeAA.Off)
    settings.enable_animations = False
    apply_gaussian_settings_and_rebuild(renderer, caustica, settings, args)

    mesh = find_target_mesh(renderer.app, args.mesh_name)
    base_vertices = list(renderer.app.get_mesh_vertices(mesh))
    if not base_vertices:
        raise RuntimeError(f"Mesh '{mesh.name}' has no readable CPU vertex cache.")
    center = compute_mesh_center(base_vertices)
    print(f"[caustica] Target mesh : {mesh.name} ({mesh.vertex_count} vertices)")
    print(f"[caustica] Deform mode : {args.deform_mode}, amplitude={args.amplitude}, speed={args.speed}")
    return mesh, base_vertices, center


def render_sequence(renderer, mesh, base_vertices, center, args: argparse.Namespace, launch_cwd: Path) -> None:
    out_dir = args.out_dir
    if not out_dir.is_absolute():
        out_dir = launch_cwd / out_dir
    out_dir = out_dir.resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    settings = renderer.settings
    settings.realtime_mode = True
    settings.accumulation_target = max(args.spp_per_frame, 1)

    print(f"[caustica] Rendering {args.frames} animation frames -> {out_dir}")
    t_start = time.time()
    for frame in range(args.frames):
        time_value = (frame / max(args.frames - 1, 1)) * math.pi * 2.0 * args.speed
        apply_deformation(renderer.app, mesh, base_vertices, center, args, time_value)
        settings.reset_accumulation = True
        renderer.step_n(max(args.spp_per_frame, 1))

        out_path = out_dir / f"frame_{frame:04d}.png"
        if not renderer.save_screenshot(str(out_path)):
            raise RuntimeError(f"Failed to save screenshot: {out_path}")
        print(f"[caustica] Saved: {out_path}")

    elapsed = time.time() - t_start
    print(f"[caustica] Animation sequence done in {elapsed:.2f}s ({args.frames} frames)")


def run_window_loop(renderer, mesh, base_vertices, center, args: argparse.Namespace) -> None:
    print("[caustica] Ready. Animated mesh deformation running.")
    print("[caustica]   Close window or Ctrl+C to exit.")
    settings = renderer.settings
    frame = 0
    t0 = time.time()
    try:
        while True:
            time_value = (time.time() - t0) * args.speed
            apply_deformation(renderer.app, mesh, base_vertices, center, args, time_value)
            settings.reset_accumulation = True
            if not renderer.step(-1.0):
                break
            frame += 1
            time.sleep(0.001)
    except KeyboardInterrupt:
        print(f"\n[caustica] Interrupted after {frame} frames.")


def main() -> int:
    args = parse_args()
    if args.oidn:
        print("[caustica] OIDN is not supported for animated sequences; ignoring --oidn.")
        args.oidn = False

    launch_cwd = Path.cwd()
    configure_import_path()
    import caustica

    scene = resolve_scene_arg(args.scene)
    mode = "headless sequence" if args.headless else "windowed animation"
    print(f"[caustica] Scene : {scene}")
    print(f"[caustica] Mode  : {mode}")

    renderer = caustica.Renderer(
        width=args.width,
        height=args.height,
        headless=args.headless,
        vulkan=args.vulkan,
        scene=scene,
        realtime=True,
        accumulation_target=max(args.spp_per_frame, 1),
    )

    try:
        print(f"[caustica] Loaded scene: {renderer.app.scene_name}")
        mesh, base_vertices, center = configure_renderer(renderer, caustica, args)

        if args.headless:
            render_sequence(renderer, mesh, base_vertices, center, args, launch_cwd)
        else:
            run_window_loop(renderer, mesh, base_vertices, center, args)
    except KeyboardInterrupt:
        print("\n[caustica] Interrupted.")
    finally:
        renderer.close()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
