from __future__ import annotations

import copy
import html
import os
import shutil
import subprocess
import time
from pathlib import Path

import numpy as np
from PIL import Image

from .colmap import load
from .common import read, write, validate_geometry, vector, finite
from .provider import Provider
from .refinement import FIELDS, refine
from .review import final_quality


def find_blender(explicit=None):
    candidates = [explicit, os.environ.get("BLENDER_EXECUTABLE"), shutil.which("blender"),
                  "D:/SoftWare/Blender/blender.exe"]
    candidates += [str(p) for p in Path("C:/Program Files/Blender Foundation").glob("Blender */blender.exe")]
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return str(Path(candidate).resolve())
    raise ValueError("Blender not found; pass --blender or set BLENDER_EXECUTABLE")


def render(blender, geometry, views, output, *, export=False, diagnostic=False, probe_points=None, reference_points=None, timeout=180):
    validate_geometry(geometry)
    output = Path(output).resolve()
    output.mkdir(parents=True, exist_ok=False)
    write(output / "request.json", {"geometry": geometry, "views": views, "output_dir": str(output),
                                    "export": export, "diagnostic": diagnostic, "probe_points": probe_points or [],
                                    "reference_points": reference_points or []})
    command = [blender, "--background", "--factory-startup", "--threads", "4", "--python-exit-code", "1",
               "--python", str(Path(__file__).with_name("worker.py")), "--", "--request", str(output / "request.json")]
    with (output / "blender.log").open("w", encoding="utf-8") as log:
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
        start = time.monotonic()
        try:
            while process.poll() is None:
                if (output.parent / "CANCEL").exists():
                    raise InterruptedError("Cancellation requested")
                if time.monotonic() - start > timeout:
                    raise TimeoutError("Blender job timed out")
                time.sleep(0.1)
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
    if process.returncode or not (output / "worker-result.json").is_file():
        raise RuntimeError(f"Blender failed; inspect {output / 'blender.log'}")
    return read(output / "worker-result.json")


def safe_input(root, name):
    path = (root / name).resolve()
    try:
        path.relative_to(root.resolve())
    except ValueError:
        raise ValueError(f"Input escapes image directory: {name}") from None
    if not path.is_file():
        raise ValueError(f"Missing input: {path}")
    return path


def prepare(colmap_dir, images_dir, output_dir, *, masks_dir=None, max_views=8, max_dimension=512,
            meters_per_unit=None, target="the central foreground object"):
    if max_views < 3 or max_dimension < 32:
        raise ValueError("Need >=3 views and max_dimension>=32")
    if meters_per_unit is not None and finite(meters_per_unit) <= 0:
        raise ValueError("meters_per_unit must be positive")
    views, points = load(colmap_dir)
    if len(views) > max_views:
        views = [views[i] for i in np.linspace(0, len(views)-1, max_views, dtype=int)]
    root = Path(output_dir).resolve()
    root.mkdir(parents=True, exist_ok=False)
    scale = meters_per_unit or 1.0
    points *= scale
    for i, view in enumerate(views):
        source = safe_input(Path(images_dir), view["source_name"])
        with Image.open(source) as image:
            if image.size != (view["width"], view["height"]):
                raise ValueError(f"Image size differs from COLMAP camera: {source}")
            w, h = image.size
            factor = min(1, max_dimension / max(w, h))
            size = (max(1, round(w*factor)), max(1, round(h*factor)))
            rgb = image.convert("RGB").resize(size, Image.Resampling.LANCZOS)
            rgb.save(root / (view["id"] + ".png"))
            mask = None
            if masks_dir:
                mask_path = Path(masks_dir) / (view["source_name"] + ".png")
                if not mask_path.is_file():
                    mask_path = Path(masks_dir) / str(Path(view["source_name"]).with_suffix(".png"))
                mask_path = safe_input(Path(masks_dir), str(mask_path.relative_to(Path(masks_dir))))
                with Image.open(mask_path) as m:
                    if m.size != image.size:
                        raise ValueError("Mask size differs from reference image")
                    mask = m.convert("L").resize(size, Image.Resampling.NEAREST)
            elif "A" in image.getbands() and image.getchannel("A").getextrema()[0] < 255:
                mask = image.getchannel("A").resize(size, Image.Resampling.NEAREST)
            if mask is not None:
                if not (np.asarray(mask) > 127).any():
                    raise ValueError("Empty foreground mask")
                mask.save(root / (view["id"] + "-mask.png"))
                view["mask"] = view["id"] + "-mask.png"
        k = view["camera"]["intrinsics"]
        for key in ("fx", "cx"):
            k[key] *= size[0] / w
        for key in ("fy", "cy"):
            k[key] *= size[1] / h
        view["camera"]["position"] = (np.asarray(view["camera"]["position"]) * scale).tolist()
        view.update(width=size[0], height=size[1], image=view["id"] + ".png",
                    split="validation" if i == len(views)-1 else "train")
    np.save(root / "points.npy", points)
    data = {"schema_version": 1, "coordinates": "colmap_world", "meters_per_unit": meters_per_unit,
            "target": target, "points": "points.npy", "views": views}
    write(root / "project.json", data)
    return root / "project.json"


