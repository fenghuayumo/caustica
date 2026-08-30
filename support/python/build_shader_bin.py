from __future__ import annotations

"""Assemble ShaderMake staging blobs into bin/ShaderBin/{dxil|spirv}/manifest.bin.

CMake target ShaderBinManifest invokes this after the ShaderMake PRE_BUILD
targets finish. Runtime lookup is ShaderCompilerService::resolveLogicalShaderId
+ SHA-256("caustica-shader-id-v1|{logicalId}") -> object hash path.
"""

import argparse
import hashlib
import re
import struct
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MANIFEST_MAGIC = b"CAUSSMF1"
MANIFEST_VERSION_HEADER_SIZE = 12
ENTRY_SIZE = 64

STAGING_MODULES = (
    "caustica",
    "engine",
    "nrd",
    "omm",
)

CFG_TOKEN = re.compile(r'"([^"]+)"|(\S+)')


def shader_id_bytes(logical_name: str) -> bytes:
    normalized = logical_name.replace("\\", "/").strip("/").lower()
    return hashlib.sha256(f"caustica-shader-id-v1|{normalized}".encode("utf-8")).digest()


def resolve_logical_id(file_name: str, entry_name: str = "main") -> str:
    adjusted = file_name.replace("\\", "/")
    hlsl = adjusted.find(".hlsl")
    if hlsl != -1:
        adjusted = adjusted[:hlsl] + adjusted[hlsl + 5 :]
    if entry_name and entry_name != "main":
        adjusted = f"{adjusted}_{entry_name}"
    if adjusted.startswith("caustica/shaders"):
        adjusted = "caustica/" + adjusted
    return adjusted.lower()


def parse_cfg_line(line: str) -> tuple[str, str] | None:
    stripped = line.split("#", 1)[0].strip()
    if not stripped:
        return None
    tokens = [group1 or group2 for group1, group2 in CFG_TOKEN.findall(stripped)]
    if not tokens:
        return None
    source = tokens[0].replace("\\", "/")
    entry = "main"
    for index, token in enumerate(tokens[1:], start=1):
        if token in {"-E", "--entryPoint"} and index < len(tokens) - 1:
            entry = tokens[index + 1]
            break
        if token.startswith("-E") and token != "-E":
            entry = token[2:]
            break
    return source, entry


def iter_cfg_entries(cfg_path: Path) -> list[tuple[str, str]]:
    if not cfg_path.is_file():
        return []
    entries: list[tuple[str, str]] = []
    for line in cfg_path.read_text(encoding="utf-8", errors="replace").splitlines():
        parsed = parse_cfg_line(line)
        if parsed:
            entries.append(parsed)
    return entries


def object_path(bin_root: Path, digest_hex: str) -> Path:
    return bin_root / digest_hex[:2] / f"{digest_hex[2:]}.bin"


def write_blob(bin_root: Path, payload: bytes) -> str:
    digest_hex = hashlib.sha256(payload).hexdigest()
    dest = object_path(bin_root, digest_hex)
    dest.parent.mkdir(parents=True, exist_ok=True)
    if not dest.exists() or dest.stat().st_size != len(payload):
        dest.write_bytes(payload)
    return digest_hex


def write_manifest(dest: Path, mapping: dict[str, str]) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    items = sorted(mapping.items())
    blob = bytearray(MANIFEST_MAGIC)
    blob.extend(struct.pack("<I", len(items)))
    for logical, object_hex in items:
        key = shader_id_bytes(logical)
        obj = bytes.fromhex(object_hex)
        if len(key) != 32 or len(obj) != 32:
            raise RuntimeError(f"Invalid manifest hashes for {logical}")
        blob.extend(key)
        blob.extend(obj)
    dest.write_bytes(blob)
    if dest.stat().st_size != MANIFEST_VERSION_HEADER_SIZE + ENTRY_SIZE * len(items):
        raise RuntimeError(f"Wrote unexpected manifest size: {dest}")


def candidate_bin_names(source: str, entry: str) -> list[str]:
    stem = source
    if stem.lower().endswith(".hlsl"):
        stem = stem[: -len(".hlsl")]
    stem = stem.replace("\\", "/").lstrip("/")
    if entry and entry != "main":
        # ShaderMake emits a generic <stem>.bin for the `main` entry and an
        # entry-suffixed blob for other entry points. Prefer the suffixed blob:
        # selecting the generic file here can silently bind another entry's
        # permutation set (for example ShaderDebug's BLEND_DEBUG_BUFFER blob).
        return [
            f"{stem}_{entry}.bin",
            f"{Path(stem).parent.as_posix()}/{Path(stem).name}_{entry}.bin",
            f"{stem}.bin",
        ]
    return [f"{stem}.bin"]


