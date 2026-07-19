# OpenPBR materials

Caustica uses OpenPBR as the built-in material model (`MaterialModel`: `"OpenPBR"`).
Parameters map onto the path-tracer BSDF (diffuse, GGX specular, transmission,
anisotropy, fuzz, coat, subsurface approximation, thin-film, and dispersion).
Existing `.material.json` files remain valid.

When `MaterialModel` is `"OpenPBR"`, the inspector shows OpenPBR parameter names and
converts them internally to `StandardMaterial` fields and the GPU data layout.

## Coverage

| OpenPBR group | Status |
| --- | --- |
| Base / specular / metalness | Implemented |
| Transmission (+ color, depth, scatter) | Implemented |
| Fuzz | Implemented (sheen approximation) |
| Coat (+ darkening) | Implemented (GGX coat lobe + base attenuation) |
| Thin-film | Implemented (RGB Airy iridescence approx) |
| Dispersion | Implemented (Abbe-number RGB η scale) |
| Subsurface | Implemented as lobe mix + homogeneous `sigmaS` (not full BSSRDF random walk) |
| Volume absorption | Implemented (`volume_attenuation_*` / transmission depth) |
| Coat / base separate normals | Not yet (shared shading normal) |
| Full OpenPBR energy white-furnace model | Approximate (Turquin MS + coat attenuation) |

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
| `SpecularWeight` | Multiplies dielectric specular. |
| `Anisotropy` | Directional GGX highlight amount, range `[-1, 1]`. |
| `FuzzWeight` / `FuzzColor` / `FuzzRoughness` | Cloth/velvet/dust fuzz lobe. |
| `CoatWeight` / `CoatColor` / `CoatRoughness` / `CoatAnisotropy` / `CoatIor` / `CoatDarkening` | Clearcoat layer. |
| `SubsurfaceWeight` / `SubsurfaceColor` / `SubsurfaceRadius` / `SubsurfaceScale` / `SubsurfaceAnisotropy` | Dense scattering approx. |
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
| `subsurface_scale` | `SubsurfaceScale` |
| `subsurface_anisotropy` | `SubsurfaceAnisotropy` |
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

- **Subsurface**: opaque-base mix toward `subsurface_color`, plus homogeneous volume scattering from radius/scale. Thin-walled subsurface also raises diffuse transmission. Full path-traced BSSRDF random walk is future work.
- **Coat PSD**: path-space decomposition dominant bounce index `2` is coat reflection.
- **Dispersion**: RGB Abbe approximation on relative η; not a full spectral solver.
