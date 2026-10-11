#!/usr/bin/env python3
"""Standalone calibrated Blender authoring, separate from Caustica appearance fitting."""
import argparse
import json

from blender_fit.workflow import find_blender, prepare, run, validate_project


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    validate = commands.add_parser("validate")
    validate.add_argument("project")
    for command in ("prepare", "auto"):
        p = commands.add_parser(command)
        p.add_argument("--colmap", required=True)
        p.add_argument("--images", required=True)
        p.add_argument("--masks")
        p.add_argument("--output-dir", required=True)
        p.add_argument("--max-views", type=int, default=8)
        p.add_argument("--max-dimension", type=int, default=512)
        p.add_argument("--meters-per-unit", type=float)
        p.add_argument("--target", default="the central foreground object")
    for p in (commands.choices["auto"], commands.add_parser("run")):
        if p is commands.choices["run"]:
            p.add_argument("project")
            p.add_argument("--output-dir", required=True)
        p.add_argument("--provider-config")
        p.add_argument("--geometry", help="Initial declarative geometry JSON; allows offline fitting")
        p.add_argument("--blender")
        p.add_argument("--max-evaluations", type=int, default=30)
        p.add_argument("--model-rounds", type=int, default=3)
        p.add_argument("--numeric-fields", nargs="+", choices=("size", "position", "rotation", "profile"),
                       default=["size", "position", "rotation", "profile"],
                       help="Fields to refine, in priority order; training silhouette plus visual review when a provider is configured")
        p.add_argument("--point-outlier-iqr", type=float, default=3.0,
                       help="Sparse cloud outlier fence (default 3 IQR); 0 disables it for thin/disconnected objects")
    args = parser.parse_args()
    if args.command == "validate":
        data = validate_project(args.project)
        print(json.dumps({"status": "valid", "views": len(data["views"])}))
        return
    if args.command in ("prepare", "auto"):
        directory = args.output_dir if args.command == "prepare" else args.output_dir + "-input"
        project = prepare(args.colmap, args.images, directory, masks_dir=args.masks,
                          max_views=args.max_views, max_dimension=args.max_dimension,
                          meters_per_unit=args.meters_per_unit, target=args.target)
        if args.command == "prepare":
            print(json.dumps({"project": str(project)}))
            return
    else:
        project = args.project
    state = run(project, args.output_dir, find_blender(args.blender), provider_config=args.provider_config,
                initial_geometry=args.geometry, max_evaluations=args.max_evaluations, model_rounds=args.model_rounds,
                numeric_fields=args.numeric_fields, point_outlier_iqr=args.point_outlier_iqr or None)
    print(json.dumps(state, ensure_ascii=False, indent=2))
    if state["quality"] != "within_geometry_tolerance":
        parser.exit(2, "Geometry acceptance pending; inspect state.json/report.html before appearance fitting.\n")


if __name__ == "__main__":
    main()
