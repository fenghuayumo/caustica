from __future__ import annotations

"""Helpers for assembling a local caustica binary wheel from bin/.

Also provides write_shader_pack() for cook_shaders.py / package_shaders.py.
"""

import argparse
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import zlib
from pathlib import Path
from typing import Iterable


ROOT = Path(__file__).resolve().parents[2]
BIN_DIR = ROOT / "bin"
PYTHON_PACKAGE_DIR = ROOT / "python" / "caustica"
DIST_DIR = ROOT / "dist"
PROJECT_VERSION = (ROOT / "VERSION").read_text(encoding="utf-8").strip()

SHADER_PACK_MAGIC = b"CAUSSHD1"
SHADER_PACK_VERSION = 3
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
    "caustica",
    "causticaD",
    "caustica_thin_client",
    "caustica_thin_clientD",
    "ShaderMake",
}

# OIDN's Linux tarball ships HIP/SYCL/Level Zero stacks that this Vulkan
# runtime never loads. Keep CPU + CUDA devices, core, and TBB.
UNUSED_RUNTIME_LIB_MARKERS = (
    "device_hip",
    "device_sycl",
    "libsycl",
    "libtbbbind",
    "libumf.so",
    "libur_adapter",
    "libur_loader",
)


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
    # Must match ShaderPackFileSystem.cpp::XorShift64Star: the multiplied value
    # is stored back into state (unlike classic xorshift64*).
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


def prune_stale_shader_objects(shader_type: str) -> tuple[int, int]:
    """Remove old ShaderMake objects no longer referenced by manifest.bin.

    ShaderMake objects are named by the SHA-256 of their contents. PT and compute
    cache objects are named by command hashes, so they are deliberately left alone.
    """
    root = BIN_DIR / "ShaderBin" / shader_type
    manifest_path = root / "manifest.bin"
    manifest = manifest_path.read_bytes()
    if len(manifest) < 12 or manifest[:8] != b"CAUSSMF1":
        raise ValueError(f"Invalid shader manifest: {manifest_path}")
    count = struct.unpack_from("<I", manifest, 8)[0]
    if len(manifest) != 12 + count * 64:
        raise ValueError(f"Truncated shader manifest: {manifest_path}")
    active = {
        manifest[12 + index * 64 + 32 : 12 + (index + 1) * 64].hex()
        for index in range(count)
    }
    missing = [digest for digest in active if not (root / digest[:2] / f"{digest[2:]}.bin").is_file()]
    if missing:
        raise FileNotFoundError(f"{len(missing)} current shader objects are missing under {root}")

    removed = removed_bytes = 0
    for path in root.glob("*/*.bin"):
        digest = path.parent.name + path.stem
        if len(digest) != 64 or digest in active:
            continue
        if hashlib.sha256(path.read_bytes()).hexdigest() == digest:
            removed_bytes += path.stat().st_size
            path.unlink()
            removed += 1
    if removed:
        print(f"[caustica] pruned {removed} unreferenced ShaderMake objects ({removed_bytes} bytes) from {root}")
    return removed, removed_bytes


def prune_stale_pt_shaders(shader_type: str) -> tuple[int, int]:
    """Drop PT cache entries from prior source revisions and their old blobs."""
    manifest_path = BIN_DIR / "ShaderBin" / shader_type / "deps.manifest"
    if not manifest_path.is_file():
        return 0, 0
    lines = manifest_path.read_text(encoding="utf-8").splitlines()
    if not lines or lines[0] != "CAUSDEP1":
        raise ValueError(f"Invalid PT dependency manifest: {manifest_path}")

    from precompile_pt_shader_bins import build_hash_command, build_jobs, hash_hex

    api = "d3d12" if shader_type == "dxil" else "vulkan"
    debug_modes = (False, True) if api == "d3d12" else (False,)
    jobs = build_jobs("coverage")
    current = {
        hash_hex(build_hash_command(job["logical"], job["macros"], api=api, debug_info=debug))
        for job in jobs for debug in debug_modes
    }
    retained = [lines[0]]
    removed = removed_bytes = 0
    for line in lines[1:]:
        parts = line.split(" ", 3)
        if len(parts) == 4 and parts[0] == "B" and parts[1] not in current:
            digest = parts[1]
            if len(digest) != 64 or any(ch not in "0123456789abcdef" for ch in digest):
                raise ValueError(f"Invalid PT shader hash in {manifest_path}: {digest}")
            path = manifest_path.parent / digest[:2] / f"{digest[2:]}.bin"
            if path.is_file():
                removed_bytes += path.stat().st_size
                path.unlink()
                removed += 1
            continue
        retained.append(line)
    if len(retained) != len(lines):
        staging = manifest_path.with_name(manifest_path.name + ".tmp")
        staging.write_text("\n".join(retained) + "\n", encoding="utf-8")
        os.replace(staging, manifest_path)
        print(f"[caustica] pruned {removed} obsolete PT bins ({removed_bytes} bytes) from {manifest_path.parent}")
    return removed, removed_bytes


