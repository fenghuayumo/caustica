from __future__ import annotations

import copy
import json
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from scene_fit import FitRunner, Project
from scene_fit.metrics import evaluate
from scene_fit.project import read_json, tree_hash, write_json


def fixture(root):
    (root / "assets").mkdir()
    write_json(root / "assets" / "blue.material.json", {"OpenPBR": {"base_color": [0.2, 0.3, 0.4]}})
    Image.new("RGB", (4, 4), (128, 128, 128)).save(root / "reference.png")
    Image.new("L", (4, 4), 255).save(root / "mask.png")
    data = {"schema_version": 1, "coordinates": "caustica_y_up_meters", "comparison_space": "display_rgb",
            "asset_root": "assets", "assets": {"blue": "blue.material.json"}, "foreground_entities": ["cube"],
            "scene": {"entities": [{"id": "cube", "components": {"PrefabInstance": {"source": "builtin:cube"},
                       "Transform": {"scale": [1, 1, 1]}, "MaterialOverride": {"source": "asset:blue"}}}]},
            "render": {"width": 4, "height": 4, "spp": 2, "bounces": 2, "timeout_seconds": 10, "exposure_compensation": 1},
            "parameters": [{"name": "size", "group": "geometry", "unit": "scale", "initial": 0.25, "bounds": [0.1, 0.9], "step": 0.25,
                            "binding": {"target": "entity", "id": "cube", "path": "/components/Transform/scale/0"}},
                           {"name": "red", "group": "material", "unit": "linear_rgb", "initial": 0.2, "bounds": [0, 1], "step": 0.1,
                            "binding": {"target": "material", "id": "blue", "path": "/OpenPBR/base_color/0"}}],
            "loss_weights": {"rgb": 1, "silhouette": 1},
            "views": [{"id": name, "split": split, "image": "reference.png", "mask": "mask.png",
                       "camera": {"position": [0, 1, 3], "direction": [0, 0, -1], "up": [0, 1, 0],
                                  "intrinsics": {"fx": 4, "fy": 4, "cx": 2, "cy": 2}}}
                      for name, split in [("front", "train"), ("side", "train"), ("heldout", "validation")]]}
    write_json(root / "project.json", data)
    return data


class AnalyticRenderer:
    """Known objective independent of the optimizer: gray level equals object size."""
    def __init__(self, parameter="size"):
        self.fail = False
        self.captures = []
        self.parameter = parameter

    def capture(self, project, candidate, views, check_cancel):
        check_cancel()
        if self.fail:
            raise RuntimeError("Injected load failure")
        value = read_json(candidate / "parameters.json")[self.parameter]
        self.captures.append([view["split"] for view in views])
        return {view["id"]: {"rgb": np.full((4, 4, 3), value), "mask": np.ones((4, 4), dtype=bool),
                             "depth": np.ones((4, 4)), "provenance": {"candidate": candidate.name}}
                for view in views}

    def close(self):
        pass


class SceneFitTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.data = fixture(self.root)

    def tearDown(self):
        self.temporary.cleanup()

    def project(self):
        write_json(self.root / "project.json", self.data)
        return Project(self.root / "project.json")

    def test_invalid_calibration_and_bounds_rejected(self):
        self.data["views"][0]["camera"]["direction"] = [0, 1, 0]
        with self.assertRaises(ValueError):
            self.project()
        self.data["views"][0]["camera"]["direction"] = [0, 0, -1]
        self.data["parameters"][0]["initial"] = float("nan")
        with self.assertRaises(ValueError):
            self.project()

    def test_asset_escape_and_unknown_alias_rejected(self):
        self.data["assets"]["blue"] = "../reference.png"
        with self.assertRaises(ValueError):
            self.project()
        self.data["assets"]["blue"] = "blue.material.json"
        self.data["scene"]["entities"][0]["components"]["MaterialOverride"]["source"] = "asset:missing"
        with self.assertRaises(ValueError):
            self.project()

    def test_private_snapshot_and_restore_cover_material_contents(self):
        project = self.project()
        original = tree_hash(project.asset_root)
        renderer = AnalyticRenderer()
        runner = FitRunner(project, self.root / "run", renderer)
        result = runner.evaluate({"size": 0.5, "red": 0.8})
        self.assertEqual(result["status"], "completed")
        candidate = runner.candidates / result["candidate_id"]
        self.assertEqual(read_json(candidate / "assets/blue.material.json")["OpenPBR"]["base_color"][0], 0.8)
        self.assertEqual(tree_hash(project.asset_root), original)
        # Changing original inputs cannot alter a frozen run or its exports.
        write_json(project.asset_root / "blue.material.json", {"OpenPBR": {"base_color": [1, 1, 1]}})
        export = runner.restore()
        restored = Path(export["scene"])
        self.assertEqual(read_json(restored.parent / "assets/blue.material.json")["OpenPBR"]["base_color"], [0.8, 0.3, 0.4])
        self.assertIn(restored.parent.as_posix(), read_json(restored)["entities"][0]["components"]["MaterialOverride"]["source"])

    def test_failed_candidate_does_not_replace_best(self):
        renderer = AnalyticRenderer()
        runner = FitRunner(self.project(), self.root / "run", renderer)
        best = runner.evaluate()
        renderer.fail = True
        failed = runner.evaluate({"size": 0.5})
        self.assertEqual(failed["status"], "failed")
        self.assertEqual(runner.best["candidate_id"], best["candidate_id"])
        self.assertTrue((runner.candidates / failed["candidate_id"] / "result.json").is_file())

    def test_validation_never_selects_best_and_optimizer_improves(self):
        renderer = AnalyticRenderer()
        runner = FitRunner(self.project(), self.root / "run", renderer)
        baseline = runner.evaluate()
        validation = runner.evaluate({"size": 128 / 255}, split="validation")
        self.assertEqual(validation["loss"], 0)
        self.assertEqual(runner.best["candidate_id"], baseline["candidate_id"])
        result = runner.optimize(max_evaluations=9, groups=["geometry"])
        self.assertLess(result["best"]["loss"], baseline["loss"] / 20)
        self.assertLessEqual(result["evaluations_this_call"], 9)
        self.assertTrue(all(split == "train" for batch in renderer.captures[2:] for split in batch))

    def test_default_fit_keeps_geometry_fixed(self):
        runner = FitRunner(self.project(), self.root / "run", AnalyticRenderer(parameter="red"))
        try:
            result = runner.optimize(max_evaluations=9)
            self.assertEqual(result["groups"], ["material", "lighting"])
            baseline = read_json(runner.candidates / "candidate-000000/result.json")
            self.assertLess(result["best"]["loss"], baseline["loss"])
            for path in runner.candidates.glob("*/parameters.json"):
                self.assertEqual(read_json(path)["size"], self.data["parameters"][0]["initial"])
        finally:
            runner.close()

    def test_appearance_groups_cannot_disguise_mesh_edits(self):
        for group in ("material", "lighting"):
            self.data["parameters"][0]["group"] = group
            with self.assertRaises(ValueError):
                self.project()

    def test_cancel_and_resume_preserve_best(self):
        runner = FitRunner(self.project(), self.root / "run", AnalyticRenderer())
        runner.evaluate()
        (runner.run_dir / "CANCEL").touch()
        result = runner.optimize(max_evaluations=3)
        self.assertIn("CANCEL", result["reason"])
        with self.assertRaises(RuntimeError):
            FitRunner(None, runner.run_dir, AnalyticRenderer(), resume=True)
        runner.close()
        resumed = FitRunner(None, runner.run_dir, AnalyticRenderer(), resume=True)
        self.assertEqual(resumed.best["candidate_id"], runner.best["candidate_id"])
        resumed.close()
        write_json(runner.run_dir / "input/assets/blue.material.json", {})
        with self.assertRaises(ValueError):
            FitRunner(None, runner.run_dir, AnalyticRenderer(), resume=True)

    def test_external_gltf_dependency_rejected(self):
        write_json(self.root / "assets/model.gltf", {"buffers": [{"uri": "../../outside.bin"}]})
        with self.assertRaises(ValueError):
            self.project()

    def test_output_inside_assets_rejected_without_recursive_copy(self):
        project = self.project()
        with self.assertRaises(ValueError):
            FitRunner(project, project.asset_root / "run", AnalyticRenderer())
        self.assertFalse((project.asset_root / "run").exists())
        with self.assertRaises(ValueError):
            project.materialize(project.asset_root / "candidate", project.initial)

    def test_texture_snapshot_relocated_and_corruption_rejected(self):
        Image.new("RGB", (2, 2), (255, 0, 0)).save(self.root / "assets/color.png")
        material = read_json(self.root / "assets/blue.material.json")
        material["BaseTexture"] = "color.png"
        write_json(self.root / "assets/blue.material.json", material)
        runner = FitRunner(self.project(), self.root / "run", AnalyticRenderer())
        result = runner.evaluate()
        candidate = runner.candidates / result["candidate_id"]
        self.assertEqual(read_json(candidate / "assets/blue.material.json")["BaseTexture"], (candidate / "assets/color.png").as_posix())
        (candidate / "assets/color.png").write_bytes(b"changed")
        with self.assertRaises(ValueError):
            runner.restore()

    def test_loss_union_penalizes_extra_foreground_and_missing_depth(self):
        rgb = np.zeros((2, 2, 3))
        mask = np.array([[True, False], [False, False]])
        rendered_mask = np.ones((2, 2), dtype=bool)
        changed = rgb.copy()
        changed[~mask] = 1
        result = evaluate(rgb, changed, mask, rendered_mask, {"rgb": 1, "silhouette": 1, "depth": 0})
        self.assertAlmostEqual(result["iou"], 0.25)
        self.assertAlmostEqual(result["rgb_mae"], 0.75)
        missing = evaluate(rgb, rgb, mask, mask, {"rgb": 1, "depth": 10})
        self.assertIsNone(missing["depth_mae_m"])
        self.assertEqual(missing["loss"], 0)
        with self.assertRaises(ValueError):
            evaluate(rgb, rgb, mask, mask, {"depth": 1}, np.ones((2, 2)), np.zeros((2, 2)))


if __name__ == "__main__":
    unittest.main()
