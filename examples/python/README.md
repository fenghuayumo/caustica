# caustica Python examples

The examples are grouped by capability rather than by renderer mode. Start with
`render.py`; use the specialized scripts only when you need the corresponding
camera, animation, lighting, or Gaussian-splat workflow.

Install the extension once from the repository root:

```shell
python -m pip install .
```

## Start here

`render.py` is the single general-purpose extension example. It replaces the
former offline, realtime, launch, and framebuffer scripts:

```shell
# Reference render with OIDN.
python examples/python/render.py --scene builtin:plane_cube --out frame.png

# Fixed-count realtime render with NRD + TAA.
python examples/python/render.py --mode realtime --denoiser nrd --frames 32

# Interactive preview.
python examples/python/render.py --mode window --scene Assets/default.json

# Runtime asset spawn and framebuffer readback.
python examples/python/render.py \\
  --spawn Assets/Models/GlassSphere/GlassSphere.gltf \\
  --inspect-framebuffer
```

The renderer supports `--adapter auto`, index/name selectors, and stable
`uuid:` or `luid:` selectors. List available devices with
`caustica.enumerate_adapters()`.

## Embedded scripting

`embedded.py` runs inside `caustica.exe` and demonstrates scene lookup,
material/light/environment edits, camera settings, and realtime mode selection:

```shell
caustica.exe --pythonScript examples/python/embedded.py
caustica.exe --pythonExpr "import caustica; print(caustica.app().scene_name)"
```

The extension and embedded modes expose the same scene/settings types, but their
lifecycle differs:

| Mode | Entry point | Ownership |
|---|---|---|
| Extension | `python script.py` | Python creates and closes `caustica.Renderer` |
| Embedded | `caustica.exe --pythonScript ...` | The running application owns the renderer |

## Specialized examples

| Script | Unique capability |
|---|---|
| `animation_sequence.py` | Render scene animation at explicit timeline samples in reference or realtime mode |
| `gaussian_splats.py` | Load a PLY, frame its bounds, and compare interactive/reference/realtime 3DGS |
| `hybrid_gaussian_scene.py` | Raster 3DGS with ray-traced soft shadows and emissive proxies |
| `mesh_deformation.py` | Edit CPU mesh vertices and rebuild acceleration structures per frame |
| `colmap_views.py` | Render a 3DGS PLY from COLMAP camera poses and pinhole intrinsics |
| `camera_intrinsics.py` | Demonstrate off-center `set_camera_intrinsics` projection |
| `environment_lighting.py` | Exercise HDRI and procedural-sky parameters |

`_common.py` contains path, framing, and window-loop helpers shared by these
scripts; it is not an executable example.

## Why there are fewer files

The old layout repeated renderer creation, mode selection, frame stepping, and
screenshot code across separate offline/realtime/default/framebuffer examples.
Those paths now live in `render.py`. Three small embedded scripts were similarly
combined into `embedded.py`. Specialized examples remain separate only when
they teach a distinct API or data format.

Full Python API reference: [py_caustica.md](../../py_caustica.md).

Inspect the installed module directly with:

```python
import caustica
help(caustica)
```
