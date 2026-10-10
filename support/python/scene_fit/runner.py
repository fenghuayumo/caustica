"""Candidate transactions, training-only selection and bounded coordinate search."""
from __future__ import annotations

import html
import json
import math
import os
import time
from pathlib import Path

from .metrics import comparison_image, evaluate, load_reference
from .project import Project, candidate_hash, identifier, number, read_json, tree_hash, write_json


class FitCancelled(RuntimeError):
    pass


class FitRunner:
    def __init__(self, project, run_dir, renderer, *, resume=False):
        self.run_dir = Path(run_dir).resolve()
        self.renderer = renderer
        self.started = time.monotonic()
        self.deadline = None
        self.owns_lock = False
        if not resume:
            try:
                self.run_dir.relative_to(project.asset_root)
            except ValueError:
                pass
            else:
                raise ValueError("run_dir must be outside asset_root to avoid recursive snapshots")
            self.run_dir.mkdir(parents=True, exist_ok=False)
        try:
            with (self.run_dir / "run.lock").open("x", encoding="utf-8") as lock:
                lock.write(str(os.getpid()))
        except FileExistsError:
            raise RuntimeError("Run is already owned by a worker (run.lock). After a crash, verify that worker exited before removing the lock.") from None
        self.owns_lock = True
        try:
            self.initialize(project, resume)
        except BaseException:
            self.release_lock()
            raise

    def initialize(self, project, resume):
        if resume:
            manifest = read_json(self.run_dir / "run.json")
            if tree_hash(self.run_dir / "input") != manifest["input_sha256"]:
                raise ValueError("Frozen inputs changed; cannot resume")
            self.project = Project(self.run_dir / "input" / "project.json")
        else:
            self.project = project.freeze(self.run_dir / "input")
            write_json(self.run_dir / "run.json", {"schema_version": 1, "source_project": str(project.path),
                                                  "input_sha256": tree_hash(self.run_dir / "input")})
        self.candidates = self.run_dir / "candidates"
        self.candidates.mkdir(exist_ok=True)
        self.best = read_json(self.run_dir / "best.json") if (self.run_dir / "best.json").is_file() else None

    def close(self):
        try:
            self.renderer.close()
        finally:
            self.release_lock()

    def release_lock(self):
        if self.owns_lock:
            (self.run_dir / "run.lock").unlink()
            self.owns_lock = False

    def check_cancel(self):
        if (self.run_dir / "CANCEL").exists():
            raise FitCancelled("CANCEL file exists; best valid candidate preserved")
        if self.deadline is not None and time.monotonic() >= self.deadline:
            raise FitCancelled("Time budget exhausted; best valid candidate preserved")

    def log(self, record):
        with (self.run_dir / "iterations.jsonl").open("a", encoding="utf-8") as stream:
            stream.write(json.dumps(record, ensure_ascii=False, allow_nan=False) + "\n")

    def evaluate(self, values=None, *, split="train"):
        if split not in ("train", "validation"):
            raise ValueError("split must be train or validation")
        views = [view for view in self.project.data["views"] if view["split"] == split]
        if not views:
            raise ValueError(f"No {split} views")
        values = self.project.values(values or {})
        self.check_cancel()
        if tree_hash(self.run_dir / "input") != read_json(self.run_dir / "run.json")["input_sha256"]:
            raise ValueError("Frozen inputs changed during the run")
        index = len(list(self.candidates.iterdir()))
        candidate_id = f"candidate-{index:06d}"
        candidate = self.candidates / candidate_id
        started = time.monotonic()
        record = {"candidate_id": candidate_id, "split": split, "parameters": values, "status": "running"}
        try:
            fingerprint = self.project.materialize(candidate, values)
            record["asset_sha256"] = fingerprint
            write_json(candidate / "result.json", record)
            outputs = self.renderer.capture(self.project, candidate, views, self.check_cancel)
            metrics = {}
            provenance = {}
            for view in views:
                reference, mask, depth = load_reference(self.project, view)
                output = outputs[view["id"]]
                metrics[view["id"]] = evaluate(reference, output["rgb"], mask, output["mask"], self.project.weights, depth, output.get("depth"))
                provenance[view["id"]] = output["provenance"]
                comparison_image(reference, output["rgb"]).save(candidate / f"{view['id']}-comparison.png")
            loss = sum(item["loss"] for item in metrics.values()) / len(metrics)
            if not math.isfinite(loss):
                raise ValueError("Nonfinite loss")
            if candidate_hash(candidate) != fingerprint:
                raise ValueError("Candidate scene or assets changed during capture")
            record.update(status="completed", loss=loss, metrics=metrics, provenance=provenance)
            # Validation results are never eligible for selection.
            if split == "train" and (self.best is None or loss < self.best["loss"]):
                self.best = dict(record)
                write_json(self.run_dir / "best.json", self.best)
        except (Exception, KeyboardInterrupt) as error:
            record.update(status="cancelled" if isinstance(error, (FitCancelled, KeyboardInterrupt)) else "failed",
                          error={"type": type(error).__name__, "message": str(error)})
            if isinstance(error, (FitCancelled, KeyboardInterrupt)):
                raise
        finally:
            record["seconds"] = time.monotonic() - started
            candidate.mkdir(parents=True, exist_ok=True)
            write_json(candidate / "result.json", record)
            self.log(record)
        return record

    def optimize(self, *, max_evaluations=30, groups=None, minimum_step_fraction=0.001, improvement_tolerance=0.0, max_seconds=None):
        if type(max_evaluations) is not int or max_evaluations < 1:
            raise ValueError("max_evaluations must be a positive integer")
        if not 0 < number(minimum_step_fraction, "minimum_step_fraction") < 1:
            raise ValueError("minimum_step_fraction must be between 0 and 1")
        if number(improvement_tolerance, "improvement_tolerance") < 0:
            raise ValueError("improvement_tolerance must be nonnegative")
        groups = ["material", "lighting"] if groups is None else groups
        if set(groups) - {"geometry", "material", "lighting"}:
            raise ValueError("Unknown parameter group")
        parameters = [parameter for parameter in self.project.parameters if parameter["group"] in groups]
        if not parameters:
            raise ValueError("No selected parameters")
        if max_seconds is not None:
            if number(max_seconds, "max_seconds") <= 0:
                raise ValueError("max_seconds must be positive")
            self.deadline = time.monotonic() + max_seconds
        steps = {parameter["name"]: parameter["step"] for parameter in parameters}
        count = 0
        reason = "budget"
        try:
            if self.best is None:
                baseline = self.evaluate()
                count += 1
                if baseline["status"] != "completed":
                    raise RuntimeError(f"Baseline failed: {baseline.get('error')}")
            while count < max_evaluations:
                self.check_cancel()
                improved = False
                for parameter in parameters:
                    name = parameter["name"]
                    origin = dict(self.best["parameters"])
                    original_loss = self.best["loss"]
                    for direction in (-1, 1):
                        if count >= max_evaluations:
                            break
                        trial = dict(origin)
                        lo, hi = parameter["bounds"]
                        trial[name] = max(lo, min(hi, origin[name] + direction * steps[name]))
                        if trial[name] == origin[name]:
                            continue
                        result = self.evaluate(trial)
                        count += 1
                        if result["status"] == "completed" and original_loss - result["loss"] > improvement_tolerance:
                            improved = True
                    if count >= max_evaluations:
                        break
                if not improved:
                    steps = {name: step * 0.5 for name, step in steps.items()}
                    if all(steps[p["name"]] <= minimum_step_fraction * (p["bounds"][1] - p["bounds"][0]) for p in parameters):
                        reason = "converged"
                        break
        except (FitCancelled, KeyboardInterrupt) as error:
            reason = str(error) or "interrupted"
        except Exception:
            reason = "failed"
            raise
        finally:
            self.deadline = None
            state = {"status": "failed" if reason == "failed" else "stopped", "reason": reason, "evaluations_this_call": count, "steps": steps,
                     "groups": groups,
                     "best": self.best, "seconds": time.monotonic() - self.started}
            write_json(self.run_dir / "optimization.json", state)
        return state

    def restore(self, candidate_id=None):
        candidate_id = identifier(candidate_id or (self.best or {}).get("candidate_id"))
        source = self.candidates / candidate_id
        result = read_json(source / "result.json")
        if result["status"] != "completed":
            raise ValueError("Only completed candidates can be restored")
        if candidate_hash(source) != result["asset_sha256"]:
            raise ValueError("Candidate snapshot changed; cannot restore")
        if tree_hash(self.run_dir / "input") != read_json(self.run_dir / "run.json")["input_sha256"]:
            raise ValueError("Frozen inputs changed; cannot restore")
        if read_json(source / "parameters.json") != result["parameters"]:
            raise ValueError("Candidate parameter provenance does not match")
        destination = self.run_dir / "exports" / f"{candidate_id}-{time.time_ns()}"
        destination.parent.mkdir(exist_ok=True)
        # Rematerialize with relocated asset references; source candidates stay immutable.
        self.project.materialize(destination, result["parameters"])
        write_json(destination / "source.json", {"candidate_id": candidate_id, "asset_sha256": result["asset_sha256"]})
        return {"status": "completed", "scene": str(destination / "scene.scene.json"), "candidate_id": candidate_id}

    def report(self):
        rows = []
        summary = ""
        if self.best is not None:
            summary = "<h2>Best training candidate</h2><pre>" + html.escape(json.dumps(
                {"candidate_id": self.best["candidate_id"], "loss": self.best["loss"],
                 "parameters": self.best["parameters"], "metrics": self.best["metrics"]},
                ensure_ascii=False, indent=2)) + "</pre>"
        for path in sorted(self.candidates.glob("*/result.json")):
            result = read_json(path)
            title = html.escape(f"{result['candidate_id']} [{result['split']}] {result['status']}")
            details = html.escape(json.dumps(result.get("metrics", result.get("error", {})), ensure_ascii=False, indent=2))
            images = "".join(f'<img alt="Reference, render, overlay, difference x4" src="{html.escape(image.relative_to(self.run_dir).as_posix(), quote=True)}">' for image in path.parent.glob("*-comparison.png"))
            rows.append(f"<article><h2>{title}</h2><pre>{details}</pre>{images}</article>")
        report = self.run_dir / "report.html"
        report.write_text('<!doctype html><meta charset="utf-8"><title>Caustica fitting report</title><style>body{font:16px system-ui;margin:32px;background:#181b21;color:#eee}img{max-width:100%;display:block;margin:12px 0}pre{white-space:pre-wrap}article{border-top:1px solid #555}</style><h1>Caustica multiview fitting</h1><p>Columns: reference, render, overlay, absolute difference ×4. Validation views are excluded from selection.</p>' + summary + "".join(rows), encoding="utf-8")
        return str(report)
