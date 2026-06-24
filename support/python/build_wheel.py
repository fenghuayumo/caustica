from __future__ import annotations

import argparse
import os
import shutil
import struct
import subprocess
import sys
import sysconfig
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
BIN_DIR = ROOT / "bin"
PYTHON_PACKAGE_DIR = ROOT / "python" / "caustica"
BUILD_DIR = ROOT / "build" / "python-wheel"
STAGING_DIR = BUILD_DIR / "staging"
DIST_DIR = ROOT / "dist"

BASE_MINIMAL_ASSET_FILES = [
    "ArtLicenses.txt",
    "README.md",
    "default.json",
    "loading_splash.png",
]

# Required by SampleProceduralSky for ==PROCEDURAL_SKY== environment lights.
PROC_SKY_ASSET_FILES = [
    "StandaloneTextures/RGBANoiseMedium.png",
    "StandaloneTextures/q2rtx_env/transmittance_earth.dds",
    "StandaloneTextures/q2rtx_env/inscatter_earth.dds",
    "StandaloneTextures/q2rtx_env/irradiance_earth.dds",
    "StandaloneTextures/q2rtx_env/clouds.dds",
]

MINIMAL_ASSET_FILES = BASE_MINIMAL_ASSET_FILES + PROC_SKY_ASSET_FILES


def copy_file(src: Path, dst: Path) -> None:
    if not src.exists():
        raise FileNotFoundError(src)
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)


def copy_optional_file(src: Path, dst: Path) -> None:
    if not src.exists():
        print(f"WARNING: optional runtime asset not found, skipping: {src}")
        return
    copy_file(src, dst)


def copy_tree(
    src: Path,
    dst: Path,
    suffixes: set[str] | None = None,
    path_filter: set[str] | None = None,
) -> None:
    if not src.exists():
        raise FileNotFoundError(src)

    for item in src.rglob("*"):
        if not item.is_file():
            continue
        if suffixes is not None and item.suffix.lower() not in suffixes:
            continue
        if path_filter is not None and not set(item.relative_to(src).parts) & path_filter:
            continue
        copy_file(item, dst / item.relative_to(src))


def directory_size(path: Path) -> int:
    return sum(item.stat().st_size for item in path.rglob("*") if item.is_file())


PACK_MAGIC = b"CAUSSHD1"
PACK_VERSION = 1
PACK_HEADER = struct.Struct("<8sII")
PACK_ENTRY = struct.Struct("<QQQQ")
FNV_MASK = (1 << 64) - 1
FNV_PRIME = 1099511628211
FNV_OFFSET = 14695981039346656037
HEX_CHARS = set("0123456789abcdefABCDEF")


def fnv1a64(value: str, seed: int) -> int:
    h = (FNV_OFFSET ^ seed) & FNV_MASK
    for byte in value.encode("utf-8"):
        h ^= byte
        h = (h * FNV_PRIME) & FNV_MASK
    return h


def shader_pack_key(logical_path: str) -> tuple[int, int]:
    return (
        fnv1a64(logical_path, 0x243F6A8885A308D3),
        fnv1a64(logical_path, 0x13198A2E03707344),
    )


def rotl64(value: int, shift: int) -> int:
    return ((value << shift) | (value >> (64 - shift))) & FNV_MASK


def xorshift64star(state: int) -> tuple[int, int]:
    state ^= state >> 12
    state ^= (state << 25) & FNV_MASK
    state ^= state >> 27
    state &= FNV_MASK
    return state, (state * 2685821657736338717) & FNV_MASK


def encode_shader_payload(data: bytes, key: tuple[int, int]) -> bytes:
    state = (key[0] ^ rotl64(key[1], 1) ^ 0xA5A5A5A55A5A5A5A) & FNV_MASK
    out = bytearray(data)
    stream_word = 0
    stream_bytes_left = 0
    for index in range(len(out)):
        if stream_bytes_left == 0:
            state, stream_word = xorshift64star(state)
            stream_bytes_left = 8
        out[index] ^= stream_word & 0xFF
        stream_word >>= 8
        stream_bytes_left -= 1
    return bytes(out)


def is_hash_hex(value: str) -> bool:
    return len(value) == 64 and all(ch in HEX_CHARS for ch in value)


def normalized_dynamic_shader_rel(path: Path) -> Path:
    stem = path.stem
    if is_hash_hex(stem):
        shader_hash = stem.lower()
    else:
        maybe_hash = stem.rsplit("_", 1)[-1].lower()
        if not is_hash_hex(maybe_hash):
            return Path(path.name)
        shader_hash = maybe_hash
    return Path(shader_hash[:2]) / f"{shader_hash}.bin"


