# Caustica 场景 JSON 格式

场景 JSON 描述实体、组件和资源引用。网格通过 `PrefabInstance.source` 指向 glTF/OBJ/USD、`builtin:*`，或 `prefabs/*.prefab.json`。材质用 `PrefabInstance.materials` / `MaterialOverride` 显式引用 pack 相对 `.material.json`。OpenPBR 字段见 [OpenPBR materials](openpbr.md)。

相关实现：

- `caustica/caustica/src/scene/Scene.cpp`
- `caustica/caustica/src/scene/SceneSerializer.cpp`
- `caustica/caustica/src/scene/SceneEcs.cpp`
- `caustica/caustica/src/scene/SceneComponentBuilders.cpp`
- `caustica/caustica/src/scene/loader/`

## 最小示例

```json
{
  "settings": {
    "realtimeMode": true,
    "enableAnimations": false
  },
  "entities": [
    {
      "id": "GroundPlane",
      "name": "GroundPlane",
      "components": {
        "Transform": { "scale": [2.0, 1.0, 2.0] },
        "PrefabInstance": { "source": "builtin:plane" }
      }
    },
    {
      "id": "Antman",
      "name": "Antman",
      "components": {
        "Transform": {
          "translation": [-1.25, 0.0, 0.0],
          "euler": [0.0, 0.0, 0.0],
          "scale": 0.85
        },
        "PrefabInstance": { "source": "models/antman_merged.obj" }
      }
    },
    {
      "id": "GingySplat",
      "name": "GingySplat",
      "components": {
        "Transform": { "translation": [1.25, 0.0, 0.0] },
        "GaussianSplat": {
          "path": "D:/path/to/scans/splat_crop.ply",
          "convertRdfToRub": true,
          "enabled": true
        }
      }
    },
    { "id": "Lights", "name": "Lights" },
    {
      "id": "Sun",
      "name": "Sun",
      "parent": "Lights",
      "components": {
        "Transform": {
          "rotation": [-0.23053891, -0.15879166, -0.68904659, 0.66846975]
        },
        "DirectionalLight": {
          "angularSize": 1.5,
          "color": [1.0, 0.96, 0.9],
          "irradiance": 4.0
        }
      }
    },
    { "id": "Cameras", "name": "Cameras" },
    {
      "id": "Default",
      "name": "Default",
      "parent": "Cameras",
      "components": {
        "Transform": { "translation": [0.0, 1.6, 6.0], "rotation": [0.0, 0.0, 0.0, 1.0] },
        "PerspectiveCameraEx": {
          "verticalFov": 0.7,
          "zNear": 0.001,
          "enableAutoExposure": false,
          "exposureCompensation": 1.0
        }
      }
    }
  ]
}
```

运行：

```powershell
.\bin\caustica.exe --scene default.scene.json
```