def segment(root, views, boxes):
    for view in views:
        if view.get("mask"):
            continue
        try:
            import cv2
        except ImportError:
            raise ValueError("Provide foreground masks or install opencv-python for automatic GrabCut segmentation") from None
        box = vector(boxes[view["id"]], 4)
        left, top, right, bottom = [int(round(x)) for x in box]
        w, h = view["width"], view["height"]
        left, top = max(0, left), max(0, top)
        right, bottom = min(w-1, right), min(h-1, bottom)
        if right-left < 2 or bottom-top < 2:
            raise ValueError("Invalid model foreground box")
        image = np.array(Image.open(root / view["image"]).convert("RGB"))
        mask = np.zeros((h, w), dtype=np.uint8)
        cv2.setRNGSeed(0)
        cv2.grabCut(image, mask, (left, top, right-left, bottom-top),
                    np.zeros((1, 65), np.float64), np.zeros((1, 65), np.float64), 5, cv2.GC_INIT_WITH_RECT)
        foreground = np.isin(mask, [cv2.GC_FGD, cv2.GC_PR_FGD])
        if not foreground.any():
            raise ValueError("Automatic segmentation was empty; supply a foreground mask")
        view["mask"] = view["id"] + "-mask.png"
        Image.fromarray(foreground.astype(np.uint8)*255).save(root / view["mask"])
        preview = image.copy()
        preview[~foreground] = preview[~foreground] // 3
        Image.fromarray(preview).save(root / (view["id"] + "-segmentation.png"))


def foreground_points(points, views, root, *, diagnostics=None, outlier_iqr=3.0):
    if outlier_iqr is not None and (not np.isfinite(outlier_iqr) or outlier_iqr <= 0):
        raise ValueError("outlier_iqr must be positive or None")
    accepted, visible = np.zeros(len(points), int), np.zeros(len(points), int)
    for view in views:
        if view["split"] != "train":
            continue
        c = view["camera"]
        direction = np.array(c["direction"]); direction /= np.linalg.norm(direction)
        right = np.cross(direction, c["up"]); right /= np.linalg.norm(right)
        up = np.cross(right, direction)
        local = (points - c["position"]) @ np.stack((right, -up, direction), axis=1)
        k = c["intrinsics"]
        z = local[:, 2]
        denominator = np.where(z > 1e-10, z, 1)
        u = np.clip(np.rint(local[:, 0] * k["fx"] / denominator + k["cx"]), -1e9, 1e9).astype(int)
        v = np.clip(np.rint(local[:, 1] * k["fy"] / denominator + k["cy"]), -1e9, 1e9).astype(int)
        valid = (z > 0) & (u >= 0) & (u < view["width"]) & (v >= 0) & (v < view["height"])
        mask = np.array(Image.open(root / view["mask"])) > 127
        visible += valid
        accepted[valid] += mask[v[valid], u[valid]]
    selected = points[(visible >= 2) & (accepted == visible)]
    raw_count = len(selected)
    # Mask intersection alone permits distant points visible in only two
    # cameras. A conservative fence prevents them dominating spatial evidence.
    # Small clouds do not provide meaningful quartiles; retain them unchanged.
    if outlier_iqr is not None and len(selected) >= 16:
        low, high = np.quantile(selected, [0.25, 0.75], axis=0)
        spread = np.maximum(high - low, max(float(np.max(high - low)) * 0.01, 1e-12))
        keep = np.all((selected >= low - outlier_iqr * spread) &
                      (selected <= high + outlier_iqr * spread), axis=1)
        selected = selected[keep]
    if diagnostics is not None:
        diagnostics.update(input_points=len(points), mask_intersection_points=raw_count,
                           retained_points=len(selected), outliers_removed=raw_count-len(selected),
                           outlier_iqr=outlier_iqr)
    return selected


