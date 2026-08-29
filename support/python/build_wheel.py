from __future__ import annotations

"""Helpers for assembling a local caustica binary wheel from bin/.

Also provides write_shader_pack() for cook_shaders.py / package_shaders.py.
"""

import argparse
import os
import shutil
import struct
import subprocess
import sys
from pathlib import Path
from typing import Iterable


ROOT = Path(__file__).resolve().parents[2]
BIN_DIR = ROOT / "bin"
PYTHON_PACKAGE_DIR = ROOT / "python" / "caustica"
DIST_DIR = ROOT / "dist"
PROJECT_VERSION = (ROOT / "VERSION").read_text(encoding="utf-8").strip()

SHADER_PACK_MAGIC = b"CAUSSHD1"
SHADER_PACK_VERSION = 1
FNV_OFFSET = 14695981039346656037
FNV_PRIME = 1099511628211
PACK_SEED0 = 0x243F6A8885A308D3
PACK_SEED1 = 0x13198A2E03707344
PACK_XOR_CONST = 0xA5A5A5A55A5A5A5A
XORSHIFT_MULT = 2685821657736338717

RUNTIME_FILE_SUFFIXES = {".dll", ".pyd", ".so", ".dylib"}
RUNTIME_DIR_NAMES = {"D3D12", "usd"}
SKIP_BIN_NAMES = {
    "caustica.exe",
    "causticaD.exe",
    "caustica_thin_client.exe",
    "caustica_thin_clientD.exe",
}


def directory_size(path: Path) -> int:
    if not path.exists():
        return 0
    return sum(item.stat().st_size for item in path.rglob("*") if item.is_file())


def shader_types_for_api(shader_api: str) -> list[str]:
    if shader_api == "d3d12":
        return ["dxil"]
    if shader_api == "vulkan":
        return ["spirv"]
    if shader_api == "both":
        return ["dxil", "spirv"]
    raise ValueError(f"Unsupported shader API: {shader_api}")


def fnv1a64(value: str, seed: int) -> int:
    digest = (FNV_OFFSET ^ seed) & 0xFFFFFFFFFFFFFFFF
    for byte in value.encode("utf-8"):
        digest ^= byte
        digest = (digest * FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    return digest


def rotl64(value: int, shift: int) -> int:
    value &= 0xFFFFFFFFFFFFFFFF
    return ((value << shift) | (value >> (64 - shift))) & 0xFFFFFFFFFFFFFFFF


def xorshift64star(state: int) -> int:
    state ^= (state >> 12) & 0xFFFFFFFFFFFFFFFF
    state ^= (state << 25) & 0xFFFFFFFFFFFFFFFF
    state ^= (state >> 27) & 0xFFFFFFFFFFFFFFFF
    state = (state * XORSHIFT_MULT) & 0xFFFFFFFFFFFFFFFF
    return state


def normalize_pack_path(logical_path: str) -> str:
    normalized = logical_path.replace("\\", "/")
    while normalized.startswith("/"):
        normalized = normalized[1:]
    return normalized.lower()


def pack_key(logical_path: str) -> tuple[int, int]:
    normalized = normalize_pack_path(logical_path)
    return fnv1a64(normalized, PACK_SEED0), fnv1a64(normalized, PACK_SEED1)


def encode_payload(data: bytes, key: tuple[int, int]) -> bytes:
    h0, h1 = key
    state = (h0 ^ rotl64(h1, 1) ^ PACK_XOR_CONST) & 0xFFFFFFFFFFFFFFFF
    encoded = bytearray(data)
    stream_word = 0
    stream_bytes_left = 0
    for index in range(len(encoded)):
        if stream_bytes_left == 0:
            state = xorshift64star(state)
            stream_word = state
            stream_bytes_left = 8
        encoded[index] ^= stream_word & 0xFF
        stream_word >>= 8
        stream_bytes_left -= 1
    return bytes(encoded)


def write_shader_pack(shader_type: str, dynamic_shaders: str, output_dir: Path) -> Path:
    source_root = BIN_DIR / "ShaderBin" / shader_type
    if not source_root.is_dir():
        raise FileNotFoundError(f"{source_root} does not exist. Build ShaderBinManifest first.")

    files: list[tuple[str, Path]] = []
    for path in source_root.rglob("*"):
        if not path.is_file():
            continue
        rel = path.relative_to(source_root).as_posix()
        files.append((f"ShaderBin/{rel}", path))
    if dynamic_shaders == "none":
        files = [item for item in files if Path(item[0]).name in {"manifest.bin", "deps.manifest"}]
    if not files:
        raise FileNotFoundError(f"No shader files to pack under {source_root}")

    files.sort(key=lambda item: item[0].lower())
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    pack_path = output_dir / f"caustica.shaders.{shader_type}.pack"

    encoded_entries: list[tuple[int, int, bytes]] = []
    for logical, path in files:
        key = pack_key(logical)
        encoded_entries.append((key[0], key[1], encode_payload(path.read_bytes(), key)))

    header_size = 16
    table_size = 32 * len(encoded_entries)
    cursor = header_size + table_size
    table = bytearray()
    payload = bytearray()
    for hash0, hash1, blob in encoded_entries:
        table.extend(struct.pack("<QQQQ", hash0, hash1, cursor, len(blob)))
        payload.extend(blob)
        cursor += len(blob)

    pack_path.write_bytes(
        SHADER_PACK_MAGIC
        + struct.pack("<II", SHADER_PACK_VERSION, len(encoded_entries))
        + table
        + payload
    )
    print(f"[caustica] wrote {pack_path} ({len(encoded_entries)} entries, {pack_path.stat().st_size} bytes)")
    return pack_path


def _copy_file(src: Path, dest: Path) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dest)


