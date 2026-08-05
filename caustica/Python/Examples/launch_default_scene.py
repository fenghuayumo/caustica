#!/usr/bin/env python
"""Launch a builtin scene, optional mesh import, and FPS smoke tests.

Uses ``caustica.builtin_scene_json("plane_cube")`` so no Assets mesh is required.
Optional ``--obj-test`` / ``--scene`` / ``--fps-test`` cover the common extension
smoke paths that previously lived in separate scripts.

Usage:
    python caustica/Python/Examples/launch_default_scene.py
    python caustica/Python/Examples/launch_default_scene.py --headless --out default.png
    python caustica/Python/Examples/launch_default_scene.py --headless --obj-test --obj-path Models/foo.obj
    python caustica/Python/Examples/launch_default_scene.py --fps-test --fps-frames 100
    python caustica/Python/Examples/launch_default_scene.py --scene Assets/default.json --headless
"""

from __future__ import annotations

import argparse
import statistics
import time
from pathlib import Path

from _common import (
    REPO_ROOT,
    frame_bounds,
    resolve_output_path,
    resolve_path,
    resolve_scene_arg,
    run_window_loop,
    scene_bounds_center_radius,
)

IMPORTED_MODEL_TRANSLATION = (0.35, 0.2, -0.45)
IMPORTED_MODEL_SCALING = (0.9, 0.9, 0.9)
IMPORTED_MODEL_BASE_COLOR = (0.72, 0.84, 1.0)


def customize_imported_model(
    renderer,
    model_path: Path,
    *,
    albedo: Path | None = None,
    tint: bool = True,
) -> None:
    scene = renderer.app.scene
    if scene is None:
        raise RuntimeError("No active scene after mesh import.")

    model_name = model_path.stem
    node = scene.find_node(model_name)
    if node is not None:
        node.translation = IMPORTED_MODEL_TRANSLATION
        node.scaling = IMPORTED_MODEL_SCALING
        print(
            f"[caustica] Updated node '{model_name}' transform: "
            f"translation={IMPORTED_MODEL_TRANSLATION}, scaling={IMPORTED_MODEL_SCALING}"
        )
    else:
        print(f"[caustica] WARNING: scene node '{model_name}' not found")

    renderer.step_n(1)

    materials = [m for m in scene.get_materials() if m.model_name == model_name]
    if albedo is not None:
        if not albedo.is_file():
            raise FileNotFoundError(f"Albedo image not found: {albedo}")
        for material in materials:
            ok = material.set_base_texture(str(albedo), srgb=True)
            material.enable_base_texture = True
            print(f"[caustica] set_base_texture('{material.name}') -> {ok}")
            if not ok:
                raise RuntimeError(f"set_base_texture failed for '{material.name}'")
    elif tint:
        for material in materials:
            material.base_color = IMPORTED_MODEL_BASE_COLOR
            material.enable_base_texture = False
        if materials:
            print(
                f"[caustica] Tinted {len(materials)} material(s) for '{model_name}' "
                f"to {IMPORTED_MODEL_BASE_COLOR}"
            )
        else:
            print(f"[caustica] WARNING: no materials matched '{model_name}'")

    renderer.step_n(1)


def read_obj_bounds(path: Path) -> tuple[tuple[float, float, float], float]:
    from _common import bounds_to_center_radius

    mins = [float("inf")] * 3
    maxs = [float("-inf")] * 3
    vertex_count = 0
    with path.open("r", encoding="utf-8", errors="ignore") as file:
        for line in file:
            if not line.startswith("v "):
                continue
            parts = line.split()
            if len(parts) < 4:
                continue
            try:
                xyz = [float(parts[1]), float(parts[2]), float(parts[3])]
            except ValueError:
                continue
            vertex_count += 1
            for axis, value in enumerate(xyz):
                mins[axis] = min(mins[axis], value)
                maxs[axis] = max(maxs[axis], value)
    if vertex_count == 0:
        raise RuntimeError(f"OBJ contains no readable vertex positions: {path}")
    result = bounds_to_center_radius((tuple(mins), tuple(maxs)))
    assert result is not None
    return result


