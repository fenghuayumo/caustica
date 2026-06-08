#!/usr/bin/env python
"""Launch caustica with a native builtin default scene.

The caustica.Renderer(scene=...) binding accepts file names, builtin primitive
references, and inline scene JSON strings. This example uses inline JSON plus a
builtin primitive model, so it does not depend on any mesh file in Assets.

Usage:
    cd <repo>/bin
    python ../caustica/Python/Examples/launch_default_scene.py

    # Non-interactive smoke test:
    python ../caustica/Python/Examples/launch_default_scene.py --headless --out default_scene.png

    # OBJ import smoke test:
    python ../caustica/Python/Examples/launch_default_scene.py --headless --obj-test --out antman_obj.png
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
DEFAULT_OBJ_MODEL = Path(r"D:\ScanVideo\models\antman_merged.obj")
IMPORTED_MODEL_TRANSLATION = (0.35, 0.2, -0.45)
IMPORTED_MODEL_SCALING = (0.9, 0.9, 0.9)
IMPORTED_MODEL_BASE_COLOR = (0.72, 0.84, 1.0)


def configure_import_path() -> None:
    try:
        import caustica  # noqa: F401

        return
    except ImportError:
        pass

    candidates = [
        REPO_ROOT / "bin",
        REPO_ROOT / "build-linux" / "bin",
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


def build_default_scene_description(caustica) -> str:
    """Return an inline scene description string for a plane + cube."""
    if hasattr(caustica, "builtin_scene_json"):
        return caustica.builtin_scene_json("plane_cube")

    # Fallback for source readability; current C++ builds provide the helper.
    scene = {
        "models": ["builtin:plane_cube"],
        "graph": [
            {
                "name": "DefaultPlaneCube",
                "model": 0,
            },
            {
                "name": "Lights",
                "children": [
                    {
                        "name": "Sun",
                        "type": "DirectionalLight",
                        "rotation": [
                            -0.2305389071743629,
                            -0.15879165885860183,
                            -0.6890465942713406,
                            0.6684697541989844,
                        ],
                        "angularSize": 1.5,
                        "color": [1.0, 0.96, 0.9],
                        "irradiance": 3.0,
                    },
                    {
                        "name": "Fill",
                        "type": "PointLight",
                        "translation": [0.0, 2.5, 3.0],
                        "color": [1.0, 0.95, 0.85],
                        "intensity": 30.0,
                        "radius": 0.05,
                        "range": 10.0,
                    }
                ],
            },
            {
                "name": "Cameras",
                "children": [
                    {
                        "name": "Default",
                        "type": "PerspectiveCameraEx",
                        "translation": [0.0, 1.15, 5.0],
                        "rotation": [0.0, 0.0, 0.0, 1.0],
                        "verticalFov": 0.7,
                        "zNear": 0.001,
                        "exposureCompensation": 1.0,
                        "enableAutoExposure": False,
                    }
                ],
            },
            {
                "name": "SampleSettings",
                "type": "SampleSettings",
                "realtimeMode": True,
                "startingCamera": -1,
            },
        ],
    }
    return json.dumps(scene, indent=2)


def resolve_path(path: str) -> Path:
    resolved = Path(path).expanduser()
    if not resolved.is_absolute():
        resolved = Path.cwd() / resolved
    return resolved.resolve()


def bounds_to_center_radius(
    bounds: tuple[tuple[float, float, float], tuple[float, float, float]] | None,
) -> tuple[tuple[float, float, float], float] | None:
    """Convert a ((min), (max)) AABB into a (center, radius) framing helper."""
    if not bounds:
        return None
    mins, maxs = bounds
    center = tuple((lo + hi) * 0.5 for lo, hi in zip(mins, maxs))
    extent = [hi - lo for lo, hi in zip(mins, maxs)]
    radius = max(max(extent) * 0.5, 0.1)
    return center, radius


def scene_bounds_center_radius(renderer) -> tuple[tuple[float, float, float], float] | None:
    """Pull the world-space AABB from the bound C++ Renderer helper."""
    return bounds_to_center_radius(renderer.get_scene_bounds())


def customize_imported_model(renderer, model_path: Path) -> None:
    """Adjust the imported model transform and tint its materials light blue."""
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
        print(f"[caustica] WARNING: scene node '{model_name}' not found, skipping transform override")

    # Runtime-imported PT materials are populated on the next rebuild step.
    renderer.step_n(1)

    tinted_count = 0
    for material in scene.get_materials():
        if material.model_name != model_name:
            continue
        material.base_color = IMPORTED_MODEL_BASE_COLOR
        material.enable_base_texture = False
        tinted_count += 1

    if tinted_count > 0:
        print(
            f"[caustica] Updated {tinted_count} material(s) for '{model_name}' "
            f"to light blue base_color={IMPORTED_MODEL_BASE_COLOR}"
        )
    else:
        print(f"[caustica] WARNING: no materials matched imported model '{model_name}'")

    # Propagate the light-blue material override before reading updated bounds.
    renderer.step_n(1)


def read_obj_bounds(path: Path) -> tuple[tuple[float, float, float], float]:
    """Fallback: estimate (center, radius) by parsing the OBJ's `v` lines."""
    mins = [float("inf"), float("inf"), float("inf")]
    maxs = [float("-inf"), float("-inf"), float("-inf")]
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


