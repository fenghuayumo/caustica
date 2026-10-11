"""Actual Blender CPU acceptance: COLMAP -> fixed cameras -> geometry fit -> GLB."""
import argparse
import math
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from blender_fit.colmap import load, rotation
from blender_fit.common import write, read
from blender_fit.workflow import prepare, render, run, find_blender


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--output-dir", required=True)
    p.add_argument("--blender")
    args = p.parse_args()
    root = Path(args.output_dir).resolve()
    root.mkdir(parents=True, exist_ok=False)
    sparse = root / "sparse"
    sparse.mkdir()
    (sparse / "cameras.txt").write_text("1 PINHOLE 96 72 90 83 43 39\n", encoding="utf-8")
    rows = []
    for i, angle in enumerate([0, 90, 45], 1):
        q = [math.cos(math.radians(angle)/2), 0, math.sin(math.radians(angle)/2), 0]
        rows.append(f"{i} " + " ".join(map(str, q)) + f" 0 0 4 1 reference{i}.png\n\n")
    (sparse / "images.txt").write_text("".join(rows), encoding="utf-8")
    points = np.array([[x, y, z] for x in [-0.4, 0, 0.4] for y in [-0.4, 0, 0.4] for z in [-0.4, 0, 0.4]])
    (sparse / "points3D.txt").write_text("".join(f"{i+1} {x} {y} {z} 128 128 128 0\n" for i, (x,y,z) in enumerate(points)), encoding="utf-8")
    views, _ = load(sparse)
    truth = {"parts": [{"name": "object", "kind": "box", "position": [0,0,0], "rotation": [0,0,0], "size": [1,1,1]}]}
    probes = [[0,0,0], [0.2,0.1,0.3]]
    blender = find_blender(args.blender)
    result = render(blender, truth, views, root / "truth", probe_points=probes)
    max_error = 0
    for view, angle in zip(views, [0,90,45]):
        r = rotation([math.cos(math.radians(angle)/2), 0, math.sin(math.radians(angle)/2), 0])
        for point, projected in zip(probes, result["projections"][view["id"]]):
            local = r @ point + [0,0,4]
            expected = [90*local[0]/local[2]+43, 83*local[1]/local[2]+39]
            max_error = max(max_error, float(np.max(np.abs(np.array(projected[:2])-expected))))
    assert max_error < 0.002, max_error
    variants = {"parts": [
        {"name":"box", "kind":"box", "position":[-0.7,0,0], "rotation":[0,0,10], "size":[0.4,0.4,0.4]},
        {"name":"sphere", "kind":"ellipsoid", "position":[0.7,0,0], "rotation":[0,0,0], "size":[0.4,0.5,0.4]},
        {"name":"cylinder", "kind":"cylinder", "position":[0,0.7,0], "rotation":[90,0,0], "size":[0.4,0.4,0.6]},
        {"name":"shell", "kind":"lathe", "position":[0,0,0], "rotation":[0,0,0], "size":[1,1,1],
         "profile":[[-0.3,0.25],[0.3,0.3]], "thickness":0.02}]}
    variants_result = render(blender, variants, views[:1], root / "geometry-operations", export=True)
    assert variants_result["mesh_stats"]["shell"]["vertices"] == 256, variants_result
    assert all(info["uv_layers"] >= 1 for info in variants_result["mesh_stats"].values()), variants_result
    images = root / "images"
    images.mkdir()
    import shutil
    for i, view in enumerate(views, 1):
        shutil.copy2(root / "truth" / (view["id"] + ".png"), images / f"reference{i}.png")
    project = prepare(sparse, images, root / "project", meters_per_unit=1, max_dimension=96)
    initial = read(root / "truth" / "request.json")["geometry"]
    initial["parts"][0]["size"][0] = 1/1.2
    write(root / "initial.json", initial)
    # Exercise the actual HTTP adapter and model revision loop with an explicitly mocked endpoint.
    import json
    import os
    import threading
    from http.server import BaseHTTPRequestHandler, HTTPServer
    calls = []
    proposal_calls = []
    class MockModel(BaseHTTPRequestHandler):
        def do_POST(self):
            payload = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            calls.append(payload)
            prompt = payload["messages"][0]["content"][0]["text"]
            if prompt.startswith("GEOMETRY_REVIEW:"):
                from blender_fit.review import CRITERIA
                context = json.loads(prompt.split("\n")[-1])
                size = context["geometry"]["parts"][0]["size"]
                severity = min(1, max(abs(s-1) for s in size))
                answer = {"checks": [{"part":"object", "criterion":c,
                            "status":"pass" if severity < 1e-6 else "fail",
                            "severity":0 if severity < 1e-6 else severity,
                            "views":[v["id"] for v in context["views"]], "reason":"Mock geometric comparison"} for c in CRITERIA]}
            else:
                proposal_calls.append(payload)
                proposal = initial if len(proposal_calls) == 1 else {"parts": [dict(initial["parts"][0], size=[2,2,2])]}
                answer = {"geometry":proposal,"boxes":{}}
            response = json.dumps({"choices":[{"message":{"content":json.dumps(answer)}}]}).encode()
            self.send_response(200)
            self.end_headers()
            self.wfile.write(response)
        def log_message(self, *args):
            pass
    server = HTTPServer(("127.0.0.1",0), MockModel)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    write(root / "mock-provider.json", {"base_url":f"http://127.0.0.1:{server.server_port}/v1", "model":"mock-vision",
                                        "supports_images":True, "allow_local_http":True, "api_key_env":"BLENDER_FIT_TEST_KEY"})
    os.environ["BLENDER_FIT_TEST_KEY"] = "local-test-only"
    try:
        state = run(project, root / "run", blender, provider_config=root / "mock-provider.json", max_evaluations=8, model_rounds=1)
    finally:
        server.shutdown()
        server.server_close()
        os.environ.pop("BLENDER_FIT_TEST_KEY", None)
    assert len(proposal_calls) == 2 and state["model_calls"] == 2
    assert state["review_calls"] == state["evaluations"] + 1
    revisions = [item for item in state["history"] if item["label"] == "model-revision"]
    assert len(revisions) == 1 and not revisions[0]["accepted"]
    assert all("view_3" not in json.dumps(payload) for payload in calls[:-1]), "Validation view leaked into geometry selection"
    assert "view_3" in json.dumps(calls[-1]), "Final audit did not inspect held-out reference"
    assert state["quality"] == "within_geometry_tolerance", state
    assert (root / "run" / state["best_directory"] / "view_1-diagnostic.png").is_file()
    baseline = state["history"][0]["train"]["loss"]
    assert state["best_loss"] < baseline * 0.1, state
    assert state["validation"]["loss"] < 0.01, state
    assert (root / "run/export/model.glb").stat().st_size > 100
    assert (root / "run/export/model.blend").stat().st_size > 100
    from scene_fit import Project
    Project(root / "run/export/scene-fit-project.json")
    write(root / "verification.json", {"projection_max_pixel_error": max_error, "baseline": baseline,
                                       "best": state["best_loss"], "validation": state["validation"],
                                       "blender": result["blender_version"], "scene_fit_handoff_valid": True,
                                       "geometry_operations": variants_result["mesh_stats"],
                                       "model_adapter_mock_calls": len(calls), "live_model_tested": False})
    print(read(root / "verification.json"))


if __name__ == "__main__":
    main()
