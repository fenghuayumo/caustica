#!/usr/bin/env python3
"""Caustica Scene Pack tool.

Packs a caustica scene document and its full reference closure (prefabs,
materials + textures, environment maps, glTF buffers/images, OBJ mtls) into a
single self-contained binary ``.caustica`` file (see docs/scene-pack-format.md).

Usage:
    python pack_scene.py pack <scene.json> [-o OUT.caustica] [--assets DIR] [--lz4] [--store]
    python pack_scene.py inspect <file.caustica>
    python pack_scene.py extract <file.caustica> [-o DIR]
"""

from __future__ import annotations

import argparse
import json
import os
import struct
import sys
import zlib
from pathlib import Path

try:
    import lz4.block as _lz4block
except ImportError:  # pragma: no cover - optional dependency
    _lz4block = None

MAGIC = b"CAUSTICP"
FORMAT_VERSION = 1
HEADER_SIZE = 64
ALIGN = 16

COMPRESSION_STORE = 0
COMPRESSION_LZ4 = 1

# PackEntryType
TYPE_OTHER = 0
TYPE_SCENE = 1
TYPE_MODEL = 2
TYPE_TEXTURE = 3
TYPE_MATERIAL = 4
TYPE_ENV_MAP = 5
TYPE_PREFAB = 6
TYPE_MANIFEST = 7

_TYPE_NAMES = {
    TYPE_OTHER: "other",
    TYPE_SCENE: "scene",
    TYPE_MODEL: "model",
    TYPE_TEXTURE: "texture",
    TYPE_MATERIAL: "material",
    TYPE_ENV_MAP: "env",
    TYPE_PREFAB: "prefab",
    TYPE_MANIFEST: "manifest",
}

MODEL_EXTS = {".gltf", ".glb", ".obj", ".fbx", ".usd", ".usda", ".usdc", ".usdz", ".urdf", ".stl", ".ply"}
TEXTURE_EXTS = {".dds", ".png", ".jpg", ".jpeg", ".tga", ".hdr", ".exr", ".bmp", ".tif", ".tiff"}


class PackError(RuntimeError):
    pass


def _norm_rel(path: Path) -> str:
    return path.as_posix()


def _is_builtin_source(source: str) -> bool:
    return source.startswith("builtin:") or source.startswith("procedural:")


def _json_load(path: Path):
    try:
        with path.open("r", encoding="utf-8") as f:
            return json.load(f)
    except (OSError, json.JSONDecodeError) as exc:
        raise PackError(f"cannot parse JSON '{path}': {exc}") from exc


def _detect_type(path: Path, override: int | None = None) -> int:
    if override is not None:
        return override
    name = path.name.lower()
    if name.endswith(".scene.json"):
        return TYPE_SCENE
    if name.endswith(".prefab.json"):
        return TYPE_PREFAB
    if name.endswith(".material.json"):
        return TYPE_MATERIAL
    ext = path.suffix.lower()
    if ext in TEXTURE_EXTS:
        return TYPE_TEXTURE
    if ext in MODEL_EXTS:
        return TYPE_MODEL
    return TYPE_OTHER


def _resolve_reference(reference: str, pack_root: Path, scene_dir: Path, extra_dirs: tuple[Path, ...] = ()) -> Path | None:
    """Resolve a media reference the way the engine does: pack-root relative,
    then scene-relative, then as given (absolute or CWD relative)."""
    ref = reference.replace("\\", "/")
    candidates = [
        *(base / ref for base in extra_dirs),
        pack_root / ref,
        scene_dir / ref,
        Path(ref),
    ]
    for candidate in candidates:
        try:
            resolved = candidate.resolve()
            if resolved.is_file():
                return resolved
        except OSError:
            continue
    return None


