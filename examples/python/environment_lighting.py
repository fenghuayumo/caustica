#!/usr/bin/env python
"""Light a scene with an HDRI environment map or the procedural sky.

Two things are worth copying out of this example. First, the environment source
is selected with ``app.set_environment_map`` using either a path or one of the
``==PROCEDURAL_SKY*==`` tokens. Second, ``settings.environment_map`` exposes the
intensity, tint, rotation and visibility of whichever source is active, and
those can be changed between frames without reloading the scene.

The scene is an inline JSON string with a rough floor, a metal cube and a mirror
ball, so the environment shows up in both the lighting and the reflections.

Examples:
    python examples/python/environment_lighting.py
    python examples/python/environment_lighting.py --case proc_sky_evening --window
    python examples/python/environment_lighting.py --list-cases
"""

from __future__ import annotations

import argparse
import json
from dataclasses import dataclass
from pathlib import Path

from _common import (
    ASSETS_DIR,
    add_device_args,
    add_quality_args,
    add_window_args,
    apply_common_settings,
    apply_realtime_mode,
    apply_reference_mode,
    import_caustica,
    make_renderer,
    render_reference_to,
    resolve_output_path,
    run_window_loop,
)

DEFAULT_HDRI = "EnvironmentMaps/20060807_wells6_hd.hdr"

# Environment source tokens understood by App.set_environment_map. Anything else
# is treated as a file path.
SCENE_DEFAULT_ENV = "==SCENE_DEFAULT=="
PROC_SKY = "==PROCEDURAL_SKY=="
PROC_SKY_MORNING = "==PROCEDURAL_SKY_MORNING=="
PROC_SKY_MIDDAY = "==PROCEDURAL_SKY_MIDDAY=="
PROC_SKY_EVENING = "==PROCEDURAL_SKY_EVENING=="


@dataclass(frozen=True)
class EnvCase:
    name: str
    notes: str
    env_source: str
    intensity: float = 1.0
    tint_color: tuple[float, float, float] = (1.0, 1.0, 1.0)
    rotation_xyz: tuple[float, float, float] = (0.0, 0.0, 0.0)
    visible_to_camera: bool = True
    # The procedural sky bakes over several frames before it is stable.
    warmup_frames: int = 0


def build_cases(hdri: str) -> list[EnvCase]:
    return [
        EnvCase("hdri_baseline", "HDRI at unit intensity, no rotation.", hdri),
        EnvCase(
            "hdri_warm_tint",
            "Warm tint multiplier and a brighter environment.",
            hdri,
            intensity=1.4,
            tint_color=(1.0, 0.82, 0.65),
        ),
        EnvCase(
            "hdri_rotated",
            "HDRI rotated 90 degrees around Y.",
            hdri,
            rotation_xyz=(0.0, 90.0, 0.0),
        ),
        EnvCase(
            "hdri_hidden_background",
            "HDRI lights the scene but is not drawn behind it.",
            hdri,
            visible_to_camera=False,
        ),
        EnvCase(
            "proc_sky_midday", "Procedural sky, midday preset.", PROC_SKY_MIDDAY, warmup_frames=24
        ),
        EnvCase(
            "proc_sky_evening",
            "Procedural sky, evening preset, rotated.",
            PROC_SKY_EVENING,
            rotation_xyz=(0.0, 120.0, 0.0),
            warmup_frames=24,
        ),
    ]


def resolve_hdri(path: str | Path) -> str:
    """Resolve an HDRI path against the repository Assets folder."""
    candidate = Path(path).expanduser()
    tried = [candidate, ASSETS_DIR / candidate, ASSETS_DIR / "EnvironmentMaps" / candidate.name]
    for entry in tried:
        if entry.is_file():
            return str(entry.resolve())
    listing = "\n".join(f"  {entry}" for entry in tried)
    raise SystemExit(
        f"HDRI not found. Tried:\n{listing}\n"
        "Place HDRIs under Assets/EnvironmentMaps/ or pass an absolute --hdri path."
    )