def _copy_tree(src: Path, dest: Path, *, ignore_names: Iterable[str] = ()) -> None:
    ignore = set(ignore_names)
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True, exist_ok=True)
    for path in src.rglob("*"):
        if any(part in ignore for part in path.relative_to(src).parts):
            continue
        if path.is_dir():
            continue
        _copy_file(path, dest / path.relative_to(src))


def _is_runtime_shared_lib(path: Path) -> bool:
    if path.name in SKIP_BIN_NAMES:
        return False
    suffix = path.suffix.lower()
    if suffix in RUNTIME_FILE_SUFFIXES:
        return True
    if suffix.startswith(".so"):
        return True
    name = path.name.lower()
    return name.startswith("sl.") and suffix == ".json"


def copy_runtime_files(
    package_dir: Path,
    *,
    dynamic_shaders: str = "bin",
    shader_api: str = "d3d12",
    assets: str = "minimal",
    shader_pack: bool = True,
) -> None:
    if not BIN_DIR.exists():
        raise FileNotFoundError(f"{BIN_DIR} does not exist. Build caustica first.")
    package_dir.mkdir(parents=True, exist_ok=True)

    copied_extension = False
    for path in BIN_DIR.iterdir():
        if not path.is_file():
            continue
        if path.name.startswith("caustica") and path.suffix.lower() in {".pyd", ".so"}:
            _copy_file(path, package_dir / path.name)
            copied_extension = True
            continue
        if _is_runtime_shared_lib(path):
            _copy_file(path, package_dir / path.name)

    if not copied_extension:
        raise FileNotFoundError(
            f"No caustica Python extension found in {BIN_DIR}. Build target caustica_py first."
        )

    for dir_name in RUNTIME_DIR_NAMES:
        src = BIN_DIR / dir_name
        if src.is_dir():
            _copy_tree(src, package_dir / dir_name)

    types = shader_types_for_api(shader_api)
    if shader_pack:
        for shader_type in types:
            pack_src = BIN_DIR / f"caustica.shaders.{shader_type}.pack"
            if not pack_src.is_file():
                pack_src = write_shader_pack(shader_type, "bin", BIN_DIR)
            _copy_file(pack_src, package_dir / pack_src.name)

    if dynamic_shaders != "none":
        for shader_type in types:
            src = BIN_DIR / "ShaderBin" / shader_type
            if src.is_dir():
                _copy_tree(src, package_dir / "ShaderBin" / shader_type)
        if dynamic_shaders == "full":
            for extra in ("ShaderDev", "ShaderBin"):
                src = BIN_DIR / extra
                if extra == "ShaderBin":
                    continue
                if src.is_dir():
                    _copy_tree(src, package_dir / extra)

    if assets == "none":
        return

    builtin = ROOT / "assets-builtin"
    full_assets = ROOT / "Assets"
    if assets == "full" and (full_assets / "pack.json").is_file():
        _copy_tree(full_assets, package_dir / "Assets")
    elif builtin.is_dir():
        _copy_tree(builtin, package_dir / "Assets")
        _copy_tree(builtin, package_dir / "assets-builtin")
    elif (full_assets / "pack.json").is_file():
        _copy_tree(full_assets, package_dir / "Assets")


