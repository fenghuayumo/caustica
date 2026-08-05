"""Shared helpers for caustica Python examples.

Import with a normal installed package::

    import caustica

If that fails, fix the install (``python -m pip install .`` from the repo root)
rather than patching ``sys.path`` in example scripts.
"""

from __future__ import annotations

from pathlib import Path

EXAMPLES_DIR = Path(__file__).resolve().parent
REPO_ROOT = EXAMPLES_DIR.parents[2]
ASSETS_DIR = REPO_ROOT / "Assets"


def resolve_path(path: str | Path) -> Path:
    resolved = Path(path).expanduser()
    if not resolved.is_absolute():
        resolved = Path.cwd() / resolved
    return resolved.resolve()


def resolve_scene_arg(scene_arg: str) -> str:
    """Resolve a scene CLI arg to an absolute path when it exists on disk."""
    path = Path(scene_arg)
    if path.is_file():
        return str(path.resolve())
    assets_candidate = ASSETS_DIR / scene_arg
    if assets_candidate.is_file():
        return str(assets_candidate.resolve())
    return scene_arg


def resolve_output_path(path: str | Path, launch_cwd: Path | None = None) -> Path:
    out = Path(path)
    if not out.is_absolute():
        out = (launch_cwd or Path.cwd()) / out
    return out.resolve()


def bounds_to_center_radius(
    bounds: tuple[tuple[float, float, float], tuple[float, float, float]] | None,
) -> tuple[tuple[float, float, float], float] | None:
    if not bounds:
        return None
    mins, maxs = bounds
    center = tuple((lo + hi) * 0.5 for lo, hi in zip(mins, maxs))
    extent = [hi - lo for lo, hi in zip(mins, maxs)]
    radius = max(max(extent) * 0.5, 0.1)
    return center, radius  # type: ignore[return-value]


def scene_bounds_center_radius(renderer) -> tuple[tuple[float, float, float], float] | None:
    return bounds_to_center_radius(renderer.get_scene_bounds())


def frame_bounds(
    renderer,
    center: tuple[float, float, float],
    radius: float,
    *,
    fov: float = 45.0,
) -> None:
    camera_pos = (
        center[0],
        center[1] + radius * 0.15,
        center[2] + radius * 3.1,
    )
    camera_dir = (
        center[0] - camera_pos[0],
        center[1] - camera_pos[1],
        center[2] - camera_pos[2],
    )
    renderer.set_camera(camera_pos, camera_dir, (0.0, 1.0, 0.0))
    renderer.set_camera_fov(fov)


def run_window_loop(renderer) -> None:
    """Drive a windowed Renderer until the window closes or Ctrl+C."""
    import time

    print("[caustica] Ready. Close window or Ctrl+C to exit.")
    print("[caustica]   Left-click  -> Inspector (Transform)")
    print("[caustica]   Right-click -> Material Editor")
    try:
        while renderer.step(-1.0):
            time.sleep(0.001)
    except KeyboardInterrupt:
        print("\n[caustica] Interrupted.")
