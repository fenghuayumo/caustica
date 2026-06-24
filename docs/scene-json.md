# RTXPT Scene JSON 格式说明

本文档说明 RTXPT 当前支持的场景 JSON 描述方式。场景 JSON 负责描述“加载哪些模型”和“这些对象如何挂到场景图里”；材质参数通常不直接写在 scene JSON 节点中，而是通过 `Assets/Materials` 下的 `.material.json` 文件覆盖。

相关实现主要在：

- `External/caustica/src/engine/Scene.cpp`
- `External/caustica/src/engine/SceneGraph.cpp`
- `External/caustica/src/engine/SceneTypes.cpp`
- `Rtxpt/SampleCommon/ExtendedScene.cpp`
- `Rtxpt/Materials/MaterialsBaker.cpp`

## 最小示例

```json
{
  "models": [
    "builtin:plane",
    "D:/ScanVideo/models/antman_merged.obj"
  ],
  "graph": [
    {
      "name": "GroundPlane",
      "model": 0,
      "translation": [0.0, 0.0, 0.0],
      "rotation": [0.0, 0.0, 0.0, 1.0],
      "scaling": [2.0, 1.0, 2.0]
    },
    {
      "name": "Antman",
      "model": 1,
      "translation": [-1.25, 0.0, 0.0],
      "euler": [0.0, 0.0, 0.0],
      "scaling": 0.85
    },
    {
      "name": "GingySplat",
      "type": "GaussianSplat",
      "path": "D:/ScanVideo/Gingy/splat_crop.ply",
      "translation": [1.25, 0.0, 0.0],
      "rotation": [0.0, 0.0, 0.0, 1.0],
      "scaling": [1.0, 1.0, 1.0],
      "convertRdfToDonut": true,
      "enabled": true
    },
    {
      "name": "Lights",
      "children": [
        {
          "name": "Sun",
          "type": "DirectionalLight",
          "rotation": [-0.23053891, -0.15879166, -0.68904659, 0.66846975],
          "angularSize": 1.5,
          "color": [1.0, 0.96, 0.9],
          "irradiance": 4.0
        }
      ]
    },
    {
      "name": "Cameras",
      "children": [
        {
          "name": "Default",
          "type": "PerspectiveCameraEx",
          "translation": [0.0, 1.6, 6.0],
          "rotation": [0.0, 0.0, 0.0, 1.0],
          "verticalFov": 0.7,
          "zNear": 0.001,
          "enableAutoExposure": false,
          "exposureCompensation": 1.0
        }
      ]
    },
    {
      "name": "SampleSettings",
      "type": "SampleSettings",
      "realtimeMode": true,
      "enableAnimations": false,
      "startingCamera": -1
    }
  ]
}
```

运行：

```powershell
.\bin\Rtxpt.exe --scene default.json
```

如果传入的是相对文件名，应用会优先从 `Assets/` 中查找。UI 的场景列表会扫描 `Assets/` 根目录下的 `.json` 和 `.scene.json` 文件。

## 顶层字段

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `models` | array | 模型资源列表。`graph` 节点用整数 `model` 引用这里的索引。 |
| `graph` | array | 场景图根节点列表。模型、灯光、相机、3DGS、设置节点都写在这里。 |
| `animations` | array | 可选。场景图动画通道列表。 |

未知顶层字段目前不会参与标准场景加载。

## `models`

`models` 数组的每个元素可以是：

```json
"Models/kitchen/kitchen.gltf"
```

```json
"D:/ScanVideo/models/antman_merged.obj"
```

```json
"builtin:plane"
```

```json
{ "builtin": "cube" }
```

支持的 builtin 模型：

| 名称 | 含义 |
| --- | --- |
| `builtin:plane` | 一个内置地面平面。 |
| `builtin:cube` | 一个内置立方体。 |
| `builtin:sphere` | 一个内置球体。 |
| `builtin:plane_cube` | 一个平面加一个立方体。 |
| `builtin:default` | `plane_cube` 的别名。 |
| `builtin:default_scene` | `plane_cube` 的别名。 |

路径规则：

- scene 文件中的相对模型路径相对于该 scene JSON 文件所在目录解析。
- 放在 `Assets/` 下的 scene 通常写 `Models/...`。
- 绝对路径也可以使用，建议使用 `/`，例如 `D:/ScanVideo/models/foo.glb`。
- 静态 `models` 加载路径支持 `.gltf`、`.glb` 和 `.obj`。`.gltf/.glb` 使用 Donut glTF importer，`.obj` 使用 RTXPT OBJ importer。
- 如果仍然把 OBJ 转成 GLB，要按 OBJ 的 `(position, texcoord, normal)` 三元组生成顶点，不能只按 position 合并顶点；否则 UV seam 会被破坏，表现为贴图已加载但 atlas 块贴错。OBJ 的 `vt.y` 通常还需要转换为 `1 - v`，与 RTXPT runtime OBJ importer 的行为保持一致。