class PackBuilder:
    def __init__(self, pack_root: Path, scene_dir: Path, use_lz4: bool, verbose: bool = False):
        self.pack_root = pack_root.resolve()
        self.scene_dir = scene_dir.resolve()
        self.use_lz4 = use_lz4 and _lz4block is not None
        self.verbose = verbose
        # entry path (posix str, pack-root relative) -> (abs path, type)
        self.entries: dict[str, tuple[Path, int]] = {}

    def _log(self, message: str) -> None:
        if self.verbose:
            print(message)

    def add_file(self, resolved: Path, entry_type: int | None = None) -> str | None:
        try:
            resolved = resolved.resolve()
            rel = resolved.relative_to(self.pack_root)
        except (ValueError, OSError):
            rel = None
        entry_path = _norm_rel(rel) if rel is not None else _norm_rel(resolved)
        if entry_path in self.entries:
            return entry_path
        if not resolved.is_file():
            return None
        self.entries[entry_path] = (resolved, _detect_type(resolved, entry_type))
        self._log(f"  + {entry_path}")
        return entry_path

    def add_reference(self, reference: str, entry_type: int | None = None, extra_dirs: tuple[Path, ...] = ()) -> str | None:
        if not reference or _is_builtin_source(reference):
            return None
        resolved = _resolve_reference(reference, self.pack_root, self.scene_dir, extra_dirs)
        if resolved is None:
            print(f"warning: unresolved reference '{reference}'", file=sys.stderr)
            return None
        return self.add_file(resolved, entry_type)

    # --- reference scanning -------------------------------------------------

    def scan_document(self, doc: dict, doc_dir: Path) -> None:
        """Scan a scene or prefab document for asset references."""
        if not isinstance(doc, dict):
            return

        base = doc.get("base")
        if isinstance(base, str):
            base_path = self.add_reference(base, TYPE_SCENE)
            if base_path:
                self.scan_document(_json_load(self.entries[base_path][0]), doc_dir)

        entities = doc.get("entities")
        if isinstance(entities, list):
            for entity in entities:
                if isinstance(entity, dict):
                    self._scan_entity(entity)

        self._scan_tree_deep(doc, doc_dir)

    def _scan_entity(self, entity: dict) -> None:
        components = entity.get("components")
        if not isinstance(components, dict):
            return
        for component in components.values():
            if not isinstance(component, dict):
                if isinstance(component, str):
                    # e.g. "MaterialOverride": "materials/foo.material.json"
                    if component.lower().endswith(".json"):
                        self._add_material(component)
                continue
            source = component.get("source")
            if isinstance(source, str):
                name = source.lower()
                if name.endswith(".prefab.json"):
                    prefab_path = self.add_reference(source, TYPE_PREFAB)
                    if prefab_path:
                        self.scan_document(_json_load(self.entries[prefab_path][0]), self.scene_dir)
                elif name.endswith(".material.json"):
                    self._add_material(source)
                elif Path(name).suffix in MODEL_EXTS:
                    self.add_model(source)
                else:
                    # environment maps and anything else file-backed
                    env_path = self.add_reference(source)
                    if env_path and self.entries[env_path][1] == TYPE_TEXTURE:
                        self.entries[env_path] = (self.entries[env_path][0], TYPE_ENV_MAP)
            materials = component.get("materials")
            if isinstance(materials, dict):
                for slot in materials.values():
                    if isinstance(slot, str):
                        self._add_material(slot)
            slots = component.get("slots")
            if isinstance(slots, dict):
                for slot in slots.values():
                    if isinstance(slot, str):
                        self._add_material(slot)

    def _add_material(self, reference: str) -> None:
        mat_path = self.add_reference(reference, TYPE_MATERIAL)
        if not mat_path:
            return
        doc = _json_load(self.entries[mat_path][0])
        self._scan_material(doc)

    def _scan_material(self, doc: dict) -> None:
        if not isinstance(doc, dict):
            return
        for key, value in doc.items():
            if key == "path" and isinstance(value, str):
                self.add_reference(value, TYPE_TEXTURE)
            elif isinstance(value, dict):
                self._scan_material(value)

    def add_model(self, reference: str) -> None:
        model_path = self.add_reference(reference, TYPE_MODEL)
        if not model_path:
            return
        abs_path = self.entries[model_path][0]
        ext = abs_path.suffix.lower()
        model_dir = abs_path.parent
        if ext == ".gltf":
            self._scan_gltf(abs_path, model_dir)
        elif ext == ".obj":
            self._scan_obj(abs_path, model_dir)

    def _scan_gltf(self, gltf_path: Path, model_dir: Path) -> None:
        doc = _json_load(gltf_path)
        for buffer in doc.get("buffers", []):
            uri = buffer.get("uri")
            if isinstance(uri, str) and not uri.startswith("data:"):
                self.add_reference(uri, TYPE_OTHER, extra_dirs=(model_dir,))
        for image in doc.get("images", []):
            uri = image.get("uri")
            if isinstance(uri, str) and not uri.startswith("data:"):
                self.add_reference(uri, TYPE_TEXTURE, extra_dirs=(model_dir,))

    def _scan_obj(self, obj_path: Path, model_dir: Path) -> None:
        try:
            text = obj_path.read_text(encoding="utf-8", errors="replace")
        except OSError as exc:
            print(f"warning: cannot read OBJ '{obj_path}': {exc}", file=sys.stderr)
            return
        mtls: list[str] = []
        for line in text.splitlines():
            stripped = line.strip()
            if stripped.startswith("mtllib "):
                mtls.extend(stripped.split()[1:])
        for mtl in mtls:
            mtl_path = self.add_reference(mtl, TYPE_OTHER, extra_dirs=(model_dir,))
            if not mtl_path:
                continue
            try:
                mtl_text = self.entries[mtl_path][0].read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            for line in mtl_text.splitlines():
                stripped = line.strip()
                if stripped.startswith("map_"):
                    parts = stripped.split(None, 1)
                    if len(parts) == 2:
                        self.add_reference(parts[1].split()[-1], TYPE_TEXTURE, extra_dirs=(model_dir,))

    def _scan_tree_deep(self, node, doc_dir: Path) -> None:
        """Catch-all: resolve any 'source' string anywhere in the document."""
        if isinstance(node, dict):
            for key, value in node.items():
                if key == "source" and isinstance(value, str):
                    name = value.lower()
                    if not _is_builtin_source(value):
                        if name.endswith(".material.json"):
                            self._add_material(value)
                        elif name.endswith(".prefab.json"):
                            prefab_path = self.add_reference(value, TYPE_PREFAB)
                            if prefab_path:
                                self.scan_document(_json_load(self.entries[prefab_path][0]), doc_dir)
                        elif Path(name).suffix in MODEL_EXTS | TEXTURE_EXTS:
                            if name.endswith((".dds", ".hdr", ".exr")):
                                env_path = self.add_reference(value, TYPE_ENV_MAP)
                            else:
                                self.add_reference(value)
                        else:
                            # best effort — only add if it exists
                            self.add_reference(value)
                else:
                    self._scan_tree_deep(value, doc_dir)
        elif isinstance(node, list):
            for item in node:
                self._scan_tree_deep(item, doc_dir)

    # --- serialization ------------------------------------------------------

    def build(self, primary_scene_entry: str) -> bytes:
        # manifest
        manifest = {
            "format": "caustica.scene-pack",
            "version": FORMAT_VERSION,
            "generator": "pack_scene.py 1.0",
            "primaryScene": primary_scene_entry,
            "compression": "lz4" if self.use_lz4 else "store",
            "entries": [
                {
                    "path": path,
                    "type": entry_type,
                    "typeName": _TYPE_NAMES.get(entry_type, "other"),
                    "uncompSize": abs_path.stat().st_size,
                    "crc32": zlib.crc32(abs_path.read_bytes()) & 0xFFFFFFFF,
                }
                for path, (abs_path, entry_type) in sorted(self.entries.items())
            ],
        }
        manifest_bytes = json.dumps(manifest, indent=2).encode("utf-8")

        # directory + payload
        all_entries: list[tuple[str, bytes, int]] = [
            (path, abs_path.read_bytes(), entry_type)
            for path, (abs_path, entry_type) in sorted(self.entries.items())
        ]
        all_entries.append(("manifest.json", manifest_bytes, TYPE_MANIFEST))

        directory = bytearray()
        directory += struct.pack("<I", len(all_entries))
        payload_blobs: list[bytes] = []
        payload_size = 0
        for entry_path, data, entry_type in all_entries:
            path_bytes = entry_path.encode("utf-8")
            crc = zlib.crc32(data) & 0xFFFFFFFF
            if self.use_lz4 and len(data) > 128:
                # store_size=False -> raw LZ4 block, matching the engine's
                # built-in block decoder (docs/scene-pack-format.md).
                compressed = _lz4block.compress(data, store_size=False)
                if len(compressed) < len(data):
                    compression, blob, uncomp = COMPRESSION_LZ4, compressed, len(data)
                else:
                    compression, blob, uncomp = COMPRESSION_STORE, data, len(data)
            else:
                compression, blob, uncomp = COMPRESSION_STORE, data, len(data)
            directory += struct.pack("<H", len(path_bytes))
            directory += path_bytes
            # pathLen(u16) path type(u8) compression(u8) crc32(u32)
            # uncompSize(u64) compSize(u64) offset(u64) reserved(7)
            directory += struct.pack(
                "<BBIQQQ",
                entry_type,
                compression,
                crc,
                uncomp,
                len(blob),
                0,
            ) + b"\x00" * 7
            payload_blobs.append(blob)
            payload_size += len(blob)

        # layout: pad payload blobs to ALIGN
        data_offset = HEADER_SIZE
        offsets = []
        cursor = data_offset
        for blob in payload_blobs:
            pad = (-cursor) % ALIGN
            cursor += pad
            offsets.append(cursor)
            cursor += len(blob)
        index_offset = cursor

        # patch offsets into the directory
        patched = bytearray(directory)
        pos = 4  # skip entryCount
        for offset in offsets:
            path_len = struct.unpack_from("<H", patched, pos)[0]
            fixed_pos = pos + 2 + path_len + 1 + 1 + 4 + 8 + 8
            struct.pack_into("<Q", patched, fixed_pos, offset)
            pos = fixed_pos + 8 + 7

        header = struct.pack(
            "<8sIIQQQI20x",
            MAGIC,
            FORMAT_VERSION,
            1,
            index_offset,
            len(patched),
            data_offset,
            zlib.crc32(bytes(patched)) & 0xFFFFFFFF,
        )

        out = bytearray(header)
        cursor = HEADER_SIZE
        for blob, offset in zip(payload_blobs, offsets):
            out += b"\x00" * (offset - cursor)
            out += blob
            cursor = offset + len(blob)
        out += bytes(patched)
        return bytes(out)


