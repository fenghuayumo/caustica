"""Shared helpers for the caustica Python extension examples.

Every example in this folder imports ``caustica`` as a normally installed
package. If the import fails, fix the install (``python -m pip install .`` from
the repository root) rather than patching ``sys.path`` in an example script.

This module is not an executable example. It holds the renderer setup, argument
parsing, denoiser selection, camera framing and output helpers that would
otherwise be copy-pasted into every script.

``embedded.py`` deliberately does not use this module: it runs inside
``caustica.exe``, which does not put the script directory on ``sys.path``.
"""

from __future__ import annotations

import argparse
import time
from pathlib import Path

EXAMPLES_DIR = Path(__file__).resolve().parent
REPO_ROOT = EXAMPLES_DIR.parents[1]
ASSETS_DIR = REPO_ROOT / "Assets"

#: Denoiser / anti-aliasing paths accepted by :func:`apply_realtime_mode`.
REALTIME_DENOISERS = ("auto", "off", "taa", "nrd", "dlss", "dlss-rr")


# --------------------------------------------------------------------------
# Package import
# --------------------------------------------------------------------------


def import_caustica():
    """Import the installed extension, with an actionable message on failure."""
    try:
        import caustica
    except ImportError as exc:
        raise SystemExit(
            f"Could not import caustica ({exc}).\n"
            "Install the extension from the repository root:\n"
            "    python -m pip install ."
        ) from exc
    return caustica


# --------------------------------------------------------------------------
# Paths
# --------------------------------------------------------------------------


def resolve_path(path: str | Path) -> Path:
    resolved = Path(path).expanduser()
    if not resolved.is_absolute():
        resolved = Path.cwd() / resolved
    return resolved.resolve()


def resolve_scene_arg(scene_arg: str) -> str:
    """Resolve a scene CLI arg to an absolute path when it exists on disk.

    Builtin references (``builtin:plane_cube``) and inline JSON are passed
    through unchanged for the engine to interpret.
    """
    path = Path(scene_arg)
    if path.is_file():
        return str(path.resolve())

    direct = ASSETS_DIR / scene_arg
    if direct.is_file():
        return str(direct.resolve())

    name = Path(scene_arg).name
    aliases = [name]
    if name == "default.json":
        aliases.append("default.scene.json")
    elif name.endswith(".json") and not name.endswith(".scene.json"):
        aliases.append(Path(name).stem + ".scene.json")

    scenes_root = ASSETS_DIR / "scenes"
    if scenes_root.is_dir():
        for candidate in scenes_root.rglob("*"):
            if candidate.is_file() and candidate.name in aliases:
                return str(candidate.resolve())

    return scene_arg


def resolve_output_path(path: str | Path, launch_cwd: Path | None = None) -> Path:
    out = Path(path)
    if not out.is_absolute():
        out = (launch_cwd or Path.cwd()) / out
    return out.resolve()


def require_input_file(path: str | Path, description: str, *, hint: str = "") -> Path:
    """Resolve a required input file, failing with a message the user can act on."""
    resolved = Path(path).expanduser()
    if not resolved.is_absolute():
        resolved = Path.cwd() / resolved
    resolved = resolved.resolve()
    if not resolved.is_file():
        message = f"{description} not found: {resolved}"
        if hint:
            message += f"\n{hint}"
        raise SystemExit(message)
    return resolved


# --------------------------------------------------------------------------
# Argument parsing
# --------------------------------------------------------------------------


def add_device_args(parser: argparse.ArgumentParser, *, width: int = 1280, height: int = 720) -> None:
    """Resolution and GPU selection flags shared by every example."""
    group = parser.add_argument_group("device")
    group.add_argument("--width", type=int, default=width)
    group.add_argument("--height", type=int, default=height)
    group.add_argument("--vulkan", action="store_true", help="Use the Vulkan backend instead of D3D12.")
    group.add_argument(
        "--adapter",
        default="auto",
        help="GPU selector: auto, index:N, name:text, uuid:hex, or luid:hex. "
        "List devices with caustica.enumerate_adapters().",
    )


