from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from migrate_material_refs import migrate_pack


def write_scene(path: Path, source: str, materials: dict[str, str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(
            {
                "entities": [
                    {
                        "components": {
                            "PrefabInstance": {
                                "source": source,
                                "materials": materials,
                            }
                        }
                    }
                ]
            }
        ),
        encoding="utf-8",
    )


class MaterialReferenceMigrationTests(unittest.TestCase):
    def test_scene_specialized_material_does_not_leak_into_other_scenes(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            pack = Path(temporary_directory)
            materials = pack / "materials"
            specialized = materials / "bistro.scene"
            specialized.mkdir(parents=True)
            (materials / "Dragon.Glass.material.json").write_text("{}", encoding="utf-8")
            (specialized / "Dragon.Glass.material.json").write_text("{}", encoding="utf-8")

            regular_scene = pack / "scenes" / "regular" / "regular.scene.json"
            bistro_scene = pack / "scenes" / "bistro" / "bistro.scene.json"
            leaked = "materials/bistro.scene/Dragon.Glass.material.json"
            for scene in (regular_scene, bistro_scene):
                write_scene(scene, "models/Dragon.gltf", {"Glass": leaked})

            self.assertEqual(migrate_pack(pack), 1)
            regular = json.loads(regular_scene.read_text(encoding="utf-8"))
            regular_material = regular["entities"][0]["components"]["PrefabInstance"][
                "materials"
            ]["Glass"]
            self.assertEqual(regular_material, "materials/Dragon.Glass.material.json")

            bistro = json.loads(bistro_scene.read_text(encoding="utf-8"))
            bistro_material = bistro["entities"][0]["components"]["PrefabInstance"][
                "materials"
            ]["Glass"]
            self.assertEqual(
                bistro_material, "materials/bistro.scene/Dragon.Glass.material.json"
            )
            self.assertEqual(migrate_pack(pack), 0)

    def test_matching_scene_keeps_specialized_override(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            pack = Path(temporary_directory)
            materials = pack / "materials"
            specialized = materials / "bistro-programmer-art.scene"
            specialized.mkdir(parents=True)
            (materials / "window.window_glass.material.json").write_text(
                "{}", encoding="utf-8"
            )
            (specialized / "window.window_glass.material.json").write_text(
                "{}", encoding="utf-8"
            )
            scene = (
                pack
                / "scenes"
                / "bistro"
                / "bistro-programmer-art.scene.json"
            )
            write_scene(
                scene,
                "models/window/window.gltf",
                {
                    "window_glass": "materials/window.window_glass.material.json",
                    "window_wood_painted": "materials/window.window_wood_painted.material.json",
                },
            )
            (materials / "window.window_wood_painted.material.json").write_text(
                "{}", encoding="utf-8"
            )

            self.assertEqual(migrate_pack(pack), 1)
            doc = json.loads(scene.read_text(encoding="utf-8"))
            slots = doc["entities"][0]["components"]["PrefabInstance"]["materials"]
            self.assertEqual(
                slots["window_glass"],
                "materials/bistro-programmer-art.scene/window.window_glass.material.json",
            )
            self.assertEqual(
                slots["window_wood_painted"],
                "materials/window.window_wood_painted.material.json",
            )

    def test_dangling_specialized_path_falls_back_to_shared(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            pack = Path(temporary_directory)
            materials = pack / "materials"
            materials.mkdir()
            (materials / "window.window_glass.material.json").write_text(
                "{}", encoding="utf-8"
            )
            scene = pack / "scenes" / "kitchen" / "kitchen.scene.json"
            write_scene(
                scene,
                "models/window/window.gltf",
                {
                    "window_glass": "materials/bistro-programmer-art.scene/window.window_glass.material.json"
                },
            )

            self.assertEqual(migrate_pack(pack), 1)
            doc = json.loads(scene.read_text(encoding="utf-8"))
            slots = doc["entities"][0]["components"]["PrefabInstance"]["materials"]
            self.assertEqual(
                slots["window_glass"], "materials/window.window_glass.material.json"
            )

    def test_generic_scene_gltf_does_not_mix_unrelated_materials(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            pack = Path(temporary_directory)
            materials = pack / "materials"
            materials.mkdir()
            (materials / "scene.POTHOS_Mat.material.json").write_text(
                "{}", encoding="utf-8"
            )
            (materials / "scene.Material.016.material.json").write_text(
                "{}", encoding="utf-8"
            )

            pothos = pack / "models" / "plant_free_pothos_potted" / "scene.gltf"
            ivy = pack / "models" / "plant_pot_ivy" / "scene.gltf"
            pothos.parent.mkdir(parents=True)
            ivy.parent.mkdir(parents=True)
            pothos.write_text(
                json.dumps({"materials": [{"name": "POTHOS_Mat"}]}),
                encoding="utf-8",
            )
            ivy.write_text(
                json.dumps({"materials": [{"name": "Material.016"}]}),
                encoding="utf-8",
            )

            kitchen = pack / "scenes" / "kitchen" / "kitchen.scene.json"
            kitchen.parent.mkdir(parents=True)
            kitchen.write_text(
                json.dumps(
                    {
                        "entities": [
                            {
                                "components": {
                                    "PrefabInstance": {
                                        "source": "models/plant_free_pothos_potted/scene.gltf",
                                        "materials": {
                                            "POTHOS_Mat": "materials/scene.POTHOS_Mat.material.json",
                                            "Material.016": "materials/scene.Material.016.material.json",
                                        },
                                    }
                                }
                            },
                            {
                                "components": {
                                    "PrefabInstance": {
                                        "source": "models/plant_pot_ivy/scene.gltf",
                                        "materials": {
                                            "POTHOS_Mat": "materials/scene.POTHOS_Mat.material.json",
                                            "Material.016": "materials/scene.Material.016.material.json",
                                        },
                                    }
                                }
                            },
                        ]
                    }
                ),
                encoding="utf-8",
            )

            self.assertEqual(migrate_pack(pack), 1)
            doc = json.loads(kitchen.read_text(encoding="utf-8"))
            pothos_slots = doc["entities"][0]["components"]["PrefabInstance"][
                "materials"
            ]
            ivy_slots = doc["entities"][1]["components"]["PrefabInstance"]["materials"]
            self.assertEqual(
                pothos_slots, {"POTHOS_Mat": "materials/scene.POTHOS_Mat.material.json"}
            )
            self.assertEqual(
                ivy_slots,
                {"Material.016": "materials/scene.Material.016.material.json"},
            )
            self.assertEqual(migrate_pack(pack), 0)


if __name__ == "__main__":
    unittest.main()