def add_shader_pack_tree(
    entries: dict[str, Path],
    src_root: Path,
    logical_root: str,
    *,
    normalize_dynamic_names: bool = False,
) -> None:
    if not src_root.exists():
        return

    for item in src_root.rglob("*.bin"):
        if not item.is_file():
            continue
        rel = item.relative_to(src_root)
        if normalize_dynamic_names:
            rel = normalized_dynamic_shader_rel(rel)
        logical_path = f"{logical_root}/{rel.as_posix()}"
        entries.setdefault(logical_path, item)


def collect_shader_pack_entries(shader_type: str, dynamic_shaders: str) -> dict[str, Path]:
    entries: dict[str, Path] = {}
    static_roots = {
        "framework": "ShaderPrecompiled/engine",
        "caustica": "ShaderPrecompiled/app",
        "nrd": "ShaderPrecompiled/nrd",
        "omm": "ShaderPrecompiled/omm",
    }
    for source_name, logical_root in static_roots.items():
        add_shader_pack_tree(
            entries,
            BIN_DIR / "ShaderPrecompiled" / source_name / shader_type,
            logical_root,
        )

    if dynamic_shaders in {"bin", "full"}:
        add_shader_pack_tree(
            entries,
            BIN_DIR / "ShaderDynamic" / "Bin" / shader_type,
            "ShaderDynamic/Bin",
            normalize_dynamic_names=True,
        )

    return entries


def write_shader_pack(shader_type: str, dynamic_shaders: str, output_dir: Path) -> Path:
    entries = collect_shader_pack_entries(shader_type, dynamic_shaders)
    if not entries:
        raise FileNotFoundError(f"No shader binaries found for {shader_type} in {BIN_DIR}")

    output_dir.mkdir(parents=True, exist_ok=True)
    pack_path = output_dir / f"caustica.shaders.{shader_type}.pack"
    sorted_entries = sorted(entries.items())
    table_size = PACK_ENTRY.size * len(sorted_entries)
    data_offset = PACK_HEADER.size + table_size

    packed_entries: list[tuple[int, int, int, int, bytes]] = []
    cursor = data_offset
    for logical_path, source_path in sorted_entries:
        key = shader_pack_key(logical_path)
        encoded = encode_shader_payload(source_path.read_bytes(), key)
        packed_entries.append((key[0], key[1], cursor, len(encoded), encoded))
        cursor += len(encoded)

    with pack_path.open("wb") as f:
        f.write(PACK_HEADER.pack(PACK_MAGIC, PACK_VERSION, len(packed_entries)))
        for hash0, hash1, offset, size, _ in packed_entries:
            f.write(PACK_ENTRY.pack(hash0, hash1, offset, size))
        for *_, encoded in packed_entries:
            f.write(encoded)

    print(f"Built shader pack: {pack_path} ({len(packed_entries)} entries)")
    return pack_path


def find_native_extension() -> Path:
    ext_suffix = sysconfig.get_config_var("EXT_SUFFIX")
    if ext_suffix:
        exact = BIN_DIR / f"caustica{ext_suffix}"
        if exact.exists():
            return exact

    candidates = sorted(
        BIN_DIR.glob("caustica*.pyd" if os.name == "nt" else "caustica*.so")
    )
    if candidates:
        return candidates[-1]

    raise FileNotFoundError(
        f"No caustica Python extension found in {BIN_DIR}. Build target 'caustica_py' first."
    )


