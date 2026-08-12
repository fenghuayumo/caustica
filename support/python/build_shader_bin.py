from __future__ import annotations

import argparse
import hashlib
import shutil
import struct
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
BIN_DIR = ROOT / "bin"
DEFAULT_STAGING_DIR = ROOT / "build" / "shader-staging"
MANIFEST_MAGIC = b"CAUSSMF1"
MANIFEST_HEADER = struct.Struct("<8sI")
MANIFEST_ENTRY_SIZE = 64


def shader_id(logical_name: str) -> bytes:
    normalized = logical_name.replace("\\", "/").strip("/").lower()
    return hashlib.sha256(f"caustica-shader-id-v1|{normalized}".encode("utf-8")).digest()


def object_path(root: Path, digest: str) -> Path:
    return root / digest[:2] / f"{digest[2:]}.bin"


def collect_static_shader_objects(staging_dir: Path, shader_type: str, output_dir: Path) -> dict[bytes, bytes]:
    entries: dict[bytes, bytes] = {}
    for module in ("engine", "caustica", "nrd", "omm"):
        module_root = staging_dir / module / shader_type
        if not module_root.exists():
            continue

        for source_path in sorted(module_root.rglob("*.bin")):
            relative = source_path.relative_to(module_root).with_suffix("")
            logical_name = (Path(module) / relative).as_posix()
            payload = source_path.read_bytes()
            object_digest = hashlib.sha256(payload).hexdigest()
            destination = object_path(output_dir, object_digest)
            destination.parent.mkdir(parents=True, exist_ok=True)
            if not destination.exists() or destination.read_bytes() != payload:
                destination.write_bytes(payload)
            entries[shader_id(logical_name)] = bytes.fromhex(object_digest)
            # OMM's public pipeline registry exposes short names while its generated
            # ShaderMake paths omit the source-only `internal` directory component.
            if module == "omm" and logical_name.startswith("omm/shaders/render/omm/"):
                suffix = logical_name.removeprefix("omm/shaders/render/omm/")
                entries[shader_id(f"omm/shaders/render/omm/internal/{suffix}")] = bytes.fromhex(object_digest)
    return entries


def import_existing_variant_objects(bin_dir: Path, shader_type: str, output_dir: Path) -> int:
    legacy_root = bin_dir / "ShaderDynamic" / "Bin" / shader_type
    if not legacy_root.exists():
        return 0
    imported = 0
    for source_path in legacy_root.rglob("*.bin"):
        stem = source_path.stem
        prefix = source_path.parent.name
        if len(prefix) == 2 and len(stem) == 62:
            digest = (prefix + stem).lower()
        elif len(stem) == 64:
            digest = stem.lower()
        else:
            candidate = stem.rsplit("_", 1)[-1].lower()
            if len(candidate) != 64:
                continue
            digest = candidate
        if any(ch not in "0123456789abcdef" for ch in digest):
            continue
        destination = object_path(output_dir, digest)
        destination.parent.mkdir(parents=True, exist_ok=True)
        if not destination.exists():
            shutil.copy2(source_path, destination)
            imported += 1
    return imported


def write_manifest(path: Path, entries: dict[bytes, bytes]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("wb") as manifest:
        manifest.write(MANIFEST_HEADER.pack(MANIFEST_MAGIC, len(entries)))
        for stable_id, object_hash in sorted(entries.items()):
            if len(stable_id) != 32 or len(object_hash) != 32:
                raise ValueError("Shader manifest hashes must be SHA-256 values")
            manifest.write(stable_id)
            manifest.write(object_hash)


def build_shader_bin(shader_type: str, staging_dir: Path, bin_dir: Path) -> Path:
    output_dir = bin_dir / "ShaderBin" / shader_type
    output_dir.mkdir(parents=True, exist_ok=True)
    imported = import_existing_variant_objects(bin_dir, shader_type, output_dir)
    entries = collect_static_shader_objects(staging_dir, shader_type, output_dir)
    if not entries:
        raise FileNotFoundError(f"No staged {shader_type} shader binaries found under {staging_dir}")
    manifest_path = output_dir / "manifest.bin"
    write_manifest(manifest_path, entries)
    print(f"Built {manifest_path} ({len(entries)} static shader IDs, {imported} imported variant objects)")
    return manifest_path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Build the unified hash-addressed ShaderBin store.")
    parser.add_argument("--shader-api", choices=["dxil", "spirv", "both"], default="both")
    parser.add_argument("--staging-dir", type=Path, default=DEFAULT_STAGING_DIR)
    parser.add_argument("--bin-dir", type=Path, default=BIN_DIR)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    shader_types = ["dxil", "spirv"] if args.shader_api == "both" else [args.shader_api]
    for shader_type in shader_types:
        module_roots = [args.staging_dir / module / shader_type for module in ("engine", "caustica", "nrd", "omm")]
        if any(path.exists() for path in module_roots):
            build_shader_bin(shader_type, args.staging_dir, args.bin_dir)
        else:
            print(f"Skipping {shader_type}: no staged shader directory exists")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