def run_fps_test(
    renderer,
    warmup_frames: int,
    test_frames: int,
    *,
    save_frames: bool = False,
    output_dir: Path | None = None,
) -> dict:
    if save_frames and output_dir is not None:
        output_dir.mkdir(parents=True, exist_ok=True)
        print(f"[caustica] FPS Test: saving frames to {output_dir}")

    print(f"[caustica] FPS Test: warming up for {warmup_frames} frames...")
    for i in range(warmup_frames):
        if not renderer.step_n(1):
            raise RuntimeError(f"Renderer failed during FPS warmup frame {i}")

    print(f"[caustica] FPS Test: measuring {test_frames} frames...")
    frame_times: list[float] = []
    start_time = time.perf_counter()
    for i in range(test_frames):
        frame_start = time.perf_counter()
        if not renderer.step_n(1):
            raise RuntimeError(f"Renderer failed during FPS test frame {i}")
        frame_times.append((time.perf_counter() - frame_start) * 1000.0)
        if save_frames and output_dir is not None:
            frame_path = output_dir / f"frame_{i:04d}.png"
            if not renderer.save_screenshot(str(frame_path)):
                raise RuntimeError(f"Failed to save FPS frame {i}: {frame_path}")

    total_time = time.perf_counter() - start_time
    sorted_times = sorted(frame_times)
    p95 = sorted_times[min(int(len(sorted_times) * 0.95), len(sorted_times) - 1)]
    p99 = sorted_times[min(int(len(sorted_times) * 0.99), len(sorted_times) - 1)]
    max_ft = max(frame_times) / 1000.0
    min_ft = min(frame_times) / 1000.0
    return {
        "total_time": total_time,
        "avg_fps": test_frames / total_time,
        "min_fps": (1.0 / max_ft) if max_ft > 0 else float("inf"),
        "max_fps": (1.0 / min_ft) if min_ft > 0 else float("inf"),
        "avg_frame_time": statistics.mean(frame_times),
        "median_frame_time": sorted_times[len(sorted_times) // 2],
        "p95_frame_time": p95,
        "p99_frame_time": p99,
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Builtin scene launch, mesh import, and FPS smoke tests."
    )
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--headless", action="store_true", help="Render offscreen and exit.")
    mode.add_argument(
        "--window",
        dest="headless",
        action="store_false",
        help="Open an interactive window (default unless --headless/--fps-test).",
    )
    parser.set_defaults(headless=None)
    parser.add_argument("--width", type=int, default=1280)
    parser.add_argument("--height", type=int, default=720)
    parser.add_argument("--spp", type=int, default=16, help="Reference spp for headless.")
    parser.add_argument("--out", default="default_scene.png")
    parser.add_argument("--vulkan", action="store_true")
    parser.add_argument("--oidn", action="store_true", help="OIDN after reference accumulation.")
    parser.add_argument("--oidn-gpu", dest="oidn_gpu", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--oidn-quality", type=int, default=2)
    parser.add_argument(
        "--scene",
        default=None,
        help="Optional scene JSON/path. Default: builtin plane_cube JSON.",
    )
    parser.add_argument("--obj-test", action="store_true", help="Append a mesh and frame the camera.")
    parser.add_argument(
        "--obj-path",
        default=str(REPO_ROOT / "Assets" / "Models" / "GlassSphere" / "GlassSphere.gltf"),
        help="Mesh used by --obj-test (.obj/.gltf/.glb/...).",
    )
    parser.add_argument(
        "--albedo",
        default=None,
        help="Optional albedo image for Material.set_base_texture (with --obj-test).",
    )
    parser.add_argument("--fps-test", action="store_true", help="Headless FPS benchmark.")
    parser.add_argument("--fps-frames", type=int, default=100)
    parser.add_argument("--fps-warmup", type=int, default=10)
    parser.add_argument("--fps-save-frames", action="store_true")
    parser.add_argument("--fps-output-dir", type=str, default="./fps_frames")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.fps_test:
        args.headless = True
    elif args.headless is None:
        args.headless = False
    if args.oidn and not args.headless:
        print("[caustica] --oidn uses reference accumulation; enabling --headless")
        args.headless = True

    launch_cwd = Path.cwd()
    import caustica

    if args.scene:
        scene = resolve_scene_arg(args.scene)
    else:
        scene = caustica.builtin_scene_json("plane_cube")

    obj_path = resolve_path(args.obj_path) if args.obj_test else None
    albedo = resolve_path(args.albedo) if args.albedo else None
    use_reference = (args.headless or args.oidn) and not args.fps_test

    label = Path(args.scene).name if args.scene else ("mesh import" if args.obj_test else "builtin plane_cube")
    print(f"[caustica] Launching {label} ({'headless' if args.headless else 'windowed'}) ...")

    with caustica.Renderer(
        width=args.width,
        height=args.height,
        headless=args.headless,
        vulkan=args.vulkan,
        scene=scene,
        realtime=not use_reference,
        accumulation_target=args.spp,
    ) as renderer:
        s = renderer.settings
        if use_reference:
            renderer.app.set_reference_mode(
                spp=args.spp,
                oidn=args.oidn,
                oidn_quality=args.oidn_quality,
            )
            s.oidn_use_gpu = args.oidn_gpu
            if args.oidn:
                s.oidn_apply()
        else:
            renderer.app.set_realtime_mode(
                standalone_denoiser=False,
                realtime_aa=int(caustica.RealtimeAA.Off),
            )
        s.bounce_count = 8
        s.enable_tone_mapping = True

        if obj_path is not None:
            if not obj_path.exists():
                raise FileNotFoundError(f"Mesh not found: {obj_path}")
            print(f"[caustica] Loading mesh: {obj_path}")
            node = renderer.app.spawn_from_file(str(obj_path))
            if node is None:
                raise RuntimeError(f"spawn_from_file failed: {obj_path}")
            customize_imported_model(renderer, obj_path, albedo=albedo, tint=albedo is None)

            framing = scene_bounds_center_radius(renderer)
            if framing is None and obj_path.suffix.lower() == ".obj":
                framing = read_obj_bounds(obj_path)
            if framing is not None:
                center, radius = framing
                print(f"[caustica] Framing center={center}, radius={radius:.3f}")
                frame_bounds(renderer, center, radius)

        if args.fps_test:
            results = run_fps_test(
                renderer,
                args.fps_warmup,
                args.fps_frames,
                save_frames=args.fps_save_frames,
                output_dir=Path(args.fps_output_dir),
            )
            print("\n" + "=" * 50)
            print("FPS Benchmark Results")
            print("=" * 50)
            for key in (
                "avg_fps",
                "min_fps",
                "max_fps",
                "avg_frame_time",
                "median_frame_time",
                "p95_frame_time",
                "p99_frame_time",
                "total_time",
            ):
                print(f"  {key:20s} {results[key]}")
            print("=" * 50 + "\n")
        elif args.headless:
            label_spp = f"{args.spp} spp" + (" + OIDN" if args.oidn else "")
            print(f"[caustica] Rendering {label_spp} ...")
            frames = renderer.step_until_accumulated()
            out_path = resolve_output_path(args.out, launch_cwd)
            if not renderer.save_screenshot(str(out_path)):
                raise RuntimeError(f"Failed to save screenshot: {out_path}")
            print(f"[caustica] Saved: {out_path} ({frames} frames)")
        else:
            run_window_loop(renderer)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