Debug 配置的可执行文件名为 `causticaD.exe`。如果传入的是相对文件名，应用会从运行时资源根的 `Assets/` 中查找。运行时目录与资源根的完整解析规则见 [构建和运行指南](build-and-run.md#runtime-files-and-asset-lookup)。

## 顶层字段

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `entities` | array | 扁平实体列表。层级用 `parent` 指向另一个实体的 `id`。 |
| `settings` | object | 可选。初始渲染设置（原 SceneSettings 节点）。 |
| `animations` | array | 可选。场景动画通道。 |
| `base` / `overrides` | string / array | 可选。在另一个场景上做少量覆盖（例如只改天空）。 |

## 实体字段

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `id` | string | 稳定引用。`parent` 和 overlay 用这个。 |
| `name` | string | 显示名；省略时用 `id`。 |
| `parent` | string | 父实体 `id`。省略则挂到场景根。 |
| `components` | object | 组件字典。键是组件名。 |

### `Transform`

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `translation` | `[x, y, z]` | 本地平移。 |
| `rotation` | `[x, y, z, w]` | 本地四元数 XYZW。 |
| `euler` | `[x, y, z]` | 弧度欧拉角；未写 `rotation` 时读取。 |
| `scale` | number 或 `[x, y, z]` | 本地缩放。 |

### `PrefabInstance`

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `source` | string | pack 相对路径、绝对路径，`builtin:plane` / `cube` / `sphere` / `plane_cube`，或 `prefabs/*.prefab.json`。 |
| `materials` | object | 可选。导入材质名 → pack 相对 `.material.json` / `.mat.json`。 |

相对路径相对资源包根解析（`models/...`），并回退到 scene 文件目录。

`.prefab.json` 与场景相同，是 `entities[]` 文档，可以再嵌套 `PrefabInstance`（网格 glTF 或另一个 prefab）。不要把编辑器 `game/` 下的玩法 JSON 当成引擎 prefab。

```json
{
  "name": "white-cube",
  "entities": [
    {
      "id": "cube",
      "name": "Cube",
      "components": {
        "PrefabInstance": { "source": "builtin:cube" }
      }
    }
  ]
}
```

场景里实例化：

```json
"PrefabInstance": { "source": "prefabs/white-cube.prefab.json" }
```

### `MaterialOverride`

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `source` | string | 该实体上所有材质槽使用的材质资产路径。 |
| `slots` | object | 可选。按导入材质名覆盖。 |

```json
"PrefabInstance": {
  "source": "models/kitchen/kitchen.gltf",
  "materials": {
    "Mushroom_Sliced_MDL": "materials/kitchen.Mushroom_Sliced_MDL.material.json"
  }
}
```

## 组件类型

灯光、相机、3DGS 写在 `components` 下，字段与下文各节相同（不再使用节点上的 `type` / `children` / `model` 下标）。

| 组件 | 含义 |
| --- | --- |
| `DirectionalLight` | 方向光。方向来自同实体 `Transform.rotation`。 |
| `PointLight` | 点光。 |
| `SpotLight` | 聚光灯。 |
| `EnvironmentLight` | 环境光。贴图用 `source`（可用 `procedural:sky`）。 |
| `PerspectiveCamera` / `PerspectiveCameraEx` | 透视相机。 |
| `OrthographicCamera` | 正交相机。 |
| `GaussianSplat` | 3DGS PLY。 |
| `MaterialOverride` | 显式材质资产。 |
| `GameSettings` | 编辑器 `game/` 层原始 JSON。 |

## 灯光参数

所有灯光节点都可以使用通用 Transform 字段。方向光通过节点旋转决定方向；点光和聚光灯通过节点平移决定位置。

### `DirectionalLight`

```json
{
  "name": "Sun",
  "type": "DirectionalLight",
  "rotation": [-0.23053891, -0.15879166, -0.68904659, 0.66846975],
  "color": [1.0, 0.96, 0.9],
  "irradiance": 4.0,
  "angularSize": 1.5
}
```

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `color` | `[r, g, b]` | 线性 RGB 颜色倍率。 |
| `irradiance` | number | 目标照度，乘以 `color`。 |
| `angularSize` | number | 光源角直径，单位为度，会被限制在 0 到 90 度。 |

### `PointLight`

```json
{
  "name": "Fill",
  "type": "PointLight",
  "translation": [0.0, 2.5, 3.0],
  "color": [1.0, 0.95, 0.85],
  "intensity": 30.0,
  "radius": 0.05,
  "range": 10.0
}
```

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `color` | `[r, g, b]` | 线性 RGB 颜色倍率。 |
| `intensity` | number | 发光强度，乘以 `color`。 |
| `radius` | number | 球形光半径，单位为世界单位。 |
| `range` | number | 影响范围。`0` 表示无限范围。 |
| `proxyMeshNodes` | string array | 可选。把网格节点作为 analytic light proxy。 |

### `SpotLight`

```json
{
  "name": "KeySpot",
  "type": "SpotLight",
  "translation": [0.0, 3.0, 2.0],
  "rotation": [0.0, 0.0, 0.0, 1.0],
  "color": [1.0, 0.95, 0.9],
  "intensity": 60.0,
  "innerAngle": 20.0,
  "outerAngle": 40.0,
  "radius": 0.05,
  "range": 8.0
}
```

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `color` | `[r, g, b]` | 线性 RGB 颜色倍率。 |
| `intensity` | number | 主方向发光强度，乘以 `color`。 |
| `innerAngle` | number | 内锥角，单位为度。内锥内为满亮度。 |
| `outerAngle` | number | 外锥角，单位为度。外锥外无光。 |
| `radius` | number | 光源半径。 |
| `range` | number | 影响范围。`0` 表示无限范围。 |
| `proxyMeshNodes` | string array | 可选。把网格节点作为 analytic light proxy。 |

### `EnvironmentLight`

```json
{
  "radianceScale": [1.0, 1.0, 1.0],
  "rotation": 0.0,
  "source": "env/simons_town_rocks_4k_cube_bc6u.dds"
}
```

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `radianceScale` | `[r, g, b]` | 环境光辐射亮度倍率。 |
| `textureIndex` | integer | 环境贴图索引，通常写 `0`。 |
| `rotation` | number | 环境贴图旋转值。 |
| `source` | string | 环境贴图路径，或 `procedural:sky`。 |

## 相机参数

相机节点同样使用通用 Transform 字段。常用类型是 `PerspectiveCameraEx`。

### `PerspectiveCamera` / `PerspectiveCameraEx`

```json
{
  "name": "Default",
  "type": "PerspectiveCameraEx",
  "translation": [0.0, 1.6, 6.0],
  "rotation": [0.0, 0.0, 0.0, 1.0],
  "verticalFov": 0.7,
  "aspectRatio": 1.7777778,
  "zNear": 0.001,
  "zFar": 10000.0,
  "enableAutoExposure": false,
  "exposureCompensation": 1.0,
  "exposureValue": 0.0,
  "exposureValueMin": -4.0,
  "exposureValueMax": 5.0
}
```

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `verticalFov` | number | 垂直视场角，单位为弧度。 |
| `aspectRatio` | number | 宽高比。可省略，由窗口/渲染目标决定。 |
| `zNear` | number | 近裁剪面。 |
| `zFar` | number | 远裁剪面。 |
| `enableAutoExposure` | bool | `PerspectiveCameraEx` 扩展字段。是否启用自动曝光。 |
| `exposureCompensation` | number | `PerspectiveCameraEx` 扩展字段。曝光补偿。 |
| `exposureValue` | number | `PerspectiveCameraEx` 扩展字段。固定曝光值。 |
| `exposureValueMin` | number | `PerspectiveCameraEx` 扩展字段。自动曝光最小值。 |
| `exposureValueMax` | number | `PerspectiveCameraEx` 扩展字段。自动曝光最大值。 |

### `OrthographicCamera`

```json
{
  "name": "Ortho",
  "type": "OrthographicCamera",
  "translation": [0.0, 5.0, 5.0],
  "rotation": [0.0, 0.0, 0.0, 1.0],
  "xMag": 4.0,
  "yMag": 3.0,
  "zNear": 0.001,
  "zFar": 10000.0
}
```

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `xMag` | number | 正交相机横向视域大小。 |
| `yMag` | number | 正交相机纵向视域大小。 |
| `zNear` | number | 近裁剪面。 |
| `zFar` | number | 远裁剪面。 |

## 3DGS 节点

3DGS 节点是普通 scene graph 节点，因此使用 `translation`、`rotation`/`euler`、`scaling` 控制摆放。

```json
{
  "name": "ScanA",
  "type": "GaussianSplat",
  "path": "D:/path/to/scans/splats_a.ply",
  "translation": [0.0, 0.0, 0.0],
  "scaling": 1.0,
  "convertRdfToRub": true,
  "enabled": true
}
```

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `path` | string | 3DGS `.ply` 文件路径。 |
| `file` | string | `path` 的别名。 |
| `fileName` | string | `path` 的别名。 |
| `convertRdfToRub` | bool | 是否把原始 3DGS right/down/front 坐标转换到 Caustica 的 right/up/back 坐标约定。默认 `true`。 |
| `enabled` | bool | 是否启用该 splat 节点。默认 `true`。 |

路径规则：

- 绝对路径可以直接写。
- 相对路径会相对于 scene JSON 文件所在目录解析。

注意：

- `gaussian_splat_scale`、`gaussian_splat_alpha_scale`、`gaussian_splat_brightness` 等会话级外观现在可以写入顶层 `settings.gaussianSplat`，Save Scene 会把 Inspector 里的 Footprint / Alpha / Brightness 等存回去。
- 节点 Transform 控制对象整体位置、旋转、缩放。
- 当前 RTX/path-tracing splat shadow 资源槽仍以第一个启用的 3DGS 对象为主要 shadow source。

## `settings`

顶层 `settings` 用来初始化渲染状态。`startingCamera` 可以是相机实体的 `id`。

```json
{
  "settings": {
    "realtimeMode": true,
    "enableAnimations": false,
    "enableKeyframes": false,
    "realtimeFireflyFilter": 0.15,
    "maxBounces": 8,
    "maxDiffuseBounces": 4,
    "textureMIPBias": 0.0,
    "environment": {
      "tintColor": [1.0, 1.0, 1.0],
      "intensity": 1.0,
      "rotation": [0.0, 0.0, 0.0],
      "visibleToCamera": true,
      "enabled": true,
      "override": "==SCENE_DEFAULT=="
    },
    "gaussianSplat": {
      "footprintScale": 1.0,
      "alphaScale": 1.0,
      "brightness": 1.0
    }
  }
}
```

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `realtimeMode` | bool | 初始是否进入实时模式。 |
| `enableAnimations` | bool | 初始是否启用导入/骨骼动画（非 editorAuthored）。 |
| `enableKeyframes` | bool | 初始是否启用编辑器关键帧时间线播放（editorAuthored）。默认 false。 |
| `startingCamera` | integer | 初始相机索引。`-1` 表示 free flight camera；`0` 表示第一个 scene camera。 |
| `realtimeFireflyFilter` | number | 设置实时 firefly filter 阈值，并启用该 filter。 |
| `maxBounces` | integer | 最大反弹次数。 |
| `maxDiffuseBounces` | integer | 最大 diffuse 反弹次数。 |
| `textureMIPBias` | number | 纹理 MIP bias。 |
| `environment` | object | 可选。Inspector 环境光 Tint / Intensity / Rotation / Visible to Camera / Enabled / Override。缺少该键时不改会话环境参数。 |
| `gaussianSplat` | object | 可选。Inspector 3DGS Footprint / Alpha / Brightness 等会话外观。缺少该键时不改。 |
| `hiddenEntities` | string[] | 可选。要隐藏的 mesh / 3DGS / light 的场景路径。只关掉列出的实体，不会隐藏其余物体。 |

Save Scene 还会把已有实体上的灯光 Color / Intensity / Irradiance / Radius / Angle、相机 FOV / Near / Far、3DGS `enabled` 写回对应组件。Prefab 内部的灯光/相机写在顶层 `entityOverrides`（按 `path` 匹配，路径找不到就跳过）。

## `GameSettings`

`GameSettings` 会保存整段 JSON 给编辑器 `game/` 层使用。核心 scene loader 不解析其内部字段。

```json
{
  "name": "GameSettings",
  "type": "GameSettings",
  "someGameField": 1
}
```

## 动画 `animations`

`animations` 是顶层数组。每个动画包含若干 channel。

```json
{
  "animations": [
    {
      "name": "MoveAntman",
      "channels": [
        {
          "target": "/Antman",
          "attribute": "translation",
          "mode": "linear",
          "data": [
            { "time": 0.0, "value": [0.0, 0.0, 0.0] },
            { "time": 1.0, "value": [1.0, 0.0, 0.0] }
          ]
        }
      ]
    }
  ]
}
```

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `name` | string | 动画名。 |
| `channels` | array | 动画通道。 |

Channel 字段：

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `target` | string | 目标节点路径，或 `material:<MaterialName>`。 |
| `targets` | string array | 多个目标。没有 `target` 时使用。 |
| `attribute` | string | `translation`、`rotation`、`scaling`，或 leaf/material 属性名。 |
| `mode` | string | 插值模式：`step`、`linear`、`slerp`、`hermite`、`catmull-rom`。 |
| `data` | array | keyframe 数组。 |

Keyframe 字段：

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `time` | number | 关键帧时间。 |
| `value` | number 或最多 4 维数组 | 属性值。 |
| `inTangent` | number 或最多 4 维数组 | Hermite 等模式使用的入切线。 |
| `outTangent` | number 或最多 4 维数组 | Hermite 等模式使用的出切线。 |

说明：

- `translation` 和 `scaling` 使用 `value` 的 XYZ。
- `rotation` 使用四元数 XYZW，通常配合 `slerp`。
- 目标路径建议写从根节点开始的路径，例如 `/Antman`、`/Lights/Sun`。

## 材质覆盖

材质文件在 `materials/`，扩展名 `.material.json` 或 `.mat.json`。场景里用 `PrefabInstance.materials` 或 `MaterialOverride` **显式引用** pack 相对路径。

按「模型名.材质名」扫目录的旧查找仍然能加载未迁移文件，但已经 deprecated。

示例：

```text
materials/kitchen.Mushroom_Sliced_MDL.material.json
```

对应 `models/kitchen/kitchen.gltf` 中名为 `Mushroom_Sliced_MDL` 的导入材质。

### 材质 JSON 示例

```json
{
  "version": 1,
  "BaseOrDiffuseColor": [1.0, 1.0, 1.0],
  "SpecularColor": [0.0, 0.0, 0.0],
  "EmissiveColor": [0.0, 0.0, 0.0],
  "EmissiveIntensity": 1.0,
  "Metalness": 0.0,
  "Roughness": 0.55,
  "Opacity": 1.0,
  "TransmissionFactor": 0.0,
  "DiffuseTransmissionFactor": 0.0,
  "NormalTextureScale": 1.0,
  "IoR": 1.5,
  "UseSpecularGlossModel": false,
  "EnableBaseTexture": true,
  "EnableOcclusionRoughnessMetallicTexture": true,
  "EnableNormalTexture": true,
  "EnableEmissiveTexture": true,
  "EnableTransmissionTexture": true,
  "EnableAlphaTesting": false,
  "AlphaCutoff": 0.5,
  "EnableTransmission": false,
  "MetalnessInRedChannel": false,
  "ThinSurface": true,
  "ExcludeFromNEE": false,
  "PSDExclude": false,
  "PSDDominantDeltaLobe": -1,
  "NestedPriority": 0,
  "VolumeAttenuationDistance": 3.4028234663852886e+38,
  "VolumeAttenuationColor": [1.0, 1.0, 1.0],
  "ShadowNoLFadeout": 0.0,
  "EnableAsAnalyticLightProxy": false,
  "IgnoreMeshTangentSpace": false,
  "UseEngineEmissiveIntensity": false,
  "SkipRender": false
}
```

### 材质字段

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `BaseTexture` | object | Base/diffuse 贴图。 |
| `OcclusionRoughnessMetallicTexture` | object | ORM 或 spec-gloss 贴图。 |
| `NormalTexture` | object | 法线贴图。 |
| `EmissiveTexture` | object | 自发光贴图。 |
| `TransmissionTexture` | object | 透射贴图。 |
| `BaseOrDiffuseColor` | `[r, g, b]` | 金属粗糙模型的 base color，或 spec-gloss 模型的 diffuse color。 |
| `SpecularColor` | `[r, g, b]` | spec-gloss 模型的 specular color。 |
| `EmissiveColor` | `[r, g, b]` | 自发光颜色。 |
| `EmissiveIntensity` | number | 自发光强度倍率。 |
| `Metalness` | number | 金属度。 |
| `Roughness` | number | 粗糙度。 |
| `Opacity` | number | 不透明度。 |
| `TransmissionFactor` | number | specular transmission factor。 |
| `DiffuseTransmissionFactor` | number | diffuse transmission factor。 |
| `NormalTextureScale` | number | 法线贴图强度。 |
| `IoR` | number | 折射率。 |
| `UseSpecularGlossModel` | bool | 是否使用 specular-glossiness 模型。 |
| `EnableBaseTexture` | bool | 是否启用 base/diffuse 贴图。 |
| `EnableOcclusionRoughnessMetallicTexture` | bool | 是否启用 ORM/spec-gloss 贴图。 |
| `EnableNormalTexture` | bool | 是否启用法线贴图。 |
| `EnableEmissiveTexture` | bool | 是否启用自发光贴图。 |
| `EnableTransmissionTexture` | bool | 是否启用透射贴图。 |
| `EnableAlphaTesting` | bool | 是否启用 alpha test。 |
| `AlphaCutoff` | number | alpha test 阈值。 |
| `EnableTransmission` | bool | 是否启用透射材质逻辑。 |
| `MetalnessInRedChannel` | bool | 金属度是否存储在红色通道。 |
| `ThinSurface` | bool | 是否作为 thin surface 处理。 |
| `ExcludeFromNEE` | bool | 是否从 NEE 中排除。 |
| `PSDExclude` | bool | 是否从 path space decomposition 中排除。 |
| `PSDDominantDeltaLobe` | integer | PSD dominant delta lobe，`-1` 表示无 dominant。 |
| `PSDBlockMotionVectorsAtSurfaceType` | integer | 曲面/复杂表面的 motion vector 阻断模式。 |
| `NestedPriority` | integer | 嵌套介质优先级，最大值由材质代码限制。 |
| `VolumeAttenuationDistance` | number | 体积吸收距离。 |
| `VolumeAttenuationColor` | `[r, g, b]` | 体积吸收颜色。 |
| `ShadowNoLFadeout` | number | 低细分阴影/法线不一致缓解参数。 |
| `EnableAsAnalyticLightProxy` | bool | 是否把该材质几何作为 analytic light proxy。 |
| `IgnoreMeshTangentSpace` | bool | 是否忽略 mesh tangent space。 |
| `UseEngineEmissiveIntensity` | bool | 是否使用引擎材质的 emissive intensity，便于动画驱动。 |
| `SkipRender` | bool | 是否跳过该材质几何的渲染。 |

贴图字段格式：

```json
{
  "BaseTexture": {
    "path": "Textures/albedo.dds",
    "sRGB": true,
    "NormalMap": false
  }
}
```

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `path` | string | 贴图路径。解析顺序：绝对路径 → 运行时 `Assets/` → scene 文件所在目录。glTF 内贴图 URI 另会先相对 glTF 文件目录，再回退到 scene 目录。 |
| `sRGB` | bool | 是否按 sRGB 读取。 |
| `NormalMap` | bool | 是否是法线贴图。 |

如果场景没有写 `PrefabInstance.materials` / `MaterialOverride`，引擎会回退到按「模型名.材质名」扫 `materials/` 的旧查找（deprecated），再不行就用 glTF/OBJ 导入的默认 StandardMaterial。

注意：一旦写了 `.material.json` 覆盖文件，引擎会用这个 StandardMaterial 文件替代导入材质，而不是只覆盖其中几个字段。因此，如果原模型依赖 base color、normal、emissive 等贴图，覆盖文件里也要显式写对应的 `BaseTexture`、`NormalTexture`、`EmissiveTexture` 等字段；否则贴图会丢失，只剩颜色常量。

## 常见注意事项

- JSON 文件不能写注释。
- scene JSON 中的 Transform 是节点 Transform；材质参数走 `.material.json`。
- `PrefabInstance.source` 支持 `.gltf`、`.glb`、`.obj`、`.urdf`、可选 OpenUSD，以及 `prefabs/*.prefab.json` 和 `builtin:*`。新增网格格式应扩展 importer，而不是把所有格式强制转成 glTF。
- `rotation` 是四元数 XYZW；`verticalFov` 和 `euler` 是弧度；灯光的 `angularSize`、`innerAngle`、`outerAngle` 是度。
- 3DGS 的外观、排序、阴影等渲染选项目前是全局设置，不是每个 3DGS 节点的独立 scene JSON 字段。
- scene-specific material 目录名来自文件 stem。`foo.scene.json` 对应 `Assets/materials/foo.scene/`，不是 `foo/`。