def write_shader_pack(shader_type: str, dynamic_shaders: str, output_dir: Path) -> Path:
    source_root = BIN_DIR / "ShaderBin" / shader_type
    if not source_root.is_dir():
        raise FileNotFoundError(f"{source_root} does not exist. Build ShaderBinManifest first.")
    prune_stale_shader_objects(shader_type)
    prune_stale_pt_shaders(shader_type)

    files: list[tuple[str, Path]] = []
    for path in source_root.rglob("*"):
        if not path.is_file():
            continue
        if path.suffix.lower() in {".pdb", ".ildb"}:
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

    encoded_entries: list[tuple[int, int, int, int, bytes]] = []
    unique_blobs: dict[bytes, bytes] = {}
    for logical, path in files:
        key = pack_key(logical)
        shader = path.read_bytes()
        digest = hashlib.sha256(shader).digest()
        content_key = struct.unpack_from("<QQ", digest)
        if digest not in unique_blobs:
            compressed = struct.pack("<Q", len(shader)) + zlib.compress(shader, level=6)
            unique_blobs[digest] = encode_payload(compressed, content_key)
        encoded_entries.append((key[0], key[1], content_key[0], content_key[1], digest))

    header_size = 16
    table_size = 48 * len(encoded_entries)
    cursor = header_size + table_size
    table = bytearray()
    payload = bytearray()
    offsets: dict[bytes, int] = {}
    for hash0, hash1, content0, content1, digest in encoded_entries:
        blob = unique_blobs[digest]
        if digest not in offsets:
            offsets[digest] = cursor
            payload.extend(blob)
            cursor += len(blob)
        table.extend(struct.pack("<QQQQQQ", hash0, hash1, content0, content1, offsets[digest], len(blob)))

    staging = pack_path.with_name(pack_path.name + f".tmp{os.getpid()}")
    staging.write_bytes(
        SHADER_PACK_MAGIC
        + struct.pack("<II", SHADER_PACK_VERSION, len(encoded_entries))
        + table
        + payload
    )
    os.replace(staging, pack_path)
    print(f"[caustica] wrote {pack_path} ({len(encoded_entries)} entries, {len(unique_blobs)} unique blobs, {pack_path.stat().st_size} bytes)")
    return pack_path


def ensure_shader_pack(shader_type: str, output_dir: Path) -> Path:
    """Rebuild a distribution pack when any included loose shader has changed."""
    source_root = BIN_DIR / "ShaderBin" / shader_type
    if not source_root.is_dir():
        raise FileNotFoundError(f"{source_root} does not exist. Build ShaderBinManifest first.")
    manifest = source_root / "manifest.bin"
    if not manifest.is_file():
        raise FileNotFoundError(f"{manifest} does not exist. Build ShaderBinManifest first.")

    removed_static, _ = prune_stale_shader_objects(shader_type)
    removed_pt, _ = prune_stale_pt_shaders(shader_type)

    pack_path = output_dir / f"caustica.shaders.{shader_type}.pack"
    pack_time = -1
    if pack_path.is_file() and not (removed_static or removed_pt):
        with pack_path.open("rb") as handle:
            header = handle.read(12)
        if header == SHADER_PACK_MAGIC + struct.pack("<I", SHADER_PACK_VERSION):
            pack_time = pack_path.stat().st_mtime_ns
    for path in source_root.rglob("*"):
        if path.is_file() and path.suffix.lower() not in {".pdb", ".ildb"}:
            if path.stat().st_mtime_ns > pack_time:
                print(f"[caustica] shader pack is older than {path}; rebuilding")
                return write_shader_pack(shader_type, "bin", output_dir)
    return pack_path


def _is_elf(path: Path) -> bool:
    try:
        with path.open("rb") as handle:
            return handle.read(4) == b"\x7fELF"
    except OSError:
        return False


def _set_linux_origin_rpath(path: Path) -> None:
    """Make ELF binaries look next to themselves so a relocated package works."""
    if os.name == "nt" or not _is_elf(path):
        return
    patchelf = shutil.which("patchelf")
    if not patchelf:
        return
    subprocess.run(
        [patchelf, "--set-rpath", "$ORIGIN", str(path)],
        check=False,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


def _copy_file(src: Path, dest: Path) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dest)
    _set_linux_origin_rpath(dest)


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
    name = path.name
    suffix = path.suffix.lower()
    if suffix in RUNTIME_FILE_SUFFIXES:
        return True
    # Versioned SONAMEs: libfoo.so.2, libnvidia-ngx-dlss.so.310.3.0
    if ".so." in name or name.endswith(".so"):
        return True
    return name.startswith("sl.") and suffix == ".json"


def _is_unused_runtime_lib(path: Path) -> bool:
    name = path.name
    return any(marker in name for marker in UNUSED_RUNTIME_LIB_MARKERS)


def _runtime_libs_to_copy() -> list[Path]:
    selected: list[Path] = []
    for path in BIN_DIR.iterdir():
        if not path.is_file():
            continue
        if path.name.startswith("caustica") and path.suffix.lower() in {".pyd", ".so"}:
            continue
        if not _is_runtime_shared_lib(path) or _is_unused_runtime_lib(path):
            continue
        selected.append(path)
    return selected


def copy_runtime_files(
    package_dir: Path,
    *,
    dynamic_shaders: str = "none",
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

    for path in _runtime_libs_to_copy():
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
            pack_src = ensure_shader_pack(shader_type, BIN_DIR)
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
    cook(
        args.shader_api,
        force=force,
        global_preset=preset,
        debug_info=getattr(args, "debug_info", None),
    )


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
    parser.add_argument("--dynamic-shaders", choices=["bin", "full", "none"], default="none")
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
        # Assemble from the live bin/ + .shadercache; isolation would recook
        # shaders in a copy that does not include gitignored runtime files.
        "--no-build-isolation",
    ]
    print("[caustica] " + " ".join(cmd))
    return subprocess.call(cmd, cwd=str(ROOT))


if __name__ == "__main__":
    raise SystemExit(main())
