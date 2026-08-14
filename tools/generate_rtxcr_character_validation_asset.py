#!/usr/bin/env python3
"""Generate the license-safe synthetic RTXCR skin + DOTS hair validation scene."""

from __future__ import annotations

import base64
import argparse
import json
import math
import struct
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "Assets"
MODEL_DIR = ASSETS / "Models" / "rtxcr-character-validation"
MODEL_PATH = MODEL_DIR / "rtxcr-character-validation.gltf"
SKIN_MODEL_PATH = MODEL_DIR / "rtxcr-character-validation-skin-only.gltf"
SCENE_PATH = ASSETS / "rtxcr-character-validation.scene.json"


def ellipsoid(center, radii, rings=24, segments=36):
    positions, normals, indices = [], [], []
    cx, cy, cz = center
    rx, ry, rz = radii
    for ring in range(rings + 1):
        theta = math.pi * ring / rings
        st, ct = math.sin(theta), math.cos(theta)
        for segment in range(segments + 1):
            phi = 2.0 * math.pi * segment / segments
            cp, sp = math.cos(phi), math.sin(phi)
            x, y, z = st * cp, ct, st * sp
            positions.append((cx + rx * x, cy + ry * y, cz + rz * z))
            nx, ny, nz = x / rx, y / ry, z / rz
            inv = 1.0 / math.sqrt(nx * nx + ny * ny + nz * nz)
            normals.append((nx * inv, ny * inv, nz * inv))
    width = segments + 1
    for ring in range(rings):
        for segment in range(segments):
            a = ring * width + segment
            b = a + width
            indices.extend((a, b, a + 1, a + 1, b, b + 1))
    return positions, normals, indices


def merge_ellipsoids(parts):
    positions, normals, indices = [], [], []
    for center, radii, rings, segments in parts:
        p, n, i = ellipsoid(center, radii, rings, segments)
        base = len(positions)
        positions.extend(p)
        normals.extend(n)
        indices.extend(base + index for index in i)
    return positions, normals, indices


def hair_strands():
    positions, texcoords, radii, indices = [], [], [], []
    center = (0.0, 1.40, 0.0)
    head_radii = (0.74, 0.93, 0.74)
    strand_points = 11
    for band in range(9):
        theta = 0.12 + 1.02 * band / 8.0
        for azimuth_index in range(36):
            phi = 2.0 * math.pi * azimuth_index / 36.0
            st, ct = math.sin(theta), math.cos(theta)
            cp, sp = math.cos(phi), math.sin(phi)
            radial = (st * cp, ct, st * sp)
            root = (
                center[0] + head_radii[0] * radial[0] * 1.015,
                center[1] + head_radii[1] * radial[1] * 1.015,
                center[2] + head_radii[2] * radial[2] * 1.015,
            )
            front = max(sp, 0.0)
            side = abs(cp)
            length = 1.18 + 0.38 * (1.0 - front) + 0.18 * side
            if front > 0.45:
                length = 0.48 + 0.30 * theta
            first = len(positions)
            phase = phi * 2.0 + theta * 3.0
            for point in range(strand_points):
                t = point / (strand_points - 1)
                curl = math.sin(phase + t * math.pi * 2.2) * 0.035 * t
                flare = 0.12 * t * t
                positions.append((
                    root[0] + radial[0] * flare + curl * sp,
                    root[1] - length * t + 0.08 * math.sin(math.pi * t),
                    root[2] + radial[2] * flare - curl * cp,
                ))
                texcoords.append((t, azimuth_index / 35.0))
                radii.append(0.010 * (1.0 - t) + 0.0025 * t)
                if point:
                    indices.extend((first + point - 1, first + point))
    return positions, texcoords, radii, indices


def bounds(values):
    return ([min(v[i] for v in values) for i in range(len(values[0]))],
            [max(v[i] for v in values) for i in range(len(values[0]))])


class BufferBuilder:
    def __init__(self):
        self.data = bytearray()
        self.views = []
        self.accessors = []

    def accessor(self, values, fmt, component_type, accessor_type, target, include_bounds=False):
        while len(self.data) % 4:
            self.data.append(0)
        offset = len(self.data)
        flat = [component for value in values for component in (value if isinstance(value, tuple) else (value,))]
        self.data.extend(struct.pack("<" + fmt * len(flat), *flat))
        view = len(self.views)
        self.views.append({"buffer": 0, "byteOffset": offset, "byteLength": len(self.data) - offset, "target": target})
        accessor = {"bufferView": view, "componentType": component_type, "count": len(values), "type": accessor_type}
        if include_bounds:
            accessor["min"], accessor["max"] = bounds(values)
        result = len(self.accessors)
        self.accessors.append(accessor)
        return result


