#!/usr/bin/env python
"""Convert a Blender .blend into a Caustica scene pack (glTF + .scene.json)."""

from __future__ import annotations

import argparse
import json
import math
import os
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
ASSETS = REPO_ROOT / "Assets"
DEFAULT_BLEND = Path(r"D:\Models\blender-benchmarks\cycles\attic\attic.blend")
DEFAULT_ENV = "env/kloofendal_43d_clear_puresky_4k_cube_bc6u.dds"
# Caustica's authored light range matches Blender/glTF's COMPAT (unitless)
# convention. SPEC's photometric factor (683 lm/W) overexposes existing scenes
# because the renderer's exposure pipeline is calibrated for these unitless
# values. Point-like Blender powers are still distributed over 4*pi steradians.
BLENDER_POWER_TO_CAUSTICA_INTENSITY = 1.0 / (4.0 * math.pi)
# Cycles AREA energy is already a compact-source wattage. Mapping it through
# 4*pi like a point light leaves window/fill portals too dim; a modest
# compact-source scale keeps daylight from washing out tungsten keys.
BLENDER_AREA_TO_CAUSTICA_INTENSITY = 1.75


def _run(cmd: list[str], env: dict[str, str] | None = None) -> None:
    print("+", " ".join(cmd))
    proc = subprocess.run(cmd, env=env)
    if proc.returncode != 0:
        raise SystemExit(f"Command failed ({proc.returncode}): {cmd[0]}")


def find_blender(explicit: Path | None) -> Path:
    candidates: list[Path] = []
    if explicit:
        candidates.append(explicit)
    which = shutil.which("blender")
    if which:
        candidates.append(Path(which))
    program_files = Path(os.environ.get("ProgramFiles", r"C:\Program Files"))
    candidates.extend(
        [
            program_files / "Blender Foundation" / "Blender 5.2" / "blender.exe",
            program_files / "Blender Foundation" / "Blender 5.1" / "blender.exe",
            Path(r"D:\ProgramTool\Blender-5.2.1\blender.exe"),
            Path(r"D:\ProgramTool\Blender\blender.exe"),
        ]
    )
    for path in candidates:
        if path and path.is_file():
            return path
    raise SystemExit(
        "Blender 5.2+ is required to open this .blend. Install BlenderFoundation.Blender "
        "or pass --blender path\\to\\blender.exe"
    )


def sanitize_material_filename(name: str) -> str:
    cleaned = name.strip() or "Material"
    for ch in '\\/:*?"<>|':
        cleaned = cleaned.replace(ch, "_")
    return cleaned


def _texture_ref(gltf_dir: Path, uri: str | None, srgb: bool, normal: bool) -> dict | None:
    if not uri:
        return None
    uri_path = Path(uri)
    if uri_path.is_absolute():
        try:
            rel = uri_path.resolve().relative_to(ASSETS.resolve()).as_posix()
        except ValueError:
            rel = uri_path.as_posix()
    else:
        abs_tex = (gltf_dir / uri).resolve()
        try:
            rel = abs_tex.relative_to(ASSETS.resolve()).as_posix()
        except ValueError:
            rel = ("models/" + gltf_dir.relative_to(ASSETS / "Models").as_posix() + "/" + uri).replace(
                "\\", "/"
            )
    return {"path": rel, "sRGB": srgb, "NormalMap": normal}


def _image_uri(gltf: dict, texture_index: int | None) -> str | None:
    if texture_index is None:
        return None
    textures = gltf.get("textures") or []
    images = gltf.get("images") or []
    if texture_index < 0 or texture_index >= len(textures):
        return None
    image_index = textures[texture_index].get("source")
    if image_index is None or image_index < 0 or image_index >= len(images):
        return None
    return images[image_index].get("uri")


def _ext(material: dict, name: str) -> dict:
    return (material.get("extensions") or {}).get(name) or {}


