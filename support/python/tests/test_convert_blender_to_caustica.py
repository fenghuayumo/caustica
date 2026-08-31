from __future__ import annotations

import json
import math
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import convert_blender_to_caustica as converter


class BlenderConverterTests(unittest.TestCase):
    def test_material_names_do_not_change_physical_properties(self) -> None:
        gltf = {"textures": [], "images": []}
        glass = converter.material_doc(
            gltf,
            Path("."),
            {"name": "decorative_glass_name", "pbrMetallicRoughness": {"roughnessFactor": 0.7}},
        )
        lamp = converter.material_doc(
            gltf,
            Path("."),
            {"name": "lamp", "emissiveFactor": [0.1, 0.2, 0.3]},
        )
        self.assertFalse(glass["EnableTransmission"])
        self.assertEqual(glass["Roughness"], 0.7)
        self.assertEqual(lamp["EmissiveIntensity"], 1.0)
        self.assertEqual(lamp["EmissiveColor"], [0.1, 0.2, 0.3])

    def test_camera_pose_uses_active_camera_without_translation(self) -> None:
        sidecar = {
            "active_camera": "Active",
            "cameras": [
                {"name": "Other", "gltf_yup": {"translation": [9, 9, 9]}},
                {
                    "name": "Active",
                    "vertical_fov": 0.8,
                    "clip_start": 1.0,
                    "gltf_yup": {
                        "translation": [1, 2, 3],
                        "rotation": [0, 0, 0, 1],
                    },
                },
            ],
        }
        translation, rotation, vfov, znear = converter.camera_pose({}, sidecar)
        self.assertEqual(translation, [1, 2, 3])
        self.assertEqual(rotation, [0, 0, 0, 1])
        self.assertEqual(vfov, 0.8)
        self.assertEqual(znear, 1.0)

    def test_camera_moves_only_past_measured_clip_obstruction(self) -> None:
        sidecar = {
            "active_camera": "Camera",
            "cameras": [
                {
                    "name": "Camera",
                    "vertical_fov": 0.8,
                    "clip_start": 1.1,
                    "clip_obstruction_distance": 0.53,
                    "gltf_yup": {
                        "translation": [0, 0, 0],
                        "rotation": [0, 0, 0, 1],
                    },
                }
            ],
        }
        translation, _rotation, _vfov, znear = converter.camera_pose({}, sidecar)
        self.assertAlmostEqual(translation[2], -0.55)
        self.assertEqual(znear, 0.05)

    def test_sidecar_lights_include_area_approximation(self) -> None:
        sidecar = {
            "lights": [
                {
                    "name": "Window Area",
                    "type": "AREA",
                    "energy": 2.0,
                    "size": 4.0,
                    "color": [1, 0.8, 0.6],
                    "gltf_yup": {"translation": [1, 2, 3], "rotation": [0, 0, 0, 1]},
                },
                {"name": "Fill", "type": "POINT", "energy": 1.0},
            ]
        }
        entities = converter.sidecar_light_entities(sidecar)
        self.assertEqual(len(entities), 2)
        area = entities[0]["components"]["SpotLight"]
        self.assertAlmostEqual(area["intensity"], 2.0 / (4.0 * math.pi))
        self.assertEqual(area["radius"], 2.0)
        self.assertIn("PointLight", entities[1]["components"])

    def test_nishita_sky_becomes_directional_sun(self) -> None:
        entity = converter.sky_texture_sun_entity(
            {"sky_texture": {"sun_elevation": 0.4, "sun_rotation": 2.0, "sun_intensity": 1.5}}
        )
        self.assertIsNotNone(entity)
        self.assertEqual(entity["id"], "WorldSun")
        self.assertAlmostEqual(
            entity["components"]["DirectionalLight"]["irradiance"], 150.0
        )

    def test_scene_uses_native_gltf_materials_by_default(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            old_assets = converter.ASSETS
            converter.ASSETS = root
            try:
                model_dir = root / "Models" / "sample"
                model_dir.mkdir(parents=True)
                gltf_path = model_dir / "sample.gltf"
                gltf_path.write_text(
                    json.dumps({"materials": [{"name": "Glass"}]}), encoding="utf-8"
                )
                sidecar_path = model_dir / "sample.export.json"
                sidecar_path.write_text(
                    json.dumps(
                        {
                            "active_camera": "Camera",
                            "cameras": [
                                {
                                    "name": "Camera",
                                    "vertical_fov": 0.7,
                                    "clip_start": 0.1,
                                    "gltf_yup": {},
                                }
                            ],
                            "lights": [],
                        }
                    ),
                    encoding="utf-8",
                )
                scene_path = converter.write_scene_pack(
                    "sample", gltf_path, sidecar_path, converter.DEFAULT_ENV
                )
                scene = json.loads(scene_path.read_text(encoding="utf-8"))
                prefab = scene["entities"][0]["components"]["PrefabInstance"]
                self.assertEqual(prefab, {"source": "Models/sample/sample.gltf"})
                self.assertFalse((root / "Materials" / "sample.Glass.material.json").exists())
            finally:
                converter.ASSETS = old_assets


if __name__ == "__main__":
    unittest.main()