def copy_runtime_files(
    package_dir: Path,
    dynamic_shaders: str,
    shader_api: str,
    assets: str,
    shader_pack: bool = True,
) -> None:
    native_extension = find_native_extension()
    copy_file(native_extension, package_dir / native_extension.name)

    if os.name == "nt":
        for item in BIN_DIR.iterdir():
            if item.is_file() and item.suffix.lower() in {".dll", ".json"}:
                copy_file(item, package_dir / item.name)
        if (BIN_DIR / "D3D12").exists():
            copy_tree(BIN_DIR / "D3D12", package_dir / "D3D12")
    else:
        for item in BIN_DIR.iterdir():
            if item.is_file() and (item.suffix == ".so" or ".so." in item.name):
                copy_file(item, package_dir / item.name)

    shader_filter = None
    tool_filter = None
    if shader_api == "d3d12":
        shader_filter = {"dxil"}
        tool_filter = {"d3d12"}
    elif shader_api == "vulkan":
        shader_filter = {"spirv"}
        tool_filter = {"vk"}
    elif shader_api != "both":
        raise ValueError(f"Unknown shader API: {shader_api}")

    shader_types = ["dxil"] if shader_api == "d3d12" else ["spirv"] if shader_api == "vulkan" else ["dxil", "spirv"]

    if shader_pack:
        for shader_type in shader_types:
            write_shader_pack(shader_type, dynamic_shaders, package_dir)
    else:
        copy_tree(
            BIN_DIR / "ShaderPrecompiled",
            package_dir / "ShaderPrecompiled",
            path_filter=shader_filter,
        )

    if not shader_pack and dynamic_shaders in {"bin", "full"} and (BIN_DIR / "ShaderDynamic" / "Bin").exists():
        copy_tree(
            BIN_DIR / "ShaderDynamic" / "Bin",
            package_dir / "ShaderDynamic" / "Bin",
            suffixes={".bin"},
            path_filter=shader_filter,
        )

    if dynamic_shaders == "full":
        if (BIN_DIR / "ShaderDynamic" / "Source").exists():
            copy_tree(
                BIN_DIR / "ShaderDynamic" / "Source",
                package_dir / "ShaderDynamic" / "Source",
            )
        if (BIN_DIR / "ShaderDynamic" / "Tools").exists():
            copy_tree(
                BIN_DIR / "ShaderDynamic" / "Tools",
                package_dir / "ShaderDynamic" / "Tools",
                suffixes={"", ".exe", ".json", ".marker", ".dll", ".so"},
                path_filter=tool_filter,
            )

    if assets == "minimal":
        for relative in MINIMAL_ASSET_FILES:
            copy_file(
                ROOT / "Assets" / relative,
                package_dir / "Assets" / relative,
            )
        copy_tree(ROOT / "Assets" / "Fonts", package_dir / "Assets" / "Fonts")
        return

    if assets == "full":
        copy_tree(ROOT / "Assets", package_dir / "Assets")
        return

    if assets == "none":
        keep = package_dir / "Assets" / ".caustica-wheel-runtime"
        keep.parent.mkdir(parents=True, exist_ok=True)
        keep.write_text("Runtime asset root placeholder for caustica wheels.\n", encoding="utf-8")
        return

    raise ValueError(f"Unknown assets mode: {assets}")


def write_build_project(version: str) -> None:
    (STAGING_DIR / "pyproject.toml").write_text(
        '[build-system]\n'
        'requires = ["setuptools>=68", "wheel"]\n'
        'build-backend = "setuptools.build_meta"\n',
        encoding="utf-8",
    )
    (STAGING_DIR / "MANIFEST.in").write_text(
        "recursive-include caustica *\n",
        encoding="utf-8",
    )
    (STAGING_DIR / "README.md").write_text(
        "# caustica Python Wheel\n\n"
        "Local binary wheel assembled from the current caustica build output.\n",
        encoding="utf-8",
    )
    (STAGING_DIR / "setup.py").write_text(
        'from setuptools import Distribution, setup\n\n\n'
        "class BinaryDistribution(Distribution):\n"
        "    def has_ext_modules(self):\n"
        "        return True\n\n\n"
        "setup(\n"
        '    name="caustica",\n'
        f'    version={version!r},\n'
        '    description="Python bindings for caustica",\n'
        '    long_description=open("README.md", encoding="utf-8").read(),\n'
        '    long_description_content_type="text/markdown",\n'
        '    packages=["caustica"],\n'
        "    include_package_data=True,\n"
        '    license_files=["LICENSE.txt"],\n'
        "    distclass=BinaryDistribution,\n"
        '    python_requires=">=3.8",\n'
        ")\n",
        encoding="utf-8",
    )
    copy_file(ROOT / "LICENSE.txt", STAGING_DIR / "LICENSE.txt")


def build_wheel() -> Path:
    DIST_DIR.mkdir(parents=True, exist_ok=True)
    before = set(DIST_DIR.glob("caustica-*.whl"))
    subprocess.run(
        [
            sys.executable,
            "-m",
            "pip",
            "wheel",
            "--no-deps",
            "--no-build-isolation",
            "--wheel-dir",
            str(DIST_DIR),
            str(STAGING_DIR),
        ],
        check=True,
        cwd=ROOT,
    )
    after = set(DIST_DIR.glob("caustica-*.whl"))
    created = sorted(after - before, key=lambda path: path.stat().st_mtime)
    if created:
        return created[-1]
    existing = sorted(after, key=lambda path: path.stat().st_mtime)
    if existing:
        return existing[-1]
    raise RuntimeError("pip did not produce a caustica wheel")


