"""Independent Caustica GPU capture of a Blender-fit GLB export."""
import argparse
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from scene_fit import Project
from scene_fit.renderer import CausticaRenderer
from blender_fit.common import write


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("project")
    parser.add_argument("--output-dir", required=True)
    args = parser.parse_args()
    project = Project(args.project)
    output = Path(args.output_dir).resolve()
    project.materialize(output, project.initial)
    renderer = CausticaRenderer()
    try:
        results = renderer.capture(project, output, project.data["views"], lambda: None)
    finally:
        renderer.close()
    ious = {}
    for view in project.data["views"]:
        reference = np.array(Image.open(project.root / view["mask"])) > 127
        actual = results[view["id"]]["mask"]
        ious[view["id"]] = float((reference & actual).sum() / (reference | actual).sum())
    write(output / "verification.json", {"silhouette_iou": ious, "independent_engine": True})
    assert min(ious.values()) > 0.94, ious
    print(ious)


if __name__ == "__main__":
    main()