def add_quality_args(
    parser: argparse.ArgumentParser, *, spp: int = 64, bounces: int = 8
) -> None:
    """Sampling flags shared by the reference-rendering examples."""
    group = parser.add_argument_group("quality")
    group.add_argument("--spp", type=int, default=spp, help="Reference samples per pixel.")
    group.add_argument("--bounces", type=int, default=bounces)


def add_window_args(parser: argparse.ArgumentParser, *, headless: bool = True) -> None:
    """Mutually exclusive ``--headless`` / ``--window`` pair."""
    group = parser.add_mutually_exclusive_group()
    group.add_argument(
        "--headless",
        action="store_true",
        default=headless,
        help="Render offscreen and save an image." + (" (default)" if headless else ""),
    )
    group.add_argument(
        "--window",
        "--no-headless",
        dest="headless",
        action="store_false",
        help="Open an interactive preview window." + ("" if headless else " (default)"),
    )


def validate_positive(args: argparse.Namespace, *names: str) -> None:
    for name in names:
        value = getattr(args, name, None)
        if value is not None and value <= 0:
            raise SystemExit(f"--{name.replace('_', '-')} must be positive (got {value})")


# --------------------------------------------------------------------------
# Renderer construction
# --------------------------------------------------------------------------


def make_renderer(
    caustica,
    args: argparse.Namespace,
    *,
    scene: str,
    realtime: bool,
    headless: bool | None = None,
    accumulation_target: int = 1,
):
    """Create a Renderer from the flags added by :func:`add_device_args`."""
    return caustica.Renderer(
        width=args.width,
        height=args.height,
        headless=getattr(args, "headless", True) if headless is None else headless,
        vulkan=getattr(args, "vulkan", False),
        adapter=getattr(args, "adapter", "auto"),
        scene=scene,
        realtime=realtime,
        accumulation_target=accumulation_target,
    )


def apply_common_settings(
    renderer, caustica, *, bounces: int = 8, tone_mapping: bool = True, nee: bool = True
) -> None:
    settings = renderer.settings
    settings.bounce_count = bounces
    settings.use_nee = nee
    settings.enable_tone_mapping = tone_mapping


# --------------------------------------------------------------------------
# Render modes
# --------------------------------------------------------------------------


def apply_reference_mode(
    renderer, caustica, *, spp: int, oidn: bool = True, gpu: bool = True
) -> str:
    """Configure reference accumulation, optionally with OIDN. Returns a label."""
    renderer.app.set_reference_mode(
        spp=spp,
        oidn=oidn,
        oidn_quality=int(caustica.OidnQuality.High),
        oidn_passes=int(caustica.OidnPasses.AlbedoNormal),
        oidn_prefilter=int(caustica.OidnPrefilter.Accurate),
    )
    renderer.settings.oidn_use_gpu = gpu
    if oidn:
        renderer.settings.oidn_apply()
    return f"reference {spp} spp" + (" + OIDN" if oidn else "")


