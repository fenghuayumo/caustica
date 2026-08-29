from __future__ import annotations

"""Assemble a portable Caustica executable package from bin/."""

import argparse
import shutil
import zipfile
from pathlib import Path

from build_wheel import (
    BIN_DIR,
    DIST_DIR,
    PROJECT_VERSION,
    RUNTIME_DIR_NAMES,
    directory_size,
    shader_types_for_api,
    write_shader_pack,
)


ROOT = Path(__file__).resolve().parents[2]
RUNTIME_DLL_SUFFIXES = {".dll", ".so", ".dylib"}


def _copy_file(src: Path, dest: Path) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dest)


def _copy_tree(src: Path, dest: Path) -> None:
    if not src.is_dir():
        return
    if dest.exists():
        shutil.rmtree(dest)
    shutil.copytree(src, dest, ignore=shutil.ignore_patterns(".git"))


def _copy_assets(stage_dir: Path, assets: str) -> None:
    if assets == "none":
        return

    builtin = ROOT / "assets-builtin"
    full_assets = ROOT / "Assets"
    if assets == "full":
        if not (full_assets / "pack.json").is_file():
            raise FileNotFoundError(f"Full asset pack not found: {full_assets / 'pack.json'}")
        _copy_tree(full_assets, stage_dir / "Assets")
        return

    if builtin.is_dir():
        _copy_tree(builtin, stage_dir / "Assets")
    elif (full_assets / "pack.json").is_file():
        _copy_tree(full_assets, stage_dir / "Assets")
    else:
        raise FileNotFoundError("Neither assets-builtin nor Assets/pack.json is available")


def assemble_package(
    stage_dir: Path,
    *,
    assets: str,
    dynamic_shaders: str,
    shader_api: str,
    shader_pack: bool,
) -> None:
    if not BIN_DIR.is_dir():
        raise FileNotFoundError(f"{BIN_DIR} does not exist. Build caustica first.")

    executable = BIN_DIR / "caustica.exe"
    if not executable.is_file():
        raise FileNotFoundError(f"Executable not found: {executable}")

    if stage_dir.exists():
        shutil.rmtree(stage_dir)
    stage_dir.mkdir(parents=True, exist_ok=True)

    _copy_file(executable, stage_dir / executable.name)

    # CMake deploys the native runtime beside the executable. Include every
    # shared library and Streamline JSON manifest, while leaving tests/PDBs and
    # build tools out of the portable package.
    for path in BIN_DIR.iterdir():
        if not path.is_file():
            continue
        suffix = path.suffix.lower()
        if suffix in RUNTIME_DLL_SUFFIXES or (path.name.lower().startswith("sl.") and suffix == ".json"):
            _copy_file(path, stage_dir / path.name)

    for directory_name in RUNTIME_DIR_NAMES:
        _copy_tree(BIN_DIR / directory_name, stage_dir / directory_name)

    # Copy the checked-in examples directly instead of relying on the generated
    # bin/PythonExamples directory, which may contain stale scripts from older
    # builds.
    _copy_tree(ROOT / "examples" / "python", stage_dir / "PythonExamples")

    for shader_type in shader_types_for_api(shader_api):
        if shader_pack:
            pack_src = BIN_DIR / f"caustica.shaders.{shader_type}.pack"
            if not pack_src.is_file():
                pack_src = write_shader_pack(shader_type, "bin", BIN_DIR)
            _copy_file(pack_src, stage_dir / pack_src.name)

        if dynamic_shaders != "none":
            shader_bin = BIN_DIR / "ShaderBin" / shader_type
            _copy_tree(shader_bin, stage_dir / "ShaderBin" / shader_type)

        if dynamic_shaders == "full":
            _copy_tree(BIN_DIR / "ShaderDev", stage_dir / "ShaderDev")

    _copy_assets(stage_dir, assets)
    _copy_file(ROOT / "VERSION", stage_dir / "VERSION")
    _copy_file(ROOT / "README.md", stage_dir / "README.md")


def write_zip(stage_dir: Path, zip_path: Path) -> None:
    if zip_path.exists():
        zip_path.unlink()
    zip_path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for path in sorted(stage_dir.rglob("*")):
            if path.is_file():
                archive.write(path, Path(stage_dir.name) / path.relative_to(stage_dir))


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Assemble a portable Caustica executable package from bin/."
    )
    parser.add_argument("--output-dir", type=Path, default=DIST_DIR)
    parser.add_argument("--assets", choices=["minimal", "full", "none"], default="minimal")
    parser.add_argument("--dynamic-shaders", choices=["bin", "full", "none"], default="bin")
    parser.add_argument("--shader-api", choices=["d3d12", "vulkan", "both"], default="d3d12")
    parser.add_argument("--shader-pack", dest="shader_pack", action="store_true", default=True)
    parser.add_argument("--no-shader-pack", dest="shader_pack", action="store_false")
    parser.add_argument("--no-zip", action="store_true", help="Keep only the unpacked directory")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    output_dir = args.output_dir.resolve()
    package_name = f"caustica-{PROJECT_VERSION}-windows-x64"
    stage_dir = output_dir / package_name
    zip_path = output_dir / f"{package_name}.zip"

    assemble_package(
        stage_dir,
        assets=args.assets,
        dynamic_shaders=args.dynamic_shaders,
        shader_api=args.shader_api,
        shader_pack=args.shader_pack,
    )
    print(f"[caustica] staged {stage_dir} ({directory_size(stage_dir)} bytes)")

    if not args.no_zip:
        write_zip(stage_dir, zip_path)
        print(f"[caustica] wrote {zip_path} ({zip_path.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