def run_dynamic_shader_precompile(args: argparse.Namespace) -> None:
    shader_apis = ["d3d12", "vulkan"] if args.shader_api == "both" else [args.shader_api]
    for shader_api in shader_apis:
        command = [
            sys.executable,
            str(ROOT / "Support" / "python" / "precompile_dynamic_shaders.py"),
            "--shader-api",
            shader_api,
            "--modes",
            args.precompile_modes,
            "--frames",
            str(args.precompile_frames),
            "--global-variant-preset",
            args.precompile_global_preset,
        ]
        for scene in args.precompile_scene or []:
            command.extend(["--scene", scene])
        for variant in args.precompile_global_variant or []:
            command.extend(["--global-variant", variant])
        subprocess.run(command, check=True, cwd=ROOT)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Build a local caustica Python wheel from bin/.")
    parser.add_argument("--version", default="0.6.0", help="Wheel package version.")
    parser.add_argument(
        "--assets",
        choices=["minimal", "full", "none"],
        default="minimal",
        help="Asset payload to include. 'full' is very large.",
    )
    parser.add_argument(
        "--dynamic-shaders",
        choices=["full", "bin", "none"],
        default="bin",
        help=(
            "ShaderDynamic payload. 'bin' includes compiled runtime variants only; "
            "'full' also includes Source and Tools for runtime compilation; "
            "'none' omits ShaderDynamic."
        ),
    )
    parser.add_argument(
        "--shader-api",
        choices=["d3d12", "vulkan", "both"],
        default="d3d12" if os.name == "nt" else "vulkan",
        help="Shader backend payload to include. Windows wheels default to D3D12 only.",
    )
    parser.add_argument(
        "--shader-pack",
        dest="shader_pack",
        action="store_true",
        default=True,
        help="Package shader binaries into caustica.shaders.<api>.pack instead of copying shader directories.",
    )
    parser.add_argument(
        "--no-shader-pack",
        dest="shader_pack",
        action="store_false",
        help="Copy shader directories in the legacy layout.",
    )
    parser.add_argument(
        "--no-dynamic-shader-bin",
        action="store_true",
        default=argparse.SUPPRESS,
    )
    parser.add_argument(
        "--precompile-dynamic-shaders",
        action="store_true",
        help=(
            "Before staging the wheel, launch the local caustica extension headlessly to "
            "generate ShaderDynamic/Bin entries for selected scenes."
        ),
    )
    parser.add_argument(
        "--precompile-scene",
        action="append",
        help=(
            "Scene used by --precompile-dynamic-shaders. Repeat for multiple scenes. "
            "Defaults to builtin:plane_cube."
        ),
    )
    parser.add_argument(
        "--precompile-modes",
        default="reference,realtime",
        help="Comma/semicolon separated modes for shader precompile: reference,realtime.",
    )
    parser.add_argument(
        "--precompile-frames",
        type=int,
        default=1,
        help="Frames to render per precompile scene/mode.",
    )
    parser.add_argument(
        "--precompile-global-preset",
        choices=["default", "coverage"],
        default="default",
        help=(
            "Global macro coverage preset passed to precompile_dynamic_shaders.py. "
            "'coverage' warms common wheel compatibility and quality toggles."
        ),
    )
    parser.add_argument(
        "--precompile-global-variant",
        action="append",
        help=(
            "Additional Settings overrides for one shader warmup pass, for example "
            "'use_nee=0,nee_candidate_samples=8'. Repeat for multiple passes."
        ),
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()

    if os.name != "nt" and args.shader_api == "d3d12":
        raise ValueError(
            "D3D12 shader payload is only valid for Windows wheels. "
            "Use --shader-api vulkan on Linux."
        )

    if not BIN_DIR.exists():
        raise FileNotFoundError(f"{BIN_DIR} does not exist. Build caustica first.")
    if not PYTHON_PACKAGE_DIR.exists():
        raise FileNotFoundError(f"{PYTHON_PACKAGE_DIR} does not exist.")

    if STAGING_DIR.exists():
        shutil.rmtree(STAGING_DIR)
    STAGING_DIR.mkdir(parents=True)

    package_dir = STAGING_DIR / "caustica"
    shutil.copytree(PYTHON_PACKAGE_DIR, package_dir)

    dynamic_shaders = "none" if getattr(args, "no_dynamic_shader_bin", False) else args.dynamic_shaders

    if args.precompile_dynamic_shaders:
        if dynamic_shaders == "none":
            print(
                "WARNING: --precompile-dynamic-shaders used while dynamic shader bins are omitted."
            )
        run_dynamic_shader_precompile(args)

    copy_runtime_files(
        package_dir,
        dynamic_shaders,
        args.shader_api,
        args.assets,
        shader_pack=args.shader_pack,
    )
    write_build_project(args.version)

    print(f"Staged package size: {directory_size(package_dir) / (1024 * 1024):.1f} MiB")
    wheel = build_wheel()
    print(f"Built wheel: {wheel}")
    print(f"Wheel size: {wheel.stat().st_size / (1024 * 1024):.1f} MiB")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
