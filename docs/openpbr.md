# OpenPBR materials

Caustica uses OpenPBR as the built-in material model (`MaterialModel`: `"OpenPBR"`).
Parameters map onto the path-tracer BSDF (rough diffuse, GGX specular,
transmission, anisotropy, fuzz, coat, subsurface approximation, thin-film, and
RGB hero-wavelength dispersion).
Existing `.material.json` files remain valid. Runtime shading is OpenPBR-only:
legacy RTXPT files are converted on load (white dielectric specular if the
legacy specular was zero; roughness is copied to `baseDiffuseRoughness` unless
the file already authored OpenPBR). glTF specular-gloss remains a texture layout,
not a second material model.

The inspector always shows OpenPBR parameter names and maps them to
`StandardMaterial` fields and the GPU data layout.
The JSON reader accepts OpenPBR snake_case fields inside an `OpenPBR` object or
at the material root. If both legacy/PascalCase fields and an `OpenPBR` object
are present, the OpenPBR object is read after the legacy fields and therefore
wins for mapped values. A material without an explicit model defaults to
OpenPBR.

Authoritative implementation paths:

- `caustica/caustica/src/render/passes/lighting/MaterialGpuCache.cpp` — JSON
  load/save and GPU constant baking.
- `application/editor/ui/MaterialEditorGui.cpp` — inspector controls.
- `caustica/caustica/shaders/PathTracer/Rendering/Materials/BxDF.hlsli` —
  path-tracer lobe evaluation.
- `caustica/Python/PythonBindingsCore.cpp` — Python property names.

