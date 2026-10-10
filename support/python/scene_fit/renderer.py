"""Serial reference captures through the public EngineApp API."""
from __future__ import annotations

import platform
import time

import numpy as np
from PIL import Image


class CausticaRenderer:
    def __init__(self, *, vulkan=False, adapter="auto"):
        self.vulkan = vulkan
        self.adapter = adapter
        self.engine = None

    def close(self):
        if self.engine is not None:
            self.engine.shutdown()
            self.engine = None

    def capture(self, project, candidate, views, check_cancel):
        import caustica
        if not hasattr(caustica, "EngineApp") or not hasattr(caustica, "SceneDocument"):
            raise RuntimeError("Native caustica extension unavailable. Install the wheel or set PYTHONPATH to the built bin directory.")
        config = project.render
        scene_path = candidate / "scene.scene.json"
        # Validate the authored document before GPU publication.
        document = caustica.SceneDocument.load(str(scene_path))
        if self.engine is None:
            self.engine = caustica.EngineApp.create(
                width=config["width"], height=config["height"], headless=True,
                vulkan=self.vulkan, adapter=self.adapter, scene=str(scene_path),
                realtime=False, accumulation_target=config["spp"])
        else:
            self.engine.set_scene(str(scene_path), force_reload=True)
        engine = self.engine
        if not engine.wait_until_ready(timeout_seconds=config["timeout_seconds"]):
            raise TimeoutError("Scene load did not become ready")
        check_cancel()
        engine.use_camera(None)
        engine.set_reference_mode(spp=config["spp"], oidn=False)
        settings = engine.settings
        settings.accumulation_prewarm_realtime_caches = False
        settings.bounce_count = config["bounces"]
        settings.use_nee = True
        settings.enable_tone_mapping = True
        settings.enable_bloom = False
        settings.camera_aperture = 0.0
        settings.tone_mapping_params.auto_exposure = False
        settings.tone_mapping_params.exposure_compensation = config["exposure_compensation"]
        settings.tone_mapping_params.camera_lut_enabled = False
        settings.tone_mapping_params.tone_map_operator = caustica.ToneMapOperator.AgX
        # Resolve authored roots to all expanded mesh leaves, including prefab children.
        items = []
        offset = 0
        generation = None
        while True:
            page = engine.inspect_scene(offset=offset, limit=1000)
            if page["status"] != "completed":
                raise RuntimeError(f"Inspection failed: {page}")
            if generation is not None and generation != page["scene_generation"]:
                raise RuntimeError("Scene changed during inspection")
            generation = page["scene_generation"]
            items.extend(page["entities"])
            if len(page["entities"]) < 1000:
                break
            offset += len(page["entities"])
        by_id = {item["runtime_id"]: item for item in items}
        selected = set()
        roots = [item for item in items if item["authored_id"] in project.data["foreground_entities"] and item["authored_scope_runtime_id"] is None]
        if len(roots) != len(project.data["foreground_entities"]):
            raise RuntimeError("Foreground authored roots did not publish")
        pending = [item["runtime_id"] for item in roots]
        while pending:
            runtime_id = pending.pop()
            if runtime_id not in selected:
                selected.add(runtime_id)
                pending.extend(by_id[runtime_id]["children_runtime_ids"])
        mesh_count = 0
        foreground_instance_ids = []
        mesh_entities = {entity.path: entity for entity in engine.scene.get_mesh_entities()}
        for item in items:
            entity = mesh_entities.get(item["path"])
            if entity is not None and entity.is_mesh:
                if item["runtime_id"] in selected:
                    foreground_instance_ids.append(entity.instance_id)
                    mesh_count += 1
        if not mesh_count:
            raise RuntimeError("Foreground has no renderable mesh leaves")
        outputs = {}
        if len(set(foreground_instance_ids)) != len(foreground_instance_ids) or 0 in foreground_instance_ids:
            raise RuntimeError("Foreground instance IDs must be unique and nonzero")
        all_instance_ids = [entity.instance_id for entity in mesh_entities.values()]
        if any(all_instance_ids.count(value) != 1 for value in foreground_instance_ids):
            raise RuntimeError("Foreground instance ID collides with another mesh")
        aovs = caustica.Aov.rgb | caustica.Aov.instance_id | caustica.Aov.depth | caustica.Aov.linear_rgb
        for view in views:
            check_cancel()
            camera = view["camera"]
            engine.set_camera_pos_dir_up(camera["position"], camera["direction"], camera["up"])
            k = camera["intrinsics"]
            engine.set_camera_intrinsics(k["fx"], k["fy"], k["cx"], k["cy"], config["width"], config["height"])
            engine.reset_temporal_history()
            engine.reset_accumulation()
            started = time.monotonic()
            # A completed previous camera must not skip the first frame consuming reset.
            frames = 0
            while True:
                check_cancel()
                if time.monotonic() - started > config["timeout_seconds"]:
                    raise TimeoutError(f"Render timed out: {view['id']}")
                if not engine.step_frame(0.0):
                    raise RuntimeError("Engine stopped during capture")
                frames += 1
                # step_frame dispatches asynchronously. Sensor readback fences the render
                # thread before reading completion, preventing capture of the previous view.
                engine.read_sensor_output(caustica.Aov.none)
                if engine.accumulation_completed and engine.accumulation_sample_index >= config["spp"]:
                    break
            output = engine.read_sensor_output(aovs)
            rgb = np.asarray(engine.get_pixels())[..., :3].copy()
            mask = np.isin(np.asarray(output.instance_id), foreground_instance_ids)
            depth = np.asarray(output.depth).copy()
            expected = (config["height"], config["width"])
            if rgb.shape != expected + (3,) or mask.shape != expected or depth.shape != expected:
                raise RuntimeError("AOV size differs from configured reference size")
            live = engine.inspect_scene(limit=1)
            if live["scene_generation"] != generation:
                raise RuntimeError("Scene generation changed during capture")
            directory = candidate / "renders" / view["id"]
            directory.mkdir(parents=True)
            Image.fromarray(rgb).save(directory / "rgb.png")
            Image.fromarray(mask.astype(np.uint8) * 255).save(directory / "mask.png")
            np.save(directory / "depth.npy", depth, allow_pickle=False)
            if output.linear_rgb is not None:
                np.save(directory / "linear_rgb.npy", np.asarray(output.linear_rgb), allow_pickle=False)
            outputs[view["id"]] = {
                "rgb": rgb.astype(np.float64) / 255, "mask": mask, "depth": depth,
                "provenance": {"document_session_id": document.session_id, "document_revision": document.revision,
                               "scene_generation": generation, "frame_index": engine.frame_index,
                               "accumulation_samples": engine.accumulation_sample_index,
                               "frames": frames, "seconds": time.monotonic() - started,
                               "camera": camera, "render": config, "denoiser": "off", "tone_map": "AgX",
                               "foreground_instance_ids": foreground_instance_ids,
                               "comparison_space": "display_rgb", "caustica_version": getattr(caustica, "__version__", "unknown"),
                               "adapter": str(engine.selected_adapter), "backend": "vulkan" if self.vulkan else "d3d12",
                               "platform": platform.platform(), "python": platform.python_version()}}
        return outputs