def score(root, output, views, split):
    scores = {}
    for view in views:
        if view["split"] != split:
            continue
        reference = np.array(Image.open(root / view["mask"])) > 127
        with Image.open(output / (view["id"] + ".png")) as image:
            actual = np.array(image.getchannel("A")) > 127
        if actual.shape != reference.shape:
            raise ValueError("Rendered/reference mask size mismatch")
        union = (reference | actual).sum()
        scores[view["id"]] = float(1 - (reference & actual).sum() / max(1, union))
    return {"loss": float(np.mean(list(scores.values()))), "worst_loss": max(scores.values()), "views": scores}


def quality(training, validation, tolerance=0.05):
    # One bad viewpoint must not disappear in the mean of many easy views.
    failures = [{"split": split, "view": view, "loss": loss}
                for split, result in (("train", training), ("validation", validation))
                for view, loss in result["views"].items() if loss > tolerance]
    return {"status": "needs_geometry_review" if failures else "within_silhouette_tolerance",
            "per_view_tolerance": tolerance, "failing_views": failures,
            "scope": "silhouette only; masks and hidden surfaces require review"}


def report(root, state, views):
    best = root / state["best_directory"]
    body = ["<meta charset='utf-8'><title>Blender geometry fit</title>",
            "<style>body{font:16px sans-serif;background:#20242a;color:#eee}img{max-width:32%}section{margin:24px}</style>",
            "<h1>Blender geometry fit</h1>", "<pre>" + html.escape(str(state)) + "</pre>"]
    for v in views:
        body += ["<section><h2>" + html.escape(v["id"] + " / " + v["split"]) + "</h2>",
                 f'<img src="input/{v["image"]}"><img src="input/{v["mask"]}">',
                 f'<img src="{best.name}/{v["id"]}.png">']
        if (best / (v["id"] + "-diagnostic.png")).exists():
            body.append(f'<img src="{best.name}/{v["id"]}-diagnostic.png">')
        body.append("</section>")
    (root / "report.html").write_text("\n".join(body), encoding="utf-8")


def validate_project(path):
    path = Path(path).resolve()
    data = read(path)
    if data.get("schema_version") != 1 or data.get("coordinates") != "colmap_world":
        raise ValueError("Unsupported Blender project")
    scale = data.get("meters_per_unit")
    if scale is not None and finite(scale) <= 0:
        raise ValueError("Invalid metric scale")
    if not isinstance(data.get("target"), str) or not data["target"]:
        raise ValueError("Target description is required")
    points = np.load(safe_input(path.parent, data["points"]), allow_pickle=False)
    if points.ndim != 2 or points.shape[1] != 3 or not len(points) or not np.isfinite(points).all():
        raise ValueError("Invalid sparse point cloud")
    ids, splits = set(), []
    for v in data["views"]:
        name = v["id"]
        if not isinstance(name, str) or not name.isascii() or not name.replace("_", "").isalnum() or name in ids:
            raise ValueError("Invalid/duplicate view id")
        ids.add(name)
        if v["split"] not in {"train", "validation"}:
            raise ValueError("Invalid view split")
        splits.append(v["split"])
        if any(isinstance(v[k], bool) or not isinstance(v[k], int) or not 1 <= v[k] <= 8192 for k in ("width", "height")):
            raise ValueError("Invalid image dimensions")
        for field in ("image", "mask"):
            if field in v:
                with Image.open(safe_input(path.parent, v[field])) as image:
                    if image.size != (v["width"], v["height"]):
                        raise ValueError("Image/mask dimensions differ from camera")
                    if field == "mask" and not (np.array(image.convert("L")) > 127).any():
                        raise ValueError("Empty foreground mask")
        c = v["camera"]
        for field in ("position", "direction", "up"):
            vector(c[field])
        if np.linalg.norm(np.cross(c["direction"], c["up"])) < 1e-8:
            raise ValueError("Degenerate camera orientation")
        k = c["intrinsics"]
        if any(finite(k[key]) <= 0 for key in ("fx", "fy")):
            raise ValueError("Invalid focal length")
        finite(k["cx"]); finite(k["cy"])
    if splits.count("train") < 2 or splits.count("validation") < 1:
        raise ValueError("Need two training views and at least one validation view")
    return data


