#!/usr/bin/env python3
"""Host-surface bindings: render products, camera snapshot, lights, mesh flags."""

from __future__ import annotations

import argparse
import os
import traceback
from pathlib import Path


def log(message: str) -> None:
    os.write(1, (message + "\n").encode("utf-8", errors="replace"))


def close3(actual, expected, atol: float = 1e-3) -> bool:
    return all(abs(float(a) - float(b)) <= atol for a, b in zip(actual, expected))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)

    import caustica

    failures: list[str] = []
    with caustica.EngineApp.create(
        width=320,
        height=240,
        headless=True,
        scene="builtin:plane_cube",
        realtime=True,
    ) as engine:
        if not engine.is_scene_ready and not engine.wait_until_ready(timeout_seconds=120.0):
            raise RuntimeError("plane_cube scene did not become ready")

        if engine.render_products():
            failures.append("render_products() was not empty before spawn_camera")

        aovs = int(caustica.Aov.rgb) | int(caustica.Aov.depth)
        wrist = engine.spawn_camera(
            name="wrist",
            local_translation=(0.2, 1.0, 2.0),
            aovs=aovs,
        )
        if wrist is None or not wrist.is_camera:
            failures.append("spawn_camera did not return a camera")
        else:
            products = engine.render_products()
            if len(products) != 1:
                failures.append(f"render_products() count={len(products)}")
            else:
                product = products[0]
                if product.name != "wrist":
                    failures.append(f"render product name={product.name}")
                if product.camera is None or product.camera.name != "wrist":
                    failures.append("render product camera was not the spawned wrist camera")
                if int(product.aovs) != aovs:
                    failures.append(f"render product aovs={int(product.aovs)} expected {aovs}")

            look = ((0.2, 1.1, 2.0), (0.0, 0.0, -1.0), (0.0, 1.0, 0.0))
            engine.apply_visual_snapshot({}, cameras={"wrist": look})
            pose = wrist.camera_pose
            if not (close3(pose[0], look[0]) and close3(pose[1], look[1]) and close3(pose[2], look[2])):
                failures.append(f"camera snapshot pose={pose}")

        sun = engine.spawn_directional_light(name="BindingSun", irradiance=2.0)
        if sun is None or not sun.is_light:
            failures.append("spawn_directional_light failed")
        else:
            sun.enabled = False
            if sun.enabled:
                failures.append("directional light stayed enabled")
            sun.enabled = True

        sky = engine.spawn_environment_light(name="BindingSky", rotation=10.0)
        if sky is None:
            failures.append("spawn_environment_light failed")
        else:
            sky.environment_rotation = 35.0
            sky.radiance_scale = (2.0, 3.0, 4.0)
            if abs(float(sky.environment_rotation) - 35.0) > 1e-4:
                failures.append(f"environment rotation={sky.environment_rotation}")
            if len(sky.rotation) != 4:
                failures.append(f"local rotation was overwritten: {sky.rotation}")
            scale = sky.radiance_scale
            if not close3(scale, (2.0, 3.0, 4.0)):
                failures.append(f"radiance_scale={scale}")

        meshes = engine.scene.get_mesh_entities()
        if not meshes:
            failures.append("builtin scene has no mesh entity")
        else:
            engine.request_mesh_accel_rebuild(meshes[0], reset_accumulation=False)
            try:
                engine.apply_geometry_sequence(meshes[0], 0.0)
                failures.append("apply_geometry_sequence succeeded on an entity with no sequence")
            except RuntimeError:
                pass

        captured = engine.capture_sensor_outputs()
        wrist_out = next((item for item in captured if item.name == "wrist"), None)
        if wrist_out is None:
            failures.append("capture did not return the wrist product")
        else:
            if wrist_out.camera is None or wrist_out.camera.name != "wrist":
                failures.append("SensorOutput.camera was not the wrist camera")
            if wrist_out.linear_rgb is not None:
                failures.append("linear_rgb was filled without Aov.linear_rgb")

    if failures:
        for failure in failures:
            log("FAIL " + failure)
        return 1
    log("binding surface ok")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception:
        traceback.print_exc()
        raise SystemExit(1)