def primitive(builder, positions, normals, indices, material):
    return {
        "attributes": {
            "POSITION": builder.accessor(positions, "f", 5126, "VEC3", 34962, True),
            "NORMAL": builder.accessor(normals, "f", 5126, "VEC3", 34962),
        },
        "indices": builder.accessor(indices, "I", 5125, "SCALAR", 34963),
        "material": material,
        "mode": 4,
    }


def build_model(asset_root: Path = ASSETS):
    model_dir = asset_root / "Models" / "rtxcr-character-validation"
    model_path = model_dir / "rtxcr-character-validation.gltf"
    skin_model_path = model_dir / "rtxcr-character-validation-skin-only.gltf"
    builder = BufferBuilder()
    skin = merge_ellipsoids([
        ((0.0, 1.40, 0.0), (0.74, 0.93, 0.74), 28, 44),
        ((-0.73, 1.38, 0.0), (0.15, 0.28, 0.11), 14, 20),
        ((0.73, 1.38, 0.0), (0.15, 0.28, 0.11), 14, 20),
        ((0.0, 0.34, 0.0), (0.36, 0.68, 0.34), 18, 28),
        ((0.0, -0.40, -0.03), (1.12, 0.58, 0.52), 20, 36),
        ((0.0, 1.34, 0.69), (0.13, 0.20, 0.20), 12, 18),
    ])
    eyes = merge_ellipsoids([
        ((-0.25, 1.54, 0.68), (0.15, 0.105, 0.075), 12, 20),
        ((0.25, 1.54, 0.68), (0.15, 0.105, 0.075), 12, 20),
    ])
    pupils = merge_ellipsoids([
        ((-0.25, 1.54, 0.748), (0.055, 0.055, 0.022), 10, 16),
        ((0.25, 1.54, 0.748), (0.055, 0.055, 0.022), 10, 16),
        ((0.0, 1.05, 0.724), (0.18, 0.055, 0.026), 8, 18),
    ])
    hair_p, hair_uv, hair_r, hair_i = hair_strands()
    floor_p = [(-3.0, -0.96, -2.0), (3.0, -0.96, -2.0), (3.0, -0.96, 3.0), (-3.0, -0.96, 3.0)]
    floor_n = [(0.0, 1.0, 0.0)] * 4

    surface_primitives = [
        primitive(builder, *skin, 0),
        primitive(builder, *eyes, 2),
        primitive(builder, *pupils, 3),
        primitive(builder, floor_p, floor_n, [0, 2, 1, 0, 3, 2], 4),
    ]
    hair_primitive = {
            "attributes": {
                "POSITION": builder.accessor(hair_p, "f", 5126, "VEC3", 34962, True),
                "TEXCOORD_0": builder.accessor(hair_uv, "f", 5126, "VEC2", 34962),
                "_RADIUS": builder.accessor(hair_r, "f", 5126, "SCALAR", 34962),
            },
            "indices": builder.accessor(hair_i, "I", 5125, "SCALAR", 34963),
            "material": 1,
            "mode": 1,
        }
    encoded = base64.b64encode(builder.data).decode("ascii")
    model = {
        "asset": {"version": "2.0", "generator": "Caustica synthetic RTXCR validation asset generator"},
        "extensionsUsed": ["NV_materials_subsurface", "NV_materials_hair"],
        "scene": 0,
        "scenes": [{"nodes": [0, 1]}],
        "nodes": [
            {"mesh": 0, "name": "SyntheticCharacterSurface"},
            {"mesh": 1, "name": "SyntheticCharacterHair"},
        ],
        "meshes": [
            {"name": "SyntheticCharacterSurface", "primitives": surface_primitives},
            {"name": "SyntheticCharacterHair", "primitives": [hair_primitive]},
        ],
        "materials": [
            {
                "name": "ValidationSkin",
                "doubleSided": False,
                "pbrMetallicRoughness": {"baseColorFactor": [0.68, 0.25, 0.16, 1.0], "metallicFactor": 0.0, "roughnessFactor": 0.38},
                "extensions": {"NV_materials_subsurface": {"transmissionColor": [1.0, 0.50, 0.30], "scatteringColor": [1.0, 0.48, 0.22], "scale": 0.55, "anisotropy": 0.15}},
            },
            {
                "name": "ValidationHair",
                "doubleSided": True,
                "pbrMetallicRoughness": {"baseColorFactor": [0.035, 0.012, 0.006, 1.0], "metallicFactor": 0.0, "roughnessFactor": 0.42},
                "extensions": {"NV_materials_hair": {"model": "FarField", "baseColor": [0.10, 0.028, 0.010], "melanin": 0.72, "melaninRedness": 0.22, "longitudinalRoughness": 0.32, "azimuthalRoughness": 0.58, "ior": 1.55, "cuticleAngle": 3.0, "diffuseReflectionWeight": 0.04, "diffuseReflectionTint": [0.025, 0.008, 0.004]}},
            },
            {"name": "ValidationEyeWhite", "pbrMetallicRoughness": {"baseColorFactor": [0.85, 0.88, 0.82, 1.0], "roughnessFactor": 0.22}},
            {"name": "ValidationFeatures", "pbrMetallicRoughness": {"baseColorFactor": [0.025, 0.012, 0.010, 1.0], "roughnessFactor": 0.32}},
            {"name": "ValidationFloor", "pbrMetallicRoughness": {"baseColorFactor": [0.12, 0.14, 0.18, 1.0], "roughnessFactor": 0.62}},
        ],
        "buffers": [{"byteLength": len(builder.data), "uri": "data:application/octet-stream;base64," + encoded}],
        "bufferViews": builder.views,
        "accessors": builder.accessors,
    }
    model_dir.mkdir(parents=True, exist_ok=True)
    model_path.write_text(json.dumps(model, indent=2) + "\n", encoding="utf-8")
    skin_model = dict(model)
    skin_model["scenes"] = [{"nodes": [0]}]
    skin_model["nodes"] = [model["nodes"][0]]
    skin_model["meshes"] = [model["meshes"][0]]
    skin_model_path.write_text(json.dumps(skin_model, indent=2) + "\n", encoding="utf-8")
    return model_path, skin_model_path


