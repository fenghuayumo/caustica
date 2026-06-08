from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
BIN_DIR = ROOT / "bin"
DEFAULT_SCENES = ["builtin:plane_cube"]
DEFAULT_MODES = ["reference", "realtime"]


def split_csv(value: str, default: list[str]) -> list[str]:
    items = [item.strip() for item in value.replace(";", ",").split(",") if item.strip()]
    return items or list(default)


def configure_import_path() -> None:
    if hasattr(os, "add_dll_directory"):
        os.add_dll_directory(str(BIN_DIR))
    sys.path.insert(0, str(BIN_DIR))


def shader_bin_dir(shader_api: str) -> Path:
    if shader_api == "d3d12":
        return BIN_DIR / "ShaderDynamic" / "Bin" / "dxil"
    if shader_api == "vulkan":
        return BIN_DIR / "ShaderDynamic" / "Bin" / "spirv"
    raise ValueError(f"Unsupported shader API: {shader_api}")


def count_shader_bins(shader_api: str) -> int:
    path = shader_bin_dir(shader_api)
    if not path.exists():
        return 0
    return sum(1 for item in path.rglob("*.bin") if item.is_file())


def precompile_one(caustica, *, scene: str, shader_api: str, mode: str, frames: int) -> None:
    realtime = mode == "realtime"
    renderer = caustica.Renderer(
        width=64,
        height=64,
        headless=True,
        vulkan=shader_api == "vulkan",
        scene=scene,
        realtime=realtime,
        accumulation_target=1,
    )

    try:
        settings = renderer.settings
        settings.realtime_mode = realtime
        settings.accumulation_target = 1
        settings.reset_accumulation = True
        if hasattr(settings, "realtime_aa"):
            settings.realtime_aa = int(caustica.RealtimeAA.Off)
        if hasattr(settings, "accumulation_prewarm_realtime_caches"):
            settings.accumulation_prewarm_realtime_caches = False
        renderer.step_n(max(1, frames))
    finally:
        renderer.close()


def precompile(shader_api: str, scenes: list[str], modes: list[str], frames: int) -> None:
    configure_import_path()
    import caustica  # type: ignore

    before = count_shader_bins(shader_api)
    print(
        f"[caustica] Precompiling dynamic shaders: api={shader_api}, "
        f"scenes={len(scenes)}, modes={','.join(modes)}"
    )
    for scene in scenes:
        for mode in modes:
            print(f"[caustica]   scene={scene!r}, mode={mode}")
            precompile_one(caustica, scene=scene, shader_api=shader_api, mode=mode, frames=frames)

    after = count_shader_bins(shader_api)
    print(f"[caustica] Dynamic shader bins: {before} -> {after}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Warm caustica dynamic shader bins by rendering selected scenes on the build machine."
    )
    parser.add_argument(
        "--shader-api",
        choices=["d3d12", "vulkan"],
        default="d3d12" if os.name == "nt" else "vulkan",
    )
    parser.add_argument(
        "--scene",
        action="append",
        dest="scenes",
        help="Scene to load. Repeat for multiple scenes. Defaults to builtin:plane_cube.",
    )
    parser.add_argument(
        "--modes",
        default=",".join(DEFAULT_MODES),
        help="Comma/semicolon separated modes: reference,realtime.",
    )
    parser.add_argument(
        "--global-variant-preset",
        choices=["default", "coverage"],
        default="default",
        help="Accepted for compatibility with build_wheel.py; currently uses the renderer defaults.",
    )
    parser.add_argument(
        "--global-variant",
        action="append",
        default=[],
        help="Accepted for compatibility with build_wheel.py; currently uses the renderer defaults.",
    )
    parser.add_argument("--frames", type=int, default=1)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    modes = split_csv(args.modes, DEFAULT_MODES)
    invalid_modes = sorted(set(modes) - {"reference", "realtime"})
    if invalid_modes:
        raise ValueError(f"Unknown mode(s): {', '.join(invalid_modes)}")

    precompile(args.shader_api, args.scenes or list(DEFAULT_SCENES), modes, args.frames)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