def build_scene_json(env_path: str) -> str:
    scene = {
        "models": ["builtin:plane", "builtin:cube", "builtin:sphere"],
        "graph": [
            {"name": "GroundPlane", "model": 0, "scaling": [2.0, 1.0, 2.0]},
            {"name": "SubjectCube", "model": 1, "translation": [-0.75, 0.5, 0.0]},
            {
                "name": "MirrorBall",
                "model": 2,
                "translation": [0.85, 0.55, -0.35],
                "scaling": [0.55, 0.55, 0.55],
            },
            {
                "name": "Lights",
                "children": [
                    {
                        "name": "Sky",
                        "type": "EnvironmentLight",
                        "radianceScale": [1.0, 1.0, 1.0],
                        "textureIndex": [0],
                        "rotation": [0.0],
                        "path": env_path,
                    }
                ],
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
                "name": "SceneSettings",
                "type": "SceneSettings",
                "startingCamera": -1,
                "maxBounces": 8,
                "maxDiffuseBounces": 4,
            },
        ],
    }
    return json.dumps(scene, indent=2)


def make_subjects_reflective(renderer) -> None:
    """Give the three builtin meshes materials that reveal the environment."""
    scene = renderer.app.scene
    if scene is None:
        return
    renderer.step_n(1)

    for material in scene.get_materials():
        name = material.name.lower()
        if any(key in name for key in ("plane", "floor", "ground")):
            material.base_color = (0.35, 0.35, 0.38)
            material.roughness = 0.55
            material.metalness = 0.05
        elif "cube" in name:
            material.base_color = (0.82, 0.82, 0.84)
            material.roughness = 0.18
            material.metalness = 0.85
        elif "sphere" in name:
            material.base_color = (0.95, 0.95, 0.98)
            material.roughness = 0.04
            material.metalness = 1.0

    renderer.settings.reset_accumulation = True
    renderer.step_n(1)


def apply_case(renderer, case: EnvCase, scene_hdri: str) -> None:
    app = renderer.app
    if case.env_source.startswith("==PROCEDURAL_SKY"):
        app.set_environment_map(case.env_source)
    else:
        # Point the scene's EnvironmentLight at the file and re-read it. Passing a
        # bare filename to set_environment_map would resolve it against the
        # installed module directory rather than this repository.
        resolved = resolve_hdri(case.env_source)
        if resolved != scene_hdri:
            sky = app.scene.find_light("Sky")
            if sky is not None:
                sky.environment_path = resolved
        app.set_environment_map(SCENE_DEFAULT_ENV)

    env = renderer.settings.environment_map
    env.enabled = True
    env.intensity = case.intensity
    env.tint_color = case.tint_color
    env.rotation_xyz = case.rotation_xyz
    env.visible_to_camera = case.visible_to_camera
    renderer.settings.reset_accumulation = True

    if case.warmup_frames:
        print(f"[caustica]   baking environment for {case.warmup_frames} frame(s) ...")
        renderer.step_n(case.warmup_frames)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "--case", default="", help="Render a single named case instead of all of them."
    )
    parser.add_argument("--list-cases", action="store_true", help="Print case names and exit.")
    parser.add_argument("--out-dir", default="environment_lighting_out")
    parser.add_argument("--hdri", default=DEFAULT_HDRI, help="Assets-relative or absolute HDRI.")
    add_window_args(parser)
    add_device_args(parser)
    add_quality_args(parser, spp=32)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.list_cases:
        for case in build_cases(args.hdri):
            print(f"{case.name:24s}  {case.notes}")
        return 0

    scene_hdri = resolve_hdri(args.hdri)
    cases = build_cases(scene_hdri)
    if args.case:
        cases = [case for case in cases if case.name == args.case]
        if not cases:
            known = ", ".join(c.name for c in build_cases(scene_hdri))
            raise SystemExit(f"Unknown --case {args.case!r}. Known cases: {known}")

    caustica = import_caustica()
    out_dir = resolve_output_path(args.out_dir)
    print(f"[caustica] HDRI  : {scene_hdri}")
    print(f"[caustica] Cases : {len(cases)} ({'headless' if args.headless else 'windowed'})")

    with make_renderer(
        caustica,
        args,
        scene=build_scene_json(scene_hdri),
        realtime=not args.headless,
        accumulation_target=args.spp,
    ) as renderer:
        if args.headless:
            label = apply_reference_mode(renderer, caustica, spp=args.spp, oidn=False)
        else:
            label = apply_realtime_mode(renderer.app, caustica, "off")
        apply_common_settings(renderer, caustica, bounces=args.bounces)
        make_subjects_reflective(renderer)

        for index, case in enumerate(cases, start=1):
            print(f"\n[caustica] Case {index}/{len(cases)}: {case.name} -- {case.notes}")
            apply_case(renderer, case, scene_hdri)
            if args.headless:
                render_reference_to(renderer, out_dir / f"{case.name}.png", label=label)
            else:
                run_window_loop(renderer)
                break

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
