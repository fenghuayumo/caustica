#!/usr/bin/env python
"""Test HDRI environment maps and procedural sky presets.

The scene layout follows Assets/default.json (ground plane + subject + camera)
but uses builtin primitives and an inline JSON string so no external mesh paths
are required. Each test case renders a reference frame while varying
EnvironmentMapParams (intensity, tint, rotation, visibility) and/or switching
between HDRI files and procedural-sky presets.

Usage:
    cd <repo>
    python caustica/Python/Examples/test_envmap_proc_sky.py

    # Run only HDRI or proc-sky suites:
    python caustica/Python/Examples/test_envmap_proc_sky.py --suite hdri
    python caustica/Python/Examples/test_envmap_proc_sky.py --suite proc-sky

    # Single named case, interactive preview:
    python caustica/Python/Examples/test_envmap_proc_sky.py --case hdri_rotation_y90 --window

    # Custom output folder and sample count:
    python caustica/Python/Examples/test_envmap_proc_sky.py --out-dir ./env_tests --spp 64
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[3]
REPO_ASSETS = REPO_ROOT / "Assets"
DEFAULT_HDRI_REL = "EnvironmentMaps/20060807_wells6_hd.hdr"
DEFAULT_HDRI = str((REPO_ASSETS / DEFAULT_HDRI_REL).resolve())

# Reset env-map override back to the EnvironmentLight.path from scene JSON.
SCENE_DEFAULT_ENV = "==SCENE_DEFAULT=="

# Procedural sky tokens (see SampleCommon.h / SampleUI.cpp).
PROC_SKY_DEFAULT = "==PROCEDURAL_SKY=="
PROC_SKY_MORNING = "==PROCEDURAL_SKY_MORNING=="
PROC_SKY_MIDDAY = "==PROCEDURAL_SKY_MIDDAY=="
PROC_SKY_EVENING = "==PROCEDURAL_SKY_EVENING=="
PROC_SKY_DAWN = "==PROCEDURAL_SKY_DAWN=="
PROC_SKY_PITCHBLACK = "==PROCEDURAL_SKY_PITCHBLACK=="


def configure_import_path() -> None:
    # Prefer a local build (bin/) over an installed pip package. A bare
    # "import caustica" would load site-packages first while ShaderPrecompiled
    # and the .pyd next to the repo may be newer or the only complete copy.
    candidates = [
        REPO_ROOT / "bin",
        REPO_ROOT / "build-linux" / "bin",
        REPO_ROOT / "build" / "caustica" / "Release",
        Path(__file__).resolve().parent,
    ]
    for candidate in candidates:
        if glob.glob(str(candidate / "caustica*.pyd")) or glob.glob(str(candidate / "caustica*.so")):
            for name in list(sys.modules):
                if name == "caustica" or name.startswith("caustica."):
                    del sys.modules[name]
            sys.path.insert(0, str(candidate))
            os.environ["PATH"] = str(candidate) + os.pathsep + os.environ.get("PATH", "")
            os.chdir(candidate)
            return

    try:
        import caustica  # noqa: F401

        return
    except ImportError:
        pass

    searched = "\n".join(f"  {p}" for p in candidates)
    raise RuntimeError(f"Could not find caustica Python module. Searched:\n{searched}")


def resolve_hdri_path(path: str | Path) -> str:
    """Resolve an HDRI path against the repo Assets folder.

    Relative paths such as ``EnvironmentMaps/foo.hdr`` are resolved under
    ``<repo>/Assets``. Absolute paths are returned as-is when the file exists.
    """
    candidate = Path(path).expanduser()
    if candidate.is_file():
        return str(candidate.resolve())

    repo_candidate = REPO_ASSETS / candidate
    if repo_candidate.is_file():
        return str(repo_candidate.resolve())

    envmaps_candidate = REPO_ASSETS / "EnvironmentMaps" / candidate.name
    if envmaps_candidate.is_file():
        return str(envmaps_candidate.resolve())

    raise FileNotFoundError(
        "HDRI file not found. Tried:\n"
        f"  {candidate}\n"
        f"  {repo_candidate}\n"
        f"  {envmaps_candidate}\n"
        "Place HDRIs under Assets/EnvironmentMaps/ or pass an absolute --hdri path."
    )


def build_env_test_scene_json(
    env_path: str,
    *,
    include_local_lights: bool = False,
) -> str:
    """Inline scene JSON modeled after Assets/default.json."""
    env_path = resolve_hdri_path(env_path)
    lights: list[dict[str, Any]] = []

    if include_local_lights:
        lights.extend(
            [
                {
                    "name": "Sun",
                    "type": "DirectionalLight",
                    "rotation": [-0.23053891, -0.15879166, -0.68904659, 0.66846975],
                    "angularSize": 1.5,
                    "color": [1.0, 0.96, 0.9],
                    "irradiance": 1.0,
                },
                {
                    "name": "Fill",
                    "type": "PointLight",
                    "translation": [0.0, 2.5, 3.0],
                    "color": [1.0, 0.95, 0.85],
                    "intensity": 30.0,
                    "radius": 0.05,
                    "range": 10.0,
                },
            ]
        )

    lights.append(
        {
            "name": "Sky",
            "type": "EnvironmentLight",
            "radianceScale": [1.0, 1.0, 1.0],
            "textureIndex": [0],
            "rotation": [0.0],
            "path": env_path,
        }
    )

    scene = {
        "models": ["builtin:plane", "builtin:cube", "builtin:sphere"],
        "graph": [
            {
                "name": "GroundPlane",
                "model": 0,
                "translation": [0.0, 0.0, 0.0],
                "rotation": [0.0, 0.0, 0.0, 1.0],
                "scaling": [2.0, 1.0, 2.0],
            },
            {
                "name": "SubjectCube",
                "model": 1,
                "translation": [-0.75, 0.5, 0.0],
                "rotation": [0.0, 0.0, 0.0, 1.0],
                "scaling": [1.0, 1.0, 1.0],
            },
            {
                "name": "MirrorBall",
                "model": 2,
                "translation": [0.85, 0.55, -0.35],
                "rotation": [0.0, 0.0, 0.0, 1.0],
                "scaling": [0.55, 0.55, 0.55],
            },
            {
                "name": "Lights",
                "children": lights,
            },
            {
                "name": "Cameras",
                "children": [
                    {
                        "name": "Default",
                        "type": "PerspectiveCameraEx",
                        "translation": [0.0, 1.6, 6.0],
                        "rotation": [0.0, 0.0, 0.0, 1.0],
                        "verticalFov": 0.7,
                        "zNear": 0.001,
                        "enableAutoExposure": False,
                        "exposureCompensation": 1.0,
                    }
                ],
            },
            {
                "name": "SampleSettings",
                "type": "SampleSettings",
                "realtimeMode": True,
                "enableAnimations": False,
                "startingCamera": -1,
                "maxBounces": 8,
                "maxDiffuseBounces": 4,
                "realtimeFireflyFilter": 0.15,
            },
        ],
    }
    return json.dumps(scene, indent=2)


@dataclass(frozen=True)
class EnvTestCase:
    name: str
    suite: str
    env_source: str | None = None
    intensity: float = 1.0
    tint_color: tuple[float, float, float] = (1.0, 1.0, 1.0)
    rotation_xyz: tuple[float, float, float] = (0.0, 0.0, 0.0)
    enabled: bool = True
    visible_to_camera: bool = True
    warmup_frames: int = 0
    notes: str = ""


def hdri_test_cases() -> list[EnvTestCase]:
    return [
        EnvTestCase(
            name="hdri_baseline",
            suite="hdri",
            env_source=DEFAULT_HDRI_REL,
            notes="Default HDRI, unit intensity and no rotation.",
        ),
        EnvTestCase(
            name="hdri_high_intensity",
            suite="hdri",
            env_source=DEFAULT_HDRI_REL,
            intensity=2.5,
            notes="Brighter environment lighting.",
        ),
        EnvTestCase(
            name="hdri_warm_tint",
            suite="hdri",
            env_source=DEFAULT_HDRI_REL,
            tint_color=(1.0, 0.82, 0.65),
            intensity=1.4,
            notes="Warm tint multiplier on the HDRI.",
        ),
        EnvTestCase(
            name="hdri_cool_tint",
            suite="hdri",
            env_source=DEFAULT_HDRI_REL,
            tint_color=(0.72, 0.86, 1.0),
            intensity=1.2,
            notes="Cool tint multiplier on the HDRI.",
        ),
        EnvTestCase(
            name="hdri_rotation_y90",
            suite="hdri",
            env_source=DEFAULT_HDRI_REL,
            rotation_xyz=(0.0, 90.0, 0.0),
            notes="Rotate the environment 90 degrees around Y.",
        ),
        EnvTestCase(
            name="hdri_rotation_x45",
            suite="hdri",
            env_source=DEFAULT_HDRI_REL,
            rotation_xyz=(45.0, 0.0, 0.0),
            notes="Tilt the environment map 45 degrees around X.",
        ),
        EnvTestCase(
            name="hdri_hidden_background",
            suite="hdri",
            env_source=DEFAULT_HDRI_REL,
            visible_to_camera=False,
            notes="HDRI contributes lighting but not visible background.",
        ),
        EnvTestCase(
            name="hdri_disabled",
            suite="hdri",
            env_source=DEFAULT_HDRI_REL,
            enabled=False,
            notes="Environment disabled; scene should fall back to black sky.",
        ),
    ]


def proc_sky_test_cases() -> list[EnvTestCase]:
    warmup = 24
    return [
        EnvTestCase(
            name="proc_sky_default",
            suite="proc-sky",
            env_source=PROC_SKY_DEFAULT,
            warmup_frames=warmup,
            notes="Animated procedural sky (time-of-day cycles).",
        ),
        EnvTestCase(
            name="proc_sky_morning",
            suite="proc-sky",
            env_source=PROC_SKY_MORNING,
            warmup_frames=warmup,
            notes="Morning preset.",
        ),
        EnvTestCase(
            name="proc_sky_midday",
            suite="proc-sky",
            env_source=PROC_SKY_MIDDAY,
            warmup_frames=warmup,
            notes="Midday preset.",
        ),
        EnvTestCase(
            name="proc_sky_evening",
            suite="proc-sky",
            env_source=PROC_SKY_EVENING,
            warmup_frames=warmup,
            notes="Evening preset.",
        ),
        EnvTestCase(
            name="proc_sky_dawn",
            suite="proc-sky",
            env_source=PROC_SKY_DAWN,
            warmup_frames=warmup,
            notes="Dawn preset.",
        ),
        EnvTestCase(
            name="proc_sky_pitchblack",
            suite="proc-sky",
            env_source=PROC_SKY_PITCHBLACK,
            warmup_frames=warmup,
            notes="Night / pitch-black preset.",
        ),
        EnvTestCase(
            name="proc_sky_midday_bright",
            suite="proc-sky",
            env_source=PROC_SKY_MIDDAY,
            intensity=2.0,
            tint_color=(1.0, 0.98, 0.92),
            warmup_frames=warmup,
            notes="Midday with boosted intensity and warm tint.",
        ),
        EnvTestCase(
            name="proc_sky_midday_hidden_bg",
            suite="proc-sky",
            env_source=PROC_SKY_MIDDAY,
            visible_to_camera=False,
            warmup_frames=warmup,
            notes="Midday lighting only, background hidden from camera.",
        ),
        EnvTestCase(
            name="proc_sky_evening_rotated",
            suite="proc-sky",
            env_source=PROC_SKY_EVENING,
            rotation_xyz=(0.0, 120.0, 0.0),
            warmup_frames=warmup,
            notes="Evening preset with 120-degree Y rotation.",
        ),
    ]


def all_test_cases() -> list[EnvTestCase]:
    return hdri_test_cases() + proc_sky_test_cases()


def is_procedural_sky(path: str) -> bool:
    return path.startswith("==PROCEDURAL_SKY")


def customize_reflective_materials(renderer) -> None:
    """Make the test subjects show environment reflections clearly."""
    scene = renderer.app.scene
    if scene is None:
        return

    renderer.step_n(1)

    for material in scene.get_materials():
        name_lower = material.name.lower()
        if "plane" in name_lower or "floor" in name_lower or "ground" in name_lower:
            material.base_color = (0.35, 0.35, 0.38)
            material.roughness = 0.55
            material.metalness = 0.05
        elif "cube" in name_lower:
            material.base_color = (0.82, 0.82, 0.84)
            material.roughness = 0.18
            material.metalness = 0.85
        elif "sphere" in name_lower:
            material.base_color = (0.95, 0.95, 0.98)
            material.roughness = 0.04
            material.metalness = 1.0

    renderer.settings.reset_accumulation = True
    renderer.step_n(1)


def apply_env_params(settings, case: EnvTestCase) -> None:
    env = settings.environment_map
    env.intensity = case.intensity
    env.tint_color = case.tint_color
    env.rotation_xyz = case.rotation_xyz
    env.enabled = case.enabled
    env.visible_to_camera = case.visible_to_camera
    settings.reset_accumulation = True


def switch_environment(renderer, case: EnvTestCase, scene_hdri_path: str) -> None:
    if case.env_source is None:
        return

    app = renderer.app
    if is_procedural_sky(case.env_source):
        app.set_environment_map(case.env_source)
        return

    # HDRI: keep using the absolute path embedded in scene JSON. Calling
    # set_environment_map(filename) would prepend EnvironmentMaps/ and resolve
    # against the caustica module directory (pip install), not this repo.
    resolved = resolve_hdri_path(case.env_source)
    if resolved != scene_hdri_path:
        sky = app.scene.find_light("Sky")
        if sky is not None:
            sky.environment_path = resolved
    app.set_environment_map(SCENE_DEFAULT_ENV)


def render_case(
    renderer,
    case: EnvTestCase,
    *,
    scene_hdri_path: str,
    spp: int,
    warmup_frames: int,
    out_dir: Path | None,
) -> Path | None:
    switch_environment(renderer, case, scene_hdri_path)
    apply_env_params(renderer.settings, case)

    warmup = max(case.warmup_frames, warmup_frames)
    if warmup > 0:
        print(f"[caustica]   warmup {warmup} frame(s) for env-map bake ...")
        renderer.step_n(warmup)

    print(f"[caustica]   accumulating {spp} spp ...")
    t0 = time.time()
    frames = renderer.step_until_accumulated()
    elapsed = time.time() - t0
    print(f"[caustica]   done in {elapsed:.2f}s ({frames} frames)")

    if out_dir is None:
        return None

    out_dir.mkdir(parents=True, exist_ok=True)
    out_path = (out_dir / f"{case.name}.png").resolve()
    if not renderer.save_screenshot(str(out_path)):
        raise RuntimeError(f"Failed to save screenshot: {out_path}")
    print(f"[caustica]   saved: {out_path}")
    return out_path


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Test HDRI env maps and procedural sky with inline scene JSON."
    )
    parser.add_argument(
        "--suite",
        choices=["all", "hdri", "proc-sky"],
        default="all",
        help="Which test group to run (default: all).",
    )
    parser.add_argument(
        "--case",
        default="",
        help="Run a single named test case (see --list-cases).",
    )
    parser.add_argument(
        "--list-cases",
        action="store_true",
        help="Print available test case names and exit.",
    )
    mode_group = parser.add_mutually_exclusive_group()
    mode_group.add_argument(
        "--headless",
        action="store_true",
        default=True,
        help="Render offscreen and save PNGs (default).",
    )
    mode_group.add_argument(
        "--window",
        "--no-headless",
        dest="headless",
        action="store_false",
        help="Open an interactive preview for the selected case(s).",
    )
    parser.add_argument("--width", type=int, default=1280)
    parser.add_argument("--height", type=int, default=720)
    parser.add_argument("--spp", type=int, default=32, help="Reference samples per pixel.")
    parser.add_argument("--bounces", type=int, default=8)
    parser.add_argument(
        "--out-dir",
        default="envmap_proc_sky_tests",
        help="Output directory for PNG screenshots (headless mode).",
    )
    parser.add_argument(
        "--warmup-frames",
        type=int,
        default=4,
        help="Extra warmup frames applied to every case (proc-sky cases add more).",
    )
    parser.add_argument(
        "--with-local-lights",
        action="store_true",
        help="Include directional + point lights from default.json (off by default).",
    )
    parser.add_argument(
        "--hdri",
        default=DEFAULT_HDRI_REL,
        help="HDRI path for scene JSON (repo Assets-relative or absolute).",
    )
    parser.add_argument("--vulkan", action="store_true", help="Use Vulkan backend.")
    return parser


def select_cases(args: argparse.Namespace) -> list[EnvTestCase]:
    cases = all_test_cases()
    if args.suite == "hdri":
        cases = hdri_test_cases()
    elif args.suite == "proc-sky":
        cases = proc_sky_test_cases()

    if args.case:
        matched = [c for c in cases if c.name == args.case]
        if not matched:
            known = ", ".join(c.name for c in all_test_cases())
            raise SystemExit(f"Unknown --case {args.case!r}. Known cases: {known}")
        return matched
    return cases


def main() -> int:
    args = build_arg_parser().parse_args()

    if args.list_cases:
        for case in all_test_cases():
            print(f"{case.suite:9s}  {case.name:28s}  {case.notes}")
        return 0

    launch_cwd = Path.cwd()
    configure_import_path()
    import caustica

    cases = select_cases(args)
    scene_hdri_path = resolve_hdri_path(args.hdri)
    scene_json = build_env_test_scene_json(
        scene_hdri_path,
        include_local_lights=args.with_local_lights,
    )

    mode = "headless" if args.headless else "windowed"
    print(f"[caustica] Envmap / proc-sky test  ({len(cases)} case(s), {mode})")
    print(f"[caustica] HDRI file          : {scene_hdri_path}")

    renderer = caustica.Renderer(
        width=args.width,
        height=args.height,
        headless=args.headless,
        vulkan=args.vulkan,
        scene=scene_json,
        realtime=not args.headless,
        accumulation_target=args.spp,
    )

    try:
        settings = renderer.settings
        settings.realtime_mode = args.headless is False
        settings.accumulation_target = args.spp
        settings.accumulation_prewarm_realtime_caches = False
        settings.bounce_count = args.bounces
        settings.use_nee = True
        settings.enable_tone_mapping = True
        settings.realtime_aa = int(caustica.RealtimeAA.Off)
        settings.enable_animations = True

        customize_reflective_materials(renderer)

        out_dir: Path | None = None
        if args.headless:
            out_dir = Path(args.out_dir)
            if not out_dir.is_absolute():
                out_dir = launch_cwd / out_dir

        for index, case in enumerate(cases, start=1):
            print(
                f"\n[caustica] Case {index}/{len(cases)}: {case.name}"
                f"  ({case.suite})"
            )
            if case.notes:
                print(f"[caustica]   {case.notes}")
            if case.env_source:
                print(f"[caustica]   env_source={case.env_source}")
            print(
                f"[caustica]   intensity={case.intensity}, tint={case.tint_color}, "
                f"rotation={case.rotation_xyz}, enabled={case.enabled}, "
                f"visible_to_camera={case.visible_to_camera}"
            )

            if args.headless:
                render_case(
                    renderer,
                    case,
                    scene_hdri_path=scene_hdri_path,
                    spp=args.spp,
                    warmup_frames=args.warmup_frames,
                    out_dir=out_dir,
                )
            else:
                switch_environment(renderer, case, scene_hdri_path)
                apply_env_params(settings, case)
                warmup = max(case.warmup_frames, args.warmup_frames)
                if warmup > 0:
                    renderer.step_n(warmup)
                print("[caustica] Interactive preview ready. Close window or Ctrl+C to exit.")
                while renderer.step(-1.0):
                    time.sleep(0.001)
                break
    except KeyboardInterrupt:
        print("\n[caustica] Interrupted.")
    finally:
        renderer.close()

    if args.headless:
        print(f"\n[caustica] Finished {len(cases)} case(s).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
