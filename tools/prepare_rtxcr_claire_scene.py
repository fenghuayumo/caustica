#!/usr/bin/env python3
"""Create a Caustica scene shim for a locally installed RTXCR Claire asset set.

The generated scene contains absolute model paths and is intentionally ignored
by Git. It does not copy or redistribute any NVIDIA character assets.
"""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_ASSETS = Path(r"D:\ProgramCode\C++\RTXCR\assets")
DEFAULT_OUTPUT = ROOT / "Assets" / "rtxcr-claire.external.scene.json"
DEFAULT_ENVIRONMENT = Path("EnvironmentMaps/venice_sunset_1k.exr")


def walk_nodes(nodes: list[object]):
    for node in nodes:
        if not isinstance(node, dict):
            continue
        yield node
        children = node.get("children")
        if isinstance(children, list):
            yield from walk_nodes(children)


def load_json_with_line_comments(path: Path) -> dict[str, object]:
    text = path.read_text(encoding="utf-8")
    # The official scene contains only whole-line // comments. Avoid a JSON5
    # dependency while leaving strings (and therefore paths) untouched.
    text = re.sub(r"(?m)^\s*//.*$", "", text)
    # Some disabled properties leave the preceding live property with a
    # trailing comma. JsonCpp accepts that scene syntax; Python's strict JSON
    # parser does not, so normalize those separators after removing comments.
    text = re.sub(r",\s*([}\]])", r"\1", text)
    return json.loads(text)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--rtxcr-assets", type=Path, default=DEFAULT_ASSETS)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--animated", action="store_true")
    parser.add_argument("--environment-map", type=Path, default=DEFAULT_ENVIRONMENT)
    parser.add_argument("--environment-intensity", type=float, default=0.02)
    parser.add_argument("--directional-light-scale", type=float, default=2.0)
    parser.add_argument("--exposure-compensation", type=float, default=2.5)
    args = parser.parse_args()

    assets = args.rtxcr_assets.resolve()
    source_name = "claire.animation.scene.json" if args.animated else "claire.scene.json"
    source = assets / source_name
    if not source.is_file():
        raise SystemExit(
            f"RTXCR scene not found: {source}\n"
            "Install the official NVIDIA-RTX/RTXCR-Assets repository first."
        )

    scene = load_json_with_line_comments(source)
    models = scene.get("models")
    if not isinstance(models, list) or not all(isinstance(item, str) for item in models):
        raise SystemExit(f"Unexpected RTXCR scene model list: {source}")

    resolved_models = [(assets / item).resolve() for item in models]
    missing = [path for path in resolved_models if not path.is_file()]
    if missing:
        preview = "\n".join(f"  {path}" for path in missing[:12])
        raise SystemExit(
            "RTXCR assets are incomplete (run `git lfs pull` in the asset repository):\n"
            + preview
        )

    scene["models"] = [path.as_posix() for path in resolved_models]
    graph = scene.setdefault("graph", [])
    if not isinstance(graph, list):
        raise SystemExit(f"Unexpected RTXCR scene graph: {source}")

    environment = args.environment_map
    if not environment.is_absolute():
        environment = assets / environment
    environment = environment.resolve()
    if not environment.is_file():
        raise SystemExit(f"RTXCR environment map not found: {environment}")

    # RTXCR's sample adds this environment outside claire.scene.json. Mirror
    # that application-level default so the adapter reproduces the demo rather
    # than rendering the character against black.
    if not any(node.get("type") == "EnvironmentLight" for node in walk_nodes(graph)):
        graph.append(
            {
                "name": "RTXCR Venice Sunset",
                "type": "EnvironmentLight",
                "radianceScale": [args.environment_intensity] * 3,
                "path": environment.as_posix(),
            }
        )

    # The RTXCR renderer uses an application-level exposure adjustment while
    # the authored camera requests auto exposure. Use a stable Caustica camera
    # exposure so the black pixels outside Claire do not bias auto exposure.
    for node in walk_nodes(graph):
        if node.get("type") in ("PerspectiveCamera", "PerspectiveCameraEx"):
            node["enableAutoExposure"] = False
            node["exposureCompensation"] = args.exposure_compensation
        elif node.get("type") == "DirectionalLight" and isinstance(node.get("irradiance"), (int, float)):
            node["irradiance"] *= args.directional_light_scale

    if not any(
        isinstance(node, dict) and node.get("type") in ("SceneSettings", "SampleSettings")
        for node in graph
    ):
        graph.append(
            {
                "name": "SceneSettings",
                "type": "SceneSettings",
                "realtimeMode": True,
                "enableAnimations": args.animated,
                "startingCamera": -1,
            }
        )

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(scene, indent=2) + "\n", encoding="utf-8")
    print(args.output.resolve())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
