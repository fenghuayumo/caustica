from __future__ import annotations

import sys
from pathlib import Path


SUPPORT_PYTHON = Path(__file__).resolve().parents[1]
if str(SUPPORT_PYTHON) not in sys.path:
    sys.path.insert(0, str(SUPPORT_PYTHON))

from shader_cook_cache import DependencyManifest


def test_dependency_manifest_round_trip_preserves_prior_bins(tmp_path: Path) -> None:
    path = tmp_path / "deps.manifest"
    original = DependencyManifest()
    original.add_closure("Root.hlsl", {"Root.hlsl", "Shared.hlsli"})
    original.add_bin("old-hash", "Root.hlsl", "old-fingerprint")
    original.write(path)

    updated = DependencyManifest.load(path)
    updated.add_bin("new-hash", "Root.hlsl", "new-fingerprint")
    updated.write(path)

    reloaded = DependencyManifest.load(path)
    assert reloaded.closures["Root.hlsl"] == {"Root.hlsl", "Shared.hlsli"}
    assert reloaded.bins == {
        "old-hash": ("Root.hlsl", "old-fingerprint"),
        "new-hash": ("Root.hlsl", "new-fingerprint"),
    }