def apply_realtime_mode(app, caustica, requested: str = "auto") -> str:
    """Select a realtime denoiser / AA path, falling back when unsupported.

    ``app`` is a ``caustica.App`` (``renderer.app`` in extension mode). Returns a
    label describing the path that was actually selected, which may differ from
    ``requested`` when the GPU or driver does not support DLSS.
    """
    if requested not in REALTIME_DENOISERS:
        raise SystemExit(
            f"unknown denoiser {requested!r}; choose from {', '.join(REALTIME_DENOISERS)}"
        )

    settings = app.settings
    denoiser = "nrd" if requested == "auto" else requested

    if denoiser == "dlss-rr" and not settings.is_dlss_rr_supported:
        print("[caustica] DLSS-RR unavailable on this device; falling back to DLSS.")
        denoiser = "dlss"
    if denoiser == "dlss" and not getattr(settings, "is_dlss_supported", False):
        print("[caustica] DLSS unavailable on this device; falling back to NRD + TAA.")
        denoiser = "nrd"

    if denoiser == "dlss-rr":
        app.set_realtime_mode(
            standalone_denoiser=False, realtime_aa=int(caustica.RealtimeAA.DLSS_RR)
        )
        settings.dlss_mode = int(caustica.DLSSMode.Balanced)
        settings.dlss_rr_preset = int(caustica.DLSSRRPreset.PresetE)
        settings.disable_restirs_with_dlss_rr = True
        return "realtime DLSS-RR"
    if denoiser == "dlss":
        app.set_realtime_mode(
            standalone_denoiser=False, realtime_aa=int(caustica.RealtimeAA.DLSS)
        )
        settings.dlss_mode = int(caustica.DLSSMode.Balanced)
        return "realtime DLSS"
    if denoiser == "nrd":
        app.set_realtime_mode(
            standalone_denoiser=True, realtime_aa=int(caustica.RealtimeAA.TAA)
        )
        return "realtime NRD + TAA"
    if denoiser == "taa":
        app.set_realtime_mode(
            standalone_denoiser=False, realtime_aa=int(caustica.RealtimeAA.TAA)
        )
        return "realtime TAA"

    app.set_realtime_mode(standalone_denoiser=False, realtime_aa=int(caustica.RealtimeAA.Off))
    return "realtime without denoising"


# --------------------------------------------------------------------------
# Camera framing
# --------------------------------------------------------------------------


def normalize(v) -> tuple[float, float, float]:
    length = sum(float(x) * float(x) for x in v) ** 0.5
    if length <= 1e-8:
        return (0.0, 0.0, 1.0)
    return (float(v[0]) / length, float(v[1]) / length, float(v[2]) / length)


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
    """Place the camera so a sphere of ``radius`` around ``center`` is in frame."""
    camera_pos = (center[0], center[1] + radius * 0.15, center[2] + radius * 3.1)
    camera_dir = (
        center[0] - camera_pos[0],
        center[1] - camera_pos[1],
        center[2] - camera_pos[2],
    )
    renderer.set_camera(camera_pos, camera_dir, (0.0, 1.0, 0.0))
    renderer.set_camera_fov(fov)


# --------------------------------------------------------------------------
# Output
# --------------------------------------------------------------------------


def save_screenshot(renderer, path: str | Path, *, launch_cwd: Path | None = None) -> Path:
    out = resolve_output_path(path, launch_cwd)
    out.parent.mkdir(parents=True, exist_ok=True)
    if not renderer.save_screenshot(str(out)):
        raise RuntimeError(f"failed to save screenshot: {out}")
    return out


def render_reference_to(
    renderer, path: str | Path, *, launch_cwd: Path | None = None, label: str = ""
) -> Path:
    """Accumulate until the reference target is reached, then save a screenshot."""
    started = time.perf_counter()
    frames = renderer.step_until_accumulated()
    out = save_screenshot(renderer, path, launch_cwd=launch_cwd)
    suffix = f" [{label}]" if label else ""
    print(
        f"[caustica] Saved {out} after {frames} engine frame(s) "
        f"in {time.perf_counter() - started:.2f}s{suffix}"
    )
    return out


def run_window_loop(renderer, on_frame=None) -> None:
    """Drive a windowed Renderer until the window closes or Ctrl+C.

    ``on_frame`` is called with the elapsed wall-clock seconds before each step,
    which lets animated examples drive per-frame scene edits without writing
    their own loop.
    """
    print("[caustica] Ready. Close window or Ctrl+C to exit.")
    print("[caustica]   Left-click  -> Inspector (Transform)")
    print("[caustica]   Right-click -> Material Editor")
    started = time.monotonic()
    frames = 0
    try:
        while True:
            if on_frame is not None:
                on_frame(time.monotonic() - started)
            if not renderer.step(-1.0):
                break
            frames += 1
            time.sleep(0.001)
    except KeyboardInterrupt:
        print(f"\n[caustica] Interrupted after {frames} frame(s).")
