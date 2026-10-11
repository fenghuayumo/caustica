import copy
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from blender_fit.colmap import load
from blender_fit.common import validate_geometry, write
from blender_fit.provider import Provider
from blender_fit.workflow import prepare, foreground_points, validate_project, segment, quality
from blender_fit.refinement import refine, propose, dimensions, moves


def fixture(root, binary=False):
    model = root / "sparse"
    model.mkdir()
    images = root / "images"
    images.mkdir()
    for i in range(3):
        image = Image.new("RGBA", (16, 12), (100, 110, 120, 0))
        for x in range(5, 11):
            for y in range(3, 9):
                image.putpixel((x,y), (100,110,120,255))
        image.save(images / f"image {i}.png")
    if binary:
        with (model / "cameras.bin").open("wb") as f:
            f.write(struct.pack("<QiiQQdddd", 1, 7, 1, 16, 12, 16, 17, 8, 6))
        with (model / "images.bin").open("wb") as f:
            f.write(struct.pack("<Q", 3))
            for i in range(3):
                f.write(struct.pack("<idddddddi", i+1, 1,0,0,0, 0,0,4,7))
                f.write(f"image {i}.png".encode()+b"\0"+struct.pack("<Q", 0))
        with (model / "points3D.bin").open("wb") as f:
            f.write(struct.pack("<Q", 2))
            for i, point in enumerate([(0,0,0), (5,0,0)]):
                f.write(struct.pack("<QdddBBBdQ", i+1, *point, 128,128,128,0,0))
    else:
        (model / "cameras.txt").write_text("7 PINHOLE 16 12 16 17 8 6\n")
        (model / "images.txt").write_text("# poses and empty observations\n"+"".join(
            f"{i+1} 1 0 0 0 0 0 4 7 image {i}.png\n\n" for i in range(3)))
        (model / "points3D.txt").write_text("1 0 0 0 128 128 128 0\n2 5 0 0 128 128 128 0\n")
    return model, images


