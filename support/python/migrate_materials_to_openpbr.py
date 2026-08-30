"""Convert legacy RTXPT metal-rough .material.json files to authored OpenPBR.

Scene JSON is not rewritten: it only references material files. Spec-gloss
Bistro materials keep ``UseSpecularGlossModel`` because their
``*_spec.dds`` maps are specular color (sRGB), not ORM. Flipping that flag
without rebaking textures is what made Bistro diverge from RTXPT.
"""

from __future__ import annotations

import json
import re
from pathlib import Path

_MIGRATED_OPENPBR_FIELDS = frozenset(
    {
        "base_weight",
        "base_color",
        "base_metalness",
        "base_diffuse_roughness",
        "specular_weight",
        "specular_color",
        "specular_roughness",
        "specular_roughness_anisotropy",
        "specular_ior",
        "transmission_weight",
        "transmission_diffuse_weight",
        "transmission_color",
        "transmission_depth",
        "geometry_opacity",
        "geometry_thin_walled",
        "emission_color",
        "emission_luminance",
    }
)


def load_json(path: Path) -> dict:
    text = path.read_text(encoding="utf-8")
    text = re.sub(r",\s*([}\]])", r"\1", text)
    return json.loads(text)


def dump_json(path: Path, data: dict) -> None:
    path.write_text(json.dumps(data, indent="\t", ensure_ascii=False) + "\n", encoding="utf-8")


def _vec3(value: object, default: list[float]) -> list[float]:
    if isinstance(value, (int, float)):
        return [float(value), float(value), float(value)]
    if isinstance(value, list) and len(value) >= 3:
        return [float(value[0]), float(value[1]), float(value[2])]
    return list(default)


def _scalar(data: dict, *names: str, default: float = 0.0) -> float:
    for name in names:
        if name in data and isinstance(data[name], (int, float)):
            return float(data[name])
    return default


def _bool(data: dict, *names: str, default: bool = False) -> bool:
    for name in names:
        if name in data and isinstance(data[name], bool):
            return data[name]
    return default


def _is_openpbr_model(data: dict) -> bool:
    model = data.get("MaterialModel", data.get("materialModel", ""))
    return str(model).lower() in {"openpbr", "openpbr-lite", "openpbr_lite"}


def _is_spec_gloss(data: dict) -> bool:
    return _bool(data, "UseSpecularGlossModel", "useSpecularGlossModel")


def _is_previous_script_migration(data: dict) -> bool:
    block = data.get("OpenPBR")
    return (
        _is_openpbr_model(data)
        and isinstance(block, dict)
        and set(block) == _MIGRATED_OPENPBR_FIELDS
        and ("Roughness" in data or "roughness" in data)
    )


def build_openpbr_block(data: dict, specular_color: list[float]) -> dict:
    roughness = _scalar(data, "Roughness", "roughness")
    # RTXPT/Frostbite used the legacy roughness for both the specular lobe and
    # the diffuse lobe. Preserve that appearance unless a material explicitly
    # authored the independent OpenPBR diffuse roughness.
    base_diffuse_roughness = _scalar(
        data, "BaseDiffuseRoughness", "baseDiffuseRoughness", default=roughness
    )
    return {
        "base_weight": _scalar(data, "BaseWeight", "baseWeight", default=1.0),
        "base_color": _vec3(data.get("BaseOrDiffuseColor", data.get("baseOrDiffuseColor")), [1.0, 1.0, 1.0]),
        "base_metalness": _scalar(data, "Metalness", "metalness"),
        "base_diffuse_roughness": base_diffuse_roughness,
        "specular_weight": _scalar(data, "SpecularWeight", "specularWeight", default=1.0),
        "specular_color": specular_color,
        "specular_roughness": roughness,
        "specular_roughness_anisotropy": _scalar(data, "Anisotropy", "anisotropy"),
        "specular_ior": _scalar(data, "IoR", "ioR", default=1.5),
        "transmission_weight": _scalar(data, "TransmissionFactor", "transmissionFactor"),
        "transmission_diffuse_weight": _scalar(
            data, "DiffuseTransmissionFactor", "diffuseTransmissionFactor"
        ),
        "transmission_color": _vec3(
            data.get("TransmissionColor", data.get("transmissionColor")), [1.0, 1.0, 1.0]
        ),
        "transmission_depth": _scalar(data, "TransmissionDepth", "transmissionDepth"),
        "geometry_opacity": _scalar(data, "Opacity", "opacity", default=1.0),
        "geometry_thin_walled": _bool(data, "ThinSurface", "thinSurface"),
        "emission_color": _vec3(data.get("EmissiveColor", data.get("emissiveColor")), [0.0, 0.0, 0.0]),
        "emission_luminance": _scalar(data, "EmissiveIntensity", "emissiveIntensity", default=1.0),
    }


def convert_material(data: dict) -> str | None:
    """Return a reason string if ``data`` was converted, otherwise None."""
    if not isinstance(data, dict):
        return None
    # Spec-gloss SpecularColor is F0, not an OpenPBR dielectric tint. Keep the
    # RTXPT path until spec maps are rebaked to ORM (or F0 is converted to IOR).
    if _is_spec_gloss(data):
        return None
    if _is_openpbr_model(data) and isinstance(data.get("OpenPBR"), dict):
        # Repair blocks emitted by the previous version of this script. Do not
        # touch hand-authored OpenPBR blocks, which may intentionally keep an
        # independent diffuse roughness such as zero.
        if _is_previous_script_migration(data):
            roughness = _scalar(data, "Roughness", "roughness")
            if "BaseDiffuseRoughness" not in data and "baseDiffuseRoughness" not in data:
                if data["OpenPBR"].get("base_diffuse_roughness") != roughness:
                    data["OpenPBR"]["base_diffuse_roughness"] = roughness
                    return "repair-legacy-diffuse-roughness"
        return None

    specular = [1.0, 1.0, 1.0]
    data["SpecularColor"] = specular
    data.pop("specularColor", None)
    data["MaterialModel"] = "OpenPBR"
    data.pop("materialModel", None)
    data["OpenPBR"] = build_openpbr_block(data, specular)
    return "metal-rough"


def iter_material_files(pack_root: Path) -> list[Path]:
    files: list[Path] = []
    for folder in ("materials", "Materials"):
        root = pack_root / folder
        if root.is_dir():
            files.extend(root.rglob("*.material.json"))
            files.extend(root.rglob("*.mat.json"))
    seen: set[str] = set()
    unique: list[Path] = []
    for path in files:
        key = str(path.resolve()).lower()
        if key in seen:
            continue
        seen.add(key)
        unique.append(path)
    return unique


def migrate_pack(pack_root: Path) -> tuple[int, int]:
    converted = 0
    skipped_spec_gloss = 0
    for path in iter_material_files(pack_root):
        data = load_json(path)
        if _is_spec_gloss(data):
            skipped_spec_gloss += 1
            continue
        if convert_material(data) is None:
            continue
        dump_json(path, data)
        converted += 1
        print(f"updated {path}")
    return converted, skipped_spec_gloss


def main() -> None:
    repo = Path(__file__).resolve().parents[2]
    for pack in (repo / "Assets", repo / "assets-builtin"):
        if pack.is_dir():
            converted, skipped = migrate_pack(pack)
            print(f"{pack.name}: converted {converted}, kept spec-gloss {skipped}")


if __name__ == "__main__":
    main()
