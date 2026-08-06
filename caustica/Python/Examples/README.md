# caustica Python Scripting Examples

caustica exposes a `caustica` Python module in **two complementary modes**:

| Mode | Binary | When | Use cases |
|---|---|---|---|
| **Embed** | `caustica.exe` | Python is hosted inside the running renderer | Live tweaking, debug overlays, capture scripts |
| **Extension** | `caustica.pyd` | Python launches the renderer (`python script.py`) | Offline rendering, batch / data generation, headless CI |

Both modes share the same `caustica.Material`, `caustica.SceneEntity`, `caustica.settings`,
`caustica.Sample` types. Inspect `caustica.MODE` (`"embed"` vs `"extension"`) when you need
to branch.

Full API reference: [py_caustica.md](../../../py_caustica.md).

Install once (`python -m pip install .`), then `import caustica` works like any package.
Shared path / framing helpers live in [`_common.py`](_common.py).

---

## Embed mode

```
caustica.exe --pythonScript caustica/Python/Examples/example_basic.py
caustica.exe --pythonExpr "import caustica; print(caustica.app().scene_name)"
```

In-app: `System -> Python scripting` → paste an expression → `Run inline`.
Reach the host via `caustica.app()`.

## Extension mode

```
python -m pip install .
python caustica/Python/Examples/offline_render.py ^
       --scene bistro-programmer-art.scene.json ^
       --width 1280 --height 720 --spp 256 --out out.png
```

```python
import caustica

with caustica.Renderer(
    width=1280, height=720, headless=True,
    scene="builtin:plane_cube",
    realtime=False,
    accumulation_target=64,
) as r:
    r.app.set_reference_mode(spp=64, oidn=False)
    r.step_until_accumulated()
    img = r.get_pixels()          # (H, W, 4) uint8
    r.save_screenshot("frame.png")
```

Runtime spawn (same path as C++ `SceneSpawn`):

```python
node = r.app.spawn_from_file("Models/GlassSphere/GlassSphere.gltf")
if node:
    node.translation = (1.5, 0.5, 0.0)
    r.app.reset_accumulation()
```

## Example scripts

| File | Purpose |
|---|---|
| `_common.py` | Shared path / framing helpers |
| `offline_render.py` | Headless reference render + screenshot |
| `realtime_render.py` | Realtime / denoiser smoke (TAA, NRD, DLSS, OIDN) |
| `launch_default_scene.py` | Builtin `plane_cube`, optional mesh import / FPS / OIDN |
| `render_default_scene.py` | `Assets/default.json` Hybrid 3DGS + 3DGRT |
| `render_default_scene_animated.py` | Default scene + mesh deformation |
| `3dgs_example.py` | 3DGS interactive / reference+OIDN / realtime+DLSS |
| `render_gs_colmap_views.py` | COLMAP-view 3DGS with pinhole intrinsics |
| `test_intrinsics_demo.py` | Off-center `set_camera_intrinsics` |
| `test_envmap_proc_sky.py` | HDRI + procedural sky cases |
| `example_basic.py` | Embed: materials / lights / env |
| `example_modes_dlss_oidn.py` | Embed/extension: realtime DLSS-RR + reference OIDN |
| `example_animate_lights.py` | Embed: per-frame light color animation |

### Removed (redundant)

- `render_assets.py` → merged into `launch_default_scene.py` (`--scene`, `--fps-test`)
- `render_m_plate.py` → use `launch_default_scene.py --obj-test --obj-path ... [--albedo ...]`
- `test_splat_interactive.py` → merged into `3dgs_example.py` (`--mode interactive|batch|...`)

Inspect the full surface with:

```python
import caustica
help(caustica)
```
