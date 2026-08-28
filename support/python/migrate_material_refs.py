"""Add PrefabInstance.materials maps from existing materials/*.material.json files."""

from __future__ import annotations

import json
import re
from pathlib import Path

EXTENSIONS = (".material.json", ".mat.json")


def load_json(path: Path) -> dict:
    text = path.read_text(encoding="utf-8")
    text = re.sub(r",\s*([}\]])", r"\1", text)
    return json.loads(text)


def dump_json(path: Path, data: dict) -> None:
    path.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def pack_relative(pack_root: Path, path: Path) -> str:
    rel = path.relative_to(pack_root).as_posix()
    lowered = rel.split("/", 1)
    if lowered and lowered[0].lower() in ("materials",):
        rest = rel.split("/", 1)[1] if "/" in rel else ""
        rel = f"materials/{rest}" if rest else "materials"
    return rel


def index_materials(pack_root: Path) -> dict[str, dict[str, str]]:
    """model stem (lower) -> { material name -> pack-relative path }"""
    by_model: dict[str, dict[str, str]] = {}
    materials_dir = pack_root / "materials"
    search_roots = [materials_dir]
    if (pack_root / "Materials").is_dir():
        search_roots.append(pack_root / "Materials")

    for root in search_roots:
        if not root.is_dir():
            continue
        for path in root.rglob("*"):
            name = path.name
            ext = next((e for e in EXTENSIONS if name.endswith(e)), None)
            if ext is None or not path.is_file():
                continue
            stem = name[: -len(ext)]
            if "." in stem:
                model, mat = stem.split(".", 1)
            else:
                continue
            by_model.setdefault(model.lower(), {})[mat] = pack_relative(pack_root, path)
    return by_model


def model_stem(source: str) -> str | None:
    if not source or source.startswith("builtin:"):
        return None
    if source.endswith(".prefab.json"):
        return None
    name = Path(source.replace("\\", "/")).name
    if name.lower().endswith(".gltf") or name.lower().endswith(".glb") or name.lower().endswith(".obj"):
        return Path(name).stem
    return Path(name).stem


def patch_entity(entity: dict, by_model: dict[str, dict[str, str]]) -> bool:
    components = entity.get("components")
    if not isinstance(components, dict):
        return False
    prefab = components.get("PrefabInstance")
    if not isinstance(prefab, dict) or not isinstance(prefab.get("source"), str):
        return False
    stem = model_stem(prefab["source"])
    if not stem:
        return False
    slots = by_model.get(stem.lower())
    if not slots:
        parent = Path(prefab["source"].replace("\\", "/")).parent.name
        slots = by_model.get(parent.lower())
    if not slots:
        return False
    materials = prefab.get("materials")
    if not isinstance(materials, dict):
        materials = {}
    changed = False
    for mat, rel in list(materials.items()):
        if isinstance(rel, str) and rel.replace("\\", "/").lower().startswith("materials/"):
            canonical = "materials/" + rel.replace("\\", "/").split("/", 1)[1]
            if canonical != rel:
                materials[mat] = canonical
                changed = True
    for mat, rel in slots.items():
        if mat not in materials:
            materials[mat] = rel
            changed = True
    if changed:
        prefab["materials"] = materials
        components["PrefabInstance"] = prefab
        entity["components"] = components
    return changed


def migrate_pack(pack_root: Path) -> int:
    by_model = index_materials(pack_root)
    updated = 0
    for scene_path in pack_root.rglob("*.scene.json"):
        doc = load_json(scene_path)
        if not isinstance(doc, dict):
            continue
        changed = False
        for entity in doc.get("entities") or []:
            if isinstance(entity, dict) and patch_entity(entity, by_model):
                changed = True
        if changed:
            dump_json(scene_path, doc)
            updated += 1
            print(f"updated {scene_path}")
    return updated


def main() -> None:
    repo = Path(__file__).resolve().parents[2]
    for pack in (repo / "Assets", repo / "assets-builtin"):
        if pack.is_dir():
            count = migrate_pack(pack)
            print(f"{pack.name}: {count} scene files")


if __name__ == "__main__":
    main()