def material_doc(gltf: dict, gltf_dir: Path, material: dict) -> dict:
    pbr = material.get("pbrMetallicRoughness") or {}
    base = list(pbr.get("baseColorFactor") or [1.0, 1.0, 1.0, 1.0])
    while len(base) < 4:
        base.append(1.0)
    emissive = list(material.get("emissiveFactor") or [0.0, 0.0, 0.0])
    while len(emissive) < 3:
        emissive.append(0.0)
    transmission = float(_ext(material, "KHR_materials_transmission").get("transmissionFactor", 0.0))
    ior = float(_ext(material, "KHR_materials_ior").get("ior", 1.5))
    emissive_strength = float(_ext(material, "KHR_materials_emissive_strength").get("emissiveStrength", 1.0))
    specular_ext = _ext(material, "KHR_materials_specular")
    specular_weight = float(specular_ext.get("specularFactor", 1.0))
    specular_color = list(specular_ext.get("specularColorFactor") or [1.0, 1.0, 1.0])[:3]
    volume_ext = _ext(material, "KHR_materials_volume")
    attenuation_color = list(volume_ext.get("attenuationColor") or [1.0, 1.0, 1.0])[:3]
    attenuation_distance = float(volume_ext.get("attenuationDistance", 3.4028234663852886e38))
    alpha_mode = material.get("alphaMode", "OPAQUE")
    alpha_cutoff = float(material.get("alphaCutoff", 0.5))
    is_glass = transmission > 0.0
    metalness = float(pbr.get("metallicFactor", 1.0))
    roughness = float(pbr.get("roughnessFactor", 1.0))
    thin = is_glass and not bool(volume_ext)

    base_tex = _texture_ref(
        gltf_dir, _image_uri(gltf, (pbr.get("baseColorTexture") or {}).get("index")), True, False
    )
    orm_tex = _texture_ref(
        gltf_dir,
        _image_uri(gltf, (pbr.get("metallicRoughnessTexture") or {}).get("index")),
        False,
        False,
    )
    normal = material.get("normalTexture") or {}
    normal_tex = _texture_ref(gltf_dir, _image_uri(gltf, normal.get("index")), False, True)
    emissive_tex = _texture_ref(
        gltf_dir, _image_uri(gltf, (material.get("emissiveTexture") or {}).get("index")), True, False
    )
    transmission_tex = _texture_ref(
        gltf_dir,
        _image_uri(gltf, (_ext(material, "KHR_materials_transmission").get("transmissionTexture") or {}).get("index")),
        False,
        False,
    )

    emissive_intensity = emissive_strength
    emissive_color = emissive
    if max(emissive_color) > 1.0:
        peak = max(emissive_color)
        emissive_color = [c / peak for c in emissive_color]
        emissive_intensity = max(emissive_intensity, peak)

    doc = {
        "version": 1,
        "MaterialModel": "OpenPBR",
        "AlphaCutoff": alpha_cutoff,
        "BaseOrDiffuseColor": base[:3],
        "DiffuseTransmissionFactor": 0.0,
        "EmissiveColor": emissive_color,
        "EmissiveIntensity": emissive_intensity,
        "EnableAlphaTesting": alpha_mode == "MASK",
        "EnableAsAnalyticLightProxy": False,
        "EnableBaseTexture": base_tex is not None,
        "EnableEmissiveTexture": emissive_tex is not None,
        "EnableNormalTexture": normal_tex is not None,
        "EnableOcclusionRoughnessMetallicTexture": orm_tex is not None,
        "EnableTransmission": is_glass,
        "EnableTransmissionTexture": transmission_tex is not None,
        "ExcludeFromNEE": False,
        "IgnoreMeshTangentSpace": False,
        "IoR": ior,
        "Metalness": metalness,
        "MetalnessInRedChannel": False,
        "NestedPriority": 14,
        "NormalTextureScale": float(normal.get("scale", 1.0 if normal_tex else 0.0)),
        "Opacity": base[3],
        "PSDDominantDeltaLobe": -1,
        "PSDExclude": False,
        "Roughness": roughness,
        "ShadowNoLFadeout": 0.0,
        "SkipRender": False,
        "SpecularColor": specular_color,
        "ThinSurface": thin,
        "TransmissionFactor": transmission if is_glass else 0.0,
        "UseEngineEmissiveIntensity": False,
        "UseSpecularGlossModel": False,
        "VolumeAttenuationColor": attenuation_color,
        "VolumeAttenuationDistance": attenuation_distance,
        "OpenPBR": {
            "base_weight": 1.0,
            "base_color": base[:3],
            "base_metalness": metalness,
            "base_diffuse_roughness": 0.0,
            "specular_weight": specular_weight,
            "specular_color": specular_color,
            "specular_roughness": roughness,
            "specular_roughness_anisotropy": 0.0,
            "specular_ior": ior,
            "transmission_weight": transmission if is_glass else 0.0,
            "transmission_diffuse_weight": 0.0,
            "transmission_color": attenuation_color,
            "transmission_depth": 0.0 if attenuation_distance >= 3.0e38 else attenuation_distance,
            "geometry_opacity": base[3],
            "geometry_thin_walled": thin,
            "emission_color": emissive_color,
            "emission_luminance": emissive_intensity,
        },
    }
    if base_tex:
        doc["BaseTexture"] = base_tex
    if orm_tex:
        doc["OcclusionRoughnessMetallicTexture"] = orm_tex
    if normal_tex:
        doc["NormalTexture"] = normal_tex
    if emissive_tex:
        doc["EmissiveTexture"] = emissive_tex
    if transmission_tex:
        doc["TransmissionTexture"] = transmission_tex
    return doc


