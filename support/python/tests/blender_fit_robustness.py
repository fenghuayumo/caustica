"""Real Blender regression from a calibrated project and a reviewed geometry.

Tests controlled initial pose/profile errors, not automatic segmentation or
online model quality. Input photos/masks are never regenerated from geometry.
"""
import argparse
import copy
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from blender_fit.common import read, write, validate_geometry
from blender_fit.refinement import dimensions
from blender_fit.workflow import find_blender, render, run, score, validate_project


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", required=True)
    parser.add_argument("--geometry", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--blender")
    args = parser.parse_args()
    project = Path(args.project).resolve()
    data = validate_project(project)
    geometry = validate_geometry(read(args.geometry))
    root = Path(args.output_dir).resolve()
    root.mkdir(parents=True, exist_ok=False)
    blender = find_blender(args.blender)
    render(blender, geometry, data["views"], root / "reviewed-baseline")
    baseline = score(project.parent, root / "reviewed-baseline", data["views"], "train")
    cases = []
    for field in ("position", "rotation", "profile"):
        candidate = copy.deepcopy(geometry)
        part = candidate["parts"][0]
        if field == "position":
            # Deliberately not a search step: recovery requires finer sweeps.
            part[field][0] += 0.073 * max(dimensions(part))
            budget = 37
        elif field == "rotation":
            part[field][0] += 7.3
            budget = 37
        else:
            indices = [i for i,p in enumerate(candidate["parts"]) if p["kind"] == "lathe"]
            if not indices:
                continue
            part = candidate["parts"][indices[0]]
            index = len(part["profile"]) // 2
            part["profile"][index][1] *= 1.13
            budget = 1 + 6 * sum(len(p.get("profile", [])) for p in candidate["parts"])
        initial = root / (field + "-initial.json")
        write(initial, candidate)
        state = run(project, root / field, blender, initial_geometry=initial,
                    max_evaluations=budget, model_rounds=0, numeric_fields=(field,))
        perturbed = state["history"][0]["train"]["loss"]
        excess = perturbed - baseline["loss"]
        recovered = perturbed - state["best_loss"]
        # Degenerate/symmetric directions cannot constitute a recovery test.
        observable = excess > 0.002
        passed = recovered >= 0.5 * excess if observable else None
        cases.append({"field": field, "baseline": baseline["loss"], "perturbed": perturbed,
                      "best": state["best_loss"], "validation": state["validation"],
                      "observable": observable, "passed": passed,
                      "evaluations": state["evaluations"]})
    write(root / "verification.json", {"cases": cases, "scope": "controlled initial errors; training-only selection"})
    print(read(root / "verification.json"))
    if any(c["passed"] is False for c in cases):
        raise SystemExit("Geometry refinement failed to recover half the excess error")
    if not any(c["observable"] for c in cases):
        raise SystemExit("No observable perturbation: choose a less symmetric reference geometry")


if __name__ == "__main__":
    main()