def find_pack_root(scene_path: Path) -> Path:
    """Walk up to the directory containing pack.json."""
    for parent in [scene_path.parent, *scene_path.parents]:
        if (parent / "pack.json").is_file():
            return parent
        if (parent / "Assets").is_dir() and scene_path.is_relative_to(parent):
            # scene outside a pack but repo-like layout; treat repo root as pack
            return parent
    return scene_path.parent


def cmd_pack(args: argparse.Namespace) -> int:
    scene_path = Path(args.scene).resolve()
    if not scene_path.is_file():
        print(f"error: scene not found: {scene_path}", file=sys.stderr)
        return 1

    if args.assets:
        pack_root = Path(args.assets).resolve()
    else:
        pack_root = find_pack_root(scene_path)

    use_lz4 = args.lz4 and not args.store
    if use_lz4 and _lz4block is None:
        print("warning: python-lz4 not installed; falling back to stored payloads", file=sys.stderr)
        use_lz4 = False

    builder = PackBuilder(pack_root, scene_path.parent, use_lz4, verbose=args.verbose)
    scene_doc = _json_load(scene_path)

    # resolve the scene itself first
    try:
        scene_rel = scene_path.relative_to(pack_root)
        scene_entry = _norm_rel(scene_rel)
    except ValueError:
        scene_entry = None
    if scene_entry is None:
        scene_entry = builder.add_file(scene_path, TYPE_SCENE)
    else:
        builder.entries[scene_entry] = (scene_path, TYPE_SCENE)
        if args.verbose:
            print(f"  + {scene_entry}")

    builder.scan_document(scene_doc, scene_path.parent)

    out_path = Path(args.output).resolve() if args.output else scene_path.with_suffix(".caustica")
    data = builder.build(scene_entry)
    out_path.write_bytes(data)

    total_uncomp = sum(p.stat().st_size for p, _ in builder.entries.values())
    print(f"packed {len(builder.entries) + 1} files ({total_uncomp:,} B) -> {out_path} ({len(data):,} B)")
    print(f"primary scene: {scene_entry}")
    return 0


