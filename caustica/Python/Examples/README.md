# caustica Python Scripting Examples

caustica exposes a `caustica` Python module in **two complementary modes**:

| Mode | Binary | When | Use cases |
|---|---|---|---|
| **Embed** | `caustica.exe` | Python is hosted inside the running renderer | Live tweaking, debug overlays, capture scripts, gameplay scripting |
| **Extension** | `caustica.pyd` | Python launches the renderer (`python script.py`) | Offline rendering, batch / data generation, headless CI |

Both modes share the same `caustica.Material`, `caustica.Light*`, `caustica.Settings`,
`caustica.Sample` types so a script can be moved between them with minimal
changes.  Inspect `caustica.MODE` (`"embed"` vs `"extension"`) when you need
to branch.

---

## Embed mode

Already shipping inside `caustica.exe`.

* Make sure caustica was built with `caustica_WITH_PYTHON=ON` (default).
* Run the renderer with a startup script:

```
caustica.exe --pythonScript caustica/Python/Examples/example_basic.py
```

* Or run a one-off expression:

```
caustica.exe --pythonExpr "import caustica; print(caustica.app().scene_name)"
```

* From within the running app, open `System -> Python scripting`,
  paste an expression in the multi-line editor and hit `Run inline`.

In embed mode the "current renderer" is the singleton inside `caustica.exe`,
so you reach it via `caustica.app()`.

## Extension mode (offline / headless rendering)

After building the `caustica_py` target, install the extension package from the
repository root:

```
python -m pip install .
python -c "import caustica; print(caustica.MODE)"
```

This creates a local binary wheel from the current `bin/` runtime payload and
installs it into the active Python environment. Then you can drive a brand-new
device + scene from a standalone Python interpreter:

```
python caustica\Python\Examples\offline_render.py ^
       --scene bistro-programmer-art.scene.json ^
       --width 1280 --height 720 --spp 256 --out out.png
```

For quick local development without installing, adding `bin/` to `PYTHONPATH`
still works.

Or interactively:

```python
import caustica
import numpy as np

r = caustica.Renderer(width=1280, height=720, headless=True,
                   scene="builtin:plane_cube")
r.settings.realtime_mode = False
r.settings.accumulation_target = 64
r.step_until_accumulated()

# CPU image buffer (LDR RGBA8) — same source as save_screenshot
img = r.get_pixels()                 # numpy (H, W, 4) uint8
# fb = r.get_framebuffer()           # .pixels is raw bytes
# r.save_screenshot("frame.png")    # optional file write

r.close()
```

`headless=True` creates a DX12/Vulkan device with offscreen back buffers,
without creating an OS window or swap chain.

For package smoke tests, `scene="builtin:plane_cube"` does not require any
mesh file from `Assets`. You can also pass inline scene JSON directly; model
entries may use `builtin:plane`, `builtin:cube`, `builtin:sphere`, or
`builtin:plane_cube`.

## Bindings overview

| Object                          | Purpose                                       |
|---------------------------------|-----------------------------------------------|
| `caustica.MODE`                    | `"embed"` or `"extension"`                    |
| `caustica.Renderer(...)`           | (extension only) creates a new headless device|
| `caustica.builtin_scene_json(...)` | Inline JSON for a builtin primitive scene     |
| `caustica.app()`                   | Returns the current `Sample` renderer         |
| `caustica.settings()`              | Shortcut for `caustica.app().settings`           |
| `Sample.scene`                  | Current loaded `Scene`                        |
| `Sample.set_realtime_mode(...)` | Switch to realtime mode + AA + denoiser       |
| `Sample.set_reference_mode(...)`| Switch to reference accumulation + OIDN       |
| `Scene.get_materials()`         | List of `StandardMaterial` in the current scene |
| `Scene.find_material(name)`     | Lookup by `Name` or `UniqueName`              |
| `Scene.get_lights()`            | List of `Light` (Directional/Spot/Point/Env)  |
| `Sample.set_environment_map`    | Override the scene's HDRI                     |
| `Sample.set_camera_fov`         | Override vertical FOV (degrees)               |
| `Settings.path_tracer_mode`     | caustica.PathTracerMode (Realtime / Reference)   |
| `Settings.realtime_aa`          | caustica.RealtimeAA (Off / TAA / DLSS / DLSS-RR) |
| `Settings.dlss_mode` etc.       | DLSS / DLSS-RR / DLSS-G / Reflex parameters   |
| `Settings.oidn_*`               | OIDN denoiser parameters (reference mode)     |
| `Settings.gaussian_splat_*`     | 3DGS raster, storage, culling, shadow controls|
| `Settings.environment_map`      | Tint/intensity/rotation/visibility of env map |
| `Settings.bounce_count` etc.    | Path tracer / NEE / RTXDI knobs               |
| `Renderer.step()/step_n(n)`     | (extension) drive the loop one frame at a time|
| `Renderer.step_until_accumulated()` | (extension) render to SPP target          |
| `Renderer.save_screenshot(path)`| (extension) write LDR final color to PNG/JPG/BMP |
| `Renderer.get_framebuffer()`    | (extension) LDR RGBA8 CPU buffer (`Framebuffer`) |
| `Renderer.get_pixels()`         | (extension) LDR `(H,W,4) uint8` NumPy array   |
| `caustica.Framebuffer`          | width/height/pixels/shape for get_framebuffer |

### Enums

* `caustica.PathTracerMode` - `Realtime`, `Reference`
* `caustica.RealtimeAA` - `Off`, `TAA`, `DLSS`, `DLSS_RR`
* `caustica.DLSSMode` - `Off`, `MaxPerformance`, `Balanced`, `MaxQuality`, `UltraPerformance`, `UltraQuality`, `DLAA`
* `caustica.DLSSFGMode` - `Off`, `On`, `Auto`
* `caustica.DLSSRRPreset` - `Default`, `PresetA`..`PresetH`
* `caustica.ReflexMode` - `Off`, `LowLatency`, `LowLatencyWithBoost`
* `caustica.OidnPasses` - `ColorOnly`, `Albedo`, `AlbedoNormal`
* `caustica.OidnPrefilter` - `None_`, `Fast`, `Accurate`
* `caustica.OidnQuality` - `Fast`, `Balanced`, `High`
* `caustica.GaussianSplatSortMode` - `GpuSort`, `StochasticSplats`
* `caustica.GaussianSplatStorageFormat` - `Float32`, `Float16`, `Uint8`
* `caustica.GaussianSplatFrustumCulling` - `Disabled`, `AtDistanceStage`, `AtRasterStage`
* `caustica.GaussianSplatShadowMode` - `Disabled`, `Hard`, `Soft`
* `caustica.GaussianSplatFTBSyncMode` - `Disabled`, `Interlock`

All enums support `int(caustica.<EnumName>.<value>)` so they can be assigned
directly to settings fields that store ints.

Inspect the full surface with:

```python
import caustica
help(caustica)
```