## `graph` 节点通用字段

每个 `graph` 元素都是一个场景图节点。节点可以只是分组，也可以引用模型，或者通过 `type` 创建一个灯光、相机、3DGS 等 leaf。

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `name` | string | 节点名。用于 UI、查找节点、动画 target、材质/灯光代理引用。 |
| `model` | integer | 引用 `models` 数组中的模型索引。设置后该节点会使用对应模型的根节点。 |
| `type` | string | 创建 leaf 对象，如 `DirectionalLight`、`PerspectiveCameraEx`、`GaussianSplat`。 |
| `parent` | string | 可选。把节点挂到已有节点下。建议使用从场景根开始的绝对路径，如 `/Lights`。 |
| `children` | array | 子节点数组。 |
| `translation` | number 或 `[x, y, z]` | 本地平移，单位为场景世界单位。 |
| `rotation` | number 或 `[x, y, z, w]` | 本地旋转四元数，顺序为 XYZW。常用单位四元数 `[0, 0, 0, 1]`。 |
| `euler` | number 或 `[x, y, z]` | 本地欧拉角旋转，单位为弧度。只有未设置 `rotation` 时才会读取。 |
| `scaling` | number 或 `[x, y, z]` | 本地缩放。单个数字会扩展到三个轴。 |

说明：

- `translation`、`euler`、`scaling` 都支持单个数字，读取为所有分量相同；实际使用中更推荐写数组，意图更清楚。
- `rotation` 是四元数 `[x, y, z, w]`，不是角度数组。
- `euler` 使用弧度，不是度。90 度应写约 `1.5707963`。
- 如果一个节点只有 `name` 和 `children`，它就是普通分组节点。

## Leaf 类型

`type` 字段会创建具体 leaf。当前 RTXPT 扩展和 Donut 基础类型支持如下。

| `type` | 含义 |
| --- | --- |
| `DirectionalLight` | 方向光。 |
| `PointLight` | 点光。RTXPT 扩展支持 `proxyMeshNodes`。 |
| `SpotLight` | 聚光灯。RTXPT 扩展支持 `proxyMeshNodes`。 |
| `EnvironmentLight` | 环境贴图光，RTXPT 扩展类型。 |
| `PerspectiveCamera` | 透视相机。 |
| `PerspectiveCameraEx` | RTXPT 扩展透视相机，支持曝光参数。推荐使用这个。 |
| `OrthographicCamera` | 正交相机。 |
| `GaussianSplat` | 3D Gaussian Splat PLY 节点。 |
| `GaussianSplats` | `GaussianSplat` 的别名。 |
| `3DGaussianSplat` | `GaussianSplat` 的别名。 |
| `SampleSettings` | 初始渲染设置节点。 |
| `GameSettings` | SampleGame 使用的设置节点，会保存原始 JSON。 |

`MaterialPatch` 是旧格式，当前已经不再支持，不要在新 scene 中使用。

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
  "name": "Sky",
  "type": "EnvironmentLight",
  "radianceScale": [1.0, 1.0, 1.0],
  "textureIndex": 0,
  "rotation": 0.0,
  "path": "EnvironmentMaps/simons_town_rocks_4k_cube_bc6u.dds"
}
```

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `radianceScale` | `[r, g, b]` | 环境光辐射亮度倍率。 |
| `textureIndex` | integer | 环境贴图索引，通常写 `0`。 |
| `rotation` | number | 环境贴图旋转值。 |
| `path` | string | 环境贴图路径。解析顺序：绝对路径 → 运行时 `Assets/` → scene 文件所在目录。UI 环境贴图列表同样先扫 `Assets/EnvironmentMaps/`，再扫 `<scene-dir>/EnvironmentMaps/`。 |

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
  "path": "D:/ScanVideo/chuan/splats_a.ply",
  "translation": [0.0, 0.0, 0.0],
  "scaling": 1.0,
  "convertRdfToDonut": true,
  "enabled": true
}
```

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `path` | string | 3DGS `.ply` 文件路径。 |
| `file` | string | `path` 的别名。 |
| `fileName` | string | `path` 的别名。 |
| `convertRdfToDonut` | bool | 是否把原始 3DGS right/down/front 坐标转换到 RTXPT/Donut 坐标约定。默认 `true`。 |
| `enabled` | bool | 是否启用该 splat 节点。默认 `true`。 |

路径规则：

- 绝对路径可以直接写。
- 相对路径会相对于 scene JSON 文件所在目录解析。

注意：

