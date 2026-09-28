from __future__ import annotations

"""Assemble a portable Caustica executable package from bin/."""

import argparse
import os
import shutil
import subprocess
import zipfile
from pathlib import Path

from build_wheel import (
    BIN_DIR,
    DIST_DIR,
    PROJECT_VERSION,
    RUNTIME_DIR_NAMES,
    _copy_file,
    _copy_tree,
    _runtime_libs_to_copy,
    directory_size,
    ensure_shader_pack,
    shader_types_for_api,
)


ROOT = Path(__file__).resolve().parents[2]
SYSTEM_LIB_PREFIXES = ("/lib/", "/lib64/", "/usr/lib/", "/usr/lib64/")


def executable_name() -> str:
    return "caustica.exe" if os.name == "nt" else "caustica"


def package_tag() -> str:
    if os.name == "nt":
        return "windows-x64"
    machine = os.uname().machine.lower()
    arch = "x64" if machine in {"x86_64", "amd64"} else machine
    return f"linux-{arch}"


def _copy_assets(stage_dir: Path, assets: str) -> None:
    if assets == "none":
        return

    builtin = ROOT / "assets-builtin"
    full_assets = ROOT / "Assets"
    if assets == "full":
        if not (full_assets / "pack.json").is_file():
            raise FileNotFoundError(f"Full asset pack not found: {full_assets / 'pack.json'}")
        _copy_tree(full_assets, stage_dir / "Assets", ignore_names=(".git",))
        return

    if builtin.is_dir():
        _copy_tree(builtin, stage_dir / "Assets", ignore_names=(".git",))
    elif (full_assets / "pack.json").is_file():
        _copy_tree(full_assets, stage_dir / "Assets", ignore_names=(".git",))
    else:
        raise FileNotFoundError("Neither assets-builtin nor Assets/pack.json is available")


def _copy_missing_needed_libs(source_binary: Path, stage_dir: Path) -> None:
    """Copy non-system NEEDED libs (e.g. conda libpython) next to the executable."""
    if os.name == "nt" or not source_binary.is_file():
        return
    try:
        output = subprocess.check_output(["ldd", str(source_binary)], text=True, stderr=subprocess.DEVNULL)
    except (OSError, subprocess.CalledProcessError):
        return

    for line in output.splitlines():
        if "=>" not in line:
            continue
        _, _, rest = line.strip().partition("=>")
        path_text = rest.strip().split(" ")[0]
        if path_text in {"", "not"}:
            continue
        src = Path(path_text)
        if not src.is_file():
            continue
        resolved = str(src.resolve())
        if any(resolved.startswith(prefix) for prefix in SYSTEM_LIB_PREFIXES):
            continue
        dest = stage_dir / src.name
        if dest.exists():
            continue
        _copy_file(src, dest)


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

    executable = BIN_DIR / executable_name()
    if not executable.is_file():
        raise FileNotFoundError(f"Executable not found: {executable}")

    if stage_dir.exists():
        shutil.rmtree(stage_dir)
    stage_dir.mkdir(parents=True, exist_ok=True)

    _copy_file(executable, stage_dir / executable.name)

    # CMake deploys the native runtime beside the executable. Reuse the wheel
    # filter so versioned Linux SONAMEs (DLSS/OIDN) are included and unused
    # HIP/SYCL stacks are left out.
    for path in _runtime_libs_to_copy():
        _copy_file(path, stage_dir / path.name)
    _copy_missing_needed_libs(executable, stage_dir)

    for directory_name in RUNTIME_DIR_NAMES:
        src = BIN_DIR / directory_name
        if src.is_dir():
            _copy_tree(src, stage_dir / directory_name)

    # Copy the checked-in examples directly instead of relying on the generated
    # bin/PythonExamples directory, which may contain stale scripts from older
    # builds.
    _copy_tree(ROOT / "examples" / "python", stage_dir / "PythonExamples")

    for shader_type in shader_types_for_api(shader_api):
        if shader_pack:
            pack_src = ensure_shader_pack(shader_type, BIN_DIR)
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
    parser.add_argument("--dynamic-shaders", choices=["bin", "full", "none"], default="none")
    parser.add_argument(
        "--shader-api",
        choices=["d3d12", "vulkan", "both"],
        default="d3d12" if os.name == "nt" else "vulkan",
    )
    parser.add_argument("--shader-pack", dest="shader_pack", action="store_true", default=True)
    parser.add_argument("--no-shader-pack", dest="shader_pack", action="store_false")
    parser.add_argument("--no-zip", action="store_true", help="Keep only the unpacked directory")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    output_dir = args.output_dir.resolve()
    package_name = f"caustica-{PROJECT_VERSION}-{package_tag()}"
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
