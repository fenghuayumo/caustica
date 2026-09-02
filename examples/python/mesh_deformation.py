#!/usr/bin/env python
"""Deform CPU mesh vertices from Python and rebuild the acceleration structures.

``engine.deform_mesh`` hands every vertex of a mesh entity to a Python callback and
writes the results back, optionally recomputing normals and rebuilding the ray
tracing acceleration structures so the change is visible to reflections and
shadows. This is the hook for driving geometry from simulation or learned
models rather than from imported animation channels.

Examples:
    python examples/python/mesh_deformation.py --window
    python examples/python/mesh_deformation.py --frames 48 --out-dir deformation_out
"""

from __future__ import annotations

import argparse
import math
import time
from pathlib import Path

from _common import (
    ASSETS_DIR,
    add_device_args,
    add_quality_args,
    add_window_args,
    apply_common_settings,
    apply_realtime_mode,
    import_caustica,
    make_engine,
    resolve_output_path,
    resolve_scene_arg,
    run_window_loop,
    save_screenshot,
)

DEFAULT_SCENE = ASSETS_DIR / "scenes" / "default" / "default.scene.json"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--scene", default=str(DEFAULT_SCENE))
    parser.add_argument(
        "--mesh-name",
        default="",
        help="Target mesh entity. Defaults to the densest mesh in the scene.",
    )
    parser.add_argument(
        "--deform-mode",
        choices=["wave", "breathe", "sway"],
        default="wave",
        help="Procedural deformation applied each frame.",
    )
    parser.add_argument("--amplitude", type=float, default=0.04, help="Strength in object units.")
    parser.add_argument("--speed", type=float, default=1.5)
    parser.add_argument("--frames", type=int, default=48, help="Frames for the headless sequence.")
    parser.add_argument("--out-dir", default="deformation_out")
    parser.add_argument(
        "--spp-per-frame", type=int, default=32, help="Accumulation steps per animation frame."
    )
    parser.add_argument(
        "--recompute-normals", action=argparse.BooleanOptionalAction, default=False
    )
    parser.add_argument(
        "--rebuild-accel",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="Rebuild ray tracing acceleration structures after each deformation.",
    )
    add_window_args(parser)
    add_device_args(parser)
    add_quality_args(parser)
    return parser.parse_args()


def find_target_mesh(engine, mesh_name: str):
    scene = engine.scene
    if scene is None:
        raise SystemExit("No scene loaded.")
    if mesh_name:
        entity = scene.find_mesh_entity(mesh_name)
        if entity is not None:
            return entity
        print(f"[caustica] Mesh {mesh_name!r} not found; picking the densest mesh instead.")

    entities = scene.get_mesh_entities()
    if not entities:
        raise SystemExit("No deformable mesh entity found in the loaded scene.")
    # Vertex count is a better proxy than the entity name: deforming a
    # four-vertex ground plane technically works but shows nothing.
    return max(entities, key=lambda entity: len(engine.get_mesh_vertices(entity)))


def mesh_center(vertices) -> tuple[float, float, float]:
    if not vertices:
        return (0.0, 0.0, 0.0)
    count = float(len(vertices))
    return tuple(sum(axis) / count for axis in zip(*vertices))  # type: ignore[return-value]


def make_deform_callback(base_vertices, center, mode: str, amplitude: float, t: float):
    cx, cy, cz = center

    def wave(index: int, _pos):
        x, y, z = base_vertices[index]
        return (x, y + amplitude * math.sin(t * 2.0 + x * 4.0 + z * 3.0), z)

    def breathe(index: int, _pos):
        x, y, z = base_vertices[index]
        scale = 1.0 + amplitude * 3.0 * math.sin(t)
        return (cx + (x - cx) * scale, cy + (y - cy) * scale, cz + (z - cz) * scale)

    def sway(index: int, _pos):
        x, y, z = base_vertices[index]
        angle = amplitude * 2.5 * math.sin(t)
        cos_a, sin_a = math.cos(angle), math.sin(angle)
        dx, dz = x - cx, z - cz
        return (cx + dx * cos_a - dz * sin_a, y, cz + dx * sin_a + dz * cos_a)

    return {"wave": wave, "breathe": breathe, "sway": sway}[mode]


def deform(engine, entity, base_vertices, center, args: argparse.Namespace, t: float) -> None:
    engine.deform_mesh(
        entity=entity,
        callback=make_deform_callback(base_vertices, center, args.deform_mode, args.amplitude, t),
        recompute_normals=args.recompute_normals,
        rebuild_acceleration_structure=args.rebuild_accel,
    )


def render_sequence(engine, entity, base_vertices, center, args, launch_cwd: Path) -> None:
    out_dir = resolve_output_path(args.out_dir, launch_cwd)
    steps = max(args.spp_per_frame, 1)
    print(f"[caustica] Rendering {args.frames} animation frames -> {out_dir}")

    started = time.perf_counter()
    for frame in range(args.frames):
        t = (frame / max(args.frames - 1, 1)) * math.pi * 2.0 * args.speed
        deform(engine, entity, base_vertices, center, args, t)
        engine.settings.reset_accumulation = True
        engine.step_n(steps)
        saved = save_screenshot(engine, out_dir / f"frame_{frame:04d}.png")
        print(f"[caustica] Saved: {saved}")

    print(
        f"[caustica] Sequence done in {time.perf_counter() - started:.2f}s "
        f"({args.frames} frames)"
    )


def main() -> int:
    args = parse_args()
    if args.frames <= 0:
        raise SystemExit("--frames must be positive")

    caustica = import_caustica()
    launch_cwd = Path.cwd()
    scene = resolve_scene_arg(args.scene)
    print(f"[caustica] Scene : {scene}")
    print(f"[caustica] Mode  : {'headless sequence' if args.headless else 'windowed animation'}")

    with make_engine(
        caustica,
        args,
        scene=scene,
        realtime=True,
        accumulation_target=max(args.spp_per_frame, 1),
    ) as engine:
        print(f"[caustica] Loaded scene: {engine.scene_name}")

        apply_realtime_mode(engine, caustica, "off")
        apply_common_settings(engine, caustica, bounces=args.bounces)
        settings = engine.settings
        settings.accumulation_target = max(args.spp_per_frame, 1)
        settings.accumulation_prewarm_realtime_caches = False
        # Imported animation channels would fight the per-frame vertex writes.
        settings.enable_animations = False
        if engine.gaussian_splat_object_count > 0:
            settings.enable_gaussian_splats = True

        entity = find_target_mesh(engine, args.mesh_name)
        base_vertices = list(engine.get_mesh_vertices(entity))
        if not base_vertices:
            raise SystemExit(f"Mesh entity {entity.name!r} has no readable CPU vertex cache.")
        center = mesh_center(base_vertices)
        print(f"[caustica] Target mesh : {entity.name} ({len(base_vertices)} vertices)")
        print(
            f"[caustica] Deformation: {args.deform_mode}, "
            f"amplitude={args.amplitude}, speed={args.speed}"
        )

        if args.headless:
            render_sequence(engine, entity, base_vertices, center, args, launch_cwd)
        else:
            def on_frame(elapsed: float) -> None:
                deform(engine, entity, base_vertices, center, args, elapsed * args.speed)
                engine.settings.reset_accumulation = True

            run_window_loop(engine, on_frame)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