def build_scene(asset_root: Path = ASSETS):
    scene_path = asset_root / "rtxcr-character-validation.scene.json"
    scene = {
        "models": ["Models/rtxcr-character-validation/rtxcr-character-validation.gltf"],
        "graph": [
            {"name": "RTXCRCharacterValidation", "model": 0},
            {"name": "Lights", "children": [
                {"name": "Key", "type": "PointLight", "translation": [-2.3, 3.2, 3.2], "color": [1.0, 0.82, 0.68], "intensity": 65.0, "radius": 0.35, "range": 12.0},
                {"name": "Fill", "type": "PointLight", "translation": [2.0, 1.7, 2.6], "color": [0.55, 0.72, 1.0], "intensity": 32.0, "radius": 0.45, "range": 10.0},
                {"name": "Rim", "type": "PointLight", "translation": [0.0, 2.8, -2.0], "color": [1.0, 0.35, 0.18], "intensity": 46.0, "radius": 0.25, "range": 9.0},
                {"name": "Sky", "type": "EnvironmentLight", "radianceScale": [0.18, 0.20, 0.25], "textureIndex": [0], "rotation": [0], "path": "==PROCEDURAL_SKY=="},
            ]},
            {"name": "Cameras", "children": [{"name": "Default", "type": "PerspectiveCameraEx", "translation": [0.0, 1.30, 5.2], "rotation": [0.0, 0.0, 0.0, 1.0], "verticalFov": 0.48, "zNear": 0.01, "enableAutoExposure": False, "exposureCompensation": 0.0}]},
            {"name": "SceneSettings", "type": "SceneSettings", "realtimeMode": True, "enableAnimations": False, "startingCamera": -1},
        ],
    }
    scene_path.write_text(json.dumps(scene, indent=2) + "\n", encoding="utf-8")
    return scene_path


def generate(asset_root: Path = ASSETS):
    model_path, skin_model_path = build_model(asset_root)
    scene_path = build_scene(asset_root)
    return model_path, skin_model_path, scene_path


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-assets", type=Path, default=ASSETS)
    args = parser.parse_args()
    model_path, skin_model_path, scene_path = generate(args.output_assets.resolve())
    print(model_path)
    print(skin_model_path)
    print(scene_path)
