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


def canonical_material_rel(rel: str) -> str:
    normalized = rel.replace("\\", "/")
    if normalized.lower().startswith("materials/"):
        return "materials/" + normalized.split("/", 1)[1]
    return normalized


def material_file_exists(pack_root: Path, rel: str) -> bool:
    rel = rel.replace("\\", "/")
    if (pack_root / rel).is_file():
        return True
    parts = rel.split("/", 1)
    if len(parts) == 2 and parts[0].lower() == "materials":
        for folder in ("materials", "Materials"):
            if (pack_root / folder / parts[1]).is_file():
                return True
    return False


def add_material(
    by_model: dict[str, dict[str, str]], pack_root: Path, path: Path
) -> None:
    name = path.name
    ext = next((e for e in EXTENSIONS if name.endswith(e)), None)
    if ext is None or not path.is_file():
        return
    stem = name[: -len(ext)]
    if "." not in stem:
        return
    model, mat = stem.split(".", 1)
    # Sorted traversal plus setdefault makes duplicate handling deterministic.
    by_model.setdefault(model.lower(), {}).setdefault(
        mat, pack_relative(pack_root, path)
    )


def index_materials(
    pack_root: Path,
) -> tuple[
    dict[str, dict[str, str]],
    dict[str, dict[str, dict[str, str]]],
]:
    """Return shared materials and legacy scene-specialized materials.

    RTXPT resolved ``Materials/<scene-file-stem>/`` before the shared
    ``Materials/`` directory.  Keeping those indexes separate is important:
    flattening a recursive scan into one model/material dictionary lets an
    unrelated scene-specialized file overwrite the shared material.
    """
    shared: dict[str, dict[str, str]] = {}
    specialized: dict[str, dict[str, dict[str, str]]] = {}
    materials_dir = pack_root / "materials"
    search_roots = [materials_dir]
    if (pack_root / "Materials").is_dir():
        search_roots.append(pack_root / "Materials")

    seen_roots: set[str] = set()
    for root in sorted(search_roots, key=lambda value: value.as_posix().lower()):
        if not root.is_dir():
            continue
        root_key = str(root.resolve()).lower()
        if root_key in seen_roots:
            continue
        seen_roots.add(root_key)

        for path in sorted(root.iterdir(), key=lambda value: value.as_posix().lower()):
            if path.is_file():
                add_material(shared, pack_root, path)
            elif path.is_dir():
                scope = path.name.lower()
                scoped = specialized.setdefault(scope, {})
                for nested in sorted(
                    path.rglob("*"), key=lambda value: value.as_posix().lower()
                ):
                    add_material(scoped, pack_root, nested)
    return shared, specialized


def model_stem(source: str) -> str | None:
    if not source or source.startswith("builtin:"):
        return None
    if source.endswith(".prefab.json"):
        return None
    name = Path(source.replace("\\", "/")).name
    if name.lower().endswith(".gltf") or name.lower().endswith(".glb") or name.lower().endswith(".obj"):
        return Path(name).stem
    return Path(name).stem


def prefab_slot_names(pack_root: Path, source: str) -> set[str] | None:
    """Material names actually present on a glTF prefab, if they can be read.

    Many Sketchfab exports are named ``scene.gltf``.  Indexing only by that
    filename stem mixes unrelated ``scene.*`` materials into every instance.
    Restricting adds/repairs to names from the glTF keeps each prefab's map
    aligned with RTXPT's per-material file lookup.
    """
    if not source or source.startswith("builtin:"):
        return None
    rel = source.replace("\\", "/")
    path = pack_root / rel
    if not path.is_file() or path.suffix.lower() != ".gltf":
        return None
    try:
        doc = load_json(path)
    except (OSError, json.JSONDecodeError, ValueError):
        return None
    materials = doc.get("materials")
    if not isinstance(materials, list) or not materials:
        return None
    names = {
        mat["name"]
        for mat in materials
        if isinstance(mat, dict) and isinstance(mat.get("name"), str)
    }
    return names or None


def material_slots(
    source: str,
    shared: dict[str, dict[str, str]],
    specialized: dict[str, dict[str, str]],
    all_specialized: list[dict[str, dict[str, str]]],
) -> tuple[dict[str, str], dict[str, set[str]]]:
    stem = model_stem(source)
    if not stem:
        return {}, {}
    keys = [stem.lower(), Path(source.replace("\\", "/")).parent.name.lower()]
    key = next(
        (
            candidate
            for candidate in keys
            if candidate in shared or candidate in specialized
        ),
        None,
    )
    if key is None:
        return {}, {}

    resolved = dict(shared.get(key, {}))
    resolved.update(specialized.get(key, {}))
    candidates: dict[str, set[str]] = {}
    for index in (shared, *all_specialized):
        for mat, path in index.get(key, {}).items():
            candidates.setdefault(mat, set()).add(path.replace("\\", "/").lower())
    return resolved, candidates


def patch_entity(
    entity: dict,
    pack_root: Path,
    shared: dict[str, dict[str, str]],
    specialized: dict[str, dict[str, str]],
    all_specialized: list[dict[str, dict[str, str]]],
) -> bool:
    components = entity.get("components")
    if not isinstance(components, dict):
        return False
    prefab = components.get("PrefabInstance")
    if not isinstance(prefab, dict) or not isinstance(prefab.get("source"), str):
        return False
    slots, candidates = material_slots(
        prefab["source"], shared, specialized, all_specialized
    )
    slot_names = prefab_slot_names(pack_root, prefab["source"])
    if slot_names:
        slots = {mat: rel for mat, rel in slots.items() if mat in slot_names}
    if not slots and not prefab.get("materials"):
        return False
    materials = prefab.get("materials")
    if not isinstance(materials, dict):
        materials = {}
    changed = False
    for mat, rel in list(materials.items()):
        if slot_names and mat not in slot_names and mat != "*":
            if mat in slots or mat in candidates:
                del materials[mat]
                changed = True
            continue
        if not isinstance(rel, str):
            continue
        canonical = canonical_material_rel(rel)
        if canonical != rel:
            materials[mat] = canonical
            rel = canonical
            changed = True
        expected = slots.get(mat)
        missing = not material_file_exists(pack_root, canonical)
        # Repair references produced by the old flattening bug, plus dangling
        # paths whose specialized files were never copied into this pack.
        # Preserve deliberate custom overrides not present in our index.
        if expected and canonical.lower() != expected.lower() and (
            missing or canonical.lower() in candidates.get(mat, set())
        ):
            materials[mat] = expected
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
    shared, specialized_by_scope = index_materials(pack_root)
    all_specialized = list(specialized_by_scope.values())
    updated = 0
    for scene_path in pack_root.rglob("*.scene.json"):
        doc = load_json(scene_path)
        if not isinstance(doc, dict):
            continue
        # Path.stem removes only '.json', matching RTXPT's legacy
        # filename().stem() lookup (for example 'foo.scene').
        scope = scene_path.stem.lower()
        specialized = specialized_by_scope.get(scope, {})
        changed = False
        for entity in doc.get("entities") or []:
            if isinstance(entity, dict) and patch_entity(
                entity, pack_root, shared, specialized, all_specialized
            ):
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
