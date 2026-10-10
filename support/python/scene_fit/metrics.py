"""Explicit display-space metrics; invalid/missing depths never count as zero loss."""
from __future__ import annotations

import numpy as np
from PIL import Image


def evaluate(reference, rendered, reference_mask, rendered_mask, weights, reference_depth=None, rendered_depth=None):
    reference = np.asarray(reference, dtype=np.float64)
    rendered = np.asarray(rendered, dtype=np.float64)
    reference_mask = np.asarray(reference_mask, dtype=bool)
    rendered_mask = np.asarray(rendered_mask, dtype=bool)
    if reference.shape != rendered.shape or reference.shape != reference_mask.shape + (3,) or rendered_mask.shape != reference_mask.shape:
        raise ValueError("RGB and mask dimensions must agree; resampling is not implicit")
    if not np.isfinite(reference).all() or not np.isfinite(rendered).all():
        raise ValueError("Nonfinite RGB")
    if not reference_mask.any():
        raise ValueError("Empty reference foreground")
    union = reference_mask | rendered_mask
    iou = float(np.count_nonzero(reference_mask & rendered_mask) / np.count_nonzero(union))
    terms = {"rgb": float(np.abs(reference - rendered)[union].mean()), "silhouette": 1 - iou}
    result = {"iou": iou, "rgb_mae": terms["rgb"], "depth_mae_m": None, "depth_valid_pixels": 0}
    if reference_depth is not None:
        if rendered_depth is None or reference_depth.shape != reference_mask.shape or rendered_depth.shape != reference_mask.shape:
            raise ValueError("Depth dimensions must match masks")
        valid = reference_mask & np.isfinite(reference_depth) & (reference_depth > 0)
        # Missing rendered foreground is penalized by silhouette, never accepted as depth=0.
        comparable = valid & rendered_mask & np.isfinite(rendered_depth) & (rendered_depth > 0)
        result["depth_valid_pixels"] = int(comparable.sum())
        if valid.any() and not comparable.any() and weights.get("depth", 0) > 0:
            raise ValueError("No comparable rendered depth; candidate cannot be evaluated")
        if comparable.any():
            terms["depth"] = float(np.abs(reference_depth[comparable] - rendered_depth[comparable]).mean())
            result["depth_mae_m"] = terms["depth"]
    active = sum(weights.get(key, 0) for key in terms)
    if active <= 0:
        raise ValueError("No available positively weighted loss terms")
    result["loss"] = sum(weights.get(key, 0) * value for key, value in terms.items()) / active
    return result


def load_reference(project, view):
    with Image.open(project.reference_path(view["image"])) as image:
        rgb = np.asarray(image.convert("RGB"), dtype=np.float64) / 255
    with Image.open(project.reference_path(view["mask"])) as image:
        mask = np.asarray(image.convert("L")) > 127
    depth = np.load(project.reference_path(view["depth"]), allow_pickle=False) if "depth" in view else None
    return rgb, mask, depth


def comparison_image(reference, rendered):
    reference = np.clip(reference * 255, 0, 255).astype(np.uint8)
    rendered = np.clip(rendered * 255, 0, 255).astype(np.uint8)
    overlay = ((reference.astype(np.float64) + rendered) / 2).astype(np.uint8)
    difference = np.clip(np.abs(reference.astype(float) - rendered) * 4, 0, 255).astype(np.uint8)
    return Image.fromarray(np.concatenate([reference, rendered, overlay, difference], axis=1))
