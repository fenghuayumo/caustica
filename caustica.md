# Caustica API

C++ `caustica::EngineApp` and Python `caustica.EngineApp` are the same operations: same argument order, same units, same effects. C++ is camelCase; Python is snake_case.

```cpp
#include <caustica.h>

auto engine = caustica::EngineApp::create({
    .width = 1280, .height = 720, .headless = true,
    .scene = "builtin:plane_cube",
});
engine->setCameraVerticalFOV(0.610865f);  // radians
engine->stepFrame();
engine->saveScreenshot("frame.png");
```

```python
import math
import caustica

with caustica.EngineApp.create(
    width=1280, height=720, headless=True,
    scene="builtin:plane_cube",
) as engine:
    engine.set_camera_vertical_fov(math.radians(35.0))
    engine.step_frame()
    engine.save_screenshot("frame.png")
```

| Topic | Rule |
| --- | --- |
| FOV | Vertical field of view is **radians**. |
| Cameras | A scene can have many camera entities. `engine.set_camera_*` writes the **active / main** camera; per-camera work is on that `SceneEntity`. See [Camera](#camera). |
| Camera pose | Position / direction / up as `float3` triples. Aim a camera with `look_to` / `camera_pose`, not `world_pose`. |
| Scene load | `setScene` starts a load. Call `waitUntilReady` / `wait_until_ready` when you need the scene committed. |
| Materials | Runtime fields are `metalness`, `roughness`, `emissive_color`, … Scene JSON keys such as `base_metalness` are a file format. |

The public host is `EngineApp`. C++ `EngineApp::app()` is the owned schedule runtime and is not a Python type. Systems, plugins, and `Query` / `Res` / `SceneTransforms` are C++-only. `GpuDevice`, NumPy `Frame`, `step_n`, and `deform_mesh` callbacks are Python-only.

Schedule internals: [docs/embedding-cpp.md](docs/embedding-cpp.md) · header allowlist: [docs/public-api.md](docs/public-api.md).

## Contents

**[Start here](#getting-started)**

- [Getting started](#getting-started)
- [Create / lifecycle](#create--lifecycle)
- [GPU selection](#gpu-selection)

**[Cookbook](#cookbook)** — copy-paste recipes, then jump to [Reference](#reference) for signatures

- Render: [headless](#headless-reference-render) · [framebuffer](#accumulate-then-read-framebuffer-cpu--numpy) · [window](#windowed-interactive-loop) · [modes](#realtime-vs-reference-helpers)
- Camera: [pose and FOV](#camera-pose-and-fov) · [reference](#camera)
- Scene: [builtin](#builtin--inline-scenes) · [OBJ](#load-obj-meshes-with-materials) · [spawn](#spawn--despawn-assets)
- Look: [materials](#edit-materials) · [textures](#read-and-replace-material-textures) · [lights](#edit-lights) · [unlit](#unlit-receivers-with-shadows)
- 3DGS: [load](#load-3d-gaussian-splats) · [batch](#3dgs-reference--realtime-batch) · [COLMAP](#colmap-camera-3dgs-alignment)
- Mesh: [deform](#deform-mesh-vertices)
- GPU: [reuse device](#reuse-a-gpu-across-scene-loads)

**[Reference](#reference)**

- [Module-level API](#module-level-api)
- [EngineApp](#engineapp)
- [Scene](#scene)
- [SceneEntity](#sceneentity)
- [Camera](#camera)
- [Sensor output / AOV](#sensor-output--aov)
- [Materials](#materials)
- [Lights](#lights)
- [Spawn / despawn](#spawn--despawn)
- [Mesh deformation](#mesh-deformation)
- [3D Gaussian splats](#3d-gaussian-splats)
- [Settings](#settings)
- [Enums](#enums)
- [Python helpers](#python-helpers)
- [C++ systems](#c-systems)

**[Host & examples](#host--examples)**

- [Embed and extension](#embed-and-extension)
- [In-tree examples](#in-tree-examples)
- [Related docs](#related-docs)

## Getting started

### C++ host

```cpp
#include <caustica.h>

int main()
{
    caustica::initializeAppPlatform();

    auto engine = caustica::EngineApp::create({
        .width = 1280,
        .height = 720,
        .scene = "default.scene.json",
        .windowTitle = "My Caustica Host",
    });
    if (!engine)
    {
        caustica::shutdownAppPlatform();
        return 1;
    }

    engine->run(); // window loop; shuts the engine down on exit
    caustica::shutdownAppPlatform();
    return 0;
}
```

Headless host-owned loop:

```cpp
auto engine = caustica::EngineApp::create({
    .headless = true,
    .scene = "builtin:plane_cube",
});

while (engine && keepRunning)
    engine->stepFrame(); // dt < 0 uses the engine clock (1/60 in headless)

if (engine)
    engine->shutdown();
caustica::shutdownAppPlatform();
```

Official in-tree host: [`examples/cpp/thin_client/Main.cpp`](examples/cpp/thin_client/Main.cpp) (`caustica_thin_client`).

### Python extension (standalone process)

After `python -m pip install .` from the repo root (or `PYTHONPATH` to `bin/`):

```python
import caustica
print(caustica.MODE)  # "extension"

with caustica.EngineApp.create(
    width=1280, height=720, headless=True,
    scene="builtin:plane_cube",
    realtime=False, accumulation_target=64,
) as engine:
    engine.step_until_accumulated()
    engine.save_screenshot("frame.png")
```

Linux builds are Vulkan-only. Pass `vulkan=True`. Use the same Python interpreter CMake found at configure time.

### Python embed (inside `caustica.exe`)

```powershell
caustica.exe --pythonScript examples/python/embedded.py
caustica.exe --pythonExpr "import caustica; print(caustica.engine().scene_name)"
```

```python
import caustica

engine = caustica.engine()   # borrow the running editor EngineApp
s = caustica.settings()      # same object as engine.settings
print(caustica.MODE)         # "embed"
```

Do not call `EngineApp.create()` in embed mode. The host already owns the session. `__enter__` / `__exit__` call `shutdown()` — do not use `with` on the borrowed embed handle.

### Install (Python)

The extension is emitted under `bin/`:

```text
bin/caustica.cp311-win_amd64.pyd
bin/caustica.cpython-313-x86_64-linux-gnu.so
```

```powershell
python -m pip install .
python support/python/build_wheel.py
python -m pip install dist/caustica-*.whl
```

| Variable | Default | Values |
| --- | --- | --- |
| `caustica_WHEEL_VERSION` | from `VERSION` | PEP 440 |
| `caustica_WHEEL_ASSETS` | `minimal` | `minimal`, `full`, `none` |
| `caustica_WHEEL_DYNAMIC_SHADERS` | `none` when a shader pack is built, else `bin` | `bin`, `full`, `none` |
| `caustica_WHEEL_SHADER_API` | `d3d12` on Windows, `vulkan` elsewhere | `d3d12`, `vulkan`, `both` |
| `CAUSTICA_WHEEL_SHADER_PACK` | `true` | `true`, `false` |

Prefer `pip install .` so examples can `import caustica` without patching `sys.path`.

## Create / lifecycle

### C++ `EngineAppDesc`

| Field | Default | Purpose |
| --- | --- | --- |
| `width`, `height` | `1920`, `1080` | Back-buffer size. |
| `headless` | `false` | Offscreen device, no window. |
| `dedicatedRenderThread` | `true` | Dedicated render thread; ignored when headless. |
| `parallelSystems` | `true` | Concurrent systems when parameters prove they cannot conflict. Turn off to debug. |
| `debugDevice` | `false` | Backend debug layer. |
| `adapter` | `AdapterSelector::automatic()` | GPU selector. |
| `useVulkan` | `false` | Vulkan instead of DX12 (Linux is Vulkan-only). |
| `scene` | `"default.scene.json"` | Scene file, `builtin:*`, or inline JSON. |
| `windowTitle` | `"caustica"` | Window title. |
| `runtimeDirectory` / `resourceRoot` / `assetPackRoot` | empty = auto | ShaderBin / Assets discovery. |
| `device` / `window` / `surface` | `nullptr` | Borrow an existing GPU. All three must be set together. |
| `cli` | empty | Snapshot of parsed command line. |
| `sceneCallbacks` | unset | Optional scene-load hooks. |

Parse argv with `EngineAppDesc::fromArgv(argc, argv)`.

### Python `EngineApp.create`

Extension mode only.

```python
caustica.EngineApp.create(
    device=None,          # reuse a GpuDevice
    width=1920,
    height=1080,
    headless=True,        # Python default is headless; C++ default is a window
    vulkan=False,
    adapter="auto",
    debug=False,
    scene='{"entities":[]}',
    realtime=False,
    accumulation_target=64,
)
```

`create()` waits until the initial scene is ready (via the render session). Later `set_scene(...)` does not wait.

Reuse a GPU across scene loads: [Cookbook](#reuse-a-gpu-across-scene-loads).

### Shared lifecycle

| C++ | Python | Notes |
| --- | --- | --- |
| `EngineApp::create(desc)` | `EngineApp.create(...)` | Python extension only. Embed uses `caustica.engine()`. |
| `isValid()` | `.valid` | False after shutdown. |
| `finishStartup()` | (automatic) | First `run` / `stepFrame` also starts. |
| `run()` | `run()` | Window loop until exit. |
| `stepFrame(dtSeconds=-1)` | `step_frame(dt=-1.0)` | One frame. `dt < 0` uses the engine clock. Returns false when the window is closed. |
| `requestExit()` | `request_exit()` | Ask the run loop to stop. |
| `shutdown()` | `shutdown()` | Tear down. Python `with` calls this on exit. |

## GPU selection

Automatic mode enumerates the requested backend, drops software and path-tracing-incompatible devices, and picks the suitable GPU with the highest capability score (hardware class, ray-tracing features, device-local memory, compute limits). Equal scores prefer the lower index. It is a deterministic heuristic, not a benchmark.

### C++ adapter selectors

```cpp
caustica::EngineAppDesc desc;
desc.adapter = caustica::rhi::AdapterSelector::automatic();
desc.adapter = caustica::rhi::AdapterSelector::byIndex(1);
desc.adapter = caustica::rhi::AdapterSelector::byName("RTX");
desc.adapter = caustica::rhi::AdapterSelector::byUuid("...");
desc.adapter = caustica::rhi::AdapterSelector::byLuid("...");
```

Or CLI: `--gpu auto|index:N|name:text|uuid:hex|luid:hex`.

### Python adapters

```python
for gpu in caustica.enumerate_adapters(vulkan=False):
    print(gpu.index, gpu.name, gpu.type, gpu.luid, gpu.suitable)

with caustica.EngineApp.create(adapter="auto", headless=True) as engine:
    print("using", engine.selected_adapter)
```

`enumerate_adapters(vulkan=False, debug=False)` does discovery without creating an EngineApp, logical device, window, or swap chain.

`AdapterInfo` (read-only):

| Property | Type | Meaning |
| --- | --- | --- |
| `index` | `int` | Index for this backend enumeration. |
| `name` | `str` | Driver-reported adapter name. |
| `backend` | `str` | `"d3d12"` or `"vulkan"`. |
| `type` | `str` | `"discrete"`, `"integrated"`, `"virtual"`, `"software"`, or `"unknown"`. |
| `vendor_id`, `device_id` | `int` | PCI/vendor identifiers. |
| `dedicated_video_memory` | `int` | Device-local memory in bytes. |
| `selection_score` | `int` | Score used by `auto`. |
| `supports_ray_tracing_pipeline` | `bool` | |
| `supports_ray_query` | `bool` | |
| `suitable` | `bool` | Meets device requirements. |
| `software` | `bool` | Software implementation. |
| `uuid` | `str \| None` | 32-hex-digit UUID when available. |
| `luid` | `str \| None` | 16-hex-digit Windows LUID when available. |

Selector strings (Python `adapter=` / CLI `--gpu`):

| Value | Behavior |
| --- | --- |
| `auto` | Highest-scoring suitable GPU. |
| `index:N` | Match `AdapterInfo.index`. A bare non-negative integer is also accepted. |
| `name:text` | Case-insensitive substring. A bare non-numeric string is treated as a name. |
| `uuid:hex` | Match UUID; separators and optional `0x` are accepted. |
| `luid:hex` | Match LUID; separators and optional `0x` are accepted. |

Explicit selectors are strict. A missing, unsuitable, or ambiguous match fails initialization; there is no silent fallback. For persistent workers prefer UUID/LUID — indices can change after driver or hardware updates.

```python
gpus = caustica.enumerate_adapters()
target = next(gpu for gpu in gpus if gpu.suitable and gpu.luid)

with caustica.EngineApp.create(adapter=f"luid:{target.luid}", headless=True) as engine:
    selected = engine.selected_adapter
    print(selected.index, selected.name, selected.backend)
```

## Cookbook

Copy a recipe and jump to [Reference](#reference) for signatures.

- Render: [headless](#headless-reference-render) · [framebuffer](#accumulate-then-read-framebuffer-cpu--numpy) · [window](#windowed-interactive-loop) · [modes](#realtime-vs-reference-helpers)
- Camera: [pose and FOV](#camera-pose-and-fov) · [reference](#camera)
- Scene: [builtin](#builtin--inline-scenes) · [OBJ](#load-obj-meshes-with-materials) · [spawn](#spawn--despawn-assets)
- Look: [materials](#edit-materials) · [textures](#read-and-replace-material-textures) · [lights](#edit-lights) · [unlit](#unlit-receivers-with-shadows)
- 3DGS: [load](#load-3d-gaussian-splats) · [batch](#3dgs-reference--realtime-batch) · [COLMAP](#colmap-camera-3dgs-alignment)
- Mesh: [deform](#deform-mesh-vertices)
- GPU: [reuse device](#reuse-a-gpu-across-scene-loads)

### Headless reference render

**Python**

```python
import caustica

with caustica.EngineApp.create(
    width=1280,
    height=720,
    headless=True,
    scene="bistro-programmer-art.scene.json",
    realtime=False,
    accumulation_target=64,
) as engine:
    engine.settings.enable_tone_mapping = True
    frames = engine.step_until_accumulated()
    print("frames:", frames)
    engine.save_screenshot("frame.png")
```

**C++**

```cpp
auto engine = caustica::EngineApp::create({
    .width = 1280, .height = 720, .headless = true,
    .scene = "bistro-programmer-art.scene.json",
});
engine->setReferenceMode(/*spp=*/64, /*oidn=*/false);
while (engine && !engine->accumulationCompleted())
    engine->stepFrame();
engine->saveScreenshot("frame.png");
engine->shutdown();
```

### Accumulate then read framebuffer (CPU / NumPy)

After reference accumulation finishes, read the same LDR final color used by `save_screenshot` without writing a file:

```python
import caustica
import numpy as np

engine = caustica.EngineApp.create(
    width=1280,
    height=720,
    headless=True,
    scene="builtin:plane_cube",
    realtime=False,
    accumulation_target=64,
)
engine.settings.realtime_mode = False
engine.settings.accumulation_target = 64
engine.settings.realtime_aa = 0

engine.step_until_accumulated()

# Option A: NumPy (H, W, 4) uint8 RGBA — requires NumPy
img = engine.get_pixels()
rgb = img[..., :3]

# Option B: raw bytes via Framebuffer
fb = engine.read_ldr_framebuffer()
raw = fb.pixels  # len == width * height * 4
arr = np.frombuffer(raw, dtype=np.uint8).reshape(fb.height, fb.width, 4).copy()

engine.shutdown()
```

Layout:

- Tightly packed **RGBA8**, row-major, **top-left** origin.
- Source is the engine LDR final color (same as `save_screenshot`).
- HDR readback is not implemented.

C++ uses `engine->readLdrFramebuffer()` (`std::optional<LdrFramebuffer>` with `width`, `height`, `channels`, `pixels`).

### Windowed interactive loop

```python
import time
import caustica

engine = caustica.EngineApp.create(
    width=1280,
    height=720,
    headless=False,
    scene="bistro-programmer-art.scene.json",
    realtime=True,
    accumulation_target=1,
)

try:
    while engine.step_frame(-1.0):  # False when the window is closed
        time.sleep(0.001)
finally:
    engine.shutdown()
```

C++ equivalent is `engine->run()`, or a host loop calling `stepFrame()` until it returns false.

### Camera pose and FOV

FOV is **radians**. Python triples match C++ `float3`. Full property / function list: [Camera](#camera).

`engine.set_camera_*` always writes the **active / main** camera: the free controller camera when `selected_camera_index == 0`, or the selected scene camera otherwise. A scene can have many cameras; look the entity up and call methods on it.

```python
import math
import caustica

with caustica.EngineApp.create(scene="builtin:plane_cube", headless=True) as engine:
    # No camera nodes in this scene: these APIs drive the free / main camera.
    engine.set_camera_vertical_fov(math.radians(35.0))
    engine.set_camera_pos_dir_up(
        (0.0, 1.5, 4.0),
        (0.0, 0.0, -1.0),
        (0.0, 1.0, 0.0),
    )
    print(engine.current_camera_pos_dir_up)
    print(engine.camera_vertical_fov)
```

```cpp
engine->setCameraVerticalFOV(0.610865f);
engine->setCameraPosDirUp({0.f, 1.5f, 4.f}, {0.f, 0.f, -1.f}, {0.f, 1.f, 0.f});
```

Named scene cameras (wrist, third-person, …) live on `SceneEntity`. Optical edits on an inactive camera are stored on the component; `activate()` / `use_camera()` selects which one the renderer uses.

`look_to` / `set_camera_pos_dir_up` write **camera view space** (the renderer applies a Z-flip). `world_pose` / `set_world_pose` write the entity TRS with **no** Z-flip. Aim cameras with `look_to`. Copying a mesh `world_pose` onto a camera points the wrong way.

```python
import math

wrist = engine.scene.find_camera("wrist")
wrist.vertical_fov = math.radians(55.0)
wrist.look_to((0.0, 1.2, 0.15), (0.0, 0.0, 1.0), (0.0, 1.0, 0.0))
wrist.set_intrinsics(fx, fy, cx, cy, width, height)
wrist.activate()

# equivalent: engine.use_camera(wrist)
# back to the free camera:
engine.use_camera(None)
```

```cpp
ecs::Entity wrist = caustica::findEntity(engine->app(), "wrist");
caustica::setSceneCameraVerticalFOV(engine->app(), wrist, dm::radians(55.f));
caustica::setSceneCameraLookTo(
    engine->app(), wrist,
    {0.f, 1.2f, 0.15f}, {0.f, 0.f, 1.f}, {0.f, 1.f, 0.f});
engine->setActiveCamera(wrist);
engine->setActiveCamera(ecs::NullEntity); // free camera
```

Off-center pinhole (COLMAP / OpenCV), same argument order in both languages. Distortion is intentionally not modeled yet; keep calibration as pinhole for this phase. On the active camera:

```python
engine.set_camera_intrinsics(fx, fy, cx, cy, width, height)
engine.clear_camera_intrinsics()  # back to symmetric FOV
```

On a specific scene camera: `camera.set_intrinsics(...)` / `camera.clear_intrinsics()`.

Switch among named cameras and render each one:

```python
for name in ("wrist", "third_person", "overview"):
    cam = engine.scene.find_camera(name)
    if cam is None:
        continue
    cam.activate()
    engine.reset_accumulation()
    engine.wait_until_ready()
    engine.save_screenshot(f"{name}.png")

engine.use_camera(None)  # back to the free / controller camera
```

### Load 3D Gaussian splats

Prefer declaring splat nodes in scene JSON:

```python
import caustica

scene = r'''
{
  "entities": [
    {
      "id": "Ground",
      "name": "Ground",
      "components": {
        "PrefabInstance": { "source": "builtin:plane" }
      }
    },
    {
      "id": "Scan",
      "name": "Scan",
      "components": {
        "Transform": { "translation": [0, 0, 0], "scale": [1, 1, 1] },
        "GaussianSplat": {
          "path": "D:/path/to/scans/splats.ply",
          "convertRdfToRub": true
        }
      }
    }
  ]
}
'''

engine = caustica.EngineApp.create(
    width=1280, height=720, headless=False, realtime=True, scene=scene)

s = engine.settings
s.enable_gaussian_splats = True
s.gaussian_splat_sorting_mode = int(caustica.GaussianSplatSortMode.GpuSort)
s.gaussian_splat_sh_format = int(caustica.GaussianSplatStorageFormat.Uint8)
s.gaussian_splat_rgba_format = int(caustica.GaussianSplatStorageFormat.Uint8)
s.gaussian_splat_scale = 1.0
s.gaussian_splat_alpha_scale = 1.0
s.gaussian_splat_brightness = 1.0

while engine.step_frame(-1.0):
    pass
engine.shutdown()
```

For script-driven append, `load_gaussian_splat_file(path, convert_rdf_to_rub=True)` (C++ `loadGaussianSplatFile`) adds a `GaussianSplat` node under the current root. `set_scene(...)` replaces the graph and destroys previously appended splat nodes — load them again after a scene switch, or declare them in the target JSON.

Multiple splat nodes in one inline scene:

```python
scene = r"""
{
  "entities": [
    {
      "id": "ScanA",
      "name": "ScanA",
      "components": {
        "Transform": { "translation": [0.0, 0.0, 0.0], "scale": 1.0 },
        "GaussianSplat": {
          "path": "D:/path/to/scans/splats_a.ply",
          "convertRdfToRub": true,
          "enabled": true
        }
      }
    },
    {
      "id": "ScanB",
      "name": "ScanB",
      "components": {
        "Transform": { "translation": [2.0, 0.0, 0.0], "scale": 0.75 },
        "GaussianSplat": { "path": "D:/path/to/scans/splats_b.ply" }
      }
    }
  ]
}
"""
engine = caustica.EngineApp.create(headless=False, realtime=True, scene=scene)
```

For scene files, relative 3DGS paths are resolved relative to the scene JSON. `path`, `file`, and `fileName` are accepted JSON aliases.

### 3DGS reference / realtime batch

`gaussian_splats.py view --mode batch` renders the same PLY twice:

- Reference mode accumulates `--spp` samples, applies OIDN, and writes `reference_oidn.png`.
- Realtime mode steps `--frames` frames and uses DLSS-RR when supported, falling back to DLSS, then NRD + TAA, and writes `realtime.png`.
- Default sorting is GPU sort. Pass `--sorting stochastic` to compare with stochastic splats.

```powershell
python .\examples\python\gaussian_splats.py view ^
    --ply D:/path/to/splats.ply ^
    --mode batch ^
    --out-dir splat_batch_out
```

Camera framing overrides:

```powershell
python .\examples\python\gaussian_splats.py view ^
    --ply D:/path/to/splats.ply ^
    --mode batch ^
    --out-dir splat_batch_out ^
    --distance-scale 4.0 ^
    --side front
```

The camera is framed from bounds sampled out of the PLY; the engine does not expose a bounds query for loaded splats.

### COLMAP camera 3DGS alignment

`gaussian_splats.py colmap` renders a 3DGS PLY from COLMAP `cameras.bin`/`images.bin` (or `.txt`). Useful for comparing caustica against gsplat from the same poses. Requires `--ply`, `--colmap-dir`, and numpy.

```powershell
python .\examples\python\gaussian_splats.py colmap ^
    --ply D:/path/to/gaussians.ply ^
    --colmap-dir D:/path/to/sparse ^
    --max-views 8 ^
    --frames-per-view 8 ^
    --warmup-frames 4 ^
    --out-dir colmap_views_out
```

The subcommand passes full COLMAP pinhole intrinsics (`fx`, `fy`, `cx`, `cy`) through `set_camera_intrinsics`, which keeps off-center principal points aligned with gsplat. Use `--symmetric-fov` only when testing the vertical-FOV-only path. Output resolution defaults to the COLMAP camera size; `--width` / `--height` override it and rescale intrinsics.

It also writes `cameras_used.json` with per-view COLMAP intrinsics and the caustica camera actually used.

`--rdf-to-rub` (default) converts both the PLY loader and COLMAP poses from RDF/COLMAP coordinates into engine coordinates. Disable with `--no-rdf-to-rub`. `--mip-antialiasing` defaults on for this subcommand.

### Load OBJ meshes with materials

`spawn_from_file` / `spawnFromFile` append mesh/prefab files (`.gltf` / `.glb` / `.obj` / `.urdf` / `.usd*`). For OBJ, the loader parses `mtllib` and resolves `.mtl` and textures relative to the OBJ/MTL directory.

Recognized MTL fields:

- Scalars/colors: `Kd`, `Ks`, `Ke`, `Ns`, `Pr`, `Pm`, `Ni`, `d`, `Tr`, `Tf`.
- Base / diffuse maps: `map_Kd`, `map_basecolor`.
- PBR: `map_Pr`, `map_roughness`, `map_Pm`, `map_metallic`, `map_metalness`.
- Packed: `map_orm`, `map_mr`, `map_metallicroughness`, `map_occlusionroughnessmetallic`.
- AO: `map_Ka`, `map_ao`, `map_occlusion`.
- Normal / bump: `map_Bump`, `bump`, `norm`, `map_normal`, including `-bm`.
- Spec-gloss: `map_Ks`, `map_Ns`.
- Emissive / opacity / transmission: `map_Ke`, `map_emissive`, `map_d`, `map_opacity`, `map_Tf`.

Separate roughness and metallic maps are packed into an in-memory ORM texture (`R=AO`, `G=roughness`, `B=metallic`, `A=1`). `-imfchan` is honored for single-channel maps. If `map_Ke` / `map_emissive` is present without `Ke`, the importer uses `(1, 1, 1)` as the emissive factor.

```python
import caustica

obj_path = r"D:/assets/m-plate-pbr_final/textured.obj"

with caustica.EngineApp.create(scene="builtin:plane", headless=True, accumulation_target=32) as engine:
    entity = engine.spawn_from_file(obj_path)
    if entity is None:
        raise RuntimeError(f"failed to load {obj_path}")

    # Materials exist after the mesh is appended and at least one update frame.
    engine.step_n(1)

    for mat in engine.scene.get_materials():
        print(mat.model_name, mat.name, mat.base_color, mat.roughness, mat.metalness)

    engine.step_until_accumulated()
    engine.save_screenshot("obj_materials.png")
```

### Edit materials

```python
import caustica

with caustica.EngineApp.create(scene="bistro-programmer-art.scene.json", headless=True) as engine:
    scene = engine.scene

    for mat in scene.get_materials():
        print(mat.model_name, mat.name, mat.unique_name)

    mat = scene.find_material("SomeMaterialName")
    if mat is None:
        raise RuntimeError("material not found")

    # Scalars/colors multiply loaded textures when that slot stays enabled.
    mat.base_color = (1.0, 0.2, 0.1)
    mat.roughness = 0.35
    mat.metalness = 0.0
    mat.normal_texture_scale = 0.75

    mat.enable_base_texture = False
    mat.enable_orm_texture = True
    mat.enable_normal_texture = True
    mat.set_base_texture(r"D:/assets/replacement_albedo.png")
    mat.set_normal_texture(r"D:/assets/replacement_normal.png")
    mat.set_coat_normal_texture(r"D:/assets/replacement_coat_normal.png")

    engine.reset_accumulation()
    engine.step_n(4)
    engine.save_screenshot("material_edit.png")
```

Writable `Material` properties mark GPU data dirty automatically. `mark_dirty()` is only needed if native-side data changed without a Python setter. Texture helpers load through the runtime cache, enable the slot, and upload on the next rendered frame.

### Read and replace material textures

Python reads the bound file path, replaces a slot, toggles sampling, or clears a slot. There is no pixel-buffer API.

```python
import caustica

with caustica.EngineApp.create(scene="bistro-programmer-art.scene.json", headless=True) as engine:
    mat = engine.scene.find_material("SomeMaterialName")
    if mat is None:
        raise RuntimeError("material not found")

    print("base:", mat.base_texture_path)
    print("orm:", mat.orm_texture_path)
    print("normal:", mat.normal_texture_path)
    print("emissive:", mat.emissive_texture_path)
    print("transmission:", mat.transmission_texture_path)

    if not mat.set_base_texture(r"D:/assets/albedo_replacement.png"):
        raise RuntimeError("base texture not found")
    mat.set_normal_texture(r"D:/assets/normal_replacement.png")

    mat.set_texture(caustica.TextureSlot.Emissive, r"D:/assets/emissive.png")
    mat.set_texture(caustica.TextureSlot.ORM, r"D:/assets/orm_linear.png", srgb=False)

    mat.enable_base_texture = True
    mat.enable_normal_texture = False

    mat.clear_emissive_texture()
    mat.clear_texture(caustica.TextureSlot.Transmission)

    engine.reset_accumulation()
    engine.step_n(4)
```

`set_*_texture(...)` returns `True` when the file was resolved, `False` if not found. Success also enables that slot. `clear_*_texture(...)` removes the binding and disables the slot.

| Slot | Helper | Default color space |
| --- | --- | --- |
| `TextureSlot.Base` | `set_base_texture(path, srgb=None)` | sRGB |
| `TextureSlot.ORM` | `set_orm_texture(path, srgb=None)` | Linear (metal-rough) / sRGB (spec-gloss) |
| `TextureSlot.Normal` | `set_normal_texture(path)` | Linear normal map |
| `TextureSlot.CoatNormal` | `set_coat_normal_texture(path)` | Linear coat normal map |
| `TextureSlot.Emissive` | `set_emissive_texture(path, srgb=None)` | sRGB |
| `TextureSlot.Transmission` | `set_transmission_texture(path, srgb=None)` | Linear |

Override with `srgb=`. Generic `set_texture(slot, path, srgb=None, normal_map=None)` accepts `normal_map` for advanced cases.

Relative paths resolve like material JSON: runtime `Assets/` first, then the current scene directory. For `.png` inputs, an existing sibling `.dds` is preferred.

### Edit lights

```python
import caustica

engine = caustica.EngineApp.create(scene="bistro-programmer-art.scene.json", headless=True)
scene = engine.scene
for light in scene.get_lights():
    print(light.name, light.light_type)
    light.color = (1.0, 0.9, 0.75)

sun = scene.find_light("Sun")
if sun:
    sun.direction = (0.0, -1.0, 0.2)

spot = engine.spawn_spot_light(
    color=(1.0, 0.95, 0.85),
    intensity=40.0,
    radius=0.05,
    range=8.0,
    inner_angle=20.0,
    outer_angle=35.0,
    name="PySpot",
)
if spot:
    spot.position = (0.0, 2.0, 0.0)
    spot.direction = (0.0, -1.0, 0.0)

engine.step_n(8)
engine.save_screenshot("lights.png")
engine.shutdown()
```

C++ fills `scene::SpotLightComponent` and calls `engine->spawnSpotLight(component, "PySpot")`.

### Spawn / despawn assets

Same path as C++ `SceneSpawn`. Supported extensions: `.gltf`, `.glb`, `.obj`, `.urdf`, `.usd` / `.usda` / `.usdc`, `.prefab.json`.

```python
import caustica

with caustica.EngineApp.create(scene="builtin:plane", headless=True, accumulation_target=16) as engine:
    entity = engine.spawn_from_file("models/GlassSphere/GlassSphere.gltf")
    if entity is None:
        raise RuntimeError("spawn_from_file failed")
    entity.translation = (1.5, 0.5, 0.0)
    entity.scaling = (0.5, 0.5, 0.5)

    prefab = engine.load("models/GlassSphere/GlassSphere.gltf")
    if prefab:
        clone = engine.spawn(prefab)
        if clone:
            clone.translation = (-1.5, 0.5, 0.0)

    engine.step_n(1)
    engine.step_until_accumulated()
    engine.save_screenshot("spawned.png")
    engine.despawn(entity)
```

```cpp
auto entity = engine->spawnFromFile("models/GlassSphere/GlassSphere.gltf");
auto prefab = engine->load("models/GlassSphere/GlassSphere.gltf");
auto clone = engine->spawn(prefab);
engine->despawn(entity);
```

### Deform mesh vertices

Mesh deformation is **entity-first**. Importers may split one authored position into several render vertices for UV/normal seams; the API returns that position once and write-back propagates to all splits. After `set_mesh_vertices` / `deform_mesh`, GPU buffers refresh and ray-tracing AS can rebuild.

```python
import math
import caustica

engine = caustica.EngineApp.create(scene="builtin:cube", headless=True, accumulation_target=8)
entity = engine.scene.find_mesh_entity("cube") or engine.scene.get_mesh_entities()[0]
vertices = list(engine.get_mesh_vertices(entity))

deformed = []
for x, y, z in vertices:
    radius = math.sqrt(x * x + z * z)
    lift = 0.15 * max(0.0, 1.0 - radius)
    deformed.append((x, y + lift, z))

engine.set_mesh_vertices(entity, deformed, recompute_normals=True)
engine.step_until_accumulated()
engine.save_screenshot("deformed_mesh.png")
engine.shutdown()
```

Callback sugar — return `None` to keep a vertex unchanged:

```python
def wave(index, p):
    x, y, z = p
    if y < 0:
        return None
    return (x, y + 0.05 * math.sin(index * 0.37), z)

engine.deform_mesh(entity, wave, recompute_normals=True)
```

World-space variants use that instance's transform (refreshed after `entity.translation = ...`):

```python
entity = engine.find_entity("cube")

def lift_world(index, p):
    x, y, z = p
    return (x, y + 0.25, z)

engine.deform_mesh_world(entity, lift_world, recompute_normals=True)
```

**C++**

```cpp
auto verts = engine->getMeshVertices(entity);
engine->setMeshVertices(entity, verts, { .recomputeNormals = true });
```

Keep `rebuild_acceleration_structure=True` for ray-tracing-correct geometry. Set it `False` only when batching several edits, then call `request_full_accel_rebuild()` once. Shared mesh buffers still apply: deforming through one mesh entity updates other instances of the same engine mesh record.

### Unlit receivers with shadows

```python
mat.unlit_receive_shadows = True
mat.unlit_shadow_strength = 0.5
engine.reset_accumulation()
```

`unlit_receive_shadows` keeps a flat base color (no BRDF) and darkens it with sampled-light visibility. `unlit_shadow_strength` is `0..1`.

### Realtime vs reference helpers

```python
engine.set_realtime_mode(
    standalone_denoiser=True,
    realtime_aa=int(caustica.RealtimeAA.DLSS),
)

engine.set_reference_mode(
    spp=128,
    oidn=True,
    oidn_quality=int(caustica.OidnQuality.Balanced),
    oidn_passes=int(caustica.OidnPasses.Albedo),
    oidn_prefilter=int(caustica.OidnPrefilter.Fast),
)
```

`spp=0` keeps the current accumulation target. `realtime_aa`: `0=Off`, `1=TAA`, `2=DLSS`, `3=DLSS_RR`.

### Builtin / inline scenes

```python
with caustica.EngineApp.create(headless=True, scene="builtin:plane_cube", accumulation_target=4) as engine:
    engine.step_until_accumulated()
    engine.save_screenshot("smoke.png")

scene = caustica.builtin_scene_json("plane_cube")
engine = caustica.EngineApp.create(headless=True, scene=scene)
```

Supported builtins: `builtin:plane`, `builtin:cube`, `builtin:sphere`, `builtin:plane_cube`.

### Reuse a GPU across scene loads

```python
import caustica

device = caustica.GpuDevice(vulkan=False, adapter="auto")
with caustica.EngineApp.create(device=device, width=1280, height=720, headless=True,
                               scene="builtin:plane_cube") as engine:
    engine.scene.find_material("Floor")
    frame = engine.render_reference(spp=64)
    rgb = frame.rgb  # numpy (H, W, 4) uint8
    engine.save_screenshot("frame.png")
# Shutdown EngineApp before device.close().
device.close()
```

## Reference

Signatures for the shared host. Recipes live in the [Cookbook](#cookbook).

## Module-level API

These exist in both embed and extension unless noted.

| C++ | Python | Returns | Notes |
| --- | --- | --- | --- |
| — | `caustica.MODE` | `str` | `"embed"` or `"extension"`. |
| running `EngineApp&` | `caustica.engine()` | `EngineApp` | Extension: most recently created host. Embed: borrowed editor session. |
| `engine.settings()` | `caustica.settings()` | `Settings` | Same object as `engine.settings`. |
| `caustica::info` | `caustica.log_info(message)` | `None` | Native log, info. |
| `caustica::warning` | `caustica.log_warning(message)` | `None` |  |
| `caustica::error` | `caustica.log_error(message)` | `None` |  |
| `GpuDevice::enumerateAvailableAdapters` | `caustica.enumerate_adapters(vulkan=False, debug=False)` | `list[AdapterInfo]` | Extension only. No EngineApp required. |
| `GpuDevice` + inject into `EngineAppDesc` | `caustica.GpuDevice(...)` | `GpuDevice` | Extension only. |
| `EngineApp::create(desc)` | `caustica.EngineApp.create(...)` | `EngineApp` | Extension only. |
| `builtinSceneJson` | `caustica.builtin_scene_json(builtin_model="plane_cube")` | `str` | Extension only. |

## EngineApp

Methods below are on `caustica::EngineApp` and Python `EngineApp` unless marked otherwise. Python names are snake_case; C++ is camelCase.

### Scene load

| C++ | Python | Returns | Notes |
| --- | --- | --- | --- |
| `setScene(name, forceReload=false)` | `set_scene(scene_name, force_reload=False)` | `void` | Starts a load. Does **not** wait. |
| `isSceneLoaded()` | `.is_scene_loaded` | `bool` | |
| `isSceneLoading()` | `.is_scene_loading` | `bool` | |
| `isSceneReady()` | `.is_scene_ready` | `bool` | Scene committed and usable. |
| `waitUntilReady(timeoutSeconds=600, warmupFrames=4)` | `wait_until_ready(timeout_seconds=600.0, warmup_frames=4)` | `bool` | |
| `currentSceneName()` | `.scene_name` | `str` | |
| `availableScenes()` | `.available_scenes` | `list[str]` | Discovered scene files. |
| `entityWorld()` | `.scene` | Python: `Scene \| None` | Query view. `None` before a scene exists. |
| `settings()` | `.settings` | `PathTracerSettings` | Live UI/settings object. |

### Active camera

`EngineApp` camera methods write the **active / main** camera. Per-camera optics, pose, and lookup live on the scene camera `SceneEntity` — full usage: [Camera](#camera). Index `0` is the free camera; `1..N` match `scene.get_cameras()` registration order. `scene.camera_count` counts entities only and does **not** include the free camera; `engine.scene_camera_count` is `camera_count + 1`. Orthographic scene cameras are exposed for inspection but are not selectable by the active-camera API until an orthographic controller path is implemented.

| C++ | Python | Returns | Notes |
| --- | --- | --- | --- |
| `setCameraPosDirUp(pos, dir, up)` | `set_camera_pos_dir_up(position, direction, up=(0,1,0))` | `bool` / `None` | Active camera. World-space triples. C++ returns `false` on failure; Python raises on failure. C++ also has a string overload matching `.current_camera_pos_dir_up`. |
| `currentCameraPosDirUp()` | `.current_camera_pos_dir_up` | `str` | `pos.xyz,dir.xyz,up.xyz`. |
| `currentCameraPose()` | `.camera_pose` | `CameraPose` / tuple | Typed active-camera `(position, direction, up)` in world space. |
| `setCameraPose(pose)` | `.camera_pose = pose` / `set_camera_pose(...)` | `bool` / `None` | Typed active-camera pose. Python raises on invalid input. |
| `setCameraVerticalFOV(radians)` | `set_camera_vertical_fov(radians)` | `bool` / `None` | Active camera. **Radians**, not degrees. Python raises on failure. |
| `cameraVerticalFOV()` | `.camera_vertical_fov` | `float` | Radians. |
| `setCameraIntrinsics(fx, fy, cx, cy, width, height)` | `set_camera_intrinsics(...)` | `bool` / `None` | Active camera. Off-center pinhole. Overrides symmetric FOV until cleared. Python raises on failure. |
| `clearCameraIntrinsics()` | `clear_camera_intrinsics()` | `bool` / `None` | Restore FOV projection on the active camera. Python raises on failure. |
| `sceneCameraCount()` | `.scene_camera_count` | `int` | Scene cameras plus the free camera. |
| `selectedCameraIndex()` / `setSelectedCameraIndex` | `.selected_camera_index` | `int` | `0` = free camera; `1..N` = scene-camera registration order. Only perspective scene cameras can be selected. |
| `activeCameraEntity()` | `.active_camera` | entity / `None` | `None` when the free camera is selected. |
| `activeCameraIsFree()` | `.active_camera_is_free` | `bool` | Whether the free/controller camera is active. |
| `activeCameraPath()` / `activeCameraName()` | `.active_camera_path` / `.active_camera_name` | `str` | Stable metadata for the selected scene camera; empty for the free camera. |
| `setActiveCamera(entity)` | `use_camera(entity)` | `bool` / `None` | `NullEntity` / `None` returns to the free camera. C++ returns `false` on failure; Python raises on failure. |
| `setActiveCameraByPath(path)` | `use_camera_path(path)` | `bool` / `None` | Select a registered scene camera by hierarchy path. |
| `saveCurrentCamera()` / `loadCurrentCamera()` | `save_current_camera()` / `load_current_camera()` | `void` | Persistence path used by the host. |
| `addRenderProduct(desc)` | `add_render_product(name, camera=None, aovs=Aov.all)` | `bool` / `None` | Register a named camera + AOV set. `camera=None` is the active camera. |
| `removeRenderProduct(name)` / `clearRenderProducts()` | `remove_render_product` / `clear_render_products` | `bool` / `void` | |
| `readSensorOutput(aovs=Aov::All)` | `read_sensor_output(aovs=Aov.all)` | `SensorOutput` | AOVs for the camera that was just rendered. |
| `captureSensorOutputs()` | `capture_sensor_outputs()` | `vector` / `list[SensorOutput]` | Every registered RenderProduct at the current physical time. Extra cameras re-render without stepping simulation or advancing the device frame clock. |
| `setEntitySemanticLabel(entity, instanceId, semanticId, label)` | `SceneEntity.instance_id` / `.semantic_id` / `.semantic_label` | `bool` / `None` | Stable ids for AOV alignment. |

Per-scene-camera C++ helpers are on `CameraApi.h` (`setSceneCameraVerticalFOV`, `setSceneCameraLookTo`, `setSceneCameraIntrinsics`, …) and take `App&` plus `ecs::Entity`. Python uses the same operations as properties / methods on `SceneEntity`.

```python
import math
engine.set_camera_vertical_fov(math.radians(35.0))
engine.set_camera_pos_dir_up((0.0, 1.5, 4.0), (0.0, 0.0, -1.0), (0.0, 1.0, 0.0))

wrist = engine.scene.find_camera("wrist")
wrist.vertical_fov = math.radians(55.0)
wrist.activate()

# Typed camera pose is view-space look-to, not entity TRS.
wrist.camera_pose = ((0.0, 1.2, 0.15), (0.0, 0.0, 1.0), (0.0, 1.0, 0.0))
```

### Spawn / lights / 3DGS

| C++ | Python | Returns | Notes |
| --- | --- | --- | --- |
| `load(path)` | `load(path)` | `ScenePrefab` / `Handle<ScenePrefabAsset>` | Import only; does not spawn. |
| `spawn(prefab)` | `spawn(prefab)` | entity | Attach under the scene root. Python: `SceneEntity \| None`. |
| `spawnFromFile(path)` | `spawn_from_file(path)` | entity | `load` + `spawn`. Python `None` on failure. |
| `spawnFromSource(source)` | `spawn_from_source(source)` | entity | Inline JSON. |
| `despawn(entity)` | `despawn(entity)` | `bool` | Entity and children. |
| `spawnDirectionalLight(component, name)` | `spawn_directional_light(color=(1,1,1), irradiance=1.0, angular_size=0.0, name="")` | entity | |
| `spawnPointLight(...)` | `spawn_point_light(color=(1,1,1), intensity=1.0, radius=0.0, range=0.0, name="")` | entity | |
| `spawnSpotLight(...)` | `spawn_spot_light(..., inner_angle=180.0, outer_angle=180.0, name="")` | entity | Angles in **degrees**. |
| `spawnRectLight(...)` | `spawn_rect_light(color=(1,1,1), intensity=1.0, width=1.0, height=1.0, name="")` | entity | Faces local −Z. |
| `spawnEnvironmentLight(...)` | `spawn_environment_light(color=(1,1,1), path="", rotation=0.0, name="")` | entity | |
| `loadGaussianSplatFile(path, convertRdfToRub=true)` | `load_gaussian_splat_file(file_name, convert_rdf_to_rub=True)` | `bool` | Append a `.ply` node. |
| `gaussianSplatCount()` | `.gaussian_splat_count` | `int` | |
| `gaussianSplatObjectCount()` | `.gaussian_splat_object_count` | `int` | |
| `gaussianSplatFileName()` | `.gaussian_splat_file_name` | `str` | Single path or multi-object summary. |

Empty light `name` auto-generates a unique name (`DirectionalLight`, `PointLight`, …).

### Find / mesh / env / time

| C++ | Python | Returns | Notes |
| --- | --- | --- | --- |
| `findEntity(path, context=NullEntity)` | `find_entity(path)` | entity / `None` | Name or path. |
| `findMaterial(materialID)` | `find_material(material_id)` | material / `None` | Cache-backed pick id. Name lookup: `engine.scene.find_material("Floor")`. |
| `getMeshVertices(entity)` | `get_mesh_vertices(entity)` | `list[(x,y,z)]` | Unique object-space positions; UV/normal splits collapsed. |
| `setMeshVertices(entity, vertices, options)` | `set_mesh_vertices(entity, vertices, recompute_normals=True, rebuild_acceleration_structure=True)` | `void` | Length must match `get_mesh_vertices`. |
| `getMeshVerticesWorld` / `setMeshVerticesWorld` | `get_mesh_vertices_world` / `set_mesh_vertices_world` | same | World space. |
| — | `deform_mesh` / `deform_mesh_world` | `int` | Python sugar: callback per unique vertex. |
| `requestMeshAccelRebuild(entity)` | `request_mesh_accel_rebuild(entity)` | `void` | One mesh BLAS. |
| `requestFullAccelRebuild()` | `request_full_accel_rebuild()` | `void` | Full scene AS. |
| `precacheRtFeaturePresets(showProgress=true)` | `precache_rt_feature_presets(show_progress=True)` | `int` | After at least one `stepFrame`. Ready-count. |
| `setEnvMapOverrideSource(path)` | `set_env_map_override_source(path)` | `void` | Override environment map source. |
| `sceneTime()` / `setSceneTime` | `.scene_time` | `float` | Seconds. Must be finite. |

### Modes / readback / diagnostics

| C++ | Python | Returns | Notes |
| --- | --- | --- | --- |
| `setRealtimeMode(standaloneDenoiser=true, realtimeAA=2)` | `set_realtime_mode(...)` | `void` | |
| `setReferenceMode(spp=0, oidn=false, oidnQuality=1, oidnPasses=1, oidnPrefilter=1)` | `set_reference_mode(...)` | `void` | `spp=0` keeps current target. |
| `prepareAnimationFrame(sceneTime, importedAnimations, keyframes)` | `prepare_animation_frame(time_seconds, imported_animations=True, keyframes=True)` | `bool` | |
| `accumulationCompleted()` | `.accumulation_completed` | `bool` | |
| `accumulationSampleIndex()` | `.accumulation_sample_index` | `int` | |
| `renderSize()` | `.render_size` | `(w,h)` | Path-tracer resolution. |
| `avgTimePerFrame()` | `.avg_time_per_frame` | `float` | |
| `fpsInfo()` / `resolutionInfo()` | `.fps_info` / `.resolution_info` | `str` | |
| `frameIndex()` | `.frame_index` | `int` | |
| `ldrColorTexture()` | — | `Texture*` | C++ GPU texture. |
| `readLdrFramebuffer()` | `read_ldr_framebuffer()` | `LdrFramebuffer` / `Framebuffer` | Extension Python helper. Embed: use `save_screenshot`. |
| `saveScreenshot(path)` | `save_screenshot(output_path)` | `bool` | LDR PNG/JPG/BMP/TGA. |

Python also has `reset_accumulation()` and `reset_realtime_caches()` (set the corresponding settings flags), plus `request_shader_reload()` (flags `renderAppState().runtime.Invalidation.ShaderReloadRequested`). After classification edits that change shaders or AS metadata, call `request_shader_reload()` and `request_full_accel_rebuild()`.

## Scene

C++ hosts iterate with `Query` / `EntityWorld::findEntity`. Python exposes a `Scene` view on `engine.scene`.

| Python | Returns | Notes |
| --- | --- | --- |
| `get_materials()` | `list[Material]` | All `StandardMaterial` instances. |
| `find_material(name)` | `Material \| None` | Match `name` or `unique_name`. |
| `find_material_by_id(material_id)` | `Material \| None` | GPU index only. Prefer `EngineApp.find_material`. |
| `get_lights()` | `list[SceneEntity]` | |
| `find_light(name)` | `SceneEntity \| None` | |
| `get_cameras()` | `list[SceneEntity]` | Scene camera entities (not the free camera). See [Camera](#camera). |
| `find_camera(name)` | `SceneEntity \| None` | Match entity name. |
| `find_entity(path)` | `SceneEntity \| None` | Name or path. |
| `get_mesh_entities()` | `list[SceneEntity]` | Mesh-instance entities. |
| `find_mesh_entity(name)` | `SceneEntity \| None` | Mesh asset name or entity name. |
| `material_count` | `int` | |
| `mesh_count` | `int` | Engine mesh records. |
| `light_count` | `int` | |
| `camera_count` | `int` | Scene camera entities only. See [Camera](#camera). |
| `bounds` | `((min),(max)) \| None` | World AABB of renderable leaves. `None` if empty / not refreshed. |
| `bounds_center` | `(x,y,z) \| None` | |
| `bounds_size` | `(x,y,z) \| None` | `max - min`. |

`EngineApp.find_entity` and `EngineApp.find_material(int)` match the C++ methods. Name-based material lookup stays on `Scene`.

### `ScenePrefab`

Returned by `engine.load(path)` / C++ `EngineApp::load`.

| Property | Type | Notes |
| --- | --- | --- |
| `valid` | `bool` | Handle still points at a loaded prefab. |
| `name` | `str` | Display name. |
| `source_path` | `str` | Import path. |
| `__bool__` | `bool` | Truthy when valid. |

## SceneEntity

Python wrapper around `ecs::Entity`. Returned by spawn / find / light / camera helpers. C++ uses `ecs::Entity` plus `EntityWorld` / `SceneTransforms`. Camera-only properties and methods are summarized here and documented under [Camera](#camera).

| Property | Type | Notes |
| --- | --- | --- |
| `name` | `str` | Read-only. |
| `path` | `str` | Read-only. |
| `mesh_handle` | `MeshHandle` | Asset identity. |
| `is_mesh` | `bool` | Has a mesh instance. |
| `is_camera` | `bool` | Has a `CameraComponent`. |
| `instance_id` | `int` | Stable instance AOV id. `0` auto-hashes authoring id / path. |
| `semantic_id` | `int` | Class AOV id. `0` = unlabeled unless `semantic_label` is set. |
| `semantic_label` | `str` | Optional class name (`SemanticLabel.class`). |
| `is_light` | `bool` | Has a typed light component. |
| `light_type` | `int` / `LightType` | `None_=0`, `Directional`, `Spot`, `Point`, `Rect`, `Environment`. |
| `translation` | `(x,y,z)` | Local. Each assign refreshes the hierarchy; prefer `set_local_pose` for a full TRS. |
| `rotation` | `(x,y,z,w)` | Local quaternion (XYZW, matching scene JSON). |
| `euler` | `(x,y,z)` | Local XYZ **radians**. Assigning converts to the stored quaternion. |
| `scaling` | `(x,y,z)` | |
| `local_pose` | `((x,y,z), (x,y,z,w), (sx,sy,sz))` | Read-only local TRS. Write with `set_local_pose`. |
| `world_pose` | `((x,y,z), (x,y,z,w), (sx,sy,sz))` | Entity world TRS, **no** camera Z-flip. |
| `set_local_pose(position, rotation, scaling=(1,1,1))` | `None` | One hierarchy refresh. |
| `set_world_pose(position, rotation, scaling=(1,1,1))` | `None` | Entity TRS through the parent. Do not use this to aim a camera. |
| `bounds` | AABB tuple or `None` | World subgraph. |
| `color` | `(r,g,b)` | Light. Writable. |
| `position` | `(x,y,z)` | World position (updates local translation). Lights and cameras. |
| `direction` | `(x,y,z)` | World look direction. Cameras keep the current up; lights use `lookatZ`. |
| `camera_pose` | `((x,y,z), (x,y,z), (x,y,z))` | Camera-only world-space look-to `(position, direction, up)`. |
| `vertical_fov` | `float` | Camera, **radians**. Assigning clears custom pinhole intrinsics. |
| `z_near` | `float` | Camera near clip. |
| `z_far` | `float \| None` | Camera far clip. |
| `aspect_ratio` | `float \| None` | Camera. `None` uses the render target. |
| `intrinsics` | `(fx,fy,cx,cy,w,h) \| None` | Camera pinhole, or `None` when using symmetric FOV. |
| `look_to(position, direction, up=(0,1,0))` | `None` | Camera view-space pose (Z-flip). Use this to aim a camera, not `world_pose`. |
| `set_intrinsics(fx, fy, cx, cy, width, height)` | `None` | Camera off-center pinhole. |
| `clear_intrinsics()` | `None` | Restore symmetric FOV on this camera. |
| `activate()` | `None` | Make this the rendered / main camera. |
| `irradiance` | `float` | Directional. |
| `angular_size` | `float` | Directional. |
| `intensity` | `float` | Point / spot, or rect emitted radiance. |
| `radius` / `range` | `float` | Point / spot. |
| `width` / `height` | `float` | Rect, local X/Y. |
| `inner_angle` / `outer_angle` | `float` | Spot, **degrees**. |
| `environment_path` | `str` | Environment light HDRI path. |

`MeshHandle`: `valid`, `name`, truthy when valid.

## Camera

A scene can contain many camera entities (wrist, third-person, COLMAP views, …). The path tracer renders **one** of them at a time: the **active / main** camera. When nothing is selected, that is the free / controller camera, which is **not** a scene entity.

Python does not expose a separate `Camera` type. A scene camera is a `SceneEntity` with `is_camera == True`. C++ uses `ecs::Entity` plus [`CameraApi.h`](caustica/caustica/include/engine/CameraApi.h) on the host and `SceneCameraAccess.h` on the scene layer. Author cameras in JSON as `PerspectiveCamera` / `PerspectiveCameraEx` / `OrthographicCamera` — see [docs/scene-json.md](docs/scene-json.md#perspectivecamera--perspectivecameraex).

### Two camera kinds

| Kind | Identity | How to get it | How to drive it |
| --- | --- | --- | --- |
| Free / controller | Not a scene node. Selection index `0`. | `engine.active_camera is None` / `engine.active_camera_is_free` | `engine.set_camera_*`, `engine.camera_pose` |
| Scene camera | Entity with a `CameraComponent` | `scene.get_cameras()`, `scene.find_camera(name)` | Properties / methods on that entity; `activate()` to render it |

Counts are easy to mix up:

| API | Counts |
| --- | --- |
| `scene.camera_count` / `scene.get_cameras()` | Scene camera **entities** only |
| `engine.scene_camera_count` | Entities **+ 1** (the free camera) |
| `engine.selected_camera_index` | `0` = free camera; `1..N` = `get_cameras()` registration order |

Only **perspective** scene cameras can become the active camera. Orthographic cameras appear in `get_cameras()` for inspection, but `activate()`, `use_camera()`, and `selected_camera_index = i` reject them until an orthographic controller path exists.

Edits on an inactive scene camera stay on the component. They take effect for rendering when that camera is activated.

### Lookup

| Python | C++ | Returns | Notes |
| --- | --- | --- | --- |
| `scene.get_cameras()` | `sceneCameraEntities(app)` | list of entities | Registration order. Does not include the free camera. |
| `scene.find_camera(name)` | `findEntity(app, name)` then check `CameraComponent` | entity / `None` | Match entity **name**. |
| `scene.camera_count` | `sceneCameraEntities(app).size()` | `int` | Entities only. |
| `engine.active_camera` | `activeCameraEntity(app)` / `EngineApp::activeCameraEntity()` | entity / `None` | `None` / `NullEntity` when the free camera is selected. |
| `engine.active_camera_is_free` | `activeCameraIsFree(app)` | `bool` | |
| `engine.active_camera_name` / `.active_camera_path` | `activeCameraName` / `activeCameraPath` | `str` | Empty for the free camera. |
| `engine.use_camera(entity)` | `setActiveCamera(app, entity)` | `None` / `bool` | `None` / `NullEntity` returns to the free camera. Python raises if the entity is not a selectable perspective camera. |
| `engine.use_camera_path(path)` | `setActiveCameraByPath(app, path)` | `None` / `bool` | Hierarchy path, not display name. |
| `camera.activate()` | `setActiveCamera(app, entity)` | `None` / `bool` | Same as `use_camera(camera)`. |
| `engine.selected_camera_index` | `selectedCameraIndex` / `setSelectedCameraIndex` | `int` | Out of range or orthographic → C++ `false`, Python raises. |

```python
print(engine.scene.camera_count)          # entities
print(engine.scene_camera_count)          # entities + free camera
print([c.name for c in engine.scene.get_cameras()])

wrist = engine.scene.find_camera("wrist")
if wrist is None or not wrist.is_camera:
    raise RuntimeError("missing wrist camera")
wrist.activate()
assert engine.active_camera_name == "wrist"

engine.use_camera(None)
assert engine.active_camera_is_free
```

```cpp
const auto& cameras = caustica::sceneCameraEntities(engine->app());
ecs::Entity wrist = caustica::findEntity(engine->app(), "wrist");
engine->setActiveCamera(wrist);
engine->setActiveCamera(ecs::NullEntity); // free camera
```

### Scene camera properties and methods

These live on the camera `SceneEntity`. C++ equivalents take `App&` plus `ecs::Entity` in `CameraApi.h` (`setSceneCamera*`). Scene-layer helpers without `App` are in `SceneCameraAccess.h`.

Assigning a property that fails validation raises in Python and returns `false` in C++.

| Python | C++ | Type | Notes |
| --- | --- | --- | --- |
| `is_camera` | `tryGetCamera` ≠ null | `bool` | Read-only. |
| `name` / `path` | `getEntityName` / `getEntityPath` | `str` | Read-only identity. |
| `camera_pose` | `tryGetCameraWorldLookTo` / `setSceneCameraLookTo` | `((x,y,z), (x,y,z), (x,y,z))` | World-space `(position, direction, up)`. Same space as `look_to`. |
| `look_to(position, direction, up=(0,1,0))` | `setSceneCameraLookTo` | `None` | Aim this camera. Direction is the look axis. |
| `position` | look-to position | `(x,y,z)` | World position. Cameras keep the current up. |
| `direction` | look-to direction | `(x,y,z)` | World look axis. Cameras keep the current up. |
| `vertical_fov` | `sceneCameraVerticalFOV` / `setSceneCameraVerticalFOV` | `float` | **Radians**, open interval `(0, π)`. Assigning **clears** custom pinhole intrinsics. |
| `z_near` | `setSceneCameraZNear` | `float` | Finite and `> 0`. Must stay `< z_far` when far is set. |
| `z_far` | `setSceneCameraZFar` | `float \| None` | `None` = infinite far. If set: finite, `> 0`, and `> z_near`. |
| `aspect_ratio` | `setSceneCameraAspectRatio` | `float \| None` | `None` uses the render target. If set: finite and `> 0`. |
| `intrinsics` | `tryGetCameraIntrinsics` | `(fx,fy,cx,cy,w,h) \| None` | Read-only. `None` when using symmetric FOV. |
| `set_intrinsics(fx, fy, cx, cy, width, height)` | `setSceneCameraIntrinsics` | `None` | Off-center pinhole (OpenCV / COLMAP order). Requires finite `fx,fy,width,height > 0`. Overrides `vertical_fov` until cleared. Distortion is not modeled. |
| `clear_intrinsics()` | `clearSceneCameraIntrinsics` | `None` | Restore symmetric FOV projection. |
| `activate()` | `setActiveCamera` | `None` | Make this the rendered camera. Perspective only. |

`world_pose` / `set_world_pose` / `set_local_pose` are **entity TRS**, not camera view space. Do not copy a mesh pose onto a camera to aim it.

```python
import math

cam = engine.scene.find_camera("wrist")
cam.vertical_fov = math.radians(55.0)
cam.z_near = 0.01
cam.z_far = 100.0
cam.aspect_ratio = None
cam.look_to((0.0, 1.2, 0.15), (0.0, 0.0, 1.0), (0.0, 1.0, 0.0))
# equivalent:
cam.camera_pose = ((0.0, 1.2, 0.15), (0.0, 0.0, 1.0), (0.0, 1.0, 0.0))
cam.set_intrinsics(fx, fy, cx, cy, width, height)
print(cam.intrinsics)
cam.clear_intrinsics()
cam.activate()
```

### Pose spaces

Two different writes:

| API | Writes | Use for |
| --- | --- | --- |
| `look_to` / `camera_pose` / `set_camera_pos_dir_up` | Camera **view space**. The renderer applies a Z-flip. | Aiming a camera |
| `world_pose` / `set_world_pose` / `local_pose` | Entity TRS (`S * R * T`). **No** Z-flip. | Moving meshes / lights / hierarchy |

Copying `mesh.world_pose` onto a camera points the wrong way. Prefer `look_to(position, direction, up)`.

`position` / `direction` on a camera entity go through look-to (keep current up). On a light, `direction` uses `lookatZ`.

### Active / main camera (`EngineApp`)

These always write the camera that is currently rendered — the free controller at index `0`, or the selected perspective scene camera. They do **not** take an entity argument. To edit a camera that is not active, use the `SceneEntity` surface above.

| Python | C++ | Notes |
| --- | --- | --- |
| `set_camera_pos_dir_up(position, direction, up=(0,1,0))` | `setCameraPosDirUp` | View-space look. C++ also has a string overload matching `current_camera_pos_dir_up`. |
| `current_camera_pos_dir_up` | `currentCameraPosDirUp()` | `pos.xyz,dir.xyz,up.xyz` string. |
| `camera_pose` / `set_camera_pose(...)` | `currentCameraPose` / `setCameraPose` | Typed `(position, direction, up)`. |
| `set_camera_vertical_fov(radians)` / `camera_vertical_fov` | `setCameraVerticalFOV` / `cameraVerticalFOV` | Radians. Must be finite and in `(0, π)`. |
| `set_camera_intrinsics(...)` / `clear_camera_intrinsics()` | `setCameraIntrinsics` / `clearCameraIntrinsics` | Active camera only. |
| `save_current_camera()` / `load_current_camera()` | `saveCurrentCamera` / `loadCurrentCamera` | Host persistence path (`campos.txt` next to the executable). |

C++ setters return `bool`. Python raises on invalid input or a failed write.

Signatures: [EngineApp → Active camera](#active-camera).

### Validation

| Write | Accepted |
| --- | --- |
| Vertical FOV | Finite, `(0, π)` radians |
| Intrinsics | Finite; `fx, fy, width, height > 0`; `cx, cy` finite (may be off-center) |
| Pose / look-to | Finite position, direction, up; direction length `> 1e-6` |
| `z_near` | Finite, `> 0`, and `< z_far` when far is set |
| `z_far` | `None`, or finite `> 0` and `> z_near` |
| `aspect_ratio` | `None`, or finite `> 0` |
| `use_camera` / `activate` / `selected_camera_index` | Free camera, or a **registered perspective** scene camera |

### JSON and settings

Declare cameras in the scene file (`PerspectiveCameraEx` is the usual type). Optional `fx, fy, cx, cy, width, height` override symmetric FOV. `settings.startingCamera` is a **scene-camera** index (`-1` = free flight, `0` = first scene camera), which is **not** the same numbering as `engine.selected_camera_index`. Field list: [docs/scene-json.md](docs/scene-json.md#perspectivecamera--perspectivecameraex).

Depth of field and fly-camera speed are **session** settings, not per-entity optics: `settings.camera_aperture`, `settings.camera_focal_distance`, `settings.camera_move_speed`. A `PerspectiveCameraEx` can override tone-mapping / exposure when it becomes active — see [Camera / firefly / tone map / bloom](#camera--firefly--tone-map--bloom).

There is no public spawn-camera helper yet. Add cameras in scene JSON (or spawn a JSON snippet that contains a camera entity).

### C++ per-entity helpers

```cpp
#include <caustica.h>

ecs::Entity wrist = caustica::findEntity(engine->app(), "wrist");
caustica::setSceneCameraVerticalFOV(engine->app(), wrist, dm::radians(55.f));
caustica::setSceneCameraLookTo(
    engine->app(), wrist,
    {0.f, 1.2f, 0.15f}, {0.f, 0.f, 1.f}, {0.f, 1.f, 0.f});
caustica::setSceneCameraIntrinsics(engine->app(), wrist, fx, fy, cx, cy, width, height);
caustica::clearSceneCameraIntrinsics(engine->app(), wrist);
caustica::setSceneCameraZNear(engine->app(), wrist, 0.01f);
caustica::setSceneCameraZFar(engine->app(), wrist, 100.f);
caustica::setSceneCameraAspectRatio(engine->app(), wrist, std::nullopt);
engine->setActiveCamera(wrist);
```

`CameraPose` is `{ position, direction, up }` in the same look-to space as Python `camera_pose`.

## Sensor output / AOV

RGB, linear depth, camera-space normals, stable instance/semantic IDs, and motion vectors for Caelis Sim and other hosts. Multiple `RenderProduct`s can be captured at the **same physical time** without stepping simulation.

| AOV | Format | Convention |
| --- | --- | --- |
| RGB | RGBA8 | Same LDR as `save_screenshot` |
| Depth | float32 `H×W` | Linear **\|view Z\|** in meters; **0 = miss** |
| Normal | float32 `H×W×3` | **Camera / view space**; background 0 |
| Instance ID | uint32 `H×W` | **0 = miss**; never auto-assigned 0 |
| Semantic ID | uint32 `H×W` | **0 = unlabeled / miss** |
| Motion | float32 `H×W×2` | Screen-space pixel motion |
| Segmentation | | Alias of instance ID |
| Diffuse | float32 `H×W×3` | First-hit linear diffuse albedo at material-AOV resolution |
| Roughness | float32 `H×W` | First-hit perceptual roughness at material-AOV resolution |
| Specular | float32 `H×W×3` | First-hit specular F0 at material-AOV resolution |
| Metallic | float32 `H×W` | First-hit metalness at material-AOV resolution |
| Throughput | float32 `H×W×3` | Primary path throughput at material-AOV resolution |
| Guide diffuse | float32 `H×W×3` | DLSS-RR / denoiser diffuse-albedo guide at its native resolution |

`depth == 0` means a camera-ray miss (including the environment); it is not a far-plane value. `normal`, `instance_id`, and `semantic_id` are also zero on a miss.
With an upscaler, RGB uses the display resolution while geometry/material/guide AOVs use the renderer's internal resolution. Use `geometry_width`, `geometry_height`, `material_width`, `material_height`, `guide_width`, and `guide_height` for those arrays; `width` and `height` describe RGB.

Motion uses the current camera view minus that camera's previous captured view, in pixels. Caustica keeps history **per camera entity**, so switching between wrist, third-person, and stereo cameras does not leak one camera's history into another. The first captured frame for a camera has zero camera motion; object motion remains available when the renderer has a previous transform.

`engine.render()` fills these on `Frame`. To capture wrist / third-person / stereo together, register products then call `capture_sensor_outputs()` after `step_frame()`:

```python
wrist = engine.find_entity("wrist")
third = engine.find_entity("third_person")
engine.add_render_product("wrist", wrist, caustica.Aov.all)
engine.add_render_product("third", third, caustica.Aov.rgb | caustica.Aov.depth)
engine.step_frame()
for product in engine.capture_sensor_outputs():
    depth = product.depth   # NumPy (H, W) float32
    inst = product.instance_id
```

```cpp
engine->addRenderProduct({ .name = "wrist", .camera = wrist, .aovs = uint32_t(caustica::Aov::All) });
engine->stepFrame();
for (const caustica::SensorOutput& product : engine->captureSensorOutputs())
    (void)product.depth;
```

Stable IDs: set `SceneEntity.instance_id` / `semantic_id` / `semantic_label`, or author `SemanticLabel` in scene JSON. Auto instance IDs hash `entities[].id`, then the entity path. Dataset alignment should set IDs explicitly, both to make object/URDF-link correspondence explicit and to avoid an authoring-name hash collision. Details: [docs/scene-json.md](docs/scene-json.md#semanticlabel). Extra views override `ResolvedActiveCamera` for one frozen Extract+Render while preserving per-camera motion history.

Orthographic cameras are not valid RenderProduct cameras.

## Materials

Python `Material` is C++ `StandardMaterial`. Property setters mark `gpuDataDirty`. Colors are linear RGB and are **not** clamped.

### Identifiers (read-only)

| Property | Type |
| --- | --- |
| `name` | `str` |
| `model_name` | `str` |
| `unique_name` | `str` |

### Writable parameters

| Property | Type | Notes |
| --- | --- | --- |
| `base_color` | `(r,g,b)` | Metal-rough base or spec-gloss diffuse. |
| `specular_color` | `(r,g,b)` | |
| `emissive_color` | `(r,g,b)` | |
| `emissive_intensity` | `float` | |
| `metalness` | `float` | |
| `roughness` | `float` | |
| `material_model` | `str` | Always `"OpenPBR"`; writes coerce. |
| `base_weight` | `float` | |
| `base_diffuse_roughness` | `float` | |
| `specular_weight` | `float` | |
| `anisotropy` | `float` | |
| `fuzz_weight` | `float` | |
| `fuzz_color` | `(r,g,b)` | |
| `fuzz_roughness` | `float` | |
| `coat_weight` | `float` | |
| `coat_color` | `(r,g,b)` | |
| `coat_roughness` | `float` | |
| `coat_roughness_anisotropy` | `float` | |
| `coat_ior` | `float` | |
| `coat_darkening` | `float` | |
| `subsurface_weight` | `float` | |
| `subsurface_color` | `(r,g,b)` | |
| `subsurface_radius` | `float` | |
| `subsurface_radius_scale` | `(r,g,b)` | |
| `subsurface_anisotropy` | `float` | |
| `thin_film_weight` | `float` | |
| `thin_film_thickness` | `float` | |
| `thin_film_ior` | `float` | |
| `transmission_color` | `(r,g,b)` | |
| `transmission_depth` | `float` | |
| `transmission_scatter` | `(r,g,b)` | |
| `transmission_scatter_anisotropy` | `float` | |
| `transmission_dispersion_scale` | `float` | |
| `transmission_dispersion_abbe_number` | `float` | |
| `opacity` | `float` | Multiplied by base-texture alpha when `enable_base_texture`. |
| `transmission_factor` | `float` | |
| `diffuse_transmission_factor` | `float` | |
| `normal_texture_scale` | `float` | |
| `coat_normal_scale` | `float` | |
| `ior` | `float` | |
| `alpha_cutoff` | `float` | Classification edit. |
| `volume_attenuation_distance` | `float` | |
| `volume_attenuation_color` | `(r,g,b)` | |
| `nested_priority` | `int` | |
| `use_specular_gloss` | `bool` | Classification edit. |
| `enable_alpha_testing` | `bool` | Classification edit. |
| `enable_transmission` | `bool` | Classification edit. |
| `thin_surface` | `bool` | |
| `exclude_from_nee` | `bool` | Classification edit. |
| `unlit_receive_shadows` | `bool` | Flat color + sampled-light shadows. Not a PBR mode. |
| `unlit_shadow_strength` | `float` | `0..1`. Only used when `unlit_receive_shadows`. |
| `enable_as_analytic_light_proxy` | `bool` | |
| `skip_render` | `bool` | Classification edit. |
| `metalness_in_red_channel` | `bool` | ORM packing. |
| `enable_base_texture` | `bool` | |
| `enable_orm_texture` | `bool` | |
| `enable_normal_texture` | `bool` | |
| `enable_coat_normal_texture` | `bool` | |
| `enable_emissive_texture` | `bool` | |
| `enable_transmission_texture` | `bool` | |

Scene **JSON** still uses OpenPBR file keys (`base_metalness`, …). Those are not Python property names.

### Texture paths (read-only)

| Property | Type |
| --- | --- |
| `base_texture_path` | `str \| None` |
| `orm_texture_path` | `str \| None` |
| `normal_texture_path` | `str \| None` |
| `coat_normal_texture_path` | `str \| None` |
| `emissive_texture_path` | `str \| None` |
| `transmission_texture_path` | `str \| None` |

### Methods

| API | Returns | Notes |
| --- | --- | --- |
| `mark_dirty()` | `None` | Force GPU buffer refresh next frame. |
| `set_texture(slot, path, srgb=None, normal_map=None)` | `bool` | Generic slot replace. `False` if unresolved. |
| `set_base_texture(path, srgb=None)` | `bool` | Default sRGB. |
| `set_orm_texture(path, srgb=None)` | `bool` | Linear metal-rough / sRGB spec-gloss. |
| `set_normal_texture(path)` | `bool` | Linear normal map. |
| `set_coat_normal_texture(path)` | `bool` | Linear coat normal. |
| `set_emissive_texture(path, srgb=None)` | `bool` | Default sRGB. |
| `set_transmission_texture(path, srgb=None)` | `bool` | Default linear. |
| `clear_texture(slot)` | `None` | Disconnect and disable. |
| `clear_base_texture()` … `clear_transmission_texture()` | `None` | Slot-specific clears. |

### Runtime update rules

- Setters already mark GPU data dirty; the material uploads on the next rendered frame.
- In reference mode, call `engine.reset_accumulation()` after visible edits or old samples stay blended in.
- If a texture slot is enabled, scalars multiply the sample. Metal-rough: `base_color * base_texture.rgb`, `roughness * ORM.g`, `metalness * ORM.b` unless `metalness_in_red_channel=True`.
- Pure parameter edits (color, roughness, metalness, opacity, texture toggles, emissive, normal scale, IOR) are next-frame updates.
- Classification edits (`use_specular_gloss`, `enable_alpha_testing`, `alpha_cutoff`, `enable_transmission`, `exclude_from_nee`, `skip_render`) can change shaders or AS metadata — follow with `request_shader_reload()` and `request_full_accel_rebuild()`.

See [Cookbook](#edit-materials) for the full edit / texture-replace recipes. OpenPBR authoring: [docs/openpbr.md](docs/openpbr.md).

## Lights

Spawn on `EngineApp`. Lookup on Python `Scene`. Typed fields are properties on `SceneEntity`. Prefer `caustica.LightType` over raw integers.

| C++ | Python | Returns | Notes |
| --- | --- | --- | --- |
| `spawnDirectionalLight(DirectionalLightComponent, name)` | `engine.spawn_directional_light(...)` | entity |  |
| `spawnPointLight` | `engine.spawn_point_light(...)` | entity |  |
| `spawnSpotLight` | `engine.spawn_spot_light(...)` | entity | `inner_angle` / `outer_angle` degrees. |
| `spawnRectLight` | `engine.spawn_rect_light(...)` | entity | One-sided, local −Z. |
| `spawnEnvironmentLight` | `engine.spawn_environment_light(...)` | entity |  |
| `Query` / light components | `scene.get_lights()` | `list[SceneEntity]` |  |
| `findEntity` | `scene.find_light(name)` | `SceneEntity \| None` | |
| `setEnvMapOverrideSource` | `engine.set_env_map_override_source(path)` | `void` |  |

See [Cookbook](#edit-lights). Environment tweaks also live on `settings.environment_map`.

## Spawn / despawn

Supported extensions: `.gltf`, `.glb`, `.obj`, `.urdf`, `.usd` / `.usda` / `.usdc`, `.prefab.json`. Extract publishes a new proxy generation; GPU mesh/AS/SBT work is built on the render thread asynchronously.

| C++ | Python | Returns | Notes |
| --- | --- | --- | --- |
| `load(path)` | `load(path)` | prefab | CPU handle, no spawn. |
| `spawn(prefab)` | `spawn(prefab)` | entity | Root entity. |
| `spawnFromFile(path)` | `spawn_from_file(path)` | entity | One-shot import + attach. |
| `spawnFromSource(source)` | `spawn_from_source(source)` | entity | Inline JSON. |
| `despawn(entity)` | `despawn(entity)` | `bool` | Entity and children. |

See [Cookbook](#spawn--despawn-assets) and [Load OBJ meshes with materials](#load-obj-meshes-with-materials).

## Mesh deformation

| C++ | Python | Returns | Notes |
| --- | --- | --- | --- |
| `getMeshVertices(entity)` | `get_mesh_vertices(entity)` | `list[tuple]` | Unique object-space positions. |
| `setMeshVertices(entity, vertices, options)` | `set_mesh_vertices(entity, vertices, recompute_normals=True, rebuild_acceleration_structure=True)` | `void` | Length must match getter. |
| — | `deform_mesh(entity, callback, ...)` | `int` | `callback(index, (x,y,z))` → new triple or `None`. |
| `getMeshVerticesWorld` / `setMeshVerticesWorld` | `get_mesh_vertices_world` / `set_mesh_vertices_world` | same | Uses that entity's transform. |
| — | `deform_mesh_world(...)` | `int` | World-space callback. |
| `requestMeshAccelRebuild(entity)` | `request_mesh_accel_rebuild(entity)` | `void` | |

`set_mesh_vertices` updates object-space mesh bounds, optionally recomputes normals, refreshes GPU vertex data, resets accumulation, and requests AS rebuild by default. Keep `rebuild_acceleration_structure=True` for ray-tracing-correct geometry. Only set it `False` when batching several edits, then call `request_full_accel_rebuild()` once.

`_world` variants refresh transform state first, so recent `entity.translation = ...` is reflected. Shared mesh buffers: deforming through one mesh entity updates other instances of the same engine mesh record.

See [Cookbook](#deform-mesh-vertices).

## 3D Gaussian splats

Prefer `GaussianSplat` nodes in scene JSON. `loadGaussianSplatFile` / `load_gaussian_splat_file` appends under the current root. `setScene` replaces the graph, including previously appended splat nodes.

Rasterization covers all enabled 3DGS objects. Emissive proxy sampling combines them. Splat shadows currently use the first enabled object as the primary shadow source.

`gaussian_splat_translation`, `gaussian_splat_rotation_euler_deg`, and `gaussian_splat_object_scale` apply when a new node is **appended** through `load_gaussian_splat_file`.

See [Cookbook](#load-3d-gaussian-splats), [3DGS reference / realtime batch](#3dgs-reference--realtime-batch), and [COLMAP camera 3DGS alignment](#colmap-camera-3dgs-alignment).

### 3DGS settings (`settings.*`)

C++ members are PascalCase on `PathTracerSettings` (`EnableGaussianSplats`, …).

| Python | Type | Notes |
| --- | --- | --- |
| `enable_gaussian_splats` | `bool` | |
| `gaussian_splat_depth_test` | `bool` | Test against scene depth. |
| `gaussian_splat_depth_bias` | `float` | Reverse-Z bias for mesh/3DGS intersections. |
| `gaussian_splat_depth_edge_dilation` | `bool` | Conservative mesh depth at silhouettes. |
| `gaussian_splat_primary_method` | `int` / `GaussianSplatPrimaryMethod` | `GS` or `GUT`. |
| `gaussian_splat_sorting_mode` | `int` / `GaussianSplatSortMode` | |
| `gaussian_splat_sh_format` | `int` / `GaussianSplatStorageFormat` | |
| `gaussian_splat_rgba_format` | `int` / `GaussianSplatStorageFormat` | |
| `gaussian_splat_use_aabbs` | `bool` | Shadow acceleration. |
| `gaussian_splat_use_tlas_instances` | `bool` | |
| `gaussian_splat_blas_compaction` | `bool` | |
| `gaussian_splat_mip_antialiasing` | `bool` | |
| `gaussian_splat_quantize_normals` | `bool` | |
| `gaussian_splat_ftb_sync_mode` | `int` / `GaussianSplatFTBSyncMode` | |
| `gaussian_splat_frustum_culling` | `int` / `GaussianSplatFrustumCulling` | |
| `gaussian_splat_frustum_dilation` | `float` | |
| `gaussian_splat_screen_size_culling` | `bool` | |
| `gaussian_splat_min_pixel_coverage` | `float` | |
| `gaussian_splat_depth_iso_threshold` | `float` | |
| `gaussian_splat_fragment_shader_barycentric` | `bool` | |
| `gaussian_splat_scale` | `float` | Projected footprint. |
| `gaussian_splat_alpha_scale` | `float` | Opacity multiplier. |
| `gaussian_splat_brightness` | `float` | Color multiplier. |
| `gaussian_splat_tint_color` | `(r,g,b)` | Multiplies SH0/base color. |
| `gaussian_splat_as_emitter` | `bool` | Emissive proxies into light sampling. |
| `gaussian_splat_emission_intensity` | `float` | |
| `gaussian_splat_emission_max_proxy_count` | `int` | |
| `gaussian_splat_alpha_cull_threshold` | `float` | |
| `gaussian_splat_translation` | `(x,y,z)` | Initial pose for newly appended Python/C++ nodes. Resets accumulation. |
| `gaussian_splat_rotation_euler_deg` | `(x,y,z)` | Degrees. |
| `gaussian_splat_object_scale` | `(x,y,z)` | |
| `gaussian_splat_shadows` | `bool` | |
| `gaussian_splat_shadows_mode` | `int` / `GaussianSplatShadowMode` | Orthogonal to primary GS/GUT. |
| `gaussian_splat_shadow_strength` | `float` | |
| `gaussian_splat_shadow_soft_radius` | `float` | |
| `gaussian_splat_shadow_soft_sample_count` | `int` | |
| `gaussian_splat_shadow_kernel_degree` | `int` | |
| `gaussian_splat_shadow_adaptive_clamp` | `bool` | |
| `gaussian_splat_shadow_ray_offset` | `float` | |
| `gaussian_splat_projection_method` | `int` | 3DGUT extent: `0` Eigen, `1` Conic. Ignored by 3DGS. |
| `gaussian_splat_covariance_dilation` | `float` | Typically `0.1` or `0.3`. |
| `gaussian_splat_reference_gamma_compositing` | `bool` | sRGB alpha compositing for GPU-sorted splats. |
| `gaussian_splat_apply_tone_mapping` | `bool` | Composite before tone map when true. |
| `gaussian_splat_object_count` | `int` | Read-only. Also on `EngineApp`. |
| `gaussian_splat_count` | `int` | Read-only. |
| `gaussian_splat_file_name` | `str` | Read-only. |

## Settings

`engine.settings` is live `PathTracerSettings` (same object as the ImGui UI). Python is snake_case; C++ is PascalCase (`realtime_mode` ↔ `RealtimeMode`). Most writes take effect on subsequent frames.

### General / path tracing

| C++ | Python | Type | Notes |
| --- | --- | --- | --- |
| `EnableAnimations` | `enable_animations` | `bool` | Imported / skeletal. |
| `EnableKeyframes` | `enable_keyframes` | `bool` | Editor timeline. |
| `EnableVsync` | `enable_vsync` | `bool` |  |
| `FPSLimiter` | `fps_limiter` | `int` | `0` = uncapped. |
| `RealtimeMode` | `realtime_mode` | `bool` | Changing it resets accumulation. |
| `RealtimeSamplesPerPixel` | `realtime_samples_per_pixel` | `int` |  |
| `AccumulationTarget` | `accumulation_target` | `int` | Reference SPP. |
| `ResetAccumulation` | `reset_accumulation` | `bool` |  |
| `ResetRealtimeCaches` | `reset_realtime_caches` | `bool` | ReSTIR / temporal history. |
| `AccumulationAA` | `accumulation_aa` | `bool` |  |
| `AccumulationPreWarmRealtimeCaches` | `accumulation_prewarm_realtime_caches` | `bool` |  |
| `DbgFreezeRealtimeNoiseSeed` | `freeze_realtime_noise_seed` | `bool` | Freeze camera jitter / realtime noise sequences for A/B and AOV diagnostics. |
| `BounceCount` | `bounce_count` | `int` |  |
| `DiffuseBounceCount` | `diffuse_bounce_count` | `int` |  |
| `EnableRussianRoulette` | `enable_russian_roulette` | `bool` |  |
| `TexLODBias` | `texture_lod_bias` | `float` |  |

### NEE / ReSTIR

| C++ | Python | Type | Notes |
| --- | --- | --- | --- |
| `UseNEE` | `use_nee` | `bool` |  |
| `NEEType` | `nee_type` | `int` | `0` uniform, `1` power, `2` NEE-AT. |
| `NEECandidateSamples` | `nee_candidate_samples` | `int` |  |
| `NEEFullSamples` | `nee_full_samples` | `int` | Each full sample is a shadow ray. |
| `NEEMISType` | `nee_mis_type` | `int` |  |
| `UseReSTIRDI` | `use_restir_di` | `bool` |  |
| `UseReSTIRGI` | `use_restir_gi` | `bool` |  |
| `UseReSTIRPT` | `use_restir_pt` | `bool` |  |

### Camera / firefly / tone map / bloom

| C++ | Python | Type |
| --- | --- | --- |
| `CameraAperture` | `camera_aperture` | `float` |
| `CameraFocalDistance` | `camera_focal_distance` | `float` |
| `CameraMoveSpeed` | `camera_move_speed` | `float` |
| `RealtimeFireflyFilterEnabled` | `realtime_firefly_filter_enabled` | `bool` |
| `RealtimeFireflyFilterThreshold` | `realtime_firefly_filter_threshold` | `float` |
| `ReferenceFireflyFilterEnabled` | `reference_firefly_filter_enabled` | `bool` |
| `ReferenceFireflyFilterThreshold` | `reference_firefly_filter_threshold` | `float` |
| `EnableToneMapping` | `enable_tone_mapping` | `bool` |
| `ToneMappingParams` | `tone_mapping_params` | `ToneMappingParams` |
| `EnableBloom` | `enable_bloom` | `bool` |
| `BloomIntensity` | `bloom_intensity` | `float` |
| `BloomRadius` | `bloom_radius` | `float` |

`tone_mapping_params` is a live object. A scene camera's `PerspectiveCameraEx.toneMapOperator` can override it; see [docs/scene-json.md](docs/scene-json.md#perspectivecamera).

```python
s = engine.settings
s.enable_tone_mapping = True
s.tone_mapping_params.tone_map_operator = caustica.ToneMapOperator.AgX
s.tone_mapping_params.auto_exposure = True
s.tone_mapping_params.exposure_compensation = 1.0
```

#### `ToneMappingParams`

| Property / method | Type | Notes |
| --- | --- | --- |
| `exposure_mode` | `ExposureMode` | Aperture / shutter priority. |
| `tone_map_operator` | `ToneMapOperator` | |
| `auto_exposure` | `bool` | |
| `exposure_compensation` | `float` | |
| `exposure_value` | `float` | |
| `film_speed` / `f_number` / `shutter` | `float` | |
| `white_balance` / `white_point` | | |
| `white_max_luminance` / `white_scale` | `float` | |
| `clamped` | `bool` | |
| `exposure_value_min` / `exposure_value_max` | `float` | |
| `camera_lut_enabled` | `bool` | |
| `camera_lut_after_tone_map` | `bool` | |
| `camera_lut_preset` | `CameraLutPreset` | |
| `camera_lut_is_3d` | `bool` | Read-only. |
| `camera_lut_domain_min` / `camera_lut_domain_max` | `(x,y,z)` | |
| `camera_lut_path` | `str` | Read-only. |
| `load_camera_lut(path)` | | Load and enable a 1D `.cube` LUT. |
| `apply_camera_lut_preset(preset)` | | Built-in look. |
| `clear_camera_lut()` | | Disable LUT. |

`Settings.load_camera_lut` / `clear_camera_lut` forward to `tone_mapping_params`.

### Realtime AA / DLSS / Reflex

Availability depends on build options and hardware.

| Python | Type | Notes |
| --- | --- | --- |
| `realtime_aa` | `int` / `RealtimeAA` | `0=Off`, `1=TAA`, `2=DLSS`, `3=DLSS_RR`. |
| `dlss_mode` | `int` / `DLSSMode` | |
| `dlss_lod_bias_use_override` | `bool` | |
| `dlss_lod_bias_override` | `float` | |
| `dlss_always_use_extents` | `bool` | |
| `dlss_fg_mode` | `int` / `DLSSFGMode` | |
| `dlss_fg_multiplier` | `int` | |
| `dlss_fg_num_frames_to_generate` | `int` | |
| `dlss_fg_max_num_frames_to_generate` | `int` | |
| `dlss_rr_preset` | `int` / `DLSSRRPreset` | |
| `dlss_rr_micro_jitter` | | |
| `dlss_rr_brightness_clamp_k` | `float` | |
| `disable_restirs_with_dlss_rr` | `bool` | |
| `reflex_mode` | `int` / `ReflexMode` | |
| `reflex_capped_fps` | `float`/`int` | |
| `is_dlss_supported` | `bool` | Read-only. |
| `is_dlss_fg_supported` | `bool` | Read-only. |
| `is_dlss_rr_supported` | `bool` | Read-only. |
| `is_reflex_supported` | `bool` | Read-only. |

### Denoisers

| Python | Type | Notes |
| --- | --- | --- |
| `standalone_denoiser` | `bool` | NRD in realtime; no effect with DLSS-RR. |
| `denoiser_radiance_clamp_k` | `float` | |
| `oidn_enabled` | `bool` | Run OIDN after accumulation completes. |
| `oidn_use_gpu` | `bool` | CUDA/HIP/SYCL if available. |
| `oidn_passes` | `int` / `OidnPasses` | |
| `oidn_prefilter` | `int` / `OidnPrefilter` | |
| `oidn_quality` | `int` / `OidnQuality` | |
| `oidn_changed` | `bool` | Set true after OIDN edits; renderer clears it. |
| `oidn_apply()` | | Marks OIDN dirty. |

### Environment map

`settings.environment_map` (`EnvironmentMapParams`):

| Property | Type |
| --- | --- |
| `tint_color` | `(r,g,b)` |
| `intensity` | `float` |
| `rotation_xyz` | `(x,y,z)` |
| `enabled` | `bool` |
| `visible_to_camera` | `bool` |

## Enums

Arithmetic: `int(enum_value)` works and enum values can be assigned to int-backed settings.

| Enum | Values |
| --- | --- |
| `ToneMapOperator` | `Linear`, `Reinhard`, `ReinhardModified`, `HejiHableAlu`, `HableUc2`, `Aces`, `PbrNeutral`, `IdentitySoftShoulder`, `AgX`, `CameraLut` |
| `ExposureMode` | `AperturePriority`, `ShutterPriority` |
| `CameraLutPreset` | `Disabled`, `Neutral`, `SoftContrast`, `WarmFilm`, `CoolFilm` |
| `RealtimeAA` | `Off=0`, `TAA=1`, `DLSS=2`, `DLSS_RR=3` |
| `DLSSMode` | `Off`, `MaxPerformance`, `Balanced`, `MaxQuality`, `UltraPerformance`, `UltraQuality`, `DLAA` |
| `DLSSFGMode` | `Off`, `On`, `Auto` |
| `DLSSRRPreset` | `Default`, `PresetA` … `PresetH` |
| `ReflexMode` | `Off`, `LowLatency`, `LowLatencyWithBoost` |
| `OidnPasses` | `ColorOnly=0`, `Albedo=1`, `AlbedoNormal=2` |
| `OidnPrefilter` | `None_=0`, `Fast=1`, `Accurate=2` |
| `OidnQuality` | `Fast=0`, `Balanced=1`, `High=2` |
| `TextureSlot` | `Base`, `ORM` / `OcclusionRoughnessMetallic`, `Normal`, `CoatNormal`, `Emissive`, `Transmission` |
| `LightType` | `None_=0`, `Directional`, `Spot`, `Point`, `Rect`, `Environment` |
| `Aov` | `none`, `rgb`, `depth`, `normal`, `instance_id`, `semantic_id`, `motion_vector`, `diffuse`, `roughness`, `specular`, `metallic`, `throughput`, `guide_diffuse`, `segmentation` (= instance_id), `all` |
| `GaussianSplatSortMode` | `GpuSort=0`, `StochasticSplats=1` |
| `GaussianSplatPrimaryMethod` | `GS=0` (3DGS), `GUT=1` (3DGUT) |
| `GaussianSplatStorageFormat` | `Float32=0`, `Float16=1`, `Uint8=2` |
| `GaussianSplatFrustumCulling` | `Disabled=0`, `AtDistanceStage=1`, `AtRasterStage=2` |
| `GaussianSplatShadowMode` | `Disabled=0`, `Hard=1`, `Soft=2` |
| `GaussianSplatFTBSyncMode` | `Disabled=0`, `Interlock=1` |

There is no Python `PathTracerMode` enum. Use `settings.realtime_mode` / C++ `RealtimeMode`.

## Python helpers

Python-only EngineApp sugar, plus `GpuDevice` and `Frame`. Module functions are also listed under [Module-level API](#module-level-api).

| API | Notes |
| --- | --- |
| `caustica.MODE` | `"extension"` or `"embed"`. |
| `caustica.engine()` | Extension: most recently created `EngineApp`. Embed: borrowed editor session. |
| `caustica.settings()` | Shortcut for `engine.settings`. |
| `caustica.log_info` / `log_warning` / `log_error` | Native log. |
| `caustica.enumerate_adapters(...)` | Extension. |
| `caustica.GpuDevice(...)` | Extension. GPU handle; no scene. Size/headless applied on first EngineApp bind. |
| `caustica.builtin_scene_json(builtin_model="plane_cube")` | Extension. Inline JSON for `plane`, `cube`, `sphere`, `plane_cube`. |
| `step_n(frames)` | Call `step_frame` N times. |
| `step_until_accumulated(max_frames=0)` | Step until `accumulation_completed`. |
| `get_pixels()` | NumPy `(H, W, 4)` uint8 RGBA. Requires NumPy. Extension. |
| `read_ldr_framebuffer()` | `Framebuffer` (raw bytes). Extension. |
| `render(dt=-1.0)` | `step_frame` then `Frame` (RGB + AOVs). Extension. |
| `render_reference(spp=64, oidn=True)` | Accumulate then `Frame`. Extension. |
| `capture_sensor_outputs()` | `list[SensorOutput]` for registered RenderProducts. |
| `render_reference_frame` / `render_realtime_frame` | Shared with embed; return a frame count, not a `Frame`. |
| `deform_mesh` / `deform_mesh_world` | Callback `(index, (x,y,z)) -> triple or None`. |
| `with EngineApp.create(...)` | Calls `shutdown()` on exit. |

### `GpuDevice`

```python
caustica.GpuDevice(vulkan=False, adapter="auto", debug=False)
```

The logical GPU, surface, and optional window are created on the first EngineApp bind. A second EngineApp on the same device must use the same width/height/headless. Close the EngineApp before `device.close()`.

| Property / method | Type | Notes |
| --- | --- | --- |
| `vulkan` | `bool` | Requested backend. |
| `adapter` | `str` | Selector string. |
| `debug` | `bool` | |
| `created` | `bool` | Logical device exists. |
| `bound` | `bool` | An EngineApp is using this device. |
| `width` / `height` / `headless` | | Applied on first bind. |
| `selected_adapter` | `AdapterInfo \| None` | Adapter chosen at device creation. |
| `close()` | | Fails if an EngineApp is still bound. |
| `with GpuDevice(...)` | | Calls `close()` on exit. |

### `Frame` / `Framebuffer`

LDR final color after at least one successful step. Tightly packed **RGBA8**, row-major, **top-left** origin. Same source as `save_screenshot`. HDR readback is not implemented.

`Frame` (from `render` / `render_reference`):

| Field | Type |
| --- | --- |
| `rgb` | NumPy `(H, W, 4)` uint8 |
| `pixels` | `bytes` |
| `width`, `height`, `channels` | |
| `depth` | NumPy `(H, W)` float32 linear \|view Z\| meters; `None` if empty |
| `normal` | NumPy `(H, W, 3)` float32 camera-space; `None` if empty |
| `instance_id` / `segmentation` | NumPy `(H, W)` uint32; `0` = miss |
| `semantic_id` | NumPy `(H, W)` uint32; `0` = unlabeled / miss |
| `motion_vector` | NumPy `(H, W, 2)` float32 pixels; `None` if empty |

`Framebuffer` (from `read_ldr_framebuffer` / C++ `LdrFramebuffer`):

| Field | Type | Notes |
| --- | --- | --- |
| `width` / `height` | `int` | |
| `channels` | `int` | Always `4`. |
| `format` | `str` | `"RGBA8"` (Python). |
| `dtype` | `str` | `"uint8"` (Python). |
| `pixels` | `bytes` / `vector<uint8_t>` | Tight RGBA8, top-left. `len == width * height * 4`. |
| `shape` | `(H, W, 4)` | Python only. |

```python
engine.step_until_accumulated()
img = engine.get_pixels()          # NumPy
fb = engine.read_ldr_framebuffer() # bytes via fb.pixels
```

## C++ systems

Python does not bind the schedule. C++ hosts add systems before `run()` / `stepFrame()`:

```cpp
engine->addSystem<MySimLabel>(caustica::AppSchedule::update,
    [](caustica::EntityWorld scene,
       caustica::ecs::Query<caustica::scene::LocalTransformComponent> q) {
        // ...
    });
engine->run();
```

Simulation systems run concurrently when their parameter lists prove they cannot conflict. You never declare access by hand.

| Parameter | Parallel? | Use |
| --- | --- | --- |
| `Query<...>` | yes, if disjoint | Iterate matching entities. |
| `Res<T>` / `ResMut<T>` | yes / exclusive vs other mutators | Resources (`Res<Time>` for the clock). |
| `SceneTransforms` | yes | Per-frame transform writes. |
| `EntityWorld` | exclusive | Spawn / despawn / emplace / find. Keep out of hot per-frame systems. |
| `SystemContext&` | exclusive | Whole engine. Structural / one-shot only. |

Scene nodes go through `EntityWorld::spawn`, not `Commands.spawn()`. See [docs/architecture-render-proxy.md](docs/architecture-render-proxy.md) and the thin client for the exclusive-setup vs parallel-spin split.

## Host & examples

How the same `EngineApp` is hosted from C++, standalone Python, or `caustica.exe`.

## Embed and extension

### Embed (`caustica.exe`)

```powershell
caustica.exe --pythonScript examples/python/embedded.py
caustica.exe --pythonExpr "import caustica; print(caustica.engine().scene_name)"
```

```python
import caustica

engine = caustica.engine()
s = caustica.settings()
s.realtime_mode = True
s.realtime_aa = int(caustica.RealtimeAA.TAA)
engine.reset_accumulation()
```

Do not call `EngineApp.create()` in embed mode. Do not `with` the borrowed handle — `__exit__` calls `shutdown()`.

### Extension (standalone Python)

Every `EngineApp.create` owns (or borrows) a GPU. Use `shutdown()` or `with` so GPU resources are released.

Windowed extension:

- `headless=False` opens a GLFW window.
- Call `step_frame()` repeatedly to pump events and render.
- Window close makes `step_frame()` return `False`.
- Resize / maximize / minimize are handled during `step_frame()`.

## In-tree examples

| Path | Purpose |
| --- | --- |
| `examples/cpp/thin_client` | Official C++ host: create, systems, spawn, camera, settings. |
| `examples/python/render.py` | Official Python host: reference / realtime / windowed, spawn, materials, camera, readback. |
| `examples/python/gaussian_splats.py` | 3DGS `view` / `hybrid` / `colmap`. |
| `examples/python/animation_sequence.py` | Timeline samples. |
| `examples/python/mesh_deformation.py` | Per-frame `deform_mesh`. |
| `examples/python/camera_intrinsics.py` | Off-center pinhole. |
| `examples/python/environment_lighting.py` | HDRI / procedural sky. |
| `examples/python/embedded.py` | Embed inside `caustica.exe`. |

Helpers (not runnable): `examples/python/_common.py`, `_gaussian.py`, `_colmap.py`. `embedded.py` imports none of them — the embed host does not put the script directory on `sys.path`.

```python
import caustica
help(caustica)
help(caustica.EngineApp)
```

## Related docs

* [docs/public-api.md](docs/public-api.md) — C++ header allowlist.
* [docs/embedding-cpp.md](docs/embedding-cpp.md) — C++ lifecycle, `EngineAppDesc`, parallel systems, CMake.
* [docs/build-and-run.md](docs/build-and-run.md) — build, runtime layout, CLI.
* [docs/scene-json.md](docs/scene-json.md) — scene files, including camera nodes.
* [docs/openpbr.md](docs/openpbr.md) — materials.
* [examples/python/README.md](examples/python/README.md) — Python example index.
