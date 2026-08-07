#!/usr/bin/env python3
"""Fail if a sample includes engine headers outside the frozen public API allowlist.

Usage:
  python tools/check_public_api_includes.py [source.cpp ...]

Defaults to application/samples/thin_client/**/*.cpp when no paths are given.
See docs/public-api.md.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]

ALLOWLIST = {
    "EngineApp.h",
    "EntryPoint.h",
    "EntityWorld.h",
    "Plugin.h",
    "SystemSets.h",
    "SystemLabel.h",
    "SystemLabels.h",
    "AppSchedules.h",
    "App.h",
    "SceneSpawn.h",
    "SceneTransform.h",
    "SceneQuery.h",
    "ActiveScene.h",
    "MeshDeformApi.h",
    "CameraApi.h",
    "RenderSessionApi.h",
    "SceneLifecycle.h",
    "EnqueueRenderCommand.h",
    "EngineSceneCallbacks.h",
    "SceneViewState.h",
}

ENGINE_INCLUDE_RE = re.compile(
    r'^\s*#\s*include\s*[<"]engine/([^>"]+)[>"]',
    re.MULTILINE,
)


def check_file(path: Path) -> list[str]:
    text = path.read_text(encoding="utf-8", errors="replace")
    violations: list[str] = []
    for match in ENGINE_INCLUDE_RE.finditer(text):
        rel = match.group(1).replace("\\", "/")
        if rel.startswith("internal/"):
            violations.append(f"{path}: engine/{rel} (internal)")
            continue
        name = Path(rel).name
        if name not in ALLOWLIST:
            violations.append(f"{path}: engine/{rel} (not in public API allowlist)")
    return violations


def default_sources() -> list[Path]:
    root = REPO_ROOT / "application" / "samples" / "thin_client"
    return sorted(root.rglob("*.cpp"))


def main(argv: list[str]) -> int:
    paths = [Path(a) for a in argv[1:]] if len(argv) > 1 else default_sources()
    if not paths:
        print("check_public_api_includes: no sources found", file=sys.stderr)
        return 1

    violations: list[str] = []
    for path in paths:
        if not path.is_file():
            print(f"check_public_api_includes: missing {path}", file=sys.stderr)
            return 1
        violations.extend(check_file(path))

    if violations:
        print("Public API include allowlist violations:", file=sys.stderr)
        for item in violations:
            print(f"  {item}", file=sys.stderr)
        print("See docs/public-api.md", file=sys.stderr)
        return 1

    print(f"check_public_api_includes: ok ({len(paths)} file(s))")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