def _read_pack(path: Path):
    data = path.read_bytes()
    if len(data) < HEADER_SIZE:
        raise PackError("file too small to be a caustica pack")
    magic, version, flags, index_offset, index_size, data_offset, index_crc = struct.unpack_from(
        "<8sIIQQQI", data, 0
    )
    if magic != MAGIC:
        raise PackError(f"bad magic {magic!r}")
    if version != FORMAT_VERSION:
        raise PackError(f"unsupported version {version}")
    directory = data[index_offset : index_offset + index_size]
    if zlib.crc32(directory) & 0xFFFFFFFF != index_crc:
        raise PackError("central directory CRC mismatch")
    (entry_count,) = struct.unpack_from("<I", directory, 0)
    entries = []
    pos = 4
    for _ in range(entry_count):
        (path_len,) = struct.unpack_from("<H", directory, pos)
        pos += 2
        entry_path = directory[pos : pos + path_len].decode("utf-8")
        pos += path_len
        entry_type, compression, crc, uncomp, comp, offset = struct.unpack_from("<BBIQQQ", directory, pos)
        pos += 2 + 4 + 8 + 8 + 8 + 7
        entries.append(
            {
                "path": entry_path,
                "type": entry_type,
                "compression": compression,
                "crc32": crc,
                "uncompSize": uncomp,
                "compSize": comp,
                "offset": offset,
            }
        )
    return {
        "data": data,
        "flags": flags,
        "dataOffset": data_offset,
        "entries": entries,
    }


