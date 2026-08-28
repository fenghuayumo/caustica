# RTXCR skin integration

Caustica integrates the permissively licensed RTX Character Rendering material
library for thick-surface skin rendering. It is enabled by setting an OpenPBR
material's `subsurface_weight` above zero. Hair materials and thin-walled
materials do not enter this path.

## Transport model

Each path-tracing surface vertex uses one temporally varying Monte Carlo sample
for each component:

1. The RTXCR Burley diffusion profile selects a color channel and a displaced
   surface position. Caustica projects the sample onto the same mesh instance
   and material, samples a scene light, traces visibility, and evaluates the
   BSSRDF.
2. A cosine-weighted ray enters the surface and finds the matching back
   boundary. RTXCR evaluates the diffuse boundary transmission with
   Beer-Lambert attenuation.
3. One distance along that interior segment and one Henyey-Greenstein direction
   are sampled. A second boundary ray and light visibility ray evaluate the
   single-scattering contribution.

The existing surface BSDF retains specular, coat, fuzz, and the non-subsurface
fraction of diffuse reflection. This prevents the Burley contribution from
being counted twice.

## OpenPBR mapping

| Caustica/OpenPBR | RTXCR |
| --- | --- |
| `subsurface_color` | `transmissionColor` |
| `subsurface_radius_scale` | `scatteringColor` |
| `subsurface_radius` (centimeters) | `scale` |
| `subsurface_scatter_anisotropy` | Henyey-Greenstein `g` |
| `subsurface_weight` | Mix weight against local diffuse |

The projection disk is bounded to eight RGB scattering radii; interior rays are
allowed a wider, still finite traversal for oblique paths through a closed head
or limb. All samples must return to the same mesh instance and material. This
prevents skin transport from leaking onto nearby clothing or unrelated geometry.
Materials with positive `subsurface_weight` are treated as closed/thick even if
specular transmission is disabled; explicitly thin-walled materials retain the
local diffuse-transmission approximation.

The glTF importer also maps NVIDIA's `NV_materials_subsurface` extension into
these OpenPBR fields, so imported character materials enter the same RTXCR path.
`transmissionColor`, `scatteringColor`, `scale`, and `anisotropy` map to
`subsurface_color`, `subsurface_radius_scale`, `subsurface_radius`, and
`subsurface_scatter_anisotropy`; the extension enables a weight of one.

## Combined character validation

`Assets/scenes/rtxcr/rtxcr-character-validation.scene.json` is a project-owned synthetic
character scene that exercises a closed skin mesh and static DOTS hair in one
render. Its glTF is checked in at
`Assets/models/rtxcr-character-validation/` (plus a `-skin-only` variant).
Run `caustica/Python/Tests/rtxcr_character_render_test.py`: it renders a
surface-only control, skin disabled, and skin plus hair, and requires both
branches to have a measurable image-space effect.

The Claire character distributed with the upstream RTXCR sample is deliberately
not bundled: its separate NVIDIA Asset License limits the mesh, textures, and
groom to NVIDIA ACE use.

If you have an authorized local RTXCR asset checkout, `Assets/scenes/rtxcr/rtxcr-claire.external.scene.json`
is a non-redistributing shim that references it. Point its `models` entries at your
checkout (they are absolute paths) and launch:

```powershell
.\bin\caustica.exe --scene Assets/scenes/rtxcr/rtxcr-claire.external.scene.json
```

The shim keeps all Claire geometry, textures, grooms, glasses, clothing, lights,
and camera in the upstream asset directory. Curve materials without an explicit
`NV_materials_hair` block use the RTXCR Far-Field defaults automatically. Strand
morph targets are imported as static DOTS, so static visual parity is the
supported Claire validation path.

## Source layout

- `shaders/PathTracer/Rendering/Materials/SubsurfaceMaterial.hlsli`
- `shaders/PathTracer/Rendering/Materials/SubsurfaceScattering.hlsli`
- `shaders/PathTracer/Rendering/Materials/SubsurfaceTransmission.hlsli`
- `shaders/PathTracer/Rendering/Materials/ScatteringCommon.hlsli`
- `shaders/PathTracer/Rendering/Materials/ScatteringMath.hlsli`
- `shaders/PathTracer/PathTracerSubsurface.hlsli`

The hair counterparts live alongside them:

- `shaders/PathTracer/Rendering/Materials/HairMaterial.hlsli`
- `shaders/PathTracer/Rendering/Materials/HairChiangBSDF.hlsli`
- `shaders/PathTracer/Rendering/Materials/HairFarFieldBCSDF.hlsli`
- `shaders/PathTracer/Rendering/Materials/HairBsdfHelpers.hlsli`

These files were adapted from the upstream RTXCR SDK rather than vendored
verbatim: they follow caustica's shader naming and the `RTXCR_` symbol prefix
is gone. Because they remain a derivative of the upstream MIT-licensed sources,
the NVIDIA copyright and permission notice is retained at the top of each file,
as MIT requires. Symbols that would otherwise collide with existing engine
shader code carry a `Hair`, `Subsurface`, or `Scatter` prefix instead.