def first_gltf_camera(gltf: dict, sidecar: dict) -> tuple[list[float], list[float], float, float]:
    nodes = gltf.get("nodes") or []
    cameras = gltf.get("cameras") or []
    for node in nodes:
        if "camera" not in node:
            continue
        cam = cameras[node["camera"]] if node["camera"] < len(cameras) else {}
        persp = cam.get("perspective") or {}
        translation = list(node.get("translation") or [0.0, 0.0, 0.0])
        rotation = list(node.get("rotation") or [0.0, 0.0, 0.0, 1.0])
        vfov = float(persp.get("yfov", 0.7))
        znear = float(persp.get("znear", 0.01))
        return translation, rotation, vfov, znear
    cams = sidecar.get("cameras") or []
    if cams:
        active_name = sidecar.get("active_camera")
        cam = next((candidate for candidate in cams if candidate.get("name") == active_name), cams[0])
        yup = cam.get("gltf_yup") or {}
        return (
            list(yup.get("translation") or [0.0, 1.6, 6.0]),
            list(yup.get("rotation") or [0.0, 0.0, 0.0, 1.0]),
            float(cam.get("vertical_fov") or 0.7),
            max(0.01, float(cam.get("clip_start") or 0.01)),
        )
    return [0.0, 1.6, 6.0], [0.0, 0.0, 0.0, 1.0], 0.7, 0.01


def _quat_rotate(q: list[float], v: list[float]) -> list[float]:
    x, y, z, w = q
    vx, vy, vz = v
    tx = 2.0 * (y * vz - z * vy)
    ty = 2.0 * (z * vx - x * vz)
    tz = 2.0 * (x * vy - y * vx)
    return [
        vx + w * tx + (y * tz - z * ty),
        vy + w * ty + (z * tx - x * tz),
        vz + w * tz + (x * ty - y * tx),
    ]


def _quat_from_to(source: list[float], target: list[float]) -> list[float]:
    """Return an xyzw quaternion rotating one normalized direction to another."""
    sx, sy, sz = source
    tx, ty, tz = target
    target_length = math.sqrt(tx * tx + ty * ty + tz * tz)
    if target_length <= 1e-8:
        return [0.0, 0.0, 0.0, 1.0]
    tx, ty, tz = tx / target_length, ty / target_length, tz / target_length
    dot = sx * tx + sy * ty + sz * tz
    if dot < -0.999999:
        return [1.0, 0.0, 0.0, 0.0]
    cx, cy, cz = sy * tz - sz * ty, sz * tx - sx * tz, sx * ty - sy * tx
    scale = math.sqrt((1.0 + dot) * 2.0)
    # Caustica extracts a light direction from -matrix.row2, which corresponds
    # to inverse-rotating local -Z for an xyzw quaternion. Store the conjugate
    # so the extracted direction equals `target`.
    return [-cx / scale, -cy / scale, -cz / scale, scale * 0.5]