def module_logical_name(module: str, staging_rel: str, entry: str) -> str:
    rel = staging_rel.replace("\\", "/").lstrip("/")
    if rel.lower().endswith(".bin"):
        rel = rel[: -len(".bin")]
    if module == "caustica":
        return resolve_logical_id(rel if rel.endswith(".hlsl") else f"{rel}.hlsl", entry)
    if module == "engine":
        return resolve_logical_id(f"engine/{rel}", entry)
    if module == "nrd":
        return resolve_logical_id(f"nrd/{rel}", entry)
    if module == "omm":
        return resolve_logical_id(f"omm/{rel}", entry)
    return resolve_logical_id(rel, entry)


def collect_module_bins(staging_dir: Path, module: str, api: str) -> list[Path]:
    root = staging_dir / module / api
    if not root.is_dir():
        return []
    return sorted(path for path in root.rglob("*.bin") if path.is_file())


def assemble_api(
    *,
    api: str,
    staging_dir: Path,
    bin_dir: Path,
    cfg_entries: dict[str, list[tuple[str, str]]],
) -> int:
    api_root = bin_dir / "ShaderBin" / api
    mapping: dict[str, str] = {}
    used_files: set[Path] = set()

    for module, entries in cfg_entries.items():
        module_root = staging_dir / module / api
        for source, entry in entries:
            payload: bytes | None = None
            chosen: Path | None = None
            for name in candidate_bin_names(source, entry):
                candidate = module_root / name
                if candidate.is_file():
                    chosen = candidate
                    payload = candidate.read_bytes()
                    break
            if payload is None:
                continue
            assert chosen is not None
            used_files.add(chosen.resolve())
            digest = write_blob(api_root, payload)
            if module == "caustica":
                logical = resolve_logical_id(source, entry)
            elif module == "engine":
                logical = resolve_logical_id(f"engine/{source}", entry)
            elif module == "nrd":
                logical = resolve_logical_id(f"nrd/{source}", entry)
            else:
                logical = resolve_logical_id(f"omm/{source}", entry)
            mapping[logical] = digest

    for module in STAGING_MODULES:
        for blob_path in collect_module_bins(staging_dir, module, api):
            payload = blob_path.read_bytes()
            digest = write_blob(api_root, payload)
            rel = blob_path.relative_to(staging_dir / module / api).as_posix()
            stem = rel[: -len(".bin")] if rel.lower().endswith(".bin") else rel
            entry = "main"
            for suffix in (
                "_main_vs",
                "_main_ps",
                "_vs_main",
                "_ps_main",
                "_cs_sort_keys",
                "_capture_cs",
            ):
                if stem.endswith(suffix):
                    entry = suffix[1:]
                    break
            logical = module_logical_name(module, rel if entry == "main" else f"{stem}.bin", entry)
            if entry != "main" and stem.endswith(f"_{entry}"):
                trimmed = stem[: -len(entry) - 1] + ".bin"
                logical = module_logical_name(module, trimmed, entry)
            mapping.setdefault(logical, digest)
            used_files.add(blob_path.resolve())

    if not mapping:
        print(f"[caustica] WARNING: no ShaderMake blobs found for {api} under {staging_dir}")
        return 0

    write_manifest(api_root / "manifest.bin", mapping)
    print(f"[caustica] ShaderBin/{api}: {len(mapping)} manifest entries, {len(used_files)} blobs")
    return len(mapping)


def default_cfg_entries() -> dict[str, list[tuple[str, str]]]:
    return {
        "caustica": iter_cfg_entries(ROOT / "caustica" / "shaders.cfg")
        + iter_cfg_entries(ROOT / "caustica" / "shaders_vk.cfg"),
        "engine": iter_cfg_entries(ROOT / "caustica" / "caustica" / "shaders" / "EngineShaders.cfg"),
        "nrd": [],
        "omm": iter_cfg_entries(ROOT / "caustica" / "caustica" / "omm.cfg"),
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Build unified ShaderBin manifest from ShaderMake staging blobs.")
    parser.add_argument("--shader-api", choices=["dxil", "spirv", "both"], default="both")
    parser.add_argument("--staging-dir", type=Path, required=True)
    parser.add_argument("--bin-dir", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    apis = ["dxil", "spirv"] if args.shader_api == "both" else [args.shader_api]
    cfg_entries = default_cfg_entries()
    nrd_cfg = ROOT / "caustica" / "caustica" / "shaders" / "render" / "nrd" / "nrd_shaders.cfg"
    cfg_entries["nrd"] = iter_cfg_entries(nrd_cfg)

    total = 0
    for api in apis:
        total += assemble_api(
            api=api,
            staging_dir=args.staging_dir,
            bin_dir=args.bin_dir,
            cfg_entries=cfg_entries,
        )
    if total == 0:
        print("[caustica] ERROR: ShaderBinManifest produced no entries", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