def frame_bounds(renderer, center: tuple[float, float, float], radius: float) -> None:
    camera_pos = (
        center[0],
        center[1] + radius * 0.15,
        center[2] + radius * 3.1,
    )
    camera_dir = (
        center[0] - camera_pos[0],
        center[1] - camera_pos[1],
        center[2] - camera_pos[2],
    )
    renderer.set_camera(camera_pos, camera_dir, (0.0, 1.0, 0.0))
    renderer.set_camera_fov(45.0)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Launch a native builtin scene and optional OBJ import test.")
    parser.add_argument("--headless", action="store_true", help="Render offscreen and exit.")
    parser.add_argument("--width", type=int, default=1280)
    parser.add_argument("--height", type=int, default=720)
    parser.add_argument("--spp", type=int, default=16, help="Reference samples for --headless.")
    parser.add_argument("--out", default="default_scene.png", help="Screenshot path for --headless.")
    parser.add_argument("--vulkan", action="store_true", help="Use Vulkan backend (recommended on Linux).")
    parser.add_argument("--oidn", action="store_true",
                        help="Enable OIDN denoising after reference accumulation (--headless recommended).")
    parser.add_argument("--oidn-gpu", dest="oidn_gpu",
                        action=argparse.BooleanOptionalAction, default=True,
                        help="Use OIDN GPU device when available.")
    parser.add_argument("--oidn-quality", type=int, default=2,
                        help="OIDN quality: 0=Fast, 1=Balanced, 2=High.")
    parser.add_argument("--obj-test", action="store_true",
                        help="Append an OBJ mesh to the default scene and frame the camera around it.")
    parser.add_argument("--obj-path", default=str(DEFAULT_OBJ_MODEL),
                        help="Mesh file used by --obj-test. Supports .obj, .gltf, and .glb.")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.oidn and not args.headless:
        print("[caustica] --oidn uses reference accumulation; enabling --headless")
        args.headless = True
    launch_cwd = Path.cwd()
    configure_import_path()
    import caustica

    scene = build_default_scene_description(caustica)
    obj_path = resolve_path(args.obj_path) if args.obj_test else None

    mode = "headless" if args.headless else "windowed"
    test_name = "Antman OBJ" if args.obj_test else "default plane + cube"
    print(f"[caustica] Launching {test_name} scene ({mode}) ...")
    use_reference = args.headless or args.oidn
    renderer = caustica.Renderer(
        width=args.width,
        height=args.height,
        headless=args.headless,
        vulkan=args.vulkan,
        scene=scene,
        realtime=not use_reference,
        accumulation_target=args.spp,
    )

    try:
        s = renderer.settings
        s.realtime_mode = not use_reference
        s.accumulation_target = args.spp
        s.bounce_count = 8
        s.enable_tone_mapping = True
        s.realtime_aa = caustica.RealtimeAA.Off
        if args.oidn:
            s.oidn_enabled = True
            s.oidn_use_gpu = args.oidn_gpu
            s.oidn_quality = args.oidn_quality
            s.oidn_apply()
            print("[caustica] OIDN enabled (reference mode, denoise after accumulation)")

        if obj_path is not None:
            if not obj_path.exists():
                raise FileNotFoundError(f"OBJ test mesh not found: {obj_path}")
            print(f"[caustica] Loading mesh: {obj_path}")
            if not renderer.load_mesh_file(str(obj_path)):
                raise RuntimeError(f"Failed to load mesh file: {obj_path}")
            customize_imported_model(renderer, obj_path)

            framing = scene_bounds_center_radius(renderer)
            if framing is not None:
                center, radius = framing
                print(f"[caustica] Framed scene bounds (C++): center={center}, radius={radius:.3f}")
            elif obj_path.suffix.lower() == ".obj":
                center, radius = read_obj_bounds(obj_path)
                print(f"[caustica] Framed OBJ bounds (fallback): center={center}, radius={radius:.3f}")
            else:
                center, radius = None, None

            if center is not None and radius is not None:
                frame_bounds(renderer, center, radius)

        if args.headless:
            label = f"{args.spp} spp"
            if args.oidn:
                label += " + OIDN"
            print(f"[caustica] Rendering {label} ...")
            frames = renderer.step_until_accumulated()
            out_path = Path(args.out)
            if not out_path.is_absolute():
                out_path = launch_cwd / out_path
            out_path = out_path.resolve()
            if not renderer.save_screenshot(str(out_path)):
                raise RuntimeError(f"Failed to save screenshot: {out_path}")
            print(f"[caustica] Saved: {out_path} ({frames} frames)")
        else:
            if args.obj_test:
                print(f"[caustica] Ready. Scene contains the default plane + cube and mesh: {obj_path}")
            else:
                print("[caustica] Ready. Default scene contains one plane with one cube on top.")
            print("[caustica]   Left-click  -> Inspector (Transform)")
            print("[caustica]   Right-click -> Material Editor")
            print("[caustica]   Close window or Ctrl+C to exit.")
            while renderer.step(-1.0):
                time.sleep(0.001)
    except KeyboardInterrupt:
        print("\n[caustica] Interrupted.")
    finally:
        renderer.close()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
