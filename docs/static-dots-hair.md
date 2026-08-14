# Static DOTS hair

Caustica imports static glTF hair strands from `LINES` or `LINE_STRIP`
primitives. Each strand vertex must have `POSITION`; `_RADIUS` is optional and
defaults to `0.001` scene units. `TEXCOORD_0` is preserved when present.

At import time, every segment is expanded to four triangles in Disjoint
Orthogonal Triangle Strips (DOTS) form. The generated mesh uses the regular
triangle BLAS path, while hit shading reconstructs a tapered cylindrical normal
from the segment endpoints and radii.

Hair shading is enabled through the material's `NV_materials_hair` extension:

```json
{
  "extensions": {
    "NV_materials_hair": {
      "model": "FarField",
      "baseColor": [0.227, 0.130, 0.035],
      "melanin": 0.6,
      "melaninRedness": 0.0,
      "longitudinalRoughness": 0.354,
      "azimuthalRoughness": 0.6,
      "ior": 1.55,
      "cuticleAngle": 3.0,
      "diffuseReflectionWeight": 0.0,
      "diffuseReflectionTint": [0.02, 0.008, 0.008]
    }
  }
}
```

Line and line-strip curve primitives automatically use the RTXCR Far-Field
hair defaults when the extension is absent. Explicit `NV_materials_hair`
values always take precedence. Imported strand radii use RTXCR's default
`0.618` scale so dense production grooms retain their intended coverage.

`model` accepts `"FarField"` (the default) or `"Chiang"`. Far-Field supports
the additional diffuse reflection weight and tint; Chiang uses the longitudinal
and azimuthal roughness parameters directly. The same settings are available
under the Hair section of the material editor and are serialized with scene
material overrides.

This path is static: animated strand control points are not retessellated.
