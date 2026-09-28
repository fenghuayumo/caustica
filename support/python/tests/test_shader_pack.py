from __future__ import annotations

import hashlib
import os
import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import build_wheel
import precompile_pt_shader_bins
from build_wheel import encode_payload, pack_key


class ShaderPackCodecTests(unittest.TestCase):
    def test_encode_payload_roundtrips_past_first_word(self) -> None:
        # The stream advances every 8 bytes. A mismatch in xorshift state update
        # (classic xorshift64* vs writing the multiplied value back) would still
        # recover the first 8 bytes and corrupt the rest.
        payload = b"CAUSSMF1" + bytes(range(64))
        key = pack_key("shaderbin/manifest.bin")
        encoded = encode_payload(payload, key)
        self.assertEqual(encode_payload(encoded, key), payload)
        self.assertNotEqual(encoded[:8], payload[:8])
        self.assertNotEqual(encoded[8:16], payload[8:16])

    def test_stale_pack_is_rebuilt_and_fresh_pack_is_reused(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            shader_root = root / "ShaderBin" / "dxil"
            shader_root.mkdir(parents=True)
            current = b"current static shader"
            current_hash = hashlib.sha256(current).hexdigest()
            current_path = shader_root / current_hash[:2] / f"{current_hash[2:]}.bin"
            current_path.parent.mkdir()
            current_path.write_bytes(current)
            (shader_root / "duplicate.bin").write_bytes(current)
            (shader_root / "manifest.bin").write_bytes(
                b"CAUSSMF1" + struct.pack("<I", 1) + bytes(32) + bytes.fromhex(current_hash)
            )
            stale = b"old static shader"
            stale_hash = hashlib.sha256(stale).hexdigest()
            stale_path = shader_root / stale_hash[:2] / f"{stale_hash[2:]}.bin"
            stale_path.parent.mkdir(exist_ok=True)
            stale_path.write_bytes(stale)
            pack = root / "caustica.shaders.dxil.pack"
            with patch.object(build_wheel, "BIN_DIR", root):
                self.assertEqual(build_wheel.ensure_shader_pack("dxil", root), pack)
                self.assertTrue(pack.is_file())
                content = pack.read_bytes()
                self.assertEqual(struct.unpack_from("<I", content, 8)[0], 3)
                key = pack_key("ShaderBin/manifest.bin")
                entries = struct.unpack_from("<I", content, 12)[0]
                locations = {}
                for index in range(entries):
                    hash0, hash1, content0, content1, offset, size = struct.unpack_from(
                        "<QQQQQQ", content, 16 + index * 48)
                    locations[(hash0, hash1)] = (offset, size)
                    if (hash0, hash1) == key:
                        decoded = encode_payload(content[offset:offset + size], (content0, content1))
                        original_size = struct.unpack_from("<Q", decoded, 0)[0]
                        self.assertEqual(zlib.decompress(decoded[8:]), (shader_root / "manifest.bin").read_bytes())
                        self.assertEqual(original_size, len((shader_root / "manifest.bin").read_bytes()))
                        break
                else:
                    self.fail("manifest entry missing from shader pack")
                self.assertEqual(locations[pack_key("ShaderBin/duplicate.bin")],
                                 locations[pack_key(f"ShaderBin/{current_hash[:2]}/{current_hash[2:]}.bin")])
                self.assertFalse(stale_path.exists())
                self.assertTrue(current_path.exists())
                original_time = pack.stat().st_mtime_ns
                self.assertEqual(build_wheel.ensure_shader_pack("dxil", root), pack)
                self.assertEqual(pack.stat().st_mtime_ns, original_time)

                shader = shader_root / "new.bin"
                shader.write_bytes(b"new shader")
                os.utime(shader, ns=(original_time + 2_000_000_000, original_time + 2_000_000_000))
                self.assertEqual(build_wheel.ensure_shader_pack("dxil", root), pack)
                self.assertEqual(struct.unpack_from("<I", pack.read_bytes(), 12)[0], 4)

    def test_prune_pt_keeps_current_coverage_and_removes_old_bins(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            shader_root = root / "ShaderBin" / "dxil"
            current_hash, stale_hash = "a" * 64, "b" * 64
            for digest in (current_hash, stale_hash):
                path = shader_root / digest[:2] / f"{digest[2:]}.bin"
                path.parent.mkdir(parents=True)
                path.write_bytes(digest.encode())
            manifest = shader_root / "deps.manifest"
            manifest.write_text(
                f"CAUSDEP1\nB {current_hash} source fingerprint\nB {stale_hash} source fingerprint\n",
                encoding="utf-8",
            )
            with (patch.object(build_wheel, "BIN_DIR", root),
                  patch.object(precompile_pt_shader_bins, "build_jobs", return_value=[{"logical": "source", "macros": []}]),
                  patch.object(precompile_pt_shader_bins, "build_hash_command", return_value="current"),
                  patch.object(precompile_pt_shader_bins, "hash_hex", return_value=current_hash)):
                self.assertEqual(build_wheel.prune_stale_pt_shaders("dxil"), (1, 64))
            self.assertTrue((shader_root / "aa" / f"{'a' * 62}.bin").exists())
            self.assertFalse((shader_root / "bb" / f"{'b' * 62}.bin").exists())
            self.assertNotIn(stale_hash, manifest.read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