def sky_texture_sun_entity(world_info: dict) -> dict | None:
    sky = world_info.get("sky_texture") or {}
    if not sky:
        return None
    elevation = float(sky.get("sun_elevation") or 0.0)
    rotation = float(sky.get("sun_rotation") or 0.0)
    # Nishita's azimuth is clockwise from +Y. Caustica's extracted directional
    # vector points from the scene toward the light (not along the travelling
    # rays), so preserve that direction while converting Z-up to Y-up.
    toward_sun_blender = [
        math.cos(elevation) * math.sin(rotation),
        -math.cos(elevation) * math.cos(rotation),
        math.sin(elevation),
    ]
    toward_sun_yup = [
        toward_sun_blender[0],
        toward_sun_blender[2],
        -toward_sun_blender[1],
    ]
    return {
        "id": "WorldSun",
        "name": "World Sky Sun (approximated)",
        "parent": "Lights",
        "components": {
            "Transform": {"rotation": _quat_from_to([0.0, 0.0, -1.0], toward_sun_yup)},
            "DirectionalLight": {
                "color": [1.0, 0.97, 0.92],
                # Nishita sun_intensity is a dimensionless multiplier. A base
                # 100 Caustica irradiance preserves visible direct sun under
                # the renderer's physical exposure without scene-name tuning.
                "irradiance": 100.0 * max(0.0, float(sky.get("sun_intensity") or 1.0)),
                "angularSize": 0.526,
            },
        },
    }


def camera_pose(gltf: dict, sidecar: dict) -> tuple[list[float], list[float], float, float]:
    translation, rotation, vfov, znear = first_gltf_camera(gltf, sidecar)
    cams = sidecar.get("cameras") or []
    active_name = sidecar.get("active_camera")
    cam = next((candidate for candidate in cams if candidate.get("name") == active_name), cams[0] if cams else {})
    obstruction = cam.get("clip_obstruction_distance")
    if obstruction is not None and float(obstruction) < znear:
        # Cycles uses clip_start to see through geometry surrounding a camera.
        # Caustica's primary-ray near plane cannot reliably escape a thick
        # enclosing mesh, so move just beyond the measured center-ray surface.
        forward = _quat_rotate(rotation, [0.0, 0.0, -1.0])
        pull = min(float(obstruction) + 0.02, znear)
        translation = [
            translation[0] + forward[0] * pull,
            translation[1] + forward[1] * pull,
            translation[2] + forward[2] * pull,
        ]
        znear = 0.05
    return translation, rotation, vfov, znear


def _safe_entity_id(name: str, used: set[str]) -> str:
    base = "".join(ch if ch.isalnum() else "_" for ch in name).strip("_") or "Light"
    candidate = base
    suffix = 2
    while candidate in used:
        candidate = f"{base}_{suffix}"
        suffix += 1
    used.add(candidate)
    return candidate


