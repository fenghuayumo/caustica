#!/usr/bin/env python3
"""Opt-in GPU acceptance: known multiview target -> search -> independent export reload."""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from scene_fit import FitRunner, Project
from scene_fit.project import read_json, write_json
from scene_fit.renderer import CausticaRenderer
from scene_fit_cli import create_demo


def verify(output_dir, vulkan=False, include_geometry=False):
    root = Path(output_dir).resolve()
    root.mkdir(parents=True, exist_ok=False)
    renderer = CausticaRenderer(vulkan=vulkan)
    try:
        demo = create_demo(root / "demo", renderer, perturb_geometry=include_geometry)
    finally:
        renderer.close()
    project = Project(demo["project"])
    runner = FitRunner(project, root / "run", CausticaRenderer(vulkan=vulkan))
    try:
        groups = ["geometry", "material", "lighting"] if include_geometry else None
        state = runner.optimize(max_evaluations=19, groups=groups)
        baseline = read_json(runner.candidates / "candidate-000000/result.json")
        best = state["best"]
        assert best is not None, "No successful candidate"
        assert best["loss"] < baseline["loss"] * 0.2, "Training objective did not improve sufficiently"
        validation = runner.evaluate(best["parameters"], split="validation")
        assert validation["status"] == "completed", validation
        for metrics in list(best["metrics"].values()) + list(validation["metrics"].values()):
            assert metrics["iou"] >= 0.95, metrics
        expected = read_json(root / "demo/ground-truth.json")["parameters"]
        assert abs(best["parameters"]["width"] - expected["width"]) <= 0.02, "Camera or appearance compensated for wrong geometry"
        if not include_geometry:
            for path in runner.candidates.glob("*/parameters.json"):
                assert read_json(path)["width"] == expected["width"], "Appearance search modified fixed geometry"
        export = runner.restore()
        report = runner.report()
    finally:
        runner.close()
    # New engine instance; ensure exported scene/material paths remain usable.
    renderer = CausticaRenderer(vulkan=vulkan)
    try:
        outputs = renderer.capture(runner.project, Path(export["scene"]).parent, runner.project.data["views"], lambda: None)
    finally:
        renderer.close()
    errors = {}
    for view in runner.project.data["views"]:
        source = best["candidate_id"] if view["split"] == "train" else validation["candidate_id"]
        with Image.open(runner.candidates / source / "renders" / view["id"] / "rgb.png") as image:
            previous = np.asarray(image.convert("RGB"), dtype=float) / 255
        errors[view["id"]] = float(np.abs(previous - outputs[view["id"]]["rgb"]).mean())
        assert errors[view["id"]] < 0.002, "Export reload exceeded RGB noise tolerance"
    summary = {"status": "completed", "baseline_train_loss": baseline["loss"], "best_train_loss": best["loss"],
               "validation_loss": validation["loss"], "best_parameters": best["parameters"],
               "groups": state["groups"], "geometry_fixed": not include_geometry,
               "export_reload_rgb_mae": errors, "report": report, "synthetic_case_only": True}
    write_json(root / "verification.json", summary)
    return summary


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--vulkan", action="store_true")
    parser.add_argument("--include-geometry", action="store_true", help="Opt into the older joint geometry/appearance benchmark")
    args = parser.parse_args()
    print(json.dumps(verify(args.output_dir, args.vulkan, args.include_geometry), indent=2))