Material override discovery and texture path rules are documented in
[Scene JSON — 材质覆盖](scene-json.md#材质覆盖).

## Coverage

| OpenPBR group | Status |
| --- | --- |
| Base / specular / metalness | Implemented (effective-IOR dielectric + F82-tint metal) |
| Transmission (+ color, depth, scatter) | Implemented |
| Fuzz | Implemented (energy-layered sheen approximation) |
| Coat (+ darkening) | Implemented (GGX coat lobe + energy-layered base attenuation) |
| Thin-film | Implemented (RGB Airy iridescence approx) |
| Dispersion | Implemented (Cauchy/Abbe RGB hero-wavelength sampling) |
| Subsurface | RTXCR Burley diffusion BSSRDF + ray-traced boundary transmission and single scattering |
| Volume absorption | Implemented (`volume_attenuation_*` / transmission depth) |
| Coat / base separate normals | Not yet (shared shading normal) |
| Full OpenPBR energy white-furnace model | Approximate; white coat/fuzz layer identities have regression tests |

## Existing material parameters

Core textures:

| Field | Meaning |
| --- | --- |
| `BaseTexture` | Base color / diffuse RGB and opacity alpha. |
| `OcclusionRoughnessMetallicTexture` | ORM texture in metal-rough mode, or specular/gloss in spec-gloss mode. |
| `NormalTexture` | Tangent-space normal map. |
| `EmissiveTexture` | Emissive color texture. |
| `TransmissionTexture` | Transmission multiplier texture. |

Core scalar and color fields:

| Field | Meaning |
| --- | --- |
| `BaseOrDiffuseColor` | Metal-rough base color, or spec-gloss diffuse color. |
| `SpecularColor` | Specular color. In OpenPBR this tints dielectric specular. |
| `Metalness` | Metalness in metal-rough mode. |
| `Roughness` | Surface roughness. |
| `Opacity` | Scalar opacity, multiplied by base texture alpha. |
| `NormalTextureScale` | Normal map strength. |
| `IoR` | Interior index of refraction. |
| `EmissiveColor` | Emissive color. |
| `EmissiveIntensity` | Emissive multiplier. |
| `TransmissionFactor` | Specular transmission weight. |
| `DiffuseTransmissionFactor` | Diffuse transmission weight. |
| `VolumeAttenuationColor` | Volume attenuation color for non-thin transmission. |
| `VolumeAttenuationDistance` | Volume attenuation distance. |

OpenPBR fields:

| Field | Meaning |
| --- | --- |
| `MaterialModel` | `"OpenPBR"` enables OpenPBR authoring and specular tinting. |
| `BaseWeight` | Multiplies the diffuse/base contribution. |
| `BaseDiffuseRoughness` | Roughness of the diffuse lobe, independent of specular roughness. |
| `SpecularWeight` | Modulates the dielectric effective IOR/Fresnel response and scales the metal Fresnel response. |
| `Anisotropy` | Directional GGX highlight amount, range `[-1, 1]`. |
| `FuzzWeight` / `FuzzColor` / `FuzzRoughness` | Cloth/velvet/dust fuzz lobe. |
| `CoatWeight` / `CoatColor` / `CoatRoughness` / `CoatAnisotropy` / `CoatIor` / `CoatDarkening` | Clearcoat layer. |
| `SubsurfaceWeight` / `SubsurfaceColor` / `SubsurfaceRadius` / `SubsurfaceRadiusScale` / `SubsurfaceAnisotropy` | RTXCR skin scattering. Radius is in centimeters; `SubsurfaceRadiusScale` is RGB. |
| `ThinFilmWeight` / `ThinFilmThickness` / `ThinFilmIor` | Iridescent thin film (thickness in µm). |
| `TransmissionColor` / `TransmissionDepth` / `TransmissionScatter` / `TransmissionScatterAnisotropy` | Transmission medium. |
| `TransmissionDispersionScale` / `TransmissionDispersionAbbeNumber` | Chromatic dispersion. |

## OpenPBR Mapping

```json
{
  "MaterialModel": "OpenPBR",
  "OpenPBR": {
    "base_weight": 1.0,
    "base_color": [0.55, 0.48, 0.40],
    "base_metalness": 0.0,
    "base_diffuse_roughness": 0.0,
    "specular_weight": 0.45,
    "specular_color": [1.0, 0.95, 0.9],
    "specular_roughness": 0.35,
    "specular_roughness_anisotropy": 0.0,
    "specular_ior": 1.5,
    "coat_weight": 1.0,
    "coat_color": [1.0, 1.0, 1.0],
    "coat_roughness": 0.05,
    "coat_ior": 1.6,
    "coat_darkening": 1.0,
    "thin_film_weight": 0.0,
    "thin_film_thickness": 0.5,
    "thin_film_ior": 1.4,
    "subsurface_weight": 0.0,
    "subsurface_color": [0.8, 0.2, 0.15],
    "subsurface_radius": 0.5,
    "subsurface_radius_scale": [1.0, 0.5, 0.25],
    "subsurface_scatter_anisotropy": 0.0,
    "transmission_weight": 0.0,
    "transmission_color": [1.0, 1.0, 1.0],
    "transmission_depth": 0.0,
    "transmission_dispersion_scale": 0.0,
    "transmission_dispersion_abbe_number": 55.0,
    "fuzz_weight": 0.0,
    "geometry_opacity": 1.0,
    "geometry_thin_walled": false,
    "emission_color": [0.0, 0.0, 0.0],
    "emission_luminance": 1.0
  }
}
```

| OpenPBR field | Backend field |
| --- | --- |
| `base_weight` | `BaseWeight` |
| `base_color` | `BaseOrDiffuseColor` |
| `base_metalness` | `Metalness` |
| `base_diffuse_roughness` | `BaseDiffuseRoughness` |
| `specular_weight` | `SpecularWeight` |
| `specular_color` | `SpecularColor` |
| `specular_roughness` | `Roughness` |
| `specular_roughness_anisotropy` | `Anisotropy` |
| `specular_ior` | `IoR` |
| `coat_weight` | `CoatWeight` |
| `coat_color` | `CoatColor` |
| `coat_roughness` | `CoatRoughness` |
| `coat_roughness_anisotropy` | `CoatAnisotropy` |
| `coat_ior` | `CoatIor` |
| `coat_darkening` | `CoatDarkening` |
| `subsurface_weight` | `SubsurfaceWeight` |
| `subsurface_color` | `SubsurfaceColor` |
| `subsurface_radius` | `SubsurfaceRadius` |
| `subsurface_radius_scale` | `SubsurfaceRadiusScale` (RGB) |
| `subsurface_scatter_anisotropy` | `SubsurfaceAnisotropy` |
| `thin_film_weight` | `ThinFilmWeight` |
| `thin_film_thickness` | `ThinFilmThickness` |
| `thin_film_ior` | `ThinFilmIor` |
| `transmission_weight` | `TransmissionFactor` |
| `transmission_diffuse_weight` | `DiffuseTransmissionFactor` |
| `transmission_color` | `TransmissionColor` |
| `transmission_depth` | `TransmissionDepth` |
| `transmission_scatter` | `TransmissionScatter` |
| `transmission_scatter_anisotropy` | `TransmissionScatterAnisotropy` |
| `transmission_dispersion_scale` | `TransmissionDispersionScale` |
| `transmission_dispersion_abbe_number` | `TransmissionDispersionAbbeNumber` |
| `geometry_opacity` | `Opacity` |
| `geometry_thin_walled` | `ThinSurface` |
| `emission_color` | `EmissiveColor` |
| `emission_luminance` | `EmissiveIntensity` |
| `fuzz_weight` | `FuzzWeight` |
| `fuzz_color` | `FuzzColor` |
| `fuzz_roughness` | `FuzzRoughness` |

## Notes

- **Subsurface**: thick surfaces use RTXCR's Burley diffusion profile, a ray-traced back-boundary transmission term, and one HG-importance-sampled single-scattering event per path vertex. `subsurface_radius` is interpreted in centimeters and multiplied per channel by `subsurface_radius_scale`. Thin-walled materials retain the diffuse-transmission approximation because they have no closed interior to trace. This is a real-time combined BSSRDF, not a full random walk.
- **Coat PSD**: path-space decomposition dominant bounce index `2` is coat reflection.
- **Compatibility aliases**: legacy scalar `subsurface_scale` is accepted and broadcast to RGB; `subsurface_anisotropy` remains accepted as an alias for `subsurface_scatter_anisotropy`.
- **Dispersion**: one RGB hero wavelength is selected per transmissive BSDF sample, with its IOR derived from the Cauchy equation and OpenPBR effective Abbe number. This produces real angular color separation but is not a full spectral solver.
- **White furnace**: `causOpenPBRMaterialTests` covers effective-IOR, F82, dispersion ordering, and the non-absorbing coat/fuzz layer identities. The GPU-labelled `causOpenPBRWhiteFurnaceRenderTests` test additionally renders an OpenPBR sphere in a constant linear-white HDR environment and compares coat/fuzz images against an unlayered paired golden over the sphere interior. Captures and amplified difference images are written under `build/test-output/openpbr-white-furnace`.
