from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from migrate_materials_to_openpbr import build_openpbr_block, convert_material, migrate_pack


class OpenPBRMaterialMigrationTests(unittest.TestCase):
    def test_metal_rough_gets_openpbr_block_and_white_specular(self) -> None:
        data = {
            "BaseOrDiffuseColor": [0.8, 0.8, 0.8],
            "SpecularColor": [0.0, 0.0, 0.0],
            "Metalness": 0.0,
            "Roughness": 0.4,
            "IoR": 1.45,
            "TransmissionFactor": 1.0,
            "ThinSurface": False,
            "UseSpecularGlossModel": False,
        }
        self.assertEqual(convert_material(data), "metal-rough")
        self.assertEqual(data["MaterialModel"], "OpenPBR")
        self.assertEqual(data["SpecularColor"], [1.0, 1.0, 1.0])
        self.assertEqual(data["OpenPBR"]["specular_color"], [1.0, 1.0, 1.0])
        self.assertEqual(data["OpenPBR"]["specular_roughness"], 0.4)
        self.assertEqual(data["OpenPBR"]["base_diffuse_roughness"], 0.4)
        self.assertEqual(data["OpenPBR"]["specular_ior"], 1.45)
        self.assertEqual(data["OpenPBR"]["transmission_weight"], 1.0)
        self.assertIsNone(convert_material(data))

    def test_spec_gloss_with_spec_texture_is_left_alone(self) -> None:
        data = {
            "UseSpecularGlossModel": True,
            "OcclusionRoughnessMetallicTexture": {
                "path": "models/Bistro/foo_spec.dds",
                "sRGB": True,
            },
            "SpecularColor": [1.0, 1.0, 1.0],
            "Roughness": 0.2,
        }
        self.assertIsNone(convert_material(data))
        self.assertTrue(data["UseSpecularGlossModel"])
        self.assertNotIn("OpenPBR", data)

    def test_previous_script_migration_repairs_diffuse_roughness(self) -> None:
        legacy = {
            "BaseOrDiffuseColor": [0.8, 0.8, 0.8],
            "SpecularColor": [1.0, 1.0, 1.0],
            "Metalness": 0.0,
            "Roughness": 0.4,
            "UseSpecularGlossModel": False,
        }
        data = dict(legacy)
        data["MaterialModel"] = "OpenPBR"
        data["OpenPBR"] = build_openpbr_block(legacy, [1.0, 1.0, 1.0])
        data["OpenPBR"]["base_diffuse_roughness"] = 0.0

        self.assertEqual(convert_material(data), "repair-legacy-diffuse-roughness")
        self.assertEqual(data["OpenPBR"]["base_diffuse_roughness"], 0.4)
        self.assertIsNone(convert_material(data))

    def test_explicit_openpbr_diffuse_roughness_is_not_rewritten(self) -> None:
        data = {
            "MaterialModel": "OpenPBR",
            "Roughness": 0.4,
            "OpenPBR": {
                "base_diffuse_roughness": 0.0,
                "base_color": [0.8, 0.8, 0.8],
            },
        }
        self.assertIsNone(convert_material(data))
        self.assertEqual(data["OpenPBR"]["base_diffuse_roughness"], 0.0)

    def test_spec_gloss_without_texture_is_left_alone(self) -> None:
        data = {
            "UseSpecularGlossModel": True,
            "BaseOrDiffuseColor": [0.2, 0.3, 0.4],
            "SpecularColor": [0.04, 0.04, 0.04],
            "Roughness": 0.3,
        }
        self.assertIsNone(convert_material(data))
        self.assertTrue(data["UseSpecularGlossModel"])
        self.assertNotIn("OpenPBR", data)

    def test_migrate_pack_skips_textured_spec_gloss(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            pack = Path(temporary_directory)
            materials = pack / "materials"
            materials.mkdir()
            metal = materials / "kitchen.Glass.material.json"
            spec = materials / "bistro.Wood.material.json"
            metal.write_text(
                json.dumps({"UseSpecularGlossModel": False, "SpecularColor": [0, 0, 0]}),
                encoding="utf-8",
            )
            spec.write_text(
                json.dumps(
                    {
                        "UseSpecularGlossModel": True,
                        "OcclusionRoughnessMetallicTexture": {"path": "a_spec.dds", "sRGB": True},
                    }
                ),
                encoding="utf-8",
            )
            converted, skipped = migrate_pack(pack)
            self.assertEqual(converted, 1)
            self.assertEqual(skipped, 1)
            updated = json.loads(metal.read_text(encoding="utf-8"))
            self.assertEqual(updated["MaterialModel"], "OpenPBR")
            unchanged = json.loads(spec.read_text(encoding="utf-8"))
            self.assertTrue(unchanged["UseSpecularGlossModel"])
            self.assertEqual(migrate_pack(pack), (0, 1))


if __name__ == "__main__":
    unittest.main()
