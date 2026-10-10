#!/usr/bin/env python3
"""Run reproducible multiview evaluation and bounded scene fitting."""
from __future__ import annotations

import argparse
import json
import sys
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image

from scene_fit import FitRunner, Project
from scene_fit.project import read_json, write_json
from scene_fit.renderer import CausticaRenderer


def create_demo(directory, renderer, *, perturb_geometry=False):
    directory = Path(directory).resolve()
    directory.mkdir(parents=True, exist_ok=False)
    (directory / "assets").mkdir()
    write_json(directory / "assets" / "object.material.json", {
        "MaterialModel": "OpenPBR", "OpenPBR": {"base_color": [0.08, 0.3, 0.65], "specular_roughness": 0.35}})
    data = {
        "schema_version": 1, "coordinates": "caustica_y_up_meters", "comparison_space": "display_rgb",
        "asset_root": "assets", "assets": {"object_material": "object.material.json"},
        "foreground_entities": ["object"],
        "render": {"width": 96, "height": 72, "spp": 16, "bounces": 4, "timeout_seconds": 120, "exposure_compensation": 1.0},
        "scene": {"format": "caustica.scene", "version": 2, "entities": [
            {"id": "object", "name": "object", "components": {
                "PrefabInstance": {"source": "builtin:cube"}, "MaterialOverride": {"source": "asset:object_material"},
                "Transform": {"translation": [0, 0, 0], "scale": [1, 1, 1]}}},
            {"id": "ground", "components": {"PrefabInstance": {"source": "builtin:plane"}, "Transform": {"scale": [8, 1, 8]}}},
            {"id": "key", "components": {"PointLight": {"color": [1, 0.95, 0.9], "intensity": 90, "radius": 0.2}, "Transform": {"translation": [-2, 3, 3]}}},
            {"id": "sky", "components": {"EnvironmentLight": {"source": "procedural:sky:midday"}}}]},
        "parameters": [
            {"name": "width", "group": "geometry", "unit": "scale", "initial": 1.0, "bounds": [0.4, 1.5], "step": 0.2,
             "binding": {"target": "entity", "id": "object", "path": "/components/Transform/scale/0"}},
            {"name": "red", "group": "material", "unit": "linear_rgb", "initial": 0.08, "bounds": [0.02, 0.5], "step": 0.08,
             "binding": {"target": "material", "id": "object_material", "path": "/OpenPBR/base_color/0"}},
            {"name": "intensity", "group": "lighting", "unit": "caustica_intensity", "initial": 90, "bounds": [20, 150], "step": 20,
             "binding": {"target": "entity", "id": "key", "path": "/components/PointLight/intensity"}}],
        "loss_weights": {"rgb": 1.0, "silhouette": 1.0, "depth": 0.0}, "views": []}
    for view_id, position, split in [("front", [0, 1.2, 4], "train"), ("side", [3, 1.4, 3], "train"), ("heldout", [-3, 1.6, 3], "validation")]:
        direction = [-position[0], 0.5 - position[1], -position[2]]
        data["views"].append({"id": view_id, "split": split, "image": f"references/{view_id}.png", "mask": f"references/{view_id}-mask.png",
                              "camera": {"position": position, "direction": direction, "up": [0, 1, 0],
                                         "intrinsics": {"fx": 95, "fy": 95, "cx": 48, "cy": 36}}})
    # Bootstrap placeholders exist only in a temporary project. Never publish them as references.
    with tempfile.TemporaryDirectory(prefix="caustica-reference-") as temporary:
        bootstrap = Path(temporary)
        import shutil
        shutil.copytree(directory / "assets", bootstrap / "assets")
        (bootstrap / "references").mkdir()
        for view in data["views"]:
            Image.new("RGB", (96, 72)).save(bootstrap / view["image"])
            Image.new("L", (96, 72), 255).save(bootstrap / view["mask"])
        write_json(bootstrap / "project.json", data)
        project = Project(bootstrap / "project.json")
        candidate = directory / "ground-truth"
        project.materialize(candidate, project.initial)
        outputs = renderer.capture(project, candidate, data["views"], lambda: None)
        (directory / "references").mkdir()
        for view in data["views"]:
            output = outputs[view["id"]]
            if not output["mask"].any():
                raise RuntimeError(f"Generated reference foreground is empty: {view['id']}")
            Image.fromarray(np.rint(output["rgb"] * 255).astype(np.uint8)).save(directory / view["image"])
            Image.fromarray(output["mask"].astype(np.uint8) * 255).save(directory / view["mask"])
        write_json(directory / "ground-truth.json", {"parameters": project.initial,
                                                      "provenance": {key: value["provenance"] for key, value in outputs.items()}})
    if perturb_geometry:
        data["parameters"][0]["initial"] = 0.6
    data["parameters"][1]["initial"] = 0.32
    data["parameters"][2]["initial"] = 50
    write_json(directory / "project.json", data)
    Project(directory / "project.json")
    return {"status": "completed", "project": str(directory / "project.json")}