class BlenderFitTests(unittest.TestCase):
    def test_foreground_cloud_rejects_distant_mask_consistent_outliers(self):
        with tempfile.TemporaryDirectory() as t:
            root = Path(t)
            sparse, images = fixture(root)
            path = prepare(sparse, images, root / "project")
            data = validate_project(path)
            cloud = np.array([[x,y,z] for x in [-0.1,0,0.1] for y in [-0.1,0,0.1]
                              for z in [-0.1,0,0.1]] + [[0,0,1000]], dtype=float)
            info = {}
            selected = foreground_points(cloud, data["views"], path.parent, diagnostics=info)
            self.assertEqual(len(selected), 27)
            self.assertEqual(info["mask_intersection_points"], 28)
            self.assertEqual(info["outliers_removed"], 1)
            self.assertEqual(len(foreground_points(cloud, data["views"], path.parent, outlier_iqr=None)), 28)

    def test_one_failed_view_cannot_pass_average_quality_gate(self):
        training = {"views": {f"v{i}": 0.0 for i in range(20)}}
        training["views"]["bad"] = 0.4
        result = quality(training, {"views": {"held_out": 0.01}})
        self.assertEqual(result["status"], "needs_geometry_review")
        self.assertEqual(result["failing_views"], [{"split":"train", "view":"bad", "loss":0.4}])

    def test_numeric_moves_interleave_parts_within_each_axis(self):
        p = {"name":"a", "kind":"box", "position":[0,0,0], "rotation":[0,0,0], "size":[1,1,1]}
        g = {"parts":[p, dict(p, name="b")]}
        first = list(moves(g, ("size",)))[:4]
        self.assertEqual([m[0] for m in first], [0,0,1,1])
        self.assertEqual([m[2] for m in first], [0,0,0,0])

    def test_planar_cloud_outlier_and_invalid_filter_configuration(self):
        with tempfile.TemporaryDirectory() as t:
            root = Path(t)
            sparse, images = fixture(root)
            path = prepare(sparse, images, root / "project")
            data = validate_project(path)
            cloud = np.array([[x,y,0] for x in np.linspace(-0.1,0.1,5)
                              for y in np.linspace(-0.1,0.1,5)] + [[0,0,1000]])
            self.assertEqual(len(foreground_points(cloud, data["views"], path.parent)), 25)
            with self.assertRaises(ValueError):
                foreground_points(cloud[:1], data["views"], path.parent, outlier_iqr=-1)

    def test_multiscale_search_recovers_pose_with_explicit_budget(self):
        geometry = {"parts":[{"name":"object", "kind":"box", "position":[0.1,0,0],
                              "rotation":[0,0,10], "size":[1,1,1]}]}
        best = copy.deepcopy(geometry)
        calls = []
        def loss(g):
            p = g["parts"][0]
            return sum(x*x for x in p["position"]) + sum((x/100)**2 for x in p["rotation"])
        def evaluate(candidate, label):
            nonlocal best
            calls.append(label)
            if loss(candidate) < loss(best):
                best = candidate
        used = refine(lambda: best, evaluate, 25, fields=("position", "rotation"))
        self.assertEqual(used, 25)
        self.assertEqual(len(calls), 25)
        self.assertAlmostEqual(loss(best), 0)
        self.assertTrue(any("level-1" in label for label in calls))

    def test_lathe_position_step_uses_extent_and_shell_stays_valid(self):
        g = {"parts":[{"name":"shell", "kind":"lathe", "position":[0,0,0], "rotation":[0,0,0],
                        "size":[1,1,1], "profile":[[0,0.01],[0.04,0.012]], "thickness":0.009}]}
        self.assertEqual(dimensions(g["parts"][0]), [0.024,0.024,0.04])
        c = propose(g, (0,"position",0,1), 0)
        self.assertAlmostEqual(c["parts"][0]["position"][0], 0.004)
        self.assertIsNone(propose(g, (0,"profile",0,-1), 0))
        self.assertIsNotNone(propose(g, (0,"profile",0,-1), 2))
        self.assertEqual(g["parts"][0]["position"], [0,0,0])

    def test_binary_and_text_same_camera_and_cloud(self):
        with tempfile.TemporaryDirectory() as a, tempfile.TemporaryDirectory() as b:
            av, ap = load(fixture(Path(a))[0])
            bv, bp = load(fixture(Path(b), True)[0])
            self.assertEqual(av, bv)
            np.testing.assert_array_equal(ap, bp)
            self.assertEqual(av[0]["camera"]["position"], [0,0,-4])
            self.assertEqual(av[0]["camera"]["up"], [0,-1,0])

    def test_prepare_preserves_metric_pose_and_filters_background(self):
        with tempfile.TemporaryDirectory() as t:
            root = Path(t)
            sparse, images = fixture(root)
            path = prepare(sparse, images, root / "project", meters_per_unit=0.5)
            data = validate_project(path)
            self.assertEqual(data["views"][0]["camera"]["position"], [0,0,-2])
            points = np.load(path.parent / "points.npy")
            selected = foreground_points(points, data["views"], path.parent)
            np.testing.assert_array_equal(selected, [[0,0,0]])
            data["views"][0]["image"] = "../images/image 0.png"
            write(path, data)
            with self.assertRaisesRegex(ValueError, "escapes"):
                validate_project(path)

    def test_distorted_cameras_fail_instead_of_ignoring_distortion(self):
        with tempfile.TemporaryDirectory() as t:
            sparse, _ = fixture(Path(t))
            (sparse / "cameras.txt").write_text("7 SIMPLE_RADIAL 16 12 16 8 6 0.1\n")
            with self.assertRaisesRegex(ValueError, "undistorter"):
                load(sparse)

    def test_truncated_binary_is_rejected(self):
        with tempfile.TemporaryDirectory() as t:
            sparse, _ = fixture(Path(t), True)
            (sparse / "images.bin").write_bytes(struct.pack("<Q", 1))
            with self.assertRaisesRegex(ValueError, "Truncated"):
                load(sparse)

    def test_geometry_contract_rejects_code_and_invalid_shell(self):
        good = {"parts": [{"name":"vessel", "kind":"lathe", "position":[0,0,0], "rotation":[0,0,0],
                            "size":[1,1,1], "profile":[[0,0.4],[1,0.5]], "thickness":0.02}]}
        validate_geometry(good)
        for field, value in [("python", "import os"), ("thickness", 0.8), ("profile", [[1,0.4],[0,0.5]]), ("size", [1,0,1])]:
            bad = copy.deepcopy(good)
            bad["parts"][0][field] = value
            with self.assertRaises(ValueError):
                validate_geometry(bad)

    def test_provider_sends_images_and_validates_model_response(self):
        with tempfile.TemporaryDirectory() as t, patch.dict(os.environ, {"BLENDER_MODEL_API_KEY":"test-only"}):
            path = Path(t) / "reference.png"
            Image.new("RGB", (8,8)).save(path)
            geometry = {"parts":[{"name":"object", "kind":"box", "position":[0,0,0], "rotation":[0,0,0], "size":[1,1,1]}]}
            provider = Provider({"supports_images":True, "model":"test-vision", "base_url":"https://example.invalid/v1"})
            import io
            def endpoint(request, timeout):
                payload = json.loads(request.data)
                self.assertEqual(payload["response_format"], {"type":"json_object"})
                self.assertTrue(payload["messages"][0]["content"][-1]["image_url"]["url"].startswith("data:image/jpeg;base64,"))
                return io.BytesIO(json.dumps({"choices":[{"message":{"content":json.dumps({"geometry":geometry,"boxes":{}})}}]}).encode())
            with patch("urllib.request.urlopen", endpoint):
                self.assertEqual(provider.propose({}, [("reference",path)])["geometry"], geometry)
            with self.assertRaises(ValueError):
                Provider({"supports_images":False})

    def test_automatic_segmentation_generates_foreground_mask(self):
        try:
            import cv2
        except ImportError:
            self.skipTest("Optional opencv-python not installed")
        with tempfile.TemporaryDirectory() as t:
            root = Path(t)
            image = np.full((40,48,3), 230, np.uint8)
            image[10:30, 15:33] = [20,60,100]
            Image.fromarray(image).save(root / "reference.png")
            views = [{"id":"front", "image":"reference.png", "width":48, "height":40}]
            segment(root, views, {"front":[10,5,38,35]})
            mask = np.array(Image.open(root / views[0]["mask"])) > 127
            expected = np.zeros((40,48), bool)
            expected[10:30,15:33] = True
            np.testing.assert_array_equal(mask, expected)


if __name__ == "__main__":
    unittest.main()
