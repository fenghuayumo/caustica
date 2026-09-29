from __future__ import annotations

import json
import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import pack_scene


class PackSceneFormatTests(unittest.TestCase):
    def _make_asset_pack(self, root: Path) -> Path:
        """Minimal asset pack: scene -> gltf(+bin, texture) + material."""
        scenes = root / "scenes" / "demo"
        models = root / "models" / "demo"
        materials = root / "materials"
        env = root / "env"
        for directory in (scenes, models, materials, env):
            directory.mkdir(parents=True)

        (models / "demo.bin").write_bytes(bytes(range(256)) * 40)
        (models / "demo.png").write_bytes(b"\x89PNG fake texture bytes")
        (env / "sky.exr").write_bytes(b"fake exr")

        gltf = {
            "asset": {"version": "2.0"},
            "buffers": [{"uri": "demo.bin", "byteLength": 40}],
            "images": [{"uri": "demo.png"}],
        }
        (models / "demo.gltf").write_text(json.dumps(gltf), encoding="utf-8")

        material = {
            "version": 1,
            "MaterialModel": "OpenPBR",
            "BaseTexture": {"path": "models/demo/demo.png", "sRGB": True},
        }
        (materials / "demo.Surface.material.json").write_text(
            json.dumps(material), encoding="utf-8"
        )

        scene = {
            "format": "caustica.scene",
            "version": 2,
            "name": "demo",
            "entities": [
                {
                    "id": "Demo",
                    "components": {
                        "PrefabInstance": {
                            "source": "models/demo/demo.gltf",
                            "materials": {
                                "Surface": "materials/demo.Surface.material.json",
                            },
                        },
                    },
                },
                {
                    "id": "Sky",
                    "components": {
                        "EnvironmentLight": {"source": "env/sky.exr"},
                    },
                },
            ],
        }
        scene_path = scenes / "demo.scene.json"
        scene_path.write_text(json.dumps(scene), encoding="utf-8")
        (root / "pack.json").write_text("{}", encoding="utf-8")
        return scene_path

    def test_pack_inspect_extract_roundtrip(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            scene_path = self._make_asset_pack(root)

            exit_code = pack_scene.main(
                [
                    "pack",
                    str(scene_path),
                    "-o",
                    str(root / "demo.caustica"),
                    "--store",
                ]
            )
            self.assertEqual(exit_code, 0)

            pack_path = root / "demo.caustica"
            data = pack_path.read_bytes()
            magic, version, flags, index_offset, index_size, data_offset, index_crc = (
                struct.unpack_from("<8sIIQQQI", data, 0)
            )
            self.assertEqual(magic, b"CAUSTICP")
            self.assertEqual(version, 1)
            self.assertEqual(data_offset, pack_scene.HEADER_SIZE)
            self.assertEqual(len(data), index_offset + index_size)
            directory_bytes = data[index_offset : index_offset + index_size]
            self.assertEqual(zlib.crc32(directory_bytes) & 0xFFFFFFFF, index_crc)

            pack = pack_scene._read_pack(pack_path)
            self.assertEqual(
                pack_scene.cmd_inspect(
                    type("Args", (), {"pack": str(pack_path)})()
                ),
                0,
            )

            out_dir = root / "extracted"
            pack_scene.cmd_extract(
                type("Args", (), {"pack": str(pack_path), "output": str(out_dir)})()
            )
            for relative in (
                "scenes/demo/demo.scene.json",
                "models/demo/demo.gltf",
                "models/demo/demo.bin",
                "models/demo/demo.png",
                "materials/demo.Surface.material.json",
                "env/sky.exr",
                "manifest.json",
            ):
                self.assertTrue((out_dir / relative).is_file(), relative)
            self.assertEqual(
                (out_dir / "models/demo/demo.bin").read_bytes(),
                bytes(range(256)) * 40,
            )

    def test_lz4_roundtrip_when_available(self) -> None:
        if pack_scene._lz4block is None:
            self.skipTest("python-lz4 not installed")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            scene_path = self._make_asset_pack(root)
            pack_path = root / "demo.caustica"
            self.assertEqual(
                pack_scene.main(
                    ["pack", str(scene_path), "-o", str(pack_path), "--lz4"]
                ),
                0,
            )
            pack = pack_scene._read_pack(pack_path)
            compressed = [
                entry for entry in pack["entries"] if entry["compression"] == pack_scene.COMPRESSION_LZ4
            ]
            self.assertTrue(compressed, "expected at least one lz4 payload")
            for entry in pack["entries"]:
                payload = pack_scene._entry_bytes(pack, entry)
                self.assertEqual(
                    zlib.crc32(payload) & 0xFFFFFFFF,
                    entry["crc32"],
                    entry["path"],
                )


if __name__ == "__main__":
    unittest.main()
