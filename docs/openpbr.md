# OpenPBR materials

Caustica uses OpenPBR as the built-in material model (`MaterialModel`: `"OpenPBR"`).
Parameters map onto the existing `PTMaterial` shader backend (diffuse, GGX specular,
transmission, anisotropy, and fuzz lobes). Existing `.material.json` files remain valid.

When `MaterialModel` is `"OpenPBR"`, the inspector shows OpenPBR parameter names and
converts them internally to `PTMaterial` fields and the GPU data layout.

Not yet implemented from the full OpenPBR specification: coat, subsurface, thin-film,
dispersion, and the complete OpenPBR energy model.

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

OpenPBR / cloth fields:

| Field | Meaning |
| --- | --- |
| `MaterialModel` | Use `"OpenPBR"` to save an `OpenPBR` block and enable OpenPBR specular tinting. |
| `BaseWeight` | Multiplies the diffuse/base contribution. |
| `SpecularWeight` | Multiplies dielectric specular. |
| `Anisotropy` | Directional GGX highlight amount, range `[-1, 1]`. |
| `FuzzWeight` | Cloth/velvet/dust fuzz lobe weight. |
| `FuzzColor` | Fuzz lobe color. |
| `FuzzRoughness` | Fuzz lobe softness. |

Render/control fields:

| Field | Meaning |
| --- | --- |
| `UseSpecularGlossModel` | Use specular-gloss material interpretation instead of metal-rough. |
| `EnableAlphaTesting`, `AlphaCutoff` | Alpha-test controls. |
| `EnableTransmission`, `ThinSurface` | Transmission and thin-surface controls. |
| `MetalnessInRedChannel` | Read metalness from ORM red instead of blue. |
| `ExcludeFromNEE` | Exclude from next-event-estimation shadow rays. |
| `PSDExclude`, `PSDDominantDeltaLobe` | Path-space decomposition controls. |
| `NestedPriority` | Nested dielectric priority. |
| `ShadowNoLFadeout` | Bias to hide low-tessellation shadow seams. |
| `EnableAsAnalyticLightProxy` | Use geometry as analytic light proxy. |
| `IgnoreMeshTangentSpace` | Rebuild/ignore mesh tangent space for shading. |
| `SkipRender` | Hide geometry using this material. |

## OpenPBR Mapping

You can write fields inside an `OpenPBR` object:

```json
{
  "MaterialModel": "OpenPBR",
  "OpenPBR": {
    "base_weight": 1.0,
    "base_color": [0.55, 0.48, 0.40],
    "base_metalness": 0.0,
    "specular_weight": 0.45,
    "specular_color": [1.0, 0.95, 0.9],
    "specular_roughness": 0.82,
    "specular_roughness_anisotropy": 0.45,
    "specular_ior": 1.5,
    "transmission_weight": 0.0,
    "transmission_diffuse_weight": 0.0,
    "geometry_opacity": 1.0,
    "geometry_thin_walled": true,
    "emission_color": [0.0, 0.0, 0.0],
    "emission_luminance": 1.0,
    "fuzz_weight": 0.35,
    "fuzz_color": [0.8, 0.72, 0.62],
    "fuzz_roughness": 0.75
  }
}
```

Or write the same snake_case fields at the top level. If any OpenPBR field
is present at the top level, Caustica treats the material as OpenPBR.

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
| `transmission_weight` | `TransmissionFactor` |
| `transmission_diffuse_weight` | `DiffuseTransmissionFactor` |
| `geometry_opacity` | `Opacity` |
| `geometry_thin_walled` | `ThinSurface` |
| `emission_color` | `EmissiveColor` |
| `emission_luminance` | `EmissiveIntensity` |
| `fuzz_weight` | `FuzzWeight` |
| `fuzz_color` | `FuzzColor` |
| `fuzz_roughness` | `FuzzRoughness` |

## Cloth Starting Points

Cotton / matte cloth:

```json
"OpenPBR": {
  "base_weight": 1.0,
  "base_metalness": 0.0,
  "specular_weight": 0.25,
  "specular_roughness": 0.9,
  "specular_roughness_anisotropy": 0.15,
  "fuzz_weight": 0.25,
  "fuzz_roughness": 0.85
}
```

Wool / fuzzy fabric:

```json
"OpenPBR": {
  "base_weight": 1.0,
  "base_metalness": 0.0,
  "specular_weight": 0.2,
  "specular_roughness": 0.95,
  "specular_roughness_anisotropy": 0.25,
  "fuzz_weight": 0.55,
  "fuzz_roughness": 0.9
}
```

Satin / directional woven fabric:

```json
"OpenPBR": {
  "base_weight": 1.0,
  "base_metalness": 0.0,
  "specular_weight": 0.7,
  "specular_roughness": 0.38,
  "specular_roughness_anisotropy": 0.75,
  "fuzz_weight": 0.12,
  "fuzz_roughness": 0.55
}
```

For clothing, normal maps still matter a lot. Use `NormalTexture` and
`NormalTextureScale` for weave detail, then use `Anisotropy` for directional
thread highlights and `FuzzWeight` for soft rim/fiber response.