- `gaussian_splat_scale`、`gaussian_splat_alpha_scale`、`gaussian_splat_brightness`、排序模式、阴影模式等是当前渲染设置，不是每个 scene JSON 节点独立字段。请通过 UI、CLI 或 Python settings 设置。
- 节点 Transform 控制对象整体位置、旋转、缩放。
- 当前 RTX/path-tracing splat shadow 资源槽仍以第一个启用的 3DGS 对象为主要 shadow source。

## `SampleSettings`

`SampleSettings` 是 RTXPT 用来初始化 UI/渲染状态的 scene 节点。

```json
{
  "name": "SampleSettings",
  "type": "SampleSettings",
  "realtimeMode": true,
  "enableAnimations": false,
  "startingCamera": -1,
  "realtimeFireflyFilter": 0.15,
  "maxBounces": 8,
  "maxDiffuseBounces": 4,
  "textureMIPBias": 0.0
}
```

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `realtimeMode` | bool | 初始是否进入实时模式。 |
| `enableAnimations` | bool | 初始是否启用动画。 |
| `startingCamera` | integer | 初始相机索引。`-1` 表示 free flight camera；`0` 表示第一个 scene camera。 |
| `realtimeFireflyFilter` | number | 设置实时 firefly filter 阈值，并启用该 filter。 |
| `maxBounces` | integer | 最大反弹次数。 |
| `maxDiffuseBounces` | integer | 最大 diffuse 反弹次数。 |
| `textureMIPBias` | number | 纹理 MIP bias。 |

## `GameSettings`

`GameSettings` 会保存整段 JSON 给 SampleGame 层使用。核心 scene loader 不解析其内部字段。

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

scene JSON 本身不推荐直接写材质参数。当前 `MaterialPatch` 已废弃。材质覆盖文件放在 `Assets/Materials`，由 `MaterialsBaker` 按模型名和材质名查找。

查找顺序（先 scene 文件所在目录，再运行时 `Assets` 根）：

1. `<scene-dir>/Materials/<scene-stem>/<model-name>.<material-name>.material.json`
2. `<scene-dir>/Materials/<scene-stem>/<material-name>.material.json`
3. `<scene-dir>/Materials/<model-name>.<material-name>.material.json`
4. `<scene-dir>/Materials/<material-name>.material.json`
5. `<Assets>/Materials/<scene-stem>/<model-name>.<material-name>.material.json`
6. `<Assets>/Materials/<scene-stem>/<material-name>.material.json`
7. `<Assets>/Materials/<model-name>.<material-name>.material.json`
8. `<Assets>/Materials/<material-name>.material.json`

其中 `<scene-dir>` 是 scene JSON 的父目录；`<Assets>` 是运行时资源根（`bin` 旁或 pip 包内）。

`scene-stem` 是 scene 文件名去掉最后一层扩展名：

| scene 文件 | scene-stem |
| --- | --- |
| `default.json` | `default` |
| `bistro-programmer-art.scene.json` | `bistro-programmer-art.scene` |

`model-name` 规则：

- 普通文件模型：模型文件名去掉扩展名，例如 `antman_merged.obj` -> `antman_merged`。
- builtin 模型：`builtin_` 加 builtin 名，例如 `builtin:plane` -> `builtin_plane`。

示例：

```text
Assets/Materials/default/antman_merged.antman_merged_0.material.json
```

对应模型 `antman_merged.obj` 中名为 `antman_merged_0` 的材质。

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
  "UseDonutEmissiveIntensity": false,
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
| `UseDonutEmissiveIntensity` | bool | 是否使用 Donut 材质的 emissive intensity，便于动画驱动。 |
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

如果缺少材质覆盖文件，RTXPT 会从 glTF/GLB 或 runtime OBJ importer 的原始材质导入默认 PTMaterial，并在日志中提示可通过 UI 保存材质文件。

注意：一旦写了 `.material.json` 覆盖文件，RTXPT 会用这个 PTMaterial 文件替代导入材质，而不是只覆盖其中几个字段。因此，如果原模型依赖 base color、normal、emissive 等贴图，覆盖文件里也要显式写对应的 `BaseTexture`、`NormalTexture`、`EmissiveTexture` 等字段；否则贴图会丢失，只剩颜色常量。

## 常见注意事项

- JSON 文件不能写注释。
- scene JSON 中的 Transform 是节点 Transform；材质参数走 `.material.json`。
- scene JSON 静态 `models` 已支持 `.gltf`、`.glb`、`.obj`；新增格式时应扩展模型 importer 分发，而不是把所有格式强制转成 glTF。
- `rotation` 是四元数 XYZW；`verticalFov` 和 `euler` 是弧度；灯光的 `angularSize`、`innerAngle`、`outerAngle` 是度。
- 3DGS 的外观、排序、阴影等渲染选项目前是全局设置，不是每个 3DGS 节点的独立 scene JSON 字段。
- scene-specific material 目录名来自文件 stem。`foo.scene.json` 对应 `Assets/Materials/foo.scene/`，不是 `foo/`。
