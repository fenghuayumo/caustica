#!/usr/bin/env python3
"""Document authoring -> save/reload -> live inspection -> render verification.

No model provider or API key is required. An Agent can submit the same execute
requests using its own tool-calling transport. Outputs go only to --output-dir.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path


def completed(result):
    if result["status"] != "completed":
        raise RuntimeError(json.dumps(result, ensure_ascii=False))
    return result


def edit(document, operations, request_id):
    return completed(document.execute({
        "request_id": request_id,
        "expected_session_id": document.session_id,
        "expected_revision": document.revision,
        "operations": operations,
    }))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--document-only", action="store_true", help="Validate authoring and persistence without a GPU")
    parser.add_argument("--vulkan", action="store_true")
    args = parser.parse_args()
    import caustica

    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    # Material assets are separate from scene-document history. Use private,
    # absolute paths so publishing does not depend on scene-directory fallback.
    material = output / "agent-blue.material.json"
    material.write_text(json.dumps({
        "MaterialModel": "OpenPBR",
        "OpenPBR": {"base_color": [0.04, 0.18, 0.65], "base_metalness": 0.15, "specular_roughness": 0.25},
    }, indent=2), encoding="utf-8")
    document = caustica.SceneDocument()
    edit(document, [
        {"op": "add_entity", "entity": {"id": "ground", "components": {
            "PrefabInstance": {"source": "builtin:plane"}, "Transform": {"scale": [8, 1, 8]},
        }}},
        {"op": "add_entity", "entity": {"id": "cube", "components": {
            "PrefabInstance": {"source": "builtin:cube"},
            "MaterialOverride": {"source": material.as_posix()},
            "Transform": {"translation": [0, 0, 0]},
        }}},
        {"op": "add_entity", "entity": {"id": "key", "components": {
            "PointLight": {"color": [1, 0.92, 0.82], "intensity": 90, "radius": 0.2},
            "Transform": {"translation": [-2, 3, 3]},
        }}},
        {"op": "add_entity", "entity": {"id": "sky", "components": {
            "EnvironmentLight": {"source": "procedural:sky:midday"},
        }}},
        {"op": "add_entity", "entity": {"id": "camera", "components": {
            "Transform": {"translation": [0, 1.2, 5], "rotation": [-0.06994285, 0, 0, 0.997551]},
            "PerspectiveCameraEx": {"verticalFov": 0.7, "zNear": 0.01, "enableAutoExposure": False},
        }}},
        {"op": "patch_settings", "patch": {
            "realtimeMode": True, "enableAnimations": False, "enableKeyframes": False, "startingCameraId": "camera",
        }},
    ], "build-scene")
    initial = document.document
    edit(document, [{"op": "patch_entity", "id": "cube", "patch": {
        "components": {"Transform": {"translation": [1, 0, 0]}},
    }}], "move-cube")
    edit(document, [{"op": "undo"}], "undo-move")
    assert document.document == initial
    edit(document, [{"op": "redo"}], "redo-move")
    assert document.get_entity("cube")["components"]["Transform"]["translation"] == [1, 0, 0]
    edit(document, [{"op": "undo"}], "restore-composition")
    # A failed batch must leave the earlier patch in that same batch unapplied.
    revision = document.revision
    failed = document.execute({"expected_revision": revision, "operations": [
        {"op": "patch_entity", "id": "cube", "patch": {"name": "must-not-persist"}},
        {"op": "patch_entity", "id": "missing", "patch": {"name": "missing"}},
    ]})
    assert failed["status"] == "failed" and document.document == initial and document.revision == revision
    scene_file = output / "agent.scene.json"
    completed(document.save(str(scene_file)))
    reloaded = caustica.SceneDocument.load(str(scene_file))
    assert reloaded.document == document.document
    manifest = {"document_session_id": document.session_id, "document_revision": document.revision,
                "scene_path": str(scene_file), "document_checks": "passed"}
    if not args.document_only:
        with caustica.EngineApp.create(width=320, height=240, headless=True, realtime=True,
                                       vulkan=args.vulkan, scene=str(scene_file)) as engine:
            assert engine.wait_until_ready(timeout_seconds=120), "Scene did not become ready"
            snapshot = completed(engine.inspect_scene(limit=1000))
            authored = {item["authored_id"]: item for item in snapshot["entities"]
                        if item["authored_id"] and item["authored_scope_runtime_id"] is None}
            assert {"ground", "cube", "key", "sky", "camera"} <= set(authored)
            assert authored["cube"]["bounds"] is not None, "Cube bounds were not propagated"
            assert abs(authored["cube"]["bounds"]["min"][1] - authored["ground"]["bounds"]["max"][1]) < 1e-4, \
                "Cube is not resting on the ground"
            by_runtime_id = {item["runtime_id"]: item for item in snapshot["entities"]}
            pending = [authored["cube"]["runtime_id"]]
            cube_slots = []
            while pending:
                item = by_runtime_id[pending.pop()]
                cube_slots.extend(item["material_slots"])
                pending.extend(item["children_runtime_ids"])
            assert cube_slots, "Cube has no material slots"
            assert any(slot["source"] for slot in cube_slots), "Material override not applied"
            assert engine.warmup(32) is not False
            image_file = output / "preview.png"
            assert engine.save_screenshot(str(image_file)), "Screenshot failed"
            pixels = engine.get_pixels()
            import numpy as np
            assert np.asarray(pixels)[..., :3].max() > 0, "Render is black"
            manifest.update({"scene_generation": snapshot["scene_generation"], "frame_index": engine.frame_index,
                             "preview": str(image_file), "render_checks": "passed"})
            (output / "live-scene.json").write_text(json.dumps(snapshot, indent=2), encoding="utf-8")
    (output / "verification.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
