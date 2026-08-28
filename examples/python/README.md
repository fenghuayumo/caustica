# caustica Python examples

Each script teaches one capability. Start with `render.py`, then move to the
specialized scripts when you need that particular camera, animation, lighting,
or Gaussian-splat workflow.

Install the extension once from the repository root:

```shell
python -m pip install .
```

All examples import `caustica` as a normally installed package. If the import
fails, fix the install rather than patching `sys.path`.

## Start here

`render.py` is the general-purpose example. It covers the three ways the
renderer can be driven, plus runtime scene edits and framebuffer readback:

```shell
# Reference accumulation with OIDN.
python examples/python/render.py --scene builtin:plane_cube --out frame.png

# Fixed-count realtime frames with NRD + TAA.
python examples/python/render.py --mode realtime --denoiser nrd --frames 32

# Interactive preview.
python examples/python/render.py --mode window --scene Assets/scenes/default/default.scene.json

# Runtime asset spawn plus framebuffer readback.
python examples/python/render.py \
  --spawn Assets/models/GlassSphere/GlassSphere.gltf \
  --inspect-framebuffer
```

## Examples

| Script | Capability |
|---|---|
| `render.py` | Reference / realtime / windowed rendering, spawning, material and camera edits, framebuffer readback |
| `gaussian_splats.py` | 3D Gaussian splats: standalone `.ply` viewing, hybrid mesh+splat scenes, COLMAP camera reproduction |
| `animation_sequence.py` | Render scene animation at explicit timeline samples in reference or realtime mode |
| `mesh_deformation.py` | Rewrite CPU mesh vertices per frame and rebuild acceleration structures |
| `camera_intrinsics.py` | Drive the camera from a pinhole intrinsic matrix with an off-center principal point |
| `environment_lighting.py` | HDRI environment maps and procedural-sky presets |
| `embedded.py` | Scripting the running editor from inside `caustica.exe` |

Every script supports `--help`, and shares `--width`, `--height`, `--vulkan`,
and `--adapter`. The adapter accepts `auto`, `index:N`, `name:text`, and the
stable `uuid:hex` / `luid:hex` selectors; list devices with
`caustica.enumerate_adapters()`.

Realtime scripts share a `--denoiser` flag accepting `auto`, `off`, `taa`,
`nrd`, `dlss`, and `dlss-rr`. `auto` selects NRD + TAA, and any DLSS path falls
back automatically when the device does not support it.

## Gaussian splats

`gaussian_splats.py` has three subcommands because the workflows differ only in
where the camera and the splat data come from:

```shell
# Interactively preview a standalone .ply, framed from its own bounds.
python examples/python/gaussian_splats.py view --ply splat.ply

# Render the same file headless, reference and realtime back to back.
python examples/python/gaussian_splats.py view --ply splat.ply \
  --mode batch --out-dir splat_out

# Mesh + splat scene with ray-traced soft shadows and emissive splat proxies.
python examples/python/gaussian_splats.py hybrid --scene my_hybrid.scene.json --window

# Reproduce COLMAP poses, including off-center principal points.
python examples/python/gaussian_splats.py colmap \
  --ply gaussians.ply --colmap-dir sparse --max-views 8
```

`--ply` and `--colmap-dir` are required; there are no machine-specific default
paths. The `colmap` subcommand additionally needs numpy (`python -m pip install
numpy`) and reads binary or text sparse models.

The `hybrid` subcommand needs a scene that already declares `GaussianSplat`
nodes alongside its meshes; see [3DGS nodes](../../docs/scene-json.md) for the
scene JSON syntax. It warns and renders the meshes alone if the scene contains
no splats.

## Embedded scripting

`embedded.py` runs inside `caustica.exe` rather than owning a renderer:

```shell
caustica.exe --pythonScript examples/python/embedded.py
caustica.exe --pythonExpr "import caustica; print(caustica.app().scene_name)"
```

Both modes expose the same scene and settings types, but the lifecycle differs:

| Mode | Entry point | Ownership |
|---|---|---|
| Extension | `python script.py` | Python creates and closes `caustica.Renderer` |
| Embedded | `caustica.exe --pythonScript ...` | The running application owns the renderer |

## Shared modules

These are not executable examples:

| Module | Contents |
|---|---|
| `_common.py` | Renderer construction, shared CLI flags, render-mode and denoiser selection, camera framing, output helpers |
| `_gaussian.py` | 3DGS settings mapping, splat-only scene template, PLY bounds reader |
| `_colmap.py` | COLMAP sparse-model reader and camera conversion |

`embedded.py` deliberately imports none of them: the embedding host does not put
the script's directory on `sys.path`, so an embedded script must stand alone.

## Further reading

Full Python API reference: [py_caustica.md](../../py_caustica.md). To inspect
the installed module directly:

```python
import caustica
help(caustica)
```