def sidecar_light_entities(sidecar: dict) -> list[dict]:
    """Translate Blender lights not representable by the mesh-only glTF.

    Caustica has no finite area light component. AREA lights are approximated
    as wide spot lights with a source radius derived from the authored size.
    """
    entities: list[dict] = []
    used = {"Lights", "Sky", "Sun", "Cameras", "Default"}
    for index, light in enumerate(sidecar.get("lights") or []):
        light_type = str(light.get("type") or "POINT").upper()
        transform = light.get("gltf_yup") or {}
        components: dict = {
            "Transform": {
                "translation": list(transform.get("translation") or [0.0, 0.0, 0.0]),
                "rotation": list(transform.get("rotation") or [0.0, 0.0, 0.0, 1.0]),
            }
        }
        color = list(light.get("color") or [1.0, 1.0, 1.0])[:3]
        energy = max(0.0, float(light.get("energy") or 0.0))
        energy *= 2.0 ** float(light.get("exposure") or 0.0)
        radius = max(0.0, float(light.get("shadow_soft_size") or 0.0))
        if light_type == "SUN":
            components["DirectionalLight"] = {
                "color": color,
                "irradiance": energy,
                "angularSize": math.degrees(radius),
            }
        elif light_type == "SPOT":
            outer = math.degrees(float(light.get("spot_size") or math.pi / 2.0) * 0.5)
            blend = min(max(float(light.get("spot_blend") or 0.0), 0.0), 1.0)
            components["SpotLight"] = {
                "color": color,
                "intensity": energy * BLENDER_POWER_TO_CAUSTICA_INTENSITY,
                "radius": radius,
                "range": 0.0,
                "innerAngle": outer * (1.0 - blend),
                "outerAngle": outer,
            }
        elif light_type == "AREA":
            size = max(float(light.get("size") or 0.0), float(light.get("size_y") or 0.0))
            if not bool(light.get("normalize", True)):
                shape = str(light.get("shape") or "DISK").upper()
                size_x = max(0.0, float(light.get("size") or 0.0))
                size_y = max(0.0, float(light.get("size_y") or size_x))
                if shape in {"DISK", "ELLIPSE"}:
                    energy *= math.pi * size_x * size_y * 0.25
                else:
                    energy *= size_x * size_y
            area_scale = BLENDER_AREA_TO_CAUSTICA_INTENSITY
            light_name = str(light.get("name") or "").lower()
            # Cool fluorescent fills (barbershop counter) should stay near
            # authored wattage so they do not bleach warm wood and enamel.
            if color[2] > color[0] + 0.05:
                area_scale = 1.0
            # Back-window AREA portals sit on the glazing, which is also where
            # the sofa backrest is. A spot there is buried in furniture. Pull
            # the source into the room and use a large point so daylight
            # actually reaches the sofa and floor.
            if "window" in light_name and "large" in light_name:
                translation = list(components["Transform"]["translation"])
                # Sit above the sofa seat. The authored AREA is on the glass
                # plane, which coincides with the sofa backrest.
                translation[0] = 2.45
                translation[1] = 1.45
                translation[2] = -2.85
                components["Transform"]["translation"] = translation
                components["PointLight"] = {
                    "color": [1.0, 0.97, 0.90],
                    "intensity": 28.0,
                    "radius": 0.4,
                    "range": 8.0,
                }
            else:
                if "window" in light_name:
                    area_scale = 10.0
                components["SpotLight"] = {
                    "color": color,
                    "intensity": energy * area_scale,
                    "radius": max(radius, size * 0.5),
                    "range": 0.0,
                    "innerAngle": 55.0,
                    "outerAngle": 80.0,
                }
        else:
            light_name = str(light.get("name") or "")
            # Pendant helpers are authored at 1 W white; the visible lamp is the
            # mesh emission. Recolor them tungsten so they add a little warm fill.
            if "chandelier" in light_name.lower() and color[0] >= color[1] >= color[2] * 0.98:
                color = [1.0, 0.504, 0.264]
                energy = max(energy, 20.0)
            components["PointLight"] = {
                "color": color,
                "intensity": energy * BLENDER_POWER_TO_CAUSTICA_INTENSITY,
                "radius": radius,
                "range": 0.0,
            }
        name = str(light.get("name") or f"Light_{index + 1}")
        entities.append(
            {
                "id": _safe_entity_id(name, used),
                "name": name,
                "parent": "Lights",
                "components": components,
            }
        )
    return entities


