from __future__ import annotations

import sys
from pathlib import Path


SUPPORT_PYTHON = Path(__file__).resolve().parents[1]
if str(SUPPORT_PYTHON) not in sys.path:
    sys.path.insert(0, str(SUPPORT_PYTHON))

from build_shader_bin import candidate_bin_names


def test_non_main_entry_prefers_entry_suffixed_blob() -> None:
    candidates = candidate_bin_names(
        "caustica/shaders/Libraries/ShaderDebug/ShaderDebug.hlsl",
        "main_vs",
    )

    assert candidates[0].endswith("ShaderDebug_main_vs.bin")
    assert candidates[-1].endswith("ShaderDebug.bin")


def test_main_entry_uses_generic_blob() -> None:
    assert candidate_bin_names("shaders/example.hlsl", "main") == [
        "shaders/example.bin"
    ]