def _entry_bytes(pack, entry) -> bytes:
    blob = pack["data"][entry["offset"] : entry["offset"] + entry["compSize"]]
    if entry["compression"] == COMPRESSION_LZ4:
        blob = _lz4block.decompress(blob, uncompressed_size=entry["uncompSize"])
    elif entry["compression"] != COMPRESSION_STORE:
        raise PackError(f"unknown compression {entry['compression']}")
    if len(blob) != entry["uncompSize"]:
        raise PackError(f"size mismatch for {entry['path']}")
    if zlib.crc32(blob) & 0xFFFFFFFF != entry["crc32"]:
        raise PackError(f"CRC mismatch for {entry['path']}")
    return blob


def cmd_inspect(args: argparse.Namespace) -> int:
    path = Path(args.pack).resolve()
    try:
        pack = _read_pack(path)
    except (OSError, PackError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    print(f"pack: {path}")
    print(f"entries: {len(pack['entries'])}  dataOffset: {pack['dataOffset']}")
    bad = 0
    for entry in pack["entries"]:
        status = "ok"
        try:
            _entry_bytes(pack, entry)
        except PackError as exc:
            status = f"FAIL ({exc})"
            bad += 1
        comp = "lz4" if entry["compression"] == COMPRESSION_LZ4 else "store"
        print(
            f"  [{_TYPE_NAMES.get(entry['type'], '?'):8s}] {entry['path']}"
            f"  {entry['uncompSize']:>12,} B  {comp:5s}  {status}"
        )
    print("all entries verified" if bad == 0 else f"{bad} entries FAILED")
    return 0 if bad == 0 else 2


def cmd_extract(args: argparse.Namespace) -> int:
    path = Path(args.pack).resolve()
    out_dir = Path(args.output).resolve() if args.output else path.with_suffix("")
    pack = _read_pack(path)
    for entry in pack["entries"]:
        target = out_dir / entry["path"]
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(_entry_bytes(pack, entry))
    print(f"extracted {len(pack['entries'])} entries -> {out_dir}")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    p_pack = sub.add_parser("pack", help="pack a scene into a .caustica file")
    p_pack.add_argument("scene", help="path to the .scene.json")
    p_pack.add_argument("-o", "--output", help="output .caustica path (default: alongside scene)")
    p_pack.add_argument("--assets", help="asset pack root (default: discovered from scene location)")
    p_pack.add_argument("--lz4", action="store_true", help="compress payloads with LZ4 (requires python-lz4)")
    p_pack.add_argument("--store", action="store_true", help="force stored (uncompressed) payloads")
    p_pack.add_argument("-v", "--verbose", action="store_true")
    p_pack.set_defaults(func=cmd_pack)

    p_inspect = sub.add_parser("inspect", help="verify and list a .caustica pack")
    p_inspect.add_argument("pack")
    p_inspect.set_defaults(func=cmd_inspect)

    p_extract = sub.add_parser("extract", help="extract a .caustica pack to a directory")
    p_extract.add_argument("pack")
    p_extract.add_argument("-o", "--output")
    p_extract.set_defaults(func=cmd_extract)

    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