def run(project_path, output_dir, blender, *, provider_config=None, initial_geometry=None,
        max_evaluations=30, model_rounds=3, numeric_fields=FIELDS, point_outlier_iqr=3.0):
    if max_evaluations < 1 or model_rounds < 0:
        raise ValueError("Invalid evaluation/model budget")
    if not numeric_fields or len(set(numeric_fields)) != len(numeric_fields) or set(numeric_fields) - set(FIELDS):
        raise ValueError("Invalid numeric fields; choose size, position, rotation, profile")
    if point_outlier_iqr is not None and (not np.isfinite(point_outlier_iqr) or point_outlier_iqr <= 0):
        raise ValueError("point_outlier_iqr must be positive or None")
    project_path = Path(project_path).resolve()
    data = validate_project(project_path)
    root = Path(output_dir).resolve()
    if root == project_path.parent or project_path.parent in root.parents or root in project_path.parent.parents:
        raise ValueError("Run directory and input directory must be separate, non-nested directories")
    provider = Provider(read(provider_config)) if provider_config else None
    if initial_geometry is None and provider is None:
        raise ValueError("Automatic modeling needs --provider-config; offline fitting needs --geometry")
    if initial_geometry is not None:
        validate_geometry(read(initial_geometry))
    root.mkdir(parents=True, exist_ok=False)
    shutil.copytree(project_path.parent, root / "input")
    source = root / "input"
    points = np.load(source / data["points"], allow_pickle=False)
    views = data["views"]
    images = [(v["id"] + " reference", source / v["image"]) for v in views if v["split"] == "train"]
    bounds = np.quantile(points, [0.05, 0.95], axis=0).tolist()
    context = {"target": data["target"], "coordinates": "COLMAP world XYZ = Blender XYZ (no implicit up-axis correction)",
               "units": "meters" if data["meters_per_unit"] else "COLMAP arbitrary scale",
               "sparse_scene_bounds": bounds, "cameras": [v for v in views if v["split"] == "train"]}
    missing = [v for v in views if not v.get("mask")]
    try:
        if missing:
            if provider is None:
                raise ValueError("Offline geometry fitting requires masks for every view")
            boxes = provider.segmentation_boxes(data["target"], missing,
                        [(v["id"], source / v["image"]) for v in missing])
            write(root / "segmentation-boxes.json", boxes)
            segment(source, views, boxes)
        point_diagnostics = {}
        selected = foreground_points(points, views, source, diagnostics=point_diagnostics, outlier_iqr=point_outlier_iqr)
        write(root / "foreground-diagnostics.json", point_diagnostics)
        np.save(root / "foreground-points.npy", selected)
        context["foreground_sparse_bounds"] = np.quantile(selected, [0.05, 0.95], axis=0).tolist() if len(selected) else None
        context["foreground_sparse_points"] = selected[np.linspace(0, len(selected)-1, min(256, len(selected)), dtype=int)].tolist() if len(selected) else []
        write(source / "project.json", data)
        if initial_geometry is None:
            proposal = provider.propose(context, images)
            write(root / "proposal-000.json", proposal)
            geometry = proposal["geometry"]
        else:
            geometry = read(initial_geometry)
        validate_geometry(geometry)
    except BaseException as error:
        write(root / "state.json", {"status": "failed", "stage": "initialization", "error": str(error), "evaluations": 0})
        raise
    state = {"evaluations": 0, "best_loss": None, "best_directory": None, "history": [],
             "model_calls": 1 if initial_geometry is None else 0, "segmentation_calls": 1 if missing else 0,
             "foreground_points": len(selected), "status": "running"}
    state["numeric_fields"] = list(numeric_fields)
    state["foreground_diagnostics"] = point_diagnostics
    if provider:
        state["provider"] = {key: provider.config[key] for key in ("base_url", "model")}
    best_geometry = None

    def review_candidate(candidate, directory, selected_views, name):
        references = [(v["id"] + " reference", source / v["image"]) for v in selected_views]
        diagnostics = [(v["id"] + " diagnostic render", directory / (v["id"] + "-diagnostic.png")) for v in selected_views]
        state["review_calls"] = state.get("review_calls", 0) + 1
        result = provider.review(candidate, selected_views, references + diagnostics)
        write(directory / name, result)
        return result

    def evaluate(candidate, label):
        nonlocal best_geometry
        if (root / "CANCEL").exists():
            raise InterruptedError("Cancellation requested")
        if state["evaluations"] >= max_evaluations:
            return False
        directory = root / f"candidate-{state['evaluations']:04d}"
        state["evaluations"] += 1
        render(blender, candidate, views, directory, diagnostic=provider is not None)
        result = score(source, directory, views, "train")
        visual = review_candidate(candidate, directory, [v for v in views if v["split"] == "train"], "review-train.json") if provider else None
        objective = result["loss"] + (0.5 * visual["loss"] if visual else 0)
        accepted = state.get("best_objective") is None or objective < state["best_objective"]
        state["history"].append({"directory": directory.name, "label": label, "train": result,
                                 "visual_review": visual, "objective": objective, "accepted": accepted})
        if accepted:
            best_geometry = copy.deepcopy(candidate)
            state.update(best_loss=result["loss"], best_objective=objective, best_review=visual, best_directory=directory.name)
            write(root / "best-geometry.json", best_geometry)
        write(root / "state.json", state)
        report(root, state, views)
        print(f"[{state['evaluations']}/{max_evaluations}] {label}: train={result['loss']:.6f}, objective={objective:.6f}, best_objective={state['best_objective']:.6f}, accepted={accepted}", flush=True)
        return accepted

    try:
        evaluate(geometry, "initial")
        for round_index in range(max(1, model_rounds+1)):
            # Geometry refinement uses training renders only. Reserve model calls.
            remaining_rounds = model_rounds + 1 - round_index if provider else 1
            quota = max(0, (max_evaluations - state["evaluations"] - max(0, remaining_rounds-1)) // max(1, remaining_rounds))
            refine(lambda: best_geometry, evaluate, quota, fields=numeric_fields, start_level=round_index)
            if provider and round_index < model_rounds and state["evaluations"] < max_evaluations:
                feedback = [{k: h[k] for k in ("label", "train", "objective", "accepted")} for h in state["history"][-7:]]
                context.update(best_geometry=best_geometry, training_feedback=feedback,
                               geometry_review=state["best_review"],
                               revision_instruction="Fix failed/uncertain geometry checks using multiple reference views. Preserve correct parts; thickness and internal walls matter even at equal silhouette score.")
                previews = [(v["id"] + " current diagnostic render", root / state["best_directory"] / (v["id"] + "-diagnostic.png"))
                            for v in views if v["split"] == "train"]
                state["model_calls"] += 1
                proposal = provider.propose(context, images + previews)
                write(root / f"proposal-{state['model_calls']:03d}.json", proposal)
                evaluate(proposal["geometry"], "model-revision")
            if state["evaluations"] >= max_evaluations:
                break
        state["validation"] = score(source, root / state["best_directory"], views, "validation")
        render(blender, best_geometry, views, root / "export", export=True,
               reference_points=context["foreground_sparse_points"])
        write(root / "export" / "cameras.json", {"source_coordinates": "colmap_world",
                                                  "gltf_world_transform": "(x,y,z) -> (x,z,-y)",
                                                  "meters_confirmed": data["meters_per_unit"] is not None,
                                                  "views": views})
        state["status"] = "completed"
        training = score(source, root / state["best_directory"], views, "train")
        silhouette_quality = quality(training, state["validation"])
        # Held-out photographs never enter proposal, numeric selection, or training review.
        # They enter this final audit only; a failed audit cannot silently pass to appearance fitting.
        audit = review_candidate(best_geometry, root / state["best_directory"], views, "review-final.json") if provider else None
        state["quality_details"] = final_quality(silhouette_quality, state["best_review"], audit,
                                                 [v["id"] for v in views if v["split"] == "validation"])
        if audit:
            write(root / state["best_directory"] / "review-final.json", state["quality_details"]["visual_audit"])
        state["quality"] = state["quality_details"]["status"]
        handoff(root, data)
        if state["quality"] != "within_geometry_tolerance":
            info = read(root / "export" / "handoff.json")
            info.update(status="needs_geometry_review", quality=state["quality_details"])
            write(root / "export" / "handoff.json", info)
        state["numeric_evaluations_by_field"] = {field: sum(h["label"].startswith(field + "/") for h in state["history"])
                                                 for field in numeric_fields}
    except BaseException as error:
        state.update(status="cancelled" if isinstance(error, (InterruptedError, KeyboardInterrupt)) else "failed",
                     error=str(error))
        write(root / "state.json", state)
        if best_geometry:
            report(root, state, views)
        raise
    write(root / "state.json", state)
    report(root, state, views)
    return state


def handoff(root, data):
    # GLB Y-up export rotates geometry; the exact same proper rotation must rotate cameras.
    def yup(value):
        x, y, z = value
        return [x, z, -y]
    export = root / "export"
    if data["meters_per_unit"] is None:
        write(export / "handoff.json", {"status": "needs_metric_scale", "model": "model.glb",
                                        "reason": "Set --meters-per-unit during prepare before Caustica appearance fitting"})
        return
    if len({(v["width"], v["height"]) for v in data["views"]}) != 1:
        write(export / "handoff.json", {"status": "needs_common_image_size",
                                        "reason": "Scene Fit currently requires all reference images to have the same dimensions"})
        return
    assets = export / "assets"
    assets.mkdir()
    shutil.copy2(export / "model.glb", assets / "model.glb")
    write(assets / "object.material.json", {"MaterialModel": "OpenPBR", "OpenPBR": {"base_color": [0.5, 0.5, 0.5], "specular_roughness": 0.35}})
    refs = export / "references"
    refs.mkdir()
    views = copy.deepcopy(data["views"])
    for v in views:
        for field in ("image", "mask"):
            shutil.copy2(root / "input" / v[field], refs / Path(v[field]).name)
            v[field] = "references/" + Path(v[field]).name
        for field in ("position", "direction", "up"):
            v["camera"][field] = yup(v["camera"][field])
    width, height = views[0]["width"], views[0]["height"]
    points = np.load(root / "foreground-points.npy", allow_pickle=False)
    center = np.median(points, axis=0) if len(points) else np.zeros(3)
    size = float(np.linalg.norm(np.ptp(points, axis=0))) if len(points) else 1.0
    light = yup(center.tolist()); light[1] += max(size, 0.1)*2; light[2] += max(size, 0.1)*2
    project = {"schema_version": 1, "coordinates": "caustica_y_up_meters", "comparison_space": "display_rgb",
               "asset_root": "assets", "assets": {"model": "model.glb", "object_material": "object.material.json"},
               "foreground_entities": ["object"], "views": views,
               "render": {"width": width, "height": height, "spp": 16, "bounces": 6, "denoiser": "oidn", "timeout_seconds": 120, "exposure_compensation": 1},
               "loss_weights": {"rgb": 1, "silhouette": 1},
               "scene": {"format": "caustica.scene", "version": 2, "entities": [
                   {"id": "object", "components": {"PrefabInstance": {"source": "asset:model"}, "MaterialOverride": {"source": "asset:object_material"}}},
                   {"id": "key", "components": {"PointLight": {"color": [1, 1, 1], "intensity": 50, "radius": 0.1}, "Transform": {"translation": light}}}]},
               "parameters": [{"name": "roughness", "group": "material", "unit": "unitless", "initial": 0.35, "bounds": [0.02, 1], "step": 0.1,
                               "binding": {"target": "material", "id": "object_material", "path": "/OpenPBR/specular_roughness"}},
                              {"name": "light_intensity", "group": "lighting", "unit": "caustica_intensity", "initial": 50, "bounds": [1, 500], "step": 25,
                               "binding": {"target": "entity", "id": "key", "path": "/components/PointLight/intensity"}}]}
    for axis in range(3):
        project["parameters"].append({"name": f"base_color_{axis}", "group": "material", "unit": "linear_rgb", "initial": 0.5,
                                      "bounds": [0, 1], "step": 0.1,
                                      "binding": {"target": "material", "id": "object_material", "path": f"/OpenPBR/base_color/{axis}"}})
    write(export / "scene-fit-project.json", project)
    write(export / "handoff.json", {"status": "ready", "scene_fit_project": "scene-fit-project.json",
                                    "note": "Neutral material/light starting values; check projection before appearance optimization"})