def capabilities():
    return {"protocol_version": 1, "operations": ["validate", "evaluate", "optimize", "restore"],
            "parameter_groups": ["geometry", "material", "lighting"], "binding_targets": ["entity", "material"],
            "default_optimization_groups": ["material", "lighting"],
            "comparison_space": "display_rgb", "coordinates": "caustica_y_up_meters",
            "validation_used_for_selection": False, "transport": "JSON request/response files",
            "limitations": ["No model SDK or MCP server", "No Blender live synchronization", "No free-topology optimization", "No transparent silhouette/depth guarantees"]}


def execute_request(request, renderer):
    operation = request["operation"]
    if operation == "capabilities":
        return capabilities()
    if operation == "validate":
        project = Project(request["project"])
        return {"status": "completed", "parameters": project.parameters, "views": project.data["views"]}
    if operation not in ("evaluate", "optimize", "restore"):
        raise ValueError(f"Unknown operation: {operation}")
    resume = request.get("resume", False)
    project = None if resume else Project(request["project"])
    runner = FitRunner(project, request["run_dir"], renderer, resume=resume)
    try:
        if operation == "evaluate":
            result = runner.evaluate(request.get("parameters"), split=request.get("split", "train"))
        elif operation == "optimize":
            options = request.get("options", {})
            result = runner.optimize(**options)
            if runner.best is not None and not (runner.run_dir / "CANCEL").exists():
                result["export"] = runner.restore()
                if any(view["split"] == "validation" for view in runner.project.data["views"]):
                    result["validation"] = runner.evaluate(runner.best["parameters"], split="validation")
        else:
            result = runner.restore(request.get("candidate_id"))
        result["report"] = runner.report()
        return result
    finally:
        runner.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vulkan", action="store_true")
    parser.add_argument("--adapter", default="auto")
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("capabilities")
    demo = commands.add_parser("demo")
    demo.add_argument("--output-dir", required=True)
    demo.add_argument("--perturb-geometry", action="store_true", help="Optional geometry-search benchmark; normal demo keeps reference geometry fixed.")
    validate = commands.add_parser("validate")
    validate.add_argument("project")
    for name in ("evaluate", "optimize", "restore"):
        command = commands.add_parser(name)
        command.add_argument("project", nargs="?")
        command.add_argument("--run-dir", required=True)
        command.add_argument("--resume", action="store_true")
        if name == "evaluate":
            command.add_argument("--parameters", help="JSON object file with parameter overrides")
            command.add_argument("--split", choices=("train", "validation"), default="train")
        if name == "optimize":
            command.add_argument("--max-evaluations", type=int, default=30)
            command.add_argument("--max-seconds", type=float)
            command.add_argument("--groups", nargs="+", choices=("geometry", "material", "lighting"), help="Default: material lighting. Geometry search requires explicit selection.")
        if name == "restore":
            command.add_argument("--candidate-id")
    tool = commands.add_parser("execute")
    tool.add_argument("request", help="JSON tool request")
    tool.add_argument("--response", required=True, help="Authoritative response file; native logs may appear on stdout")
    args = parser.parse_args()
    renderer = CausticaRenderer(vulkan=args.vulkan, adapter=args.adapter)
    exit_code = 0
    try:
        if args.command == "demo":
            result = create_demo(args.output_dir, renderer, perturb_geometry=args.perturb_geometry)
        elif args.command == "execute":
            request = read_json(args.request)
            result = execute_request(request, renderer)
            result["request_id"] = request.get("request_id")
        else:
            request = {"operation": args.command}
            for key in ("project", "run_dir", "resume", "split", "candidate_id"):
                if hasattr(args, key):
                    request[key] = getattr(args, key)
            if args.command == "evaluate" and args.parameters:
                request["parameters"] = read_json(args.parameters)
            if args.command == "optimize":
                request["options"] = {key: getattr(args, key) for key in ("max_evaluations", "max_seconds", "groups") if getattr(args, key) is not None}
            result = execute_request(request, renderer)
        if result.get("status") in ("failed", "cancelled"):
            exit_code = 1
    except (Exception, KeyboardInterrupt) as error:
        result = {"status": "failed", "error": {"type": type(error).__name__, "message": str(error)}}
        if args.command == "execute" and "request" in locals():
            result["request_id"] = request.get("request_id")
        exit_code = 1
    finally:
        renderer.close()
    if args.command == "execute":
        write_json(args.response, result)
    print(json.dumps(result, indent=2, ensure_ascii=False, allow_nan=False))
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
