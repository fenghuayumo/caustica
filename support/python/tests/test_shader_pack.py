from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

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


if __name__ == "__main__":
    unittest.main()