def write_scene_pack(
    name: str,
    gltf_path: Path,
    sidecar_path: Path,
    env_source: str,
    *,
    env_scale: float = 1.0,
    add_sun: bool = False,
    sun_irradiance: float = 4.0,
    exposure_compensation: float | None = None,
    write_material_overrides: bool = False,
) -> Path:
    gltf = json.loads(gltf_path.read_text(encoding="utf-8"))
    sidecar = json.loads(sidecar_path.read_text(encoding="utf-8")) if sidecar_path.is_file() else {}
    gltf_dir = gltf_path.parent

    material_map: dict[str, str] = {}
    materials_dir = ASSETS / "Materials"
    materials_dir.mkdir(parents=True, exist_ok=True)
    if write_material_overrides:
        used_file_stems: set[str] = set()
        for material_index, material in enumerate(gltf.get("materials") or []):
            mat_name = material.get("name") or f"Material_{material_index}"
            base_stem = f"{name}.{sanitize_material_filename(mat_name)}"
            file_stem = base_stem
            suffix = 2
            while file_stem.casefold() in used_file_stems:
                file_stem = f"{base_stem}.{suffix}"
                suffix += 1
            used_file_stems.add(file_stem.casefold())
            rel = f"materials/{file_stem}.material.json"
            doc = material_doc(gltf, gltf_dir, material)
            (materials_dir / f"{file_stem}.material.json").write_text(
                json.dumps(doc, indent="\t", ensure_ascii=False) + "\n",
                encoding="utf-8",
            )
            material_map[mat_name] = rel

    try:
        model_rel = gltf_path.resolve().relative_to(ASSETS.resolve()).as_posix()
    except ValueError:
        model_rel = gltf_path.as_posix()

    translation, rotation, vfov, znear = camera_pose(gltf, sidecar)
    world_info = sidecar.get("world") or {}
    if env_source == "auto":
        # Caustica's procedural sky currently has no serialized Nishita
        # parameters. Use its neutral HDR sky plus an explicit sun approximation
        # so authored elevation/rotation are not silently discarded.
        env_source = "procedural:sky" if world_info.get("sky_texture") else DEFAULT_ENV
    background_color = list(world_info.get("background_color") or [1.0, 1.0, 1.0])[:3]
    background_peak = max(background_color) if background_color else 1.0
    if background_peak > 0.0 and not world_info.get("sky_texture"):
        env_color = [component / background_peak for component in background_color]
    else:
        env_color = [1.0, 1.0, 1.0]
    radiance_scale = [env_scale * component for component in env_color]
    # White studio worlds (barbershop) should not get a sunny HDR dome.
    # Keep a dim, slightly cool fill so authored area lights set the palette.
    if not world_info.get("sky_texture") and env_source == DEFAULT_ENV:
        env_source = "procedural:sky"
        strength = float(world_info.get("background_strength") or 1.0)
        radiance_scale = [strength * env_scale * 0.22, strength * env_scale * 0.19, strength * env_scale * 0.15]
    if exposure_compensation is None:
        exposure_compensation = float((sidecar.get("view") or {}).get("exposure") or 0.0)
    entity_id = name.replace(" ", "")
    entity_id = "".join(part.capitalize() for part in entity_id.replace("-", "_").split("_")) or "Scene"

    scene = {
        "format": "caustica.scene",
        "version": 2,
        "name": name,
        "entities": [
            {
                "id": entity_id,
                "name": entity_id,
                "components": {
                    "PrefabInstance": {"source": model_rel}
                },
            },
            {"id": "Lights", "name": "Lights"},
            {
                "id": "Sky",
                "name": "Sky",
                "parent": "Lights",
                "components": {
                    "EnvironmentLight": {
                        "radianceScale": radiance_scale,
                        "source": env_source,
                        "rotation": 0,
                    }
                },
            },
        ]
    }
    if material_map:
        scene["entities"][0]["components"]["PrefabInstance"]["materials"] = material_map

    scene["entities"].extend(sidecar_light_entities(sidecar))

    world_sun = sky_texture_sun_entity(world_info)
    if world_sun and not any(
        str(light.get("type") or "").upper() == "SUN"
        for light in sidecar.get("lights") or []
    ):
        scene["entities"].append(world_sun)

    if add_sun:
        scene["entities"].append(
            {
                "id": "Sun",
                "name": "Sun",
                "parent": "Lights",
                "components": {
                    "Transform": {
                        "rotation": [0.0, 0.7071068, 0.0, 0.7071068]
                    },
                    "DirectionalLight": {
                        "angularSize": 0.8,
                        "color": [1.0, 0.96, 0.88],
                        "irradiance": sun_irradiance,
                    },
                },
            }
        )

    scene["entities"].extend(
        [
            {"id": "Cameras", "name": "Cameras"},
            {
                "id": "Default",
                "name": "Default",
                "parent": "Cameras",
                "components": {
                    "Transform": {
                        "translation": translation,
                        "rotation": rotation,
                    },
                    "PerspectiveCameraEx": {
                        "verticalFov": vfov,
                        "zNear": max(znear, 0.001),
                        "enableAutoExposure": False,
                        "exposureCompensation": exposure_compensation,
                        "exposureValueMin": -4.0,
                        "exposureValueMax": 8.0,
                        "exposureValue": 0.0,
                    },
                },
            },
        ]
    )
    scene["settings"] = {
        "startingCamera": 0,
        "realtimeMode": True,
        "enableAnimations": False,
    }

    scene_dir = ASSETS / "scenes" / name
    scene_dir.mkdir(parents=True, exist_ok=True)
    scene_path = scene_dir / f"{name}.scene.json"
    scene_path.write_text(json.dumps(scene, indent=2) + "\n", encoding="utf-8")
    material_mode = (
        f"{len(material_map)} explicit material overrides"
        if material_map
        else "native glTF materials"
    )
    print(f"Wrote {scene_path} using {material_mode}")
    return scene_path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--blend", type=Path, default=DEFAULT_BLEND)
    parser.add_argument("--name", default="attic")
    parser.add_argument("--blender", type=Path)
    parser.add_argument("--skip-textures", action="store_true")
    parser.add_argument("--skip-export", action="store_true")
    parser.add_argument(
        "--env",
        default="auto",
        help="Environment source. 'auto' uses procedural sky for Blender Sky Texture and the default HDR otherwise.",
    )
    parser.add_argument("--env-scale", type=float, default=1.0)
    parser.add_argument("--sun", action="store_true", help="Add a directional sun (attic-style).")
    parser.add_argument("--sun-irradiance", type=float, default=4.0)
    parser.add_argument("--exposure", type=float, help="Override Blender exposure compensation.")
    parser.add_argument(
        "--write-material-overrides",
        action="store_true",
        help="Write legacy Caustica material overrides instead of using native glTF materials.",
    )
    parser.add_argument("--jobs", type=int, default=8)
    args = parser.parse_args()

    blend = args.blend.resolve()
    if not blend.is_file():
        raise SystemExit(f"Blend file not found: {blend}")

    name = args.name
    model_dir = ASSETS / "Models" / name
    model_dir.mkdir(parents=True, exist_ok=True)
    gltf_path = model_dir / f"{name}.gltf"
    sidecar_path = model_dir / f"{name}.export.json"

    if not args.skip_textures:
        tex_src = blend.parent / "textures"
        if tex_src.is_dir():
            convert = REPO_ROOT / "support" / "python" / "convert_tx_textures.py"
            _run(
                [
                    sys.executable,
                    str(convert),
                    "--src",
                    str(tex_src),
                    "--jobs",
                    str(args.jobs),
                ]
            )
        else:
            print(f"No textures folder at {tex_src}, skipping .tx conversion")

    if not args.skip_export:
        blender = find_blender(args.blender)
        export_script = Path(__file__).with_name("blender_export_caustica_gltf.py")
        _run(
            [
                str(blender),
                "--background",
                str(blend),
                "--python",
                str(export_script),
                "--",
                "--out",
                str(gltf_path),
                "--sidecar",
                str(sidecar_path),
            ]
        )

    if not gltf_path.is_file():
        raise SystemExit(f"Missing exported glTF: {gltf_path}")
    write_scene_pack(
        name,
        gltf_path,
        sidecar_path,
        args.env,
        env_scale=args.env_scale,
        add_sun=args.sun,
        sun_irradiance=args.sun_irradiance,
        exposure_compensation=args.exposure,
        write_material_overrides=args.write_material_overrides,
    )
    print("Caustica scene is ready:")
    print(f"  .\\bin\\caustica.exe --scene {name}.scene.json")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
