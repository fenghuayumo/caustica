# caustica Python API Reference

This document describes how to use the current `caustica` Python bindings. The API is primarily defined in:

- `caustica/Python/PythonBindingsCore.cpp`
- `caustica/Python/PythonBindings_Extension.cpp`
- `caustica/Python/PythonBindings_Embed.cpp`
- `caustica/Python/RenderSession.*`

## Table of Contents

### Getting Started

- [Two Usage Modes](#two-usage-modes)
- [Import Setup](#import-setup)
- [Quick Examples](#quick-examples)

### API Reference

- [Module-Level API](#module-level-api)
- [Renderer](#renderer) — extension-mode standalone renderer
- [GPU selection](#gpu-selection) — enumeration, automatic choice, and stable selectors
- [App & Scene](#app--scene) — `app()`, scene, camera, accumulation
- [Spawn / Despawn](#spawn--despawn) — prefab load/spawn and entity removal
- [Model](#model) — `SceneEntity`, `MeshHandle`, deformation, bounds
- [Material](#material) — `Material` class and scene lookup
- [Light](#light) — light create/lookup and typed properties
- [3DGS](#3dgs) — Gaussian splat loading, settings, enums
- [Settings](#settings) — path tracing, denoiser, tone mapping, DLSS, etc.
- [Enums](#enums) — shared enumerations

### Other

- [Embedded Mode Notes](#embedded-mode-notes)
- [Extension Mode Notes](#extension-mode-notes)
- [Existing Examples](#existing-examples)
- [Introspection](#introspection)

## Two Usage Modes

The `caustica` module supports two runtime modes that share most types:

| Mode | How to use | Typical use |
| --- | --- | --- |
| `extension` | `import caustica` in a standalone Python process and create `caustica.Renderer(...)` | Offline rendering, batch jobs, screenshots, automated tests, quick 3DGS validation |
| `embed` | `import caustica` from the in-app script system inside a running `caustica.exe` | Live parameter tuning, debugging, hot edits to scenes/materials/lights |

At runtime you can check the active mode with:

```python
import caustica
print(caustica.MODE)  # "extension" or "embed"
```

In extension mode, `caustica.Renderer` creates its own window, device, and scene.  
In embed mode there is no `Renderer` class; use `caustica.app()` to access the renderer inside the running `caustica.exe`.

## Import Setup

After building the `caustica_py` target, the Python extension is emitted under `bin/`:

```text
bin/caustica.cp311-win_amd64.pyd                 # Windows
bin/caustica.cpython-313-x86_64-linux-gnu.so     # Linux (CPython tag varies)
```

The recommended install path is to run from the repository root:

```powershell
python -m pip install .
python -c "import caustica; print(caustica.MODE)"
```

On Linux you can import without pip:

```bash
PYTHONPATH="$PWD/bin" python3 -c "import caustica; print(caustica.MODE)"
```

Linux builds are Vulkan-only. `Renderer(vulkan=False)` falls back to Vulkan with a warning; prefer `vulkan=True`. Use the same Python interpreter CMake found at configure time (conda/venv included).

This assembles a local binary wheel from the native extension, runtime DLLs/shared libraries, shaders, and required assets in `bin/`, then installs it into the active Python environment. You can also build the wheel explicitly first:

```powershell
python support/python/build_wheel.py
python -m pip install dist/caustica-*.whl
```

Packaging options can be controlled with environment variables:

| Variable | Default | Values |
| --- | --- | --- |
| `caustica_WHEEL_VERSION` | `0.2.0` | Any PEP 440 version string |
| `caustica_WHEEL_ASSETS` | `minimal` | `minimal`, `full`, `none` |
| `caustica_WHEEL_DYNAMIC_SHADERS` | `none` when a shader pack is built, else `bin` | `bin`, `full`, `none` |
| `caustica_WHEEL_SHADER_API` | `d3d12` on Windows, `vulkan` elsewhere | `d3d12`, `vulkan`, `both` |
| `CAUSTICA_WHEEL_SHADER_PACK` | `true` | `true`, `false` |

Prefer installing the package (`python -m pip install .`) so examples can simply `import caustica`.
If import fails, fix the install rather than patching `sys.path` in scripts.

## Quick Examples

### Headless Reference Render

```python
import caustica

with caustica.Renderer(
    width=1280,
    height=720,
    headless=True,
    scene="bistro-programmer-art.scene.json",
    realtime=False,
    accumulation_target=64,
) as r:
    r.settings.enable_tone_mapping = True
    frames = r.step_until_accumulated()
    print("frames:", frames)
    r.save_screenshot("frame.png")
```

### Accumulate then Read Framebuffer (CPU / NumPy)

After reference accumulation finishes, read the same LDR final color used by
`save_screenshot` without writing a file:

```python
import caustica
import numpy as np

r = caustica.Renderer(
    width=1280,
    height=720,
    headless=True,
    scene="builtin:plane_cube",
    realtime=False,
    accumulation_target=64,
)
r.settings.realtime_mode = False
r.settings.accumulation_target = 64
r.settings.realtime_aa = 0

r.step_until_accumulated()              # wait until 64 spp

# Option A: NumPy array (H, W, 4) uint8 RGBA — requires NumPy
img = r.get_pixels()
rgb = img[..., :3]

# Option B: raw bytes via Framebuffer
fb = r.get_framebuffer()                # caustica.Framebuffer
raw = fb.pixels                         # bytes, len == width * height * 4
arr = np.frombuffer(raw, dtype=np.uint8).reshape(fb.height, fb.width, 4).copy()

r.close()
```

Layout notes:

- Format is tightly packed **RGBA8**, row-major, **top-left** origin.
- Source is the engine LDR final color (same as `save_screenshot`).
- `hdr=True` is reserved and not implemented yet.

### Windowed Interactive Loop

```python
import time
import caustica

r = caustica.Renderer(
    width=1280,
    height=720,
    headless=False,
    scene="bistro-programmer-art.scene.json",
    realtime=True,
    accumulation_target=1,
)

try:
    while r.step(-1.0):  # returns False if the window is closed
        time.sleep(0.001)
finally:
    r.close()
```

### Load 3D Gaussian Splats

3DGS objects are scene graph objects. Prefer declaring them in the scene JSON:

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

r = caustica.Renderer(width=1280, height=720, headless=False, realtime=True, scene=scene)

s = r.settings
s.enable_gaussian_splats = True
s.gaussian_splat_sorting_mode = int(caustica.GaussianSplatSortMode.GpuSort)
s.gaussian_splat_sh_format = int(caustica.GaussianSplatStorageFormat.Uint8)
s.gaussian_splat_rgba_format = int(caustica.GaussianSplatStorageFormat.Uint8)
s.gaussian_splat_scale = 1.0
s.gaussian_splat_alpha_scale = 1.0
s.gaussian_splat_brightness = 1.0

while r.step(-1.0):
    pass
```

For script-driven workflows, `load_gaussian_splats(path, convert_rdf_to_rub=True)` appends a `GaussianSplat` node to the current scene root. Calling `load_scene(...)` replaces the current scene graph and destroys previously appended splat nodes, so load them again after switching scenes or declare them in the target scene JSON.

### 3DGS Reference / Realtime Batch Render

`gaussian_splats.py view --mode batch` renders the same PLY twice:

- Reference mode accumulates `--spp` samples, applies OIDN, and writes `reference_oidn.png`.
- Realtime mode steps `--frames` frames and uses DLSS-RR when supported, falling back to DLSS, then NRD + TAA, and writes `realtime.png`.
- The default 3DGS sorting mode is GPU sort. Pass `--sorting stochastic` to compare with stochastic splats.

```powershell
python .\examples\python\gaussian_splats.py view ^
    --ply D:/path/to/splats.ply ^
    --mode batch ^
    --out-dir splat_batch_out
```

Useful camera overrides:

```powershell
python .\examples\python\gaussian_splats.py view ^
    --ply D:/path/to/splats.ply ^
    --mode batch ^
    --out-dir splat_batch_out ^
    --distance-scale 4.0 ^
    --side front
```

The camera is framed from bounds sampled directly out of the PLY, because the engine exposes no bounds query for loaded splats.

### COLMAP Camera 3DGS Alignment

`gaussian_splats.py colmap` renders a 3DGS PLY from COLMAP `cameras.bin`/`images.bin` (or the `.txt` equivalents) views. It is useful for comparing caustica output against gsplat output from the same camera poses. Both `--ply` and `--colmap-dir` are required, and the subcommand needs numpy.

```powershell
python .\examples\python\gaussian_splats.py colmap ^
    --ply D:/path/to/gaussians.ply ^
    --colmap-dir D:/path/to/sparse ^
    --max-views 8 ^
    --frames-per-view 8 ^
    --warmup-frames 4 ^
    --out-dir colmap_views_out
```

The subcommand passes full COLMAP pinhole intrinsics (`fx`, `fy`, `cx`, `cy`) through `Renderer.set_camera_intrinsics(...)`, which keeps off-center principal points aligned with gsplat. Use `--symmetric-fov` only when intentionally testing the vertical-FOV-only path. Output resolution defaults to the COLMAP camera size; `--width` / `--height` override it and the intrinsics are rescaled to match.

Alongside the images it writes `cameras_used.json` recording, per view, the COLMAP intrinsics and the caustica camera actually used.

When `--rdf-to-rub` is enabled, which is the default, both the PLY loader and the COLMAP camera pose are converted from RDF/COLMAP coordinates into caustica engine coordinates. Disable it with `--no-rdf-to-rub`. `--mip-antialiasing` defaults on for this subcommand and can be disabled with `--no-mip-antialiasing`.

### Load OBJ Meshes With Materials

`Renderer.load_mesh_file(...)` / `app.spawn_from_file(...)` append mesh/prefab files to the current scene (`.gltf` / `.glb` / `.obj` / `.urdf` / `.usd*`). For OBJ specifically, the loader parses `mtllib` directives and resolves `.mtl` and texture paths relative to the OBJ/MTL directory by default.

The current OBJ/MTL importer recognizes these common material fields:

- Scalars/colors: `Kd`, `Ks`, `Ke`, `Ns`, `Pr`, `Pm`, `Ni`, `d`, `Tr`, `Tf`.
- Base color / diffuse maps: `map_Kd`, `map_basecolor`.
- PBR metal-roughness: `map_Pr`, `map_roughness`, `map_Pm`, `map_metallic`, `map_metalness`.
- Packed PBR maps: `map_orm`, `map_mr`, `map_metallicroughness`, `map_occlusionroughnessmetallic`.
- AO / occlusion: `map_Ka`, `map_ao`, `map_occlusion`.
- Normal / bump: `map_Bump`, `bump`, `norm`, `map_normal`, including `-bm` strength.
- Specular / glossiness: `map_Ks`, `map_Ns` for specular-gloss materials.
- Emissive / opacity / transmission: `map_Ke`, `map_emissive`, `map_d`, `map_opacity`, `map_Tf`.

When an MTL file provides separate roughness and metallic maps, the importer builds an in-memory ORM texture for caustica: `R=AO`, `G=roughness`, `B=metallic`, `A=1`. The `-imfchan` channel selector is honored when reading single-channel maps.
If `map_Ke` or `map_emissive` is present without an explicit `Ke` color, the importer uses `(1, 1, 1)` as the emissive factor so the emissive texture is visible.

```python
import caustica

obj_path = r"D:/assets/m-plate-pbr_final/textured.obj"

with caustica.Renderer(scene="builtin:plane", headless=True, accumulation_target=32) as r:
    if not r.load_mesh_file(obj_path):
        raise RuntimeError(f"failed to load {obj_path}")

    # Imported materials are available after the mesh is appended and at least
    # one update frame has run.
    r.step_n(1)

    for mat in r.app.scene.get_materials():
        print(mat.model_name, mat.name, mat.base_color, mat.roughness, mat.metalness)

    r.step_until_accumulated()
    r.save_screenshot("obj_materials.png")
```

### Edit Materials

```python
import caustica

with caustica.Renderer(scene="bistro-programmer-art.scene.json", headless=True) as r:
    scene = r.app.scene

    # Names can come from scene.get_materials(), the MTL `newmtl` name, or the
    # material unique_name printed below.
    for mat in scene.get_materials():
        print(mat.model_name, mat.name, mat.unique_name)

    mat = scene.find_material("SomeMaterialName")
    if mat is None:
        raise RuntimeError("material not found")

    # Scalars/colors multiply the loaded texture values when the corresponding
    # texture remains enabled.
    mat.base_color = (1.0, 0.2, 0.1)
    mat.roughness = 0.35
    mat.metalness = 0.0
    mat.normal_texture_scale = 0.75

    # Texture bindings can be enabled/disabled, or replaced from Python.
    mat.enable_base_texture = False
    mat.enable_orm_texture = True
    mat.enable_normal_texture = True
    mat.set_base_texture(r"D:/assets/replacement_albedo.png")
    mat.set_normal_texture(r"D:/assets/replacement_normal.png")
    mat.set_coat_normal_texture(r"D:/assets/replacement_coat_normal.png")

    # In reference accumulation mode, reset after any visible edit so old
    # accumulated samples do not remain mixed into the image.
    r.app.reset_accumulation()

    r.step_n(4)
    r.save_screenshot("material_edit.png")
```

All writable `Material` properties mark the material GPU data dirty automatically; calling `mark_dirty()` is only needed if native-side data was changed without going through a Python property setter. Texture replacement helpers load the new image through the runtime texture cache, enable the slot, and upload updated material data on the next rendered frame.

### Read and Replace Material Textures

Texture access is exposed as file bindings on `Material`. Python can read the current
loaded texture path, replace a slot with another image file, enable or disable an
existing slot, and clear a slot. It does not expose direct pixel-buffer editing.

```python
import caustica

with caustica.Renderer(scene="bistro-programmer-art.scene.json", headless=True) as r:
    mat = r.app.scene.find_material("SomeMaterialName")
    if mat is None:
        raise RuntimeError("material not found")

    # Read current texture file bindings. Each property is str or None.
    print("base:", mat.base_texture_path)
    print("orm:", mat.orm_texture_path)
    print("normal:", mat.normal_texture_path)
    print("emissive:", mat.emissive_texture_path)
    print("transmission:", mat.transmission_texture_path)

    # Replace common slots with slot-specific helpers.
    if not mat.set_base_texture(r"D:/assets/albedo_replacement.png"):
        raise RuntimeError("base texture not found")
    mat.set_normal_texture(r"D:/assets/normal_replacement.png")

    # Replace any slot with the generic API and TextureSlot enum.
    mat.set_texture(caustica.TextureSlot.Emissive, r"D:/assets/emissive.png")
    mat.set_texture(caustica.TextureSlot.ORM, r"D:/assets/orm_linear.png", srgb=False)

    # Toggle sampling without disconnecting the loaded texture.
    mat.enable_base_texture = True
    mat.enable_normal_texture = False

    # Disconnect and disable a slot.
    mat.clear_emissive_texture()
    # Equivalent generic form:
    mat.clear_texture(caustica.TextureSlot.Transmission)

    r.app.reset_accumulation()
    r.step_n(4)
```

`set_*_texture(...)` returns `True` when the file was resolved and loaded, `False`
when it was not found. A successful set operation also enables that texture slot.
`clear_*_texture(...)` removes the binding and disables the slot.

Default color-space handling:

| Slot | Helper | Default interpretation |
| --- | --- | --- |
| `TextureSlot.Base` | `set_base_texture(path, srgb=None)` | sRGB |
| `TextureSlot.ORM` | `set_orm_texture(path, srgb=None)` | Linear in metal-rough mode, sRGB in spec-gloss mode |
| `TextureSlot.Normal` | `set_normal_texture(path)` | Linear normal map |
| `TextureSlot.CoatNormal` | `set_coat_normal_texture(path)` | Linear coat normal map |
| `TextureSlot.Emissive` | `set_emissive_texture(path, srgb=None)` | sRGB |
| `TextureSlot.Transmission` | `set_transmission_texture(path, srgb=None)` | Linear |

Use the optional `srgb` argument to override the default color-space choice. The
generic `set_texture(slot, path, srgb=None, normal_map=None)` also accepts
`normal_map` for advanced cases; leave it as `None` for the slot default.

Relative texture paths are resolved through the same runtime texture search used by
material JSON loading: runtime `Assets/` first, then the current scene directory.
For `.png` inputs, an existing sibling `.dds` is preferred, matching caustica material
loading behavior.

### Edit Lights

```python
import caustica

r = caustica.Renderer(scene="bistro-programmer-art.scene.json", headless=True)
scene = r.app.scene
for light in scene.get_lights():
    print(light.name, light.light_type)
    light.color = (1.0, 0.9, 0.75)

sun = scene.find_light("Sun")
if sun:
    sun.direction = (0.0, -1.0, 0.2)

# Or create typed lights under the scene root at runtime:
spot = scene.create_spot_light(
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

r.step_n(8)
r.save_screenshot("lights.png")
r.close()
```

### Spawn / Despawn Assets

`app.load` / `app.spawn` / `app.spawn_from_file` / `app.despawn` mirror the C++ `SceneSpawn` API.
Supported mesh/prefab extensions: `.gltf`, `.glb`, `.obj`, `.urdf`, `.usd` / `.usda` / `.usdc`, `.prefab.json`.

```python
import caustica

with caustica.Renderer(scene="builtin:plane", headless=True, accumulation_target=16) as r:
    app = r.app

    # One-shot: import + attach under the active scene root.
    entity = app.spawn_from_file("models/GlassSphere/GlassSphere.gltf")
    if entity is None:
        raise RuntimeError("spawn_from_file failed")
    entity.translation = (1.5, 0.5, 0.0)
    entity.scaling = (0.5, 0.5, 0.5)

    # Or split load/spawn when reusing a prefab:
    prefab = app.load("models/GlassSphere/GlassSphere.gltf")
    if prefab:
        clone = app.spawn(prefab)
        if clone:
            clone.translation = (-1.5, 0.5, 0.0)

    r.step_n(1)
    r.step_until_accumulated()
    r.save_screenshot("spawned.png")

    # Remove an entity (and children) when done.
    app.despawn(entity)
```

`load_mesh_file(...)` remains as a bool convenience wrapper around `spawn_from_file`.

### Deform Mesh Vertices

Mesh deformation is **entity-first**: pass a `SceneEntity` that has a mesh
instance. Importers may split one OBJ/glTF position into multiple render
vertices for UV or normal seams; Python returns that position once, and
write-back propagates the edit to all split render vertices. After
`set_mesh_vertices(...)` or `deform_mesh(...)`, caustica refreshes GPU buffers
and can rebuild ray tracing acceleration structures.

```python
import math
import caustica

r = caustica.Renderer(scene="builtin:cube", headless=True, accumulation_target=8)
app = r.app

entity = app.find_mesh_entity("cube") or app.get_mesh_entities()[0]
vertices = list(app.get_mesh_vertices(entity))

# Simple soft bulge: move upper vertices upward based on x/z radius.
deformed = []
for x, y, z in vertices:
    radius = math.sqrt(x * x + z * z)
    lift = 0.15 * max(0.0, 1.0 - radius)
    deformed.append((x, y + lift, z))

app.set_mesh_vertices(entity, deformed, recompute_normals=True)
app.step_until_accumulated()
r.save_screenshot("deformed_mesh.png")
r.close()
```

For callback-style edits, return `None` to keep a vertex unchanged:

```python
def wave(index, p):
    x, y, z = p
    if y < 0:
        return None
    return (x, y + 0.05 * math.sin(index * 0.37), z)

app.deform_mesh(entity, wave, recompute_normals=True)
```

Use the `_world` variants when values should be scene world coordinates. Always
pass the owning mesh `SceneEntity` so the local-to-world transform is correct
for that instance:

```python
entity = app.find_entity("cube")  # name or path; or find_mesh_entity(...)

def lift_world(index, p):
    x, y, z = p
    return (x, y + 0.25, z)

app.deform_mesh_world(entity, lift_world, recompute_normals=True)
```

## API Reference

The sections below are grouped by topic so you can jump directly to the API you need:

| Category | Section |
| --- | --- |
| Renderer | [Renderer](#renderer) |
| GPU selection | [GPU selection](#gpu-selection) |
| Scene / app | [App & Scene](#app--scene) |
| Spawn / despawn | [Spawn / Despawn](#spawn--despawn) |
| Mesh / entities | [Model](#model) |
| Materials | [Material](#material) |
| Lights | [Light](#light) |
| Gaussian splats | [3DGS](#3dgs) |
| Render settings | [Settings](#settings) |
| Shared enums | [Enums](#enums) |

### Module-Level API

These functions exist in both embed and extension mode unless noted.

| API | Return | Notes |
| --- | --- | --- |
| `caustica.MODE` | `str` | `"embed"` or `"extension"`. |
| `caustica.app()` | application handle | Current application handle. In extension mode, returns the most recently created `Renderer`'s application handle. |
| `caustica.settings()` | `Settings` | Shortcut to global live UI/settings state. Same object as `caustica.app().settings`. |
| `caustica.log_info(message)` | `None` | Writes to caustica log at info level. |
| `caustica.log_warning(message)` | `None` | Writes to caustica log at warning level. |
| `caustica.log_error(message)` | `None` | Writes to caustica log at error level. |
| `caustica.enumerate_adapters(vulkan=False, debug=False)` | `list[AdapterInfo]` | Extension mode only. Enumerates DX12 or Vulkan adapters without creating a renderer. |
| `caustica.Renderer(...)` | `Renderer` | Extension mode only. Creates a standalone renderer/device/window or headless backbuffer. |
| `caustica.builtin_scene_json(builtin_model="plane_cube")` | `str` | Extension mode only. Returns minimal inline scene JSON for `plane`, `cube`, `sphere`, or `plane_cube`. |

## Renderer

Extension mode only. Create with `caustica.Renderer(...)`.

### Constructor

```python
caustica.Renderer(
    width=1920,
    height=1080,
    headless=True,
    vulkan=False,
    adapter="auto",
    debug=False,
    scene="",
    realtime=False,
    accumulation_target=64,
)
```

| Argument | Meaning |
| --- | --- |
| `width`, `height` | Initial backbuffer/window size. |
| `headless` | `True`: offscreen backbuffers, no OS window. `False`: create a window and swap chain. |
| `vulkan` | `False` uses DX12. `True` requests Vulkan when available. |
| `adapter` | GPU selector: `auto`, `index:N`, `name:text`, `uuid:hex`, or `luid:hex`. Use `caustica.enumerate_adapters()` to discover values. |
| `debug` | Enable graphics debug settings. |
| `scene` | Scene file path/name, `builtin:*` primitive reference, or inline scene JSON string. Relative file paths are resolved from `Assets/`. |
| `realtime` | Start in realtime mode if `True`, reference mode if `False`. |
| `accumulation_target` | Reference SPP target. |

### Methods / Properties

| API | Return | Notes |
| --- | --- | --- |
| `close()` | `None` | Tears down renderer/device. Also called by destructor/context manager. |
| `load_scene(scene_name, wait_until_ready=True)` | `bool` | Switch scene. |
| `load_gaussian_splats(file_name, convert_rdf_to_rub=True)` | `bool` | Append a `.ply` 3DGS scene object under the current scene root. |
| `load_mesh_file(file_name)` | `bool` | Append a mesh/prefab (`.gltf` / `.glb` / `.obj` / `.urdf` / `.usd*`) under the current scene root. OBJ imports resolve referenced `.mtl` files and common material textures relative to the OBJ/MTL path. |
| `get_scene_bounds()` | `tuple | None` | Active scene world-space `((min.xyz), (max.xyz))` AABB from C++ `Scene::GetSceneBounds()`. |
| `scene_bounds` | `tuple | None` | Property alias for `get_scene_bounds()`. |
| `scene_bounds_center` | `tuple | None` | Center of `scene_bounds`. |
| `scene_bounds_size` | `tuple | None` | Extent `(max - min)` of `scene_bounds`. |
| `step(dt=-1.0)` | `bool` | Render one frame. Returns `False` on failure or when window close is requested. |
| `step_n(frames)` | `bool` | Render exactly N frames unless `step()` fails. |
| `precache_rt_feature_presets(show_progress=True)` | `int` | Load/cook-time `CreateStateObject` for every cooked PT feature preset. Call after at least one `step()` / `step_n(1)`. Returns ready count. Not used by the interactive frame loop. |
| `step_until_accumulated(max_frames=0)` | `int` | Reset accumulation and step until accumulation completes, or until `max_frames` if positive. |
| `save_screenshot(output_path)` | `bool` | Save current LDR final color to PNG/JPG/BMP/TGA. |
| `get_framebuffer(hdr=False)` | `Framebuffer` | CPU readback of current LDR final color. See `Framebuffer` below. `hdr=True` is not implemented yet. |
| `get_pixels(hdr=False)` | `numpy.ndarray` | Same LDR readback as `(H, W, 4)` `uint8` RGBA. Requires NumPy. `hdr=True` is not implemented yet. |
| `set_camera(position, direction, up=(0, 1, 0))` | `bool` | Triples can be lists/tuples of 3 floats. |
| `set_camera_fov(vertical_fov_degrees)` | `None` | Set vertical FOV in degrees. |
| `set_camera_intrinsics(fx, fy, cx, cy, width, height)` | `None` | Set an off-center pinhole projection from pixel-space intrinsics. This overrides the symmetric FOV projection until `set_camera_fov(...)` or `clear_camera_intrinsics()` is used. |
| `app` | application handle | Underlying application handle (`EngineApp::app()` in extension mode). |
| `settings` | `Settings` | Live UI/settings state. |
| `selected_adapter` | `AdapterInfo` | Adapter actually selected during device creation. |

### GPU selection

`Renderer` uses `adapter="auto"` when the argument is omitted. Automatic mode
enumerates the requested backend, filters out software and path-tracing-
incompatible devices, and chooses the suitable GPU with the highest capability
score. The score combines hardware class, required ray-tracing features,
device-local memory, and compute limits where the backend exposes useful
comparable values. It is a deterministic selection heuristic, not a performance
benchmark. Equal scores prefer the lower index.

#### Enumerating adapters

`caustica.enumerate_adapters(vulkan=False, debug=False)` performs discovery
without creating a renderer, logical device, window, or swap chain. Set
`vulkan=True` to enumerate the Vulkan backend instead of DX12.

```python
import caustica

gpus = caustica.enumerate_adapters(vulkan=False)
for gpu in gpus:
    print(
        gpu.index,
        gpu.name,
        gpu.type,
        f"{gpu.dedicated_video_memory / 2**30:.1f} GiB",
        gpu.suitable,
        gpu.luid,
    )
```

`AdapterInfo` is read-only and exposes:

| Property | Type | Meaning |
| --- | --- | --- |
| `index` | `int` | Index for this backend enumeration. |
| `name` | `str` | Driver-reported adapter name. |
| `backend` | `str` | `"d3d12"` or `"vulkan"` for extension-mode discovery. |
| `type` | `str` | `"discrete"`, `"integrated"`, `"virtual"`, `"software"`, or `"unknown"`. |
| `vendor_id`, `device_id` | `int` | Driver-reported PCI/vendor identifiers. |
| `dedicated_video_memory` | `int` | Device-local/dedicated memory in bytes. |
| `selection_score` | `int` | Backend capability score used by `auto`; not a benchmark result. |
| `supports_ray_tracing_pipeline` | `bool` | Hardware/driver exposes a ray-tracing pipeline. |
| `supports_ray_query` | `bool` | Hardware/driver exposes ray queries. |
| `suitable` | `bool` | Adapter meets the renderer's device requirements. |
| `software` | `bool` | Adapter is a software implementation. |
| `uuid` | `str \| None` | Stable 32-hex-digit device UUID when available. |
| `luid` | `str \| None` | Stable 16-hex-digit Windows adapter LUID when available. |

#### Selector formats

| Value | Behavior |
| --- | --- |
| `auto` | Choose the highest-scoring suitable GPU. |
| `index:N` | Match `AdapterInfo.index`. A bare non-negative integer is also accepted. |
| `name:text` | Case-insensitive name substring. A bare non-numeric string is also treated as a name. |
| `uuid:hex` | Match `AdapterInfo.uuid`; separators and an optional `0x` prefix are accepted. |
| `luid:hex` | Match `AdapterInfo.luid`; separators and an optional `0x` prefix are accepted. |

Explicit selectors are strict. A missing, unsuitable, or ambiguous match raises
an initialization error; the renderer does not silently fall back to another
GPU. Name selectors can become ambiguous on machines containing identical GPUs.
For persistent worker configuration, prefer UUID/LUID because enumeration
indices can change after driver or hardware updates.

```python
import caustica

gpus = caustica.enumerate_adapters()
target = next(gpu for gpu in gpus if gpu.suitable and gpu.luid)

with caustica.Renderer(
    adapter=f"luid:{target.luid}",
    headless=True,
) as renderer:
    selected = renderer.selected_adapter
    print(selected.index, selected.name, selected.backend)
```

### `Framebuffer`

Returned by `Renderer.get_framebuffer()`. Holds a CPU copy of the current LDR
image after at least one successful `step()` / `step_until_accumulated()`.

| Field / property | Type | Notes |
| --- | --- | --- |
| `width` | `int` | Image width in pixels. |
| `height` | `int` | Image height in pixels. |
| `channels` | `int` | Always `4` (RGBA). |
| `format` | `str` | `"RGBA8"`. |
| `dtype` | `str` | `"uint8"`. |
| `pixels` | `bytes` | Tightly packed RGBA8, row-major, top-left origin. `len == width * height * 4`. |
| `shape` | `tuple` | `(height, width, channels)` — NumPy image layout. |

`Renderer` supports context manager syntax:

```python
with caustica.Renderer(headless=True) as r:
    r.step_n(8)
```

### Inline / Builtin Scenes

For package smoke tests that should not depend on external mesh assets, the extension accepts builtin primitive scenes:

```python
with caustica.Renderer(headless=True, scene="builtin:plane_cube", accumulation_target=4) as r:
    r.step_until_accumulated()
    r.save_screenshot("smoke.png")
```

Supported builtin models are `builtin:plane`, `builtin:cube`, `builtin:sphere`, and `builtin:plane_cube`.

You can also pass an inline scene JSON string. Model entries may reference builtin primitives:

```python
scene = caustica.builtin_scene_json("plane_cube")
r = caustica.Renderer(headless=True, scene=scene)
```

Scene JSON may also declare one or more 3DGS nodes directly:

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
        "GaussianSplat": {
          "path": "D:/path/to/scans/splats_b.ply"
        }
      }
    }
  ]
}
"""
r = caustica.Renderer(headless=False, realtime=True, scene=scene)
```

For scene files, relative 3DGS paths are resolved relative to the scene JSON file. `path`, `file`, and `fileName` are accepted aliases.

## App & Scene

The application handle gives access to the scene, camera, and runtime operations. In extension mode, obtain it from `renderer.app`; in embed mode, use `caustica.app()`. Scene graph access goes through `app.scene`.

### Read-Only Properties

| Property | Type | Notes |
| --- | --- | --- |
| `settings` | `Settings` | Live settings object. |
| `scene` | `Scene | None` | Current loaded scene, matching the C++ `GetScene()` entry point. |
| `scene_name` | `str` | Current scene name. |
| `available_scenes` | `list[str]` | Scene files discovered by the app. |
| `gaussian_splat_object_count` | `int` | Number of loaded 3DGS scene objects. |
| `gaussian_splat_count` | `int` | Total loaded splat count across current 3DGS scene objects. |
| `gaussian_splat_file_name` | `str` | Single loaded 3DGS path, or a summary when multiple 3DGS objects are present. |
| `scene_bounds` | `tuple | None` | Shortcut for `scene.get_scene_bounds()`. |
| `scene_bounds_center` | `tuple | None` | Center of `scene_bounds`. |
| `scene_bounds_size` | `tuple | None` | Extent `(max - min)` of `scene_bounds`. |
| `accumulation_completed` | `bool` | Whether reference accumulation is complete. |
| `accumulation_sample_index` | `int` | Current accumulation sample index. |

### Scene / Assets

| API | Return | Notes |
| --- | --- | --- |
| `set_scene(scene_name, force_reload=False)` | `None` | Switch scene. |
| `load_gaussian_splats(file_name, convert_rdf_to_rub=True)` | `bool` | Append a 3DGS `.ply` object to the current scene. |
| `load_mesh_file(file_name)` | `bool` | Append a mesh/prefab (`.gltf` / `.glb` / `.obj` / `.urdf` / `.usd*`). Convenience bool wrapper over `spawn_from_file`. |
| `set_environment_map(path)` | `None` | Override scene environment map source. |
| `get_scene()` | `Scene | None` | Return the current loaded scene. |
| `get_scene_bounds()` | `tuple | None` | Shortcut for `scene.get_scene_bounds()`. |

### Camera

| API | Return | Notes |
| --- | --- | --- |
| `get_camera_pos_dir_up()` | `str` | Comma-separated `pos.xyz,dir.xyz,up.xyz`. |
| `set_camera_pos_dir_up(pos_dir_up)` | `bool` | Input format matches `get_camera_pos_dir_up()`. |
| `set_camera_fov(vertical_fov_degrees)` | `None` | Takes degrees. |
| `set_camera_intrinsics(fx, fy, cx, cy, width, height)` | `None` | Uses pixel-space pinhole intrinsics for the active projection. Useful for COLMAP/OpenCV cameras with non-centered `cx/cy`. |
| `clear_camera_intrinsics()` | `None` | Clear pixel-space intrinsics and restore the FOV-based projection. |
| `get_camera_fov()` | `float` | Returns current internal value in radians. |
| `scene_camera_count` | `int` | Number of scene cameras available for selection. |
| `selected_camera_index` | `int` | Active scene-camera index (`0 .. scene_camera_count-1`). |
| `get_cameras()` | `list[SceneEntity]` | Compatibility alias for `scene.get_cameras()`. |
| `save_current_camera()` | `None` | Save camera through app's camera persistence path. |
| `load_current_camera()` | `None` | Restore saved camera. |

Use `Renderer.set_camera()` when working in extension mode; it is simpler than building the comma-separated string manually.

### Runtime Requests

| API | Effect |
| --- | --- |
| `request_shader_reload()` | Requests shader reload. |
| `request_accel_rebuild()` | Requests a full acceleration structure rebuild. |
| `request_mesh_accel_rebuild(entity)` | Requests a BLAS rebuild for the mesh on one `SceneEntity` without forcing a full scene AS rebuild. |
| `reset_accumulation()` | Resets reference accumulation. |
| `reset_realtime_caches()` | Resets realtime caches (ReSTIR / temporal history helpers). |

### Diagnostics (read-only)

| Property | Type | Notes |
| --- | --- | --- |
| `fps_info` | `str` | Human-readable FPS summary from the live session. |
| `resolution_info` | `str` | Human-readable resolution summary. |
| `avg_time_per_frame` | `float` | Average frame time. |
| `render_size` | `(width, height)` | Current path-tracer render resolution. |

## Spawn / Despawn

Runtime attach/detach of mesh/prefab assets through `app` (same path as C++ `SceneSpawn`). Extract publishes a new proxy generation; GPU mesh/AS/SBT work is built on the render thread asynchronously.

### `ScenePrefab`

Returned by `app.load(path)`.

| Property | Type | Notes |
| --- | --- | --- |
| `valid` | `bool` | Whether the handle still points at a loaded prefab. |
| `name` | `str` | Prefab display name. |
| `source_path` | `str` | Absolute/source path used for the import. |
| `__bool__` | `bool` | Truthy when the handle is valid. |

### APIs

| API | Return | Notes |
| --- | --- | --- |
| `app.load(path)` | `ScenePrefab` | Import a mesh/prefab file into a CPU-side handle (does not spawn). |
| `app.spawn(prefab)` | `SceneEntity | None` | Spawn a previously loaded `ScenePrefab` into the active scene. Returns the root entity. |
| `app.spawn_from_file(path)` | `SceneEntity | None` | `load` + `spawn` in one call. |
| `app.despawn(entity)` | `bool` | Remove a scene entity and its children. |
| `app.load_mesh_file(path)` | `bool` | Convenience boolean wrapper for `spawn_from_file(path)`. |

Supported extensions: `.gltf`, `.glb`, `.obj`, `.urdf`, `.usd`, `.usda`, `.usdc`.

### Mode Helpers

```python
app.set_realtime_mode(
    standalone_denoiser=True,
    realtime_aa=int(caustica.RealtimeAA.DLSS),
)

app.set_reference_mode(
    spp=128,
    oidn=True,
    oidn_quality=int(caustica.OidnQuality.Balanced),
    oidn_passes=int(caustica.OidnPasses.Albedo),
    oidn_prefilter=int(caustica.OidnPrefilter.Fast),
)
```

| API | Notes |
| --- | --- |
| `set_realtime_mode(standalone_denoiser=True, realtime_aa=2)` | Sets realtime mode. `realtime_aa`: `0=Off`, `1=TAA`, `2=DLSS`, `3=DLSS_RR`. |
| `set_reference_mode(spp=0, oidn=False, oidn_quality=1, oidn_passes=1, oidn_prefilter=1)` | Sets reference mode. `spp=0` keeps current target. |

## Model

Scene entities, asset mesh identity (`MeshHandle`), and vertex deformation.
There is no public `Mesh` / `SceneNode` type — use `SceneEntity` and entity-keyed
mesh APIs. Engine CPU mesh records stay internal.

### Scene Lookup

| API | Return | Notes |
| --- | --- | --- |
| `scene.get_mesh_entities()` | `list[SceneEntity]` | All mesh-instance entities in the current scene. |
| `scene.find_mesh_entity(name)` | `SceneEntity | None` | Match by mesh asset name or entity name. |
| `scene.find_entity(path)` | `SceneEntity | None` | Find an entity by name or path. |
| `Renderer.load_mesh_file(file_name)` | `bool` | Append a mesh/prefab (extension mode). |
| `app.load_mesh_file(file_name)` | `bool` | Append a mesh/prefab (embed or extension). |
| `app.spawn_from_file(path)` | `SceneEntity | None` | Preferred spawn path; returns the root entity. |

### Vertex Deformation

| API | Return | Notes |
| --- | --- | --- |
| `app.get_mesh_vertices(entity)` | `list[tuple]` | Unique object-space `(x, y, z)` positions for the entity's mesh. |
| `app.set_mesh_vertices(entity, vertices, recompute_normals=True, rebuild_acceleration_structure=True)` | `None` | Replace unique positions. Length must match `len(get_mesh_vertices(entity))`. |
| `app.deform_mesh(entity, callback, recompute_normals=True, rebuild_acceleration_structure=True)` | `int` | Calls `callback(index, (x, y, z))` for each unique position. Return a new triple or `None`; returns the processed vertex count. |
| `app.get_mesh_vertices_world(entity)` | `list[tuple]` | Unique world-space positions using that entity's transform. |
| `app.set_mesh_vertices_world(entity, vertices, recompute_normals=True, rebuild_acceleration_structure=True)` | `None` | Replace positions from world-space coordinates. |
| `app.deform_mesh_world(entity, callback, recompute_normals=True, rebuild_acceleration_structure=True)` | `int` | World-space deform callback; return a world triple or `None`. |

`set_mesh_vertices(...)` updates object-space mesh bounds, optionally recomputes normals,
refreshes GPU vertex data, resets accumulation, and requests acceleration structure rebuild
by default. Keep `rebuild_acceleration_structure=True` for ray tracing-correct geometry.
Only set it to `False` when batching several edits and calling `request_accel_rebuild()`
after the final update.

The `_world` variants refresh transform state before converting coordinates, so
recent edits such as `entity.translation = ...` are reflected immediately. Shared
mesh buffers still apply: deforming through one mesh entity updates geometry used
by other instances of the same engine mesh record.

### Scene Bounds

| API | Return | Notes |
| --- | --- | --- |
| `scene.get_scene_bounds()` | `tuple | None` | World-space `((min.xyz), (max.xyz))` AABB from C++ `Scene::getSceneBounds()`. |
| `scene.get_bounds()` | `tuple | None` | Alias for `scene.get_scene_bounds()`. |
| `scene.bounds` | `tuple | None` | Property alias for `scene.get_scene_bounds()`. |
| `scene.bounds_center` | `tuple | None` | Center of `scene.bounds`. |
| `scene.bounds_size` | `tuple | None` | Extent `(max - min)` of `scene.bounds`. |
| `Renderer.get_scene_bounds()` / `scene_bounds` | `tuple | None` | Extension-mode world bounds shortcut. |

### `MeshHandle` Class

Asset-system mesh identity (`Handle<MeshAsset>`). Prefer this over digging engine
CPU mesh records. Returned by `SceneEntity.mesh_handle`.

| Property | Type | Notes |
| --- | --- | --- |
| `valid` | `bool` | Whether the handle resolves. |
| `name` | `str` | Mesh asset name when valid. |
| `__bool__` | `bool` | Truthy when valid. |

### `SceneEntity` Class

ECS entity wrapper returned by `find_entity`, `find_mesh_entity`, `get_mesh_entities`,
`get_lights`, `get_cameras`, `spawn` / `spawn_from_file`, and light create helpers.

| Property | Type |
| --- | --- |
| `name` | `str` |
| `path` | `str` |
| `mesh_handle` | `MeshHandle` |
| `is_mesh` | `bool` |
| `is_light` | `bool` |
| `light_type` | `int` / `LightType` |
| `translation` | `(x, y, z)` |
| `rotation` | `(x, y, z, w)` quaternion |
| `euler` | `(x, y, z)` radians |
| `scaling` | `(x, y, z)` |
| `bounds` | world AABB tuple or `None` |

`rotation` and `euler` both write the entity's local Transform rotation. Assigning
`euler` converts XYZ radians to the stored quaternion; assigning `rotation` expects
an XYZW quaternion, matching scene JSON. Python Transform edits reset accumulation
automatically so the next rendered frame does not blend with the previous pose.

## Material

Scene material lookup and the `Material` class for runtime edits and texture replacement.

### Scene Lookup

| API | Return | Notes |
| --- | --- | --- |
| `scene.get_materials()` | `list[Material]` | All `StandardMaterial` (OpenPBR) materials in the current scene. |
| `scene.find_material(name)` | `Material | None` | Match by `Name` or `UniqueName`. |
| `scene.find_material_by_id(material_id)` | `Material | None` | Lookup by material ID. |
| `scene.material_count` | `int` | Number of PT materials in the current scene. |

### `Material` Class

Returned by `Scene.get_materials()`, `Scene.find_material()`, and `Scene.find_material_by_id()`.

#### Identifiers (read-only)

| Property | Type |
| --- | --- |
| `name` | `str` |
| `model_name` | `str` |
| `unique_name` | `str` |

#### Properties (writable)

Editable properties automatically mark GPU data dirty:

| Property | Type |
| --- | --- |
| `base_color` | `(r, g, b)` |
| `specular_color` | `(r, g, b)` |
| `emissive_color` | `(r, g, b)` |
| `emission_color` | `(r, g, b)` |
| `emissive_intensity` | `float` |
| `emission_luminance` | `float` |
| `metalness` | `float` |
| `base_metalness` | `float` |
| `roughness` | `float` |
| `specular_roughness` | `float` |
| `material_model` | `str` |
| `base_weight` | `float` |
| `base_diffuse_roughness` | `float` |
| `specular_weight` | `float` |
| `anisotropy` | `float` |
| `specular_roughness_anisotropy` | `float` |
| `fuzz_weight` | `float` |
| `fuzz_color` | `(r, g, b)` |
| `fuzz_roughness` | `float` |
| `subsurface_radius_scale` | `(r, g, b)` |
| `subsurface_scale` | `float` (legacy broadcast alias) |
| `subsurface_scatter_anisotropy` | `float` |
| `subsurface_anisotropy` | `float` (legacy alias) |
| `opacity` | `float` |
| `geometry_opacity` | `float` |
| `transmission_factor` | `float` |
| `transmission_weight` | `float` |
| `diffuse_transmission_factor` | `float` |
| `transmission_diffuse_weight` | `float` |
| `normal_texture_scale` | `float` |
| `geometry_normal_scale` | `float` |
| `coat_normal_scale` | `float` |
| `ior` | `float` |
| `specular_ior` | `float` |
| `alpha_cutoff` | `float` |
| `geometry_alpha_cutoff` | `float` |
| `volume_attenuation_distance` | `float` |
| `volume_attenuation_color` | `(r, g, b)` |
| `nested_priority` | `int` |
| `use_specular_gloss` | `bool` |
| `enable_alpha_testing` | `bool` |
| `geometry_enable_alpha_test` | `bool` |
| `enable_transmission` | `bool` |
| `thin_surface` | `bool` |
| `geometry_thin_walled` | `bool` |
| `exclude_from_nee` | `bool` |
| `unlit_receive_shadows` | `bool` |
| `unlit_shadow_strength` | `float` (`0..1`) |
| `unlit_bypass_tone_mapping` | `bool` |
| `enable_as_analytic_light_proxy` | `bool` |
| `skip_render` | `bool` |
| `metalness_in_red_channel` | `bool` |
| `enable_base_texture` | `bool` |
| `enable_base_color_texture` | `bool` |
| `enable_orm_texture` | `bool` |
| `enable_base_metalness_specular_roughness_texture` | `bool` |
| `enable_normal_texture` | `bool` |
| `enable_geometry_normal_texture` | `bool` |
| `enable_coat_normal_texture` | `bool` |
| `enable_emissive_texture` | `bool` |
| `enable_emission_color_texture` | `bool` |
| `enable_transmission_texture` | `bool` |
| `enable_transmission_weight_texture` | `bool` |

#### Unlit Shadow Flags

These flags are for flat-color receivers such as stylized objects, compositing cards, or diagnostic geometry. They are not a physically based material mode.

| Property | Effect | When to use it |
| --- | --- | --- |
| `unlit_receive_shadows` | Displays the material's base color without BRDF, direct-light, or indirect-light shading, then darkens it with the visibility of sampled lights. | Enable when the color should stay constant but still show scene shadows. It also prevents the material from being treated as emissive. |
| `unlit_shadow_strength` | Controls how much that shadow mask darkens the unlit color. `0.0` keeps the color fully visible; `1.0` applies the full sampled-light shadow. | Use a value between `0.0` and `1.0` for artistic shadow softness/contrast. It matters only when `unlit_receive_shadows=True`. |
| `unlit_bypass_tone_mapping` | Keeps the reconstructed unlit/shadowed color unchanged by the tone mapper. Bloom and other rendering/post-processing stages still run. | Enable only when the flat color must not change with exposure or the selected tone-map curve. This flag has no effect unless `unlit_receive_shadows=True`. |

For example, this makes a flat green object receive half-strength shadows while preserving that green through tone mapping:

```python
mat.unlit_receive_shadows = True
mat.unlit_shadow_strength = 0.5
mat.unlit_bypass_tone_mapping = True
app.reset_accumulation()
```

#### Texture Paths (read-only)

| Property | Type |
| --- | --- |
| `base_texture_path` | `str | None` |
| `orm_texture_path` | `str | None` |
| `normal_texture_path` | `str | None` |
| `coat_normal_texture_path` | `str | None` |
| `emissive_texture_path` | `str | None` |
| `transmission_texture_path` | `str | None` |

#### Methods

| API | Notes |
| --- | --- |
| `mark_dirty()` | Force material GPU buffer refresh next frame. |
| `set_texture(slot, path, srgb=None, normal_map=None)` | Replace a texture slot. `slot` is a `TextureSlot` enum value. Returns `False` if the file cannot be resolved. |
| `set_base_texture(path, srgb=None)` | Replace base/diffuse texture. Defaults to sRGB. |
| `set_orm_texture(path, srgb=None)` | Replace ORM/spec-gloss texture. Defaults to linear for metal-rough and sRGB for spec-gloss. |
| `set_normal_texture(path)` | Replace normal texture. |
| `set_coat_normal_texture(path)` | Replace the independent OpenPBR coat normal texture. |
| `set_emissive_texture(path, srgb=None)` | Replace emissive texture. Defaults to sRGB. |
| `set_transmission_texture(path, srgb=None)` | Replace transmission texture. Defaults to linear. |
| `clear_texture(slot)` | Disconnect and disable a texture slot. |
| `clear_base_texture()`, `clear_orm_texture()`, `clear_normal_texture()`, `clear_coat_normal_texture()`, `clear_emissive_texture()`, `clear_transmission_texture()` | Slot-specific clear helpers. |

#### Runtime Update Rules

- Property setters already mark `GPUDataDirty`; the edited material is uploaded on the next rendered frame.
- In reference/accumulation mode, call `app.reset_accumulation()` or set `settings.reset_accumulation = True` after visible edits, otherwise previous samples remain blended with the old material.
- Color values are linear RGB. The Python setter does not clamp inputs, so keep factors in the physically meaningful range unless deliberately testing extremes.
- If a texture slot is enabled, scalar/color parameters multiply the texture sample. In metal-rough mode, effective base color is `base_color * base_texture.rgb`, roughness is `roughness * ORM.g`, and metalness is `metalness * ORM.b` unless `metalness_in_red_channel=True`.
- Set `material_model = "OpenPBR"` to use OpenPBR naming. Python exposes the same OpenPBR aliases as the material UI, including `base_metalness`, `specular_roughness`, `specular_roughness_anisotropy`, `specular_ior`, `transmission_weight`, `transmission_diffuse_weight`, `geometry_opacity`, `geometry_thin_walled`, `emission_color`, `emission_luminance`, `fuzz_*`, `coat_*`, `subsurface_*`, `thin_film_*`, `transmission_color`, `transmission_depth`, `transmission_scatter*`, and `transmission_dispersion_*`. Legacy aliases such as `metalness`, `roughness`, `opacity`, and `transmission_factor` remain valid.
- Setting `transmission_weight` or `transmission_diffuse_weight` from Python automatically updates `enable_transmission` from the two OpenPBR transmission weights.
- `opacity` is multiplied by the base texture alpha when `enable_base_texture=True`.
- Use `set_base_texture`, `set_orm_texture`, `set_normal_texture`, `set_emissive_texture`, or `set_transmission_texture` to replace an imported texture at runtime. Relative paths are resolved the same way as material JSON paths: runtime `Assets/` first, then the current scene directory. For `.png` inputs, an existing sibling `.dds` is preferred, matching material JSON loading.
- Pure parameter edits such as color, roughness, metalness, opacity, texture toggles, emissive intensity, normal scale, and IOR are next-frame updates. Bigger classification edits such as `use_specular_gloss`, `enable_alpha_testing`, `alpha_cutoff`, `enable_transmission`, `exclude_from_nee`, or `skip_render` can change shader hit groups, alpha handling, lighting participation, or acceleration-structure metadata; after those edits, request a shader/acceleration refresh.

Typical runtime material override:

```python
app = caustica.app()              # embed mode
# app = renderer.app           # extension mode
scene = app.scene

mat = scene.find_material("material_0")
if mat is not None:
    mat.base_color = (0.8, 0.9, 1.0)
    mat.roughness = 0.18
    mat.metalness = 0.85
    mat.set_base_texture(r"D:/assets/replacement_albedo.png")
    mat.enable_orm_texture = True
    mat.normal_texture_scale = 1.0
    app.reset_accumulation()
```

When changing material classification flags:

```python
mat.enable_alpha_testing = True
mat.alpha_cutoff = 0.4
mat.enable_transmission = True
mat.transmission_factor = 0.6

app.reset_accumulation()
app.request_shader_reload()
app.request_accel_rebuild()
```

## Light

Lights are ECS components on `SceneEntity`. Lookup returns `SceneEntity` handles; typed fields are exposed as properties on that entity. Prefer `caustica.LightType` over raw integers when branching.

### Scene Lookup

| API | Return | Notes |
| --- | --- | --- |
| `scene.get_lights()` | `list[SceneEntity]` | All light entities in the current scene. |
| `scene.find_light(name)` | `SceneEntity | None` | Match by entity name. |
| `scene.light_count` | `int` | Number of lights in the current scene. |
| `scene.camera_count` | `int` | Number of camera entities in the current scene. |
| `scene.get_cameras()` | `list[SceneEntity]` | All camera entities as `SceneEntity`. |
| `app.set_environment_map(path)` | `None` | Override scene environment map source. |

### Create Lights

| API | Notes |
| --- | --- |
| `scene.create_directional_light(color=(1,1,1), irradiance=1.0, angular_size=0.0, name="")` | Attach under scene root; returns `SceneEntity`. |
| `scene.create_point_light(color=(1,1,1), intensity=1.0, radius=0.0, range=0.0, name="")` | Attach under scene root; returns `SceneEntity`. |
| `scene.create_spot_light(color=(1,1,1), intensity=1.0, radius=0.0, range=0.0, inner_angle=180.0, outer_angle=180.0, name="")` | Attach under scene root; returns `SceneEntity`. |
| `scene.create_environment_light(color=(1,1,1), path="", rotation=0.0, name="")` | Attach under scene root; returns `SceneEntity`. |

Empty `name` auto-generates a unique name (`DirectionalLight`, `PointLight`, …).

### Light properties on `SceneEntity`

| Property | Type | Notes |
| --- | --- | --- |
| `is_light` | `bool` | True when the entity has a typed light component. |
| `light_type` | `int` / `LightType` | `None_=0`, `Directional`, `Spot`, `Point`, `Environment`. |
| `name` | `str` | Entity name. Read-only. |
| `color` | `(r, g, b)` | Writable. |
| `position` | `(x, y, z)` | World-space; updates local translation. |
| `direction` | `(x, y, z)` | World-space; updates local rotation. |
| `irradiance` / `angular_size` | `float` | Directional. |
| `intensity` / `radius` / `range` | `float` | Point / Spot. |
| `inner_angle` / `outer_angle` | `float` | Spot. |
| `environment_path` | `str` | Environment light HDRI path. |

For common environment tweaks, prefer `settings.environment_map` (see [Settings](#settings)) and `app.set_environment_map(path)`.

## 3DGS

3D Gaussian splat loading, scene entities, and render settings.

### Loading

| API | Return | Notes |
| --- | --- | --- |
| `Renderer.load_gaussian_splats(file_name, convert_rdf_to_rub=True)` | `bool` | Append a `.ply` 3DGS scene object (extension mode). |
| `app.load_gaussian_splats(file_name, convert_rdf_to_rub=True)` | `bool` | Append a 3DGS `.ply` object to the current scene. |
| Scene JSON `GaussianSplat` node | — | Declare splats in inline or file-based scene JSON. See [Renderer → Inline / Builtin Scenes](#inline--builtin-scenes). |

Read-only status on `app` / `Renderer.settings`:

| Property | Type | Notes |
| --- | --- | --- |
| `gaussian_splat_object_count` | `int` | Number of loaded 3DGS scene objects. |
| `gaussian_splat_count` | `int` | Total splat count across current 3DGS objects. |
| `gaussian_splat_file_name` | `str` | Single path or multi-object summary. |

### 3DGS Enums

| Enum | Values |
| --- | --- |
| `GaussianSplatSortMode` | `GpuSort=0`, `StochasticSplats=1` |
| `GaussianSplatStorageFormat` | `Float32=0`, `Float16=1`, `Uint8=2` |
| `GaussianSplatFrustumCulling` | `Disabled=0`, `AtDistanceStage=1`, `AtRasterStage=2` |
| `GaussianSplatShadowMode` | `Disabled=0`, `Hard=1`, `Soft=2` |
| `GaussianSplatFTBSyncMode` | `Disabled=0`, `Interlock=1` |

### Settings (`settings.*`)

3DGS data is scene-owned. Scene JSON can contain any number of `GaussianSplat`, `GaussianSplats`, or `3DGaussianSplat` nodes. `load_gaussian_splats(...)` appends another `GaussianSplat` node to the current scene root. Switching scenes clears the old scene graph, including its 3DGS objects.

Rasterization runs over all enabled 3DGS scene objects. Emissive proxy sampling combines all enabled 3DGS objects into one world-space proxy list. Splat shadows currently use the first enabled 3DGS object as the primary shadow source.

`gaussian_splat_translation`, `gaussian_splat_rotation_euler_deg`, and `gaussian_splat_object_scale` apply only when Python appends a new 3DGS node through `load_gaussian_splats(...)`.

| Property | Type | Notes |
| --- | --- | --- |
| `enable_gaussian_splats` | `bool` | Enables rendering for 3DGS scene objects. |
| `gaussian_splat_depth_test` | `bool` | Test against scene depth. |
| `gaussian_splat_sorting_mode` | `int/GaussianSplatSortMode` | `GpuSort` or `StochasticSplats`. |
| `gaussian_splat_sh_format` | `int/GaussianSplatStorageFormat` | SH payload storage format. |
| `gaussian_splat_rgba_format` | `int/GaussianSplatStorageFormat` | RGBA payload storage format. |
| `gaussian_splat_use_aabbs` | `bool` | Use AABB-based splat shadow acceleration data. |
| `gaussian_splat_use_tlas_instances` | `bool` | Use TLAS instances for splat shadow acceleration. |
| `gaussian_splat_blas_compaction` | `bool` | Enable BLAS compaction for splat shadow acceleration data. |
| `gaussian_splat_mip_antialiasing` | `bool` | Enable splat mip antialiasing path. |
| `gaussian_splat_quantize_normals` | `bool` | Quantize generated splat normals in the RTX path. |
| `gaussian_splat_ftb_sync_mode` | `int/GaussianSplatFTBSyncMode` | Front-to-back synchronization mode. |
| `gaussian_splat_frustum_culling` | `int/GaussianSplatFrustumCulling` | Frustum culling stage. |
| `gaussian_splat_frustum_dilation` | `float` | Culling frustum dilation. |
| `gaussian_splat_screen_size_culling` | `bool` | Enable screen-size splat culling. |
| `gaussian_splat_min_pixel_coverage` | `float` | Minimum pixel coverage for screen-size culling. |
| `gaussian_splat_depth_iso_threshold` | `float` | Depth/iso-surface threshold used by the splat path. |
| `gaussian_splat_fragment_shader_barycentric` | `bool` | Use fragment-shader barycentric path when supported. |
| `gaussian_splat_scale` | `float` | Projected footprint scale. |
| `gaussian_splat_alpha_scale` | `float` | Opacity multiplier. |
| `gaussian_splat_brightness` | `float` | Color multiplier. |
| `gaussian_splat_tint_color` | `(r, g, b)` | Multiplies the SH0/base color before brightness. |
| `gaussian_splat_as_emitter` | `bool` | Inject 3DGS emissive proxies into light sampling. |
| `gaussian_splat_emission_intensity` | `float` | Emissive proxy intensity multiplier. |
| `gaussian_splat_emission_max_proxy_count` | `int` | Emissive proxy budget. |
| `gaussian_splat_alpha_cull_threshold` | `float` | Cull low-alpha splats. |
| `gaussian_splat_translation` | `(x, y, z)` | Initial translation for newly attached Python 3DGS nodes. |
| `gaussian_splat_rotation_euler_deg` | `(x, y, z)` | Initial Euler rotation in degrees for newly attached Python 3DGS nodes. |
| `gaussian_splat_object_scale` | `(x, y, z)` | Initial non-uniform scale for newly attached Python 3DGS nodes. |
| `gaussian_splat_shadows` | `bool` | Enable splat shadow integration. |
| `gaussian_splat_hybrid_shadows` | `bool` | Alias for `gaussian_splat_shadows`. |
| `gaussian_splat_shadows_mode` | `int/GaussianSplatShadowMode` | Disabled, hard, or soft splat shadows. |
| `gaussian_splat_shadow_strength` | `float` | Shadow opacity/strength. |
| `gaussian_splat_shadow_soft_radius` | `float` | Soft shadow radius. |
| `gaussian_splat_shadow_soft_sample_count` | `int` | Soft shadow sample count. |
| `gaussian_splat_rtx_kernel_degree` | `int` | RTX splat kernel degree. |
| `gaussian_splat_rtx_adaptive_clamp` | `bool` | Enable adaptive RTX alpha clamp. |
| `gaussian_splat_rtx_alpha_clamp` | `float` | Manual RTX alpha clamp value. |
| `gaussian_splat_rtx_minimum_transmittance` | `float` | Minimum transmittance clamp for RTX splat tracing. |
| `gaussian_splat_rtx_trace_strategy` | `int` | RTX splat tracing strategy selector. |
| `gaussian_splat_rtx_particle_samples_per_pass` | `int` | RTX particle samples processed per pass. |
| `gaussian_splat_rtx_maximum_pass_count` | `int` | Maximum RTX splat trace pass count. |
| `gaussian_splat_rtx_particle_shadow_offset` | `float` | RTX particle shadow offset. |
| `gaussian_splat_rtx_particle_shadow_threshold` | `float` | RTX particle shadow threshold. |
| `gaussian_splat_rtx_colored_shadow_strength` | `float` | Strength for colored splat shadows. |
| `gaussian_splat_rtx_mesh_composite_threshold` | `float` | Mesh/splat composite threshold. |
| `gaussian_splat_rtx_depth_iso_threshold` | `float` | RTX depth/iso-surface threshold. |
| `gaussian_splat_object_count` | `int` | Read-only 3DGS scene object count. |
| `gaussian_splat_count` | `int` | Read-only total splat count. |
| `gaussian_splat_file_name` | `str` | Read-only single path or multi-object summary. |

## Settings

`Settings` mirrors the live ImGui UI state (`caustica.settings()` or `app.settings`). Most fields are writable and take effect on subsequent frames. For 3DGS-specific fields, see [3DGS](#3dgs).

### General

| Property | Type | Notes |
| --- | --- | --- |
| `show_ui` | `bool` | Show/hide UI. |
| `enable_animations` | `bool` | Scene animation toggle. |
| `enable_vsync` | `bool` | VSync toggle. |
| `fps_limiter` | `float/int` | FPS limiter value. |

### Path Tracing Mode / Accumulation

| Property | Type | Notes |
| --- | --- | --- |
| `realtime_mode` | `bool` | `True` realtime, `False` reference. |
| `path_tracer_mode` | `int/PathTracerMode` | `Realtime=0`, `Reference=1`; changing it resets accumulation. |
| `realtime_samples_per_pixel` | `int` | SPP in realtime mode. |
| `accumulation_target` | `int` | Reference SPP target. |
| `reset_accumulation` | `bool` | Set `True` to reset accumulation. |
| `accumulation_aa` | `bool/int` | Accumulation AA toggle/setting. |
| `accumulation_prewarm_realtime_caches` | `bool` | Prewarm realtime caches before accumulation. |

### Path Tracer Knobs

| Property | Type |
| --- | --- |
| `bounce_count` | `int` |
| `diffuse_bounce_count` | `int` |
| `enable_russian_roulette` | `bool` |
| `texture_lod_bias` | `float` |

### NEE / ReSTIR

| Property | Type | Notes |
| --- | --- | --- |
| `use_nee` | `bool` | Next event estimation. |
| `nee_type` | `int` | `0=uniform`, `1=power-based`, `2=NEE-AT`. |
| `nee_candidate_samples` | `int` | Candidate sample count. |
| `nee_full_samples` | `int` | Full sample count. |
| `nee_mis_type` | `int` | MIS mode. |
| `use_restir_di` | `bool` | ReSTIR direct illumination. |
| `use_restir_gi` | `bool` | ReSTIR global illumination. |

### Camera

| Property | Type |
| --- | --- |
| `camera_aperture` | `float` |
| `camera_focal_distance` | `float` |
| `camera_move_speed` | `float` |

### Firefly Filters

| Property | Type |
| --- | --- |
| `realtime_firefly_filter_enabled` | `bool` |
| `realtime_firefly_filter_threshold` | `float` |
| `reference_firefly_filter_enabled` | `bool` |
| `reference_firefly_filter_threshold` | `float` |

### Tone Mapping / Bloom

| Property | Type |
| --- | --- |
| `enable_tone_mapping` | `bool` |
| `enable_bloom` | `bool` |
| `bloom_intensity` | `float` |
| `bloom_radius` | `float` |

### Realtime AA / DLSS / Reflex

Availability depends on build options and hardware support.

| Property | Type | Notes |
| --- | --- | --- |
| `realtime_aa` | `int/RealtimeAA` | `0=Off`, `1=TAA`, `2=DLSS`, `3=DLSS_RR`. |
| `dlss_mode` | `int/DLSSMode` | DLSS quality preset. |
| `dlss_lod_bias_use_override` | `bool` | Override DLSS texture LOD bias. |
| `dlss_lod_bias_override` | `float` | LOD bias override. |
| `dlss_always_use_extents` | `bool` | Use DLSS extents mode. |
| `dlss_fg_mode` | `int/DLSSFGMode` | DLSS frame generation mode. |
| `dlss_fg_multiplier` | `int` | Frame generation multiplier. |
| `dlss_fg_num_frames_to_generate` | `int` | Current generated frame count. |
| `dlss_fg_max_num_frames_to_generate` | `int` | Max generated frame count. |
| `dlss_rr_preset` | `int/DLSSRRPreset` | DLSS Ray Reconstruction preset. |
| `dlss_rr_micro_jitter` | `bool/float` | DLSS-RR micro jitter setting. |
| `dlss_rr_brightness_clamp_k` | `float` | Brightness clamp factor. |
| `disable_restirs_with_dlss_rr` | `bool` | Disable ReSTIR features with DLSS-RR. |
| `reflex_mode` | `int/ReflexMode` | NVIDIA Reflex mode. |
| `reflex_capped_fps` | `float/int` | Reflex FPS cap. |

Read-only support flags:

| Property | Type |
| --- | --- |
| `is_dlss_supported` | `bool` |
| `is_dlss_fg_supported` | `bool` |
| `is_dlss_rr_supported` | `bool` |
| `is_reflex_supported` | `bool` |

### Denoisers

Realtime / NRD:

| Property | Type | Notes |
| --- | --- | --- |
| `standalone_denoiser` | `bool` | NRD denoiser in realtime mode; no effect with DLSS-RR. |
| `denoiser_radiance_clamp_k` | `float` | NRD radiance clamp. |

Reference / OIDN:

| Property | Type | Notes |
| --- | --- | --- |
| `oidn_enabled` | `bool` | Run OIDN when accumulation completes. |
| `oidn_use_gpu` | `bool` | Use OIDN GPU device when available. |
| `oidn_passes` | `int/OidnPasses` | Auxiliary guide passes. |
| `oidn_prefilter` | `int/OidnPrefilter` | Guide prefilter quality. |
| `oidn_quality` | `int/OidnQuality` | Beauty filter quality. |
| `oidn_changed` | `bool` | Set true after edits; renderer clears it. |
| `oidn_apply()` | method | Marks OIDN parameters dirty. |

### Environment Map Runtime Parameters

`settings.environment_map` is an `EnvironmentMapParams` object:

| Property | Type |
| --- | --- |
| `tint_color` | `(r, g, b)` |
| `intensity` | `float` |
| `rotation_xyz` | `(x, y, z)` |
| `enabled` | `bool` |
| `visible_to_camera` | `bool` |
| `hide_source` | `bool` inverse of `visible_to_camera` |

## Enums

All enums are arithmetic, so `int(enum_value)` works and enum values can be assigned to int-backed settings fields. 3DGS-specific enums are listed under [3DGS → 3DGS Enums](#3dgs-enums).

### Path Tracing & Realtime

#### `PathTracerMode`

| Value | Int | Meaning |
| --- | ---: | --- |
| `caustica.PathTracerMode.Realtime` | `0` | Realtime path tracing mode. |
| `caustica.PathTracerMode.Reference` | `1` | Reference accumulation mode. |

#### `RealtimeAA`

| Value | Int | Meaning |
| --- | ---: | --- |
| `caustica.RealtimeAA.Off` | `0` | No realtime AA/upscaler. |
| `caustica.RealtimeAA.TAA` | `1` | Temporal AA. |
| `caustica.RealtimeAA.DLSS` | `2` | DLSS Super Resolution. |
| `caustica.RealtimeAA.DLSS_RR` | `3` | DLSS Ray Reconstruction. |

#### `DLSSMode`

| Value | Int |
| --- | ---: |
| `Off` | `0` |
| `MaxPerformance` | `1` |
| `Balanced` | `2` |
| `MaxQuality` | `3` |
| `UltraPerformance` | `4` |
| `UltraQuality` | `5` |
| `DLAA` | `6` |

#### `DLSSFGMode`

| Value | Int |
| --- | ---: |
| `Off` | `0` |
| `On` | `1` |
| `Auto` | `2` |

#### `DLSSRRPreset`

| Value | Int |
| --- | ---: |
| `Default` | `0` |
| `PresetA` ... `PresetH` | `1` ... `8` |

#### `ReflexMode`

| Value | Int |
| --- | ---: |
| `Off` | `0` |
| `LowLatency` | `1` |
| `LowLatencyWithBoost` | `2` |

### OIDN

| Enum | Values |
| --- | --- |
| `OidnPasses` | `ColorOnly=0`, `Albedo=1`, `AlbedoNormal=2` |
| `OidnPrefilter` | `None_=0`, `Fast=1`, `Accurate=2` |
| `OidnQuality` | `Fast=0`, `Balanced=1`, `High=2` |

### Material

#### `TextureSlot`

| Value | Meaning |
| --- | --- |
| `Base` | Base/diffuse texture. |
| `ORM` / `OcclusionRoughnessMetallic` | ORM texture, or spec-gloss texture when `use_specular_gloss=True`. |
| `Normal` | Normal texture. |
| `Emissive` | Emissive texture. |
| `Transmission` | Transmission texture. |

### Lights

#### `LightType`

| Value | Meaning |
| --- | --- |
| `None_` | Not a light (`0`). |
| `Directional` | Directional / sun light. |
| `Spot` | Spot light. |
| `Point` | Point light. |
| `Environment` | Environment / HDRI light. |

`SceneEntity.light_type` stores the same integer values; compare with `int(caustica.LightType.Point)` or the enum directly.

## Embedded Mode Notes

In embedded mode, scripts run inside `caustica.exe`:

```powershell
caustica.exe --pythonScript examples/python/embedded.py
caustica.exe --pythonExpr "import caustica; print(caustica.app().scene_name)"
```

Inside the app, the Python panel can run inline code. Typical script shape:

```python
import caustica

app = caustica.app()
s = caustica.settings()

s.realtime_mode = True
s.realtime_aa = int(caustica.RealtimeAA.TAA)
app.reset_accumulation()
```

Do not create `caustica.Renderer` in embed mode; the running app already owns the renderer.

The application handle returned by `caustica.app()` is the supported entry point for new embed scripts. Use its `spawn_from_file`, `deform_mesh`, and other runtime methods directly.

## Extension Mode Notes

In extension mode, every `Renderer` owns a GPU device and scene. Use `close()` or a context manager so GPU resources are released promptly.

```python
with caustica.Renderer(headless=True, scene="...") as r:
    ...
```

For windowed extension usage:

- `headless=False` opens a GLFW window.
- `Renderer.step()` must be called repeatedly to pump events and render frames.
- Clicking the window close button makes `step()` return `False`.
- Resize/maximize/minimize are handled by the underlying device manager during `step()`.

## Existing Examples

| File | Purpose |
| --- | --- |
| `examples/python/render.py` | Reference / realtime / windowed rendering, spawning, material and camera edits, framebuffer readback. |
| `examples/python/gaussian_splats.py` | 3DGS: `view` a standalone `.ply`, `hybrid` mesh+splat scenes, `colmap` camera reproduction. |
| `examples/python/animation_sequence.py` | Scene animation rendered at explicit timeline samples. |
| `examples/python/mesh_deformation.py` | Per-frame CPU vertex rewrites via `deform_mesh` with acceleration-structure rebuilds. |
| `examples/python/camera_intrinsics.py` | Off-center pinhole intrinsics through `set_camera_intrinsics`. |
| `examples/python/environment_lighting.py` | HDRI environment maps and procedural-sky presets. |
| `examples/python/embedded.py` | Embedded scripting inside `caustica.exe`. |

Shared helper modules, not runnable examples:

| File | Purpose |
| --- | --- |
| `examples/python/_common.py` | Renderer construction, shared CLI flags, render-mode and denoiser selection, camera framing, output helpers. |
| `examples/python/_gaussian.py` | 3DGS settings mapping, splat-only scene template, PLY bounds reader. |
| `examples/python/_colmap.py` | COLMAP sparse-model reader and camera conversion. |

Extension-mode examples share `--width`, `--height`, `--vulkan`, and `--adapter`. Realtime ones share `--denoiser` (`auto`, `off`, `taa`, `nrd`, `dlss`, `dlss-rr`), which falls back automatically when the device lacks DLSS support.

Also see `examples/python/README.md` for embed vs extension setup.

## Introspection

The binding also exposes docstrings through nanobind:

```python
import caustica
help(caustica)
help(caustica.Renderer)
help(caustica.SceneEntity)
help(caustica.Settings)
```
