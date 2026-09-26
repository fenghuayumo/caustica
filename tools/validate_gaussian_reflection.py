from __future__ import annotations

import argparse
import math
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "examples" / "python"))
sys.path.insert(0, str(ROOT / "bin"))

from _common import apply_common_settings, apply_realtime_mode, import_caustica, make_engine, save_screenshot


def main() -> None:
    parser = argparse.ArgumentParser(description="Render the local 3DGS mirror regression with the workspace native module.")
    parser.add_argument("--realtime", action="store_true")
    parser.add_argument("--denoiser", choices=("off", "nrd", "taa", "dlss", "dlss-rr"), default="off")
    parser.add_argument("--secondary-off", action="store_true")
    parser.add_argument("--diffuse-lighting", choices=("on", "off"))
    options = parser.parse_args()
    caustica = import_caustica()
    print("Validation native module:", caustica.__file__, flush=True)
    args = argparse.Namespace(width=960, height=720, headless=True, vulkan=False, adapter="auto")
    scene = ROOT / "tools" / "gaussian_reflection.scene.json"
    with make_engine(caustica, args, scene=str(scene), realtime=False, accumulation_target=64) as engine:
        engine.settings.gaussian_splat_translation = (0.22, 1.285, 0.02)
        if not engine.load_gaussian_splat_file(str(ROOT / "Assets" / "statue.ply"), True):
            raise RuntimeError("failed to load statue.ply")

        engine.set_reference_mode(spp=64, oidn=False)
        apply_common_settings(engine, caustica, bounces=4)
        settings = engine.settings
        settings.enable_gaussian_splats = True
        settings.gaussian_splat_secondary_rays = not options.secondary_off
        settings.gaussian_splat_illuminate_meshes = options.diffuse_lighting == "on"
        settings.gaussian_splat_use_aabbs = True
        settings.gaussian_splat_use_tlas_instances = False
        settings.gaussian_splat_blas_compaction = True
        settings.gaussian_splat_translation = (0.22, 1.285, 0.02)
        settings.gaussian_splat_sh_format = int(caustica.GaussianSplatStorageFormat.Float16)
        if options.realtime:
            apply_realtime_mode(engine, caustica, options.denoiser)

        engine.step_n(1)
        for material in engine.scene.get_materials():
            material.base_color = (0.8, 0.8, 0.8)
            material.roughness = 1.0 if options.diffuse_lighting else 0.02
            material.metalness = 0.0 if options.diffuse_lighting else 1.0
        if options.diffuse_lighting:
            engine.settings.environment_map.intensity = 0.0

        target = (0.0, 0.0, 0.0)
        camera = (0.0, 3.0, -8.0)
        direction = tuple(target[i] - camera[i] for i in range(3))
        length = math.sqrt(sum(v * v for v in direction))
        direction = tuple(v / length for v in direction)
        engine.set_camera_pos_dir_up(camera, direction, (0.0, 1.0, 0.0))
        engine.set_camera_vertical_fov(math.radians(42.0))
        engine.request_full_accel_rebuild()
        settings.reset_accumulation = True
        engine.step_n(96)
        print("Mode:", settings.realtime_mode, "bounces:", settings.bounce_count, flush=True)
        mode = f"realtime_{options.denoiser}" if options.realtime else "reference"
        state = "off" if options.secondary_off else "on"
        suffix = f"diffuse_{options.diffuse_lighting}" if options.diffuse_lighting else state
        save_screenshot(engine, ROOT / "bin" / f"gaussian_reflection_{mode}_{suffix}.png")


if __name__ == "__main__":
    main()
