#!/usr/bin/env python
"""Customize the running editor through caustica's embedded Python module.

Run with:
    caustica.exe --pythonScript examples/python/embedded.py

The script intentionally performs one update and returns. A host-side scheduler
can call the same functions every frame when continuous animation is required.

Unlike the other examples this one does not import ``_common``: the embedding
host does not put the script's directory on ``sys.path``, so an embedded script
has to stand on its own.
"""

from __future__ import annotations

import math
import time

import caustica


def configure_realtime(app) -> str:
    """Select the best available realtime path without assuming DLSS support."""
    settings = app.settings
    if settings.is_dlss_rr_supported:
        app.set_realtime_mode(
            standalone_denoiser=False,
            realtime_aa=int(caustica.RealtimeAA.DLSS_RR),
        )
        settings.dlss_mode = int(caustica.DLSSMode.Balanced)
        settings.dlss_rr_preset = int(caustica.DLSSRRPreset.PresetE)
        settings.disable_restirs_with_dlss_rr = True
        return "DLSS-RR"

    app.set_realtime_mode(
        standalone_denoiser=True,
        realtime_aa=int(caustica.RealtimeAA.TAA),
    )
    return "NRD + TAA"


def customize_scene(app) -> None:
    scene = app.scene
    materials = scene.get_materials()
    lights = scene.get_lights()
    print(f"[caustica] scene={app.scene_name} materials={len(materials)} lights={len(lights)}")

    floor = scene.find_material("Floor")
    if floor is not None:
        floor.base_color = (0.2, 0.05, 0.05)
        floor.roughness = 0.85
        floor.metalness = 0.0

    phase = time.monotonic() % (2.0 * math.pi)
    palette = ((1.0, 0.2, 0.2), (0.2, 1.0, 0.4), (0.2, 0.4, 1.0))
    for index, light in enumerate(lights):
        if int(light.light_type) not in {
            int(caustica.LightType.Spot),
            int(caustica.LightType.Point),
        }:
            continue
        base = palette[index % len(palette)]
        light.color = tuple(
            channel * (0.7 + 0.3 * math.sin(phase + index + axis * 1.2))
            for axis, channel in enumerate(base)
        )
        light.intensity = max(50.0, light.intensity)

    settings = app.settings
    settings.bounce_count = 8
    settings.diffuse_bounce_count = 3
    settings.enable_bloom = True
    settings.bloom_intensity = 0.005
    environment = settings.environment_map
    environment.intensity = 1.5
    environment.tint_color = (1.0, 0.95, 0.9)
    environment.rotation_xyz = (0.0, 35.0, 0.0)
    environment.enabled = True
    environment.visible_to_camera = True
    app.set_camera_fov(55.0)


def main() -> None:
    if caustica.MODE != "embed":
        raise RuntimeError("embedded.py must be run by caustica.exe --pythonScript")
    app = caustica.app()
    customize_scene(app)
    mode = configure_realtime(app)
    caustica.log_info(f"Embedded customization complete; realtime mode={mode}")


if __name__ == "__main__":
    main()
