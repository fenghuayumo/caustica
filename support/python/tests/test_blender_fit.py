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
from blender_fit.workflow import prepare, foreground_points, validate_project, segment


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