def run_pt_shader_precompile(args) -> None:
    from precompile_pt_shader_bins import run_pt_shader_precompile as cook

    force = bool(getattr(args, "precompile_pt_force", False) or getattr(args, "force", False))
    preset = (
        getattr(args, "precompile_pt_global_preset", None)
        or getattr(args, "global_preset", None)
        or "coverage"
    )
    cook(args.shader_api, force=force, global_preset=preset)


def run_dynamic_shader_precompile(args) -> None:
    from precompile_dynamic_shaders import precompile, split_csv, DEFAULT_MODES, DEFAULT_SCENES

    modes = getattr(args, "precompile_modes", None)
    if isinstance(modes, str):
        mode_list = split_csv(modes, DEFAULT_MODES)
    elif modes:
        mode_list = list(modes)
    else:
        mode_list = list(DEFAULT_MODES)

    scenes = getattr(args, "precompile_scene", None) or getattr(args, "scenes", None)
    if isinstance(scenes, str):
        scene_list = split_csv(scenes, DEFAULT_SCENES)
    elif scenes:
        scene_list = list(scenes)
    else:
        scene_list = list(DEFAULT_SCENES)

    frames = int(getattr(args, "precompile_frames", 1) or 1)
    precompile(args.shader_api, scene_list, mode_list, frames)


def _apply_env_from_args(args: argparse.Namespace) -> None:
    os.environ["CAUSTICA_WHEEL_ASSETS"] = args.assets
    os.environ["CAUSTICA_WHEEL_DYNAMIC_SHADERS"] = args.dynamic_shaders
    os.environ["CAUSTICA_WHEEL_SHADER_API"] = args.shader_api
    os.environ["CAUSTICA_WHEEL_SHADER_PACK"] = "true" if args.shader_pack else "false"
    os.environ["CAUSTICA_WHEEL_PRECOMPILE_PT_SHADERS"] = "true" if args.precompile_pt_shaders else "false"
    os.environ["CAUSTICA_WHEEL_PRECOMPILE_PT_FORCE"] = "true" if args.precompile_pt_force else "false"
    os.environ["CAUSTICA_WHEEL_PRECOMPILE_PT_GLOBAL_PRESET"] = args.precompile_pt_global_preset
    os.environ["CAUSTICA_WHEEL_VERIFY_PT_SHADERS"] = "true" if args.verify_pt_shaders else "false"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Build a local caustica binary wheel from bin/.")
    parser.add_argument("--shader-api", choices=["d3d12", "vulkan", "both"], default="d3d12" if os.name == "nt" else "vulkan")
    parser.add_argument("--assets", choices=["minimal", "full", "none"], default="minimal")
    parser.add_argument("--dynamic-shaders", choices=["bin", "full", "none"], default="bin")
    parser.add_argument("--shader-pack", dest="shader_pack", action="store_true", default=True)
    parser.add_argument("--no-shader-pack", dest="shader_pack", action="store_false")
    parser.add_argument("--precompile-pt-shaders", dest="precompile_pt_shaders", action="store_true", default=True)
    parser.add_argument("--no-precompile-pt-shaders", dest="precompile_pt_shaders", action="store_false")
    parser.add_argument("--precompile-pt-force", action="store_true")
    parser.add_argument("--precompile-pt-global-preset", default="coverage")
    parser.add_argument("--verify-pt-shaders", dest="verify_pt_shaders", action="store_true", default=True)
    parser.add_argument("--no-verify-pt-shaders", dest="verify_pt_shaders", action="store_false")
    parser.add_argument("--output-dir", type=Path, default=DIST_DIR)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if not BIN_DIR.exists():
        raise FileNotFoundError(f"{BIN_DIR} does not exist. Build caustica and caustica_py first.")
    _apply_env_from_args(args)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    cmd = [
        sys.executable,
        "-m",
        "pip",
        "wheel",
        str(ROOT),
        "-w",
        str(args.output_dir),
        "--no-deps",
    ]
    print("[caustica] " + " ".join(cmd))
    return subprocess.call(cmd, cwd=str(ROOT))


if __name__ == "__main__":
    raise SystemExit(main())
