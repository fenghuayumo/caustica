"""Real Blender regression: equal union silhouettes must not hide internal errors.

The vision endpoint is a deterministic mock, explicitly not a vision quality test.
"""
import argparse
import copy
import json
import os
from pathlib import Path
import sys
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from blender_fit.common import read, write
from blender_fit.review import CRITERIA
from blender_fit.workflow import render, run, find_blender


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--blender")
    args = parser.parse_args()
    root = Path(args.output_dir).resolve()
    root.mkdir(parents=True, exist_ok=False)
    blender = find_blender(args.blender)
    truth = {"parts": [
        {"name":"tray", "kind":"cylinder", "position":[0,0,0], "rotation":[0,0,0], "size":[4,4,0.2]},
        {"name":"book", "kind":"box", "position":[0,0,0.2], "rotation":[0,0,0], "size":[1,1,0.2]}]}
    wrong = copy.deepcopy(truth)
    wrong["parts"][1].update(position=[0,0,0.29], size=[1,1,0.02])  # Cover unchanged; underside floats.
    views = []
    for index, position in enumerate(([5,0,7], [0,5,7], [-5,0,7])):
        direction = -np.asarray(position, dtype=float)
        direction /= np.linalg.norm(direction)
        views.append({"id":f"view_{index+1}", "width":128, "height":96,
                      "image":f"view_{index+1}.png", "mask":f"view_{index+1}-mask.png",
                      "split":"validation" if index == 2 else "train",
                      "camera":{"position":list(position), "direction":direction.tolist(), "up":[0,0,1],
                                "intrinsics":{"fx":140,"fy":140,"cx":64,"cy":48}}})
    render(blender, truth, views, root / "truth", diagnostic=True)
    render(blender, wrong, views, root / "wrong", diagnostic=True)
    errors = []
    source = root / "input"
    source.mkdir()
    for view in views:
        with Image.open(root / "truth" / view["image"]) as image:
            actual = np.array(image.getchannel("A")) > 127
            Image.fromarray(actual.astype(np.uint8)*255).save(source / view["mask"])
        with Image.open(root / "wrong" / view["image"]) as image:
            predicted = np.array(image.getchannel("A")) > 127
        errors.append(float(1 - (actual & predicted).sum() / (actual | predicted).sum()))
        with Image.open(root / "truth" / (view["id"] + "-diagnostic.png")) as image:
            image.convert("RGB").save(source / view["image"])
    assert max(errors) == 0, errors
    np.save(source / "points.npy", np.array([[0,0,0], [0.1,0.1,0]]))
    write(source / "project.json", {"schema_version":1, "coordinates":"colmap_world", "meters_per_unit":1,
                                   "points":"points.npy", "target":"tray and book", "views":views})
    write(root / "initial.json", wrong)
    calls = []
    class MockModel(BaseHTTPRequestHandler):
        def do_POST(self):
            payload = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            prompt = payload["messages"][0]["content"][0]["text"]
            calls.append(prompt)
            if prompt.startswith("GEOMETRY_REVIEW:"):
                context = json.loads(prompt.split("\n")[-1])
                wrong_thickness = context["geometry"]["parts"][1]["size"][2] < 0.1
                checks = []
                for part in context["geometry"]["parts"]:
                    for criterion in CRITERIA:
                        bad = wrong_thickness and part["name"] == "book" and criterion in ("proportions", "contact")
                        checks.append({"part":part["name"], "criterion":criterion, "status":"fail" if bad else "pass",
                                       "severity":0.8 if bad else 0, "views":[v["id"] for v in context["views"]],
                                       "reason":"Book underside floats and thickness is too small" if bad else "Mock reference agreement"})
                answer = {"checks":checks}
            else:
                assert "Book underside floats" in prompt, "Revision did not receive structured critique"
                answer = {"geometry":truth}
            response = json.dumps({"choices":[{"message":{"content":json.dumps(answer)}}]}).encode()
            self.send_response(200)
            self.end_headers()
            self.wfile.write(response)

        def log_message(self, *args):
            pass

    server = HTTPServer(("127.0.0.1", 0), MockModel)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    write(root / "provider.json", {"base_url":f"http://127.0.0.1:{server.server_port}/v1", "model":"mock-vision",
                                   "supports_images":True, "allow_local_http":True, "api_key_env":"BLENDER_INTERNAL_TEST_KEY"})
    prior = os.environ.get("BLENDER_INTERNAL_TEST_KEY")
    os.environ["BLENDER_INTERNAL_TEST_KEY"] = "test-only"
    try:
        state = run(source / "project.json", root / "run", blender, provider_config=root / "provider.json",
                    initial_geometry=root / "initial.json", max_evaluations=2, model_rounds=1)
    finally:
        server.shutdown()
        server.server_close()
        if prior is None:
            os.environ.pop("BLENDER_INTERNAL_TEST_KEY", None)
        else:
            os.environ["BLENDER_INTERNAL_TEST_KEY"] = prior
    assert state["history"][1]["accepted"] and state["history"][1]["label"] == "model-revision", state
    assert state["history"][0]["train"]["loss"] == state["history"][1]["train"]["loss"] == 0
    assert read(root / "run/best-geometry.json") == truth
    assert state["quality"] == "within_geometry_tolerance", state
    assert all("view_3" not in p for p in calls[:-1])
    write(root / "verification.json", {"union_silhouette_errors":errors, "equal_silhouette_repair_accepted":True,
                                       "book_thickness_before":0.02, "book_thickness_after":0.2,
                                       "quality":state["quality"], "live_model_tested":False})
    print(read(root / "verification.json"))


if __name__ == "__main__":
    main()
