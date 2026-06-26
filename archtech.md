# caustica Code Architecture Notes

本文档记录当前代码结构和阅读入口，目标是让后续修改时能快速定位“入口在哪里、每帧怎么走、各目录负责什么”。内容基于当前工作区代码整理。

## Top-Level Layout

| Path | Role |
| --- | --- |
| `CMakeLists.txt` | 顶层 CMake 工程入口，工程名为 `RTXPathTracing`。 |
| `caustica/` | 主程序、渲染框架、处理 pass、shader、Python 绑定都在这里。 |
| `Assets/` | 示例场景、模型、字体、环境贴图等运行时资源。 |
| `External/` | 第三方依赖，包括 NVRHI、NRD、RTXDI、OMM、Streamline、DXC 等。 |
| `Docs/` | 项目文档。 |
| `Support/` | 辅助工具和运行支持文件。 |
| `build/` | CMake/MSBuild 生成目录。 |
| `bin/` | 构建输出目录，Release 主程序输出为 `bin/caustica.exe`。 |

## Build Structure

主要构建逻辑在 `caustica/CMakeLists.txt`：

| Target | Source / Role |
| --- | --- |
| `caustica_shaders` | 由 `caustica/shaders.cfg` 驱动 ShaderMake 编译 HLSL。 |
| `ShaderDynamicAssets` | 复制动态 shader 编译所需工具。 |
| `ShaderDynamicAssets_CopyAlways` | 每次构建同步 shader 源到运行目录。 |
| `causticaApp` | 静态库，编辑器与 Python 应用层（原 `causticaCore`）。 |
| `caustica` | Windows GUI 可执行程序，入口是 `caustica/AdvancedSample.cpp`。 |
| `caustica_py` | 可选 Python extension module，启用 `caustica_WITH_PYTHON` 时构建。 |

常用构建命令：

```powershell
cmake --build build --config Release --target caustica
cmake --build build --config Release --target causticaApp
```

Shader 编译清单在 `caustica/shaders.cfg`。新增 shader entry point 或 macro variant 时，一般需要同步更新这个文件。

## Main Runtime Flow

当前启用的桌面程序入口是 `caustica/AdvancedSample.cpp`：

1. `WinMain` 创建 `SplashScreen`。
2. 创建 `AdvancedSample`，它继承自 `SampleBaseApp`。
3. `AdvancedSample::CreateMainRenderPass()` 创建 `AdvancedPathTracer`。
4. `SampleBaseApp::Init()` 完成设备、窗口、命令行、scene 初始化。
5. `RunMainLoop()` 进入每帧渲染。

核心渲染基类是 `caustica/Sample.h` / `caustica/Sample.cpp` 中的 `Sample`。`AdvancedPathTracer` 在 `caustica/AdvancedSample.h` 中继承 `Sample`，实现高级路径追踪 renderer：

```text
AdvancedSample.cpp
  -> SampleBaseApp
    -> AdvancedPathTracer : Sample
      -> Sample::Render()
        -> AdvancedPathTracer::SampleRenderCode()
```

`Sample::Render()` 的主干大致如下：

1. 更新 render/display 尺寸和 render targets。
2. `PreRender()`、`StreamlinePreRender()` 做帧前状态准备。
3. 更新 camera/view、jitter、path tracer constants。
4. 填充 `SampleConstants`，其中包含当前 view、previous view、lighting、path tracer、debug 参数。
5. `UpdateLighting()` 更新环境光、方向光、本地光、emissive triangle lights。
6. `UploadSubInstanceData()` 上传 instance / sub-instance 数据。
7. 写入全局 constant buffer。
8. 调用虚函数 `SampleRenderCode()` 执行具体 renderer。
9. `RenderGaussianSplats()` 叠加 3D Gaussian Splats。
10. `PostProcessAA()`、OIDN / NRD / Streamline 等后处理和 denoise。

## Core C++ Entry Points

| File | Role |
| --- | --- |
| `caustica/AdvancedSample.cpp` | 当前 `caustica.exe` 入口，创建 `AdvancedPathTracer`。 |
| `caustica/AdvancedSample.h` | 高级路径追踪 renderer，负责 PathTrace + Denoise 和 RT pipeline variants。 |
| `caustica/IntroSample.cpp/.h` | 简化/实验 renderer，包含 raster precompute、SSR、local cubemap、GTAO 等。当前 CMake 中 `IntroSample` target 被注释。 |
| `caustica/Sample.h/.cpp` | 主应用和渲染框架基类，管理 scene、camera、render targets、lighting、accel structures、UI 状态、Python host、帧循环。 |
| `caustica/SampleUI.h/.cpp` | ImGui UI 和 `SampleUIData`，大多数 runtime 开关从这里进入。 |
| `caustica/shaders.cfg` | shader variant 编译入口列表。 |

## caustica Directory Map

| Directory | Main Responsibility |
| --- | --- |
| `caustica/SampleCommon/` | 应用公共基础设施：命令行、render targets、scene 扩展、pipeline baker、capture script、shader 编译辅助、base app。 |
| `caustica/ProcessingPasses/` | 独立图像/compute/raster pass：accumulation、post process、denoising guides、OIDN、3DGS raster pass。 |
| `caustica/Shaders/` | HLSL 主体：path tracer、scene/material 类型、lighting、debug、binding 定义、常量结构。 |
| `caustica/Lighting/` | CPU/GPU lighting baking：环境贴图、procedural sky、本地光、emissive triangle lights、light proxy/sampling 数据。 |
| `caustica/RTXDI/` | RTXDI integration 和 resampling/final shading shaders。 |
| `caustica/NRD/` | NRD denoiser integration。 |
| `caustica/OpacityMicroMap/` | OMM baking/integration。 |
| `caustica/Materials/` | material baking 和 material 数据准备。 |
| `caustica/GPUSort/` | GPU radix sort wrapper 和 HLSL kernels。当前 3DGS 排序复用这里。 |
| `caustica/Python/` | embedded Python scripting、Python extension binding、offline/interactive 示例脚本。 |
| `caustica/SampleGame/` | 示例 game/scene 交互层和 scene object 逻辑。 |
| `caustica/ToneMapper/` | tone mapping、luminance、white balance、capture pass。 |
| `caustica/Misc/` | debug lines、shader debug、zoom tool 等辅助功能。 |

## Path Tracing / Rendering Modules

### Path Tracer

主要入口：

| File / Directory | Notes |
| --- | --- |
| `caustica/AdvancedSample.h` | 创建 path tracing pipeline variants，如 reference、stable planes build/fill、test raygen。 |
| `caustica/Sample.cpp` | `PathTrace()`、`Denoise()`、accel structure、lighting 和 frame constants 管理。 |
| `caustica/SampleCommon/PTPipelineBaker.*` | RT pipeline variant 创建、热更新和 shader specialization。 |
| `caustica/Shaders/PathTracer/` | path tracer HLSL 核心逻辑。 |
| `caustica/Shaders/SampleConstantBuffer.h` | C++/HLSL 共用常量结构，包括 `SampleConstants`、`GaussianSplatConstants`、`GaussianSplatData`。 |

### Lighting

`Sample::Render()` 中调用 `UpdateLighting()`，其背后主要由：

| File / Directory | Notes |
| --- | --- |
| `caustica/Lighting/LightsBaker.*` | 本地光、emissive triangles、light proxy 和 feedback history。 |
| `caustica/Lighting/Distant/EnvMapBaker.*` | 环境贴图预处理、mip、BC6U、BRDF LUT、importance sampling。 |
| `caustica/RTXDI/` | RTXDI pass、application bridge、DI/GI resampling shaders。 |

### Post Processing / Denoising

| File / Directory | Notes |
| --- | --- |
| `caustica/ProcessingPasses/PostProcess.*` | denoiser input prepare、final merge、debug viz、DLSS RR 相关 pass。 |
| `caustica/ProcessingPasses/AccumulationPass.*` | accumulation / temporal accumulation。 |
| `caustica/ProcessingPasses/DenoisingGuidesBaker.*` | denoising guide buffer 生成和 debug。 |
| `caustica/NRD/NrdIntegration.*` | NRD integration。 |
| `caustica/ProcessingPasses/OidnDenoiser.*` | OIDN denoiser path。 |

## 3D Gaussian Splats Path

3DGS 代码集中在：

| File | Role |
| --- | --- |
| `caustica/Sample.cpp` | `LoadGaussianSplatFile()` 创建/绑定 `GaussianSplatPass` 和 `GPUSort`；`RenderGaussianSplats()` 每帧调用 pass。 |
| `caustica/ProcessingPasses/GaussianSplatPass.h/.cpp` | PLY 读取、GPU buffer 创建、sort key generation、GPU sort、raster draw。 |
| `caustica/ProcessingPasses/GaussianSplatRaster.hlsl` | compute 生成 sort keys；vertex/pixel rasterize splats。 |
| `caustica/GPUSort/GPUSort.*` | 通用 GPU radix sort。 |
| `caustica/Shaders/SampleConstantBuffer.h` | `GaussianSplatConstants` 和 `GaussianSplatData`。 |

数据流：

```text
CommandLine / Python / UI
  -> Sample::LoadGaussianSplatFile()
    -> GaussianSplatPass::LoadFromFile()
      -> Load PLY vertices
      -> convert to GaussianSplatData + SH coefficients
      -> create m_splatBuffer / m_shBuffer / m_indexBuffer / m_sortKeyBuffer

Frame render
  -> Sample::RenderGaussianSplats()
    -> GaussianSplatPass::Render()
      -> upload splat data if needed
      -> write GaussianSplatConstants
      -> SortSplats()
      -> draw splat quads using sorted indices
```

当前排序优化：

- `GaussianSplatPass` 缓存上一帧排序状态。
- 缓存 key 为 `matWorldToClipNoOffset` 和 `m_splatCount`。
- 当相机 no-jitter view-projection 和 splat 数量不变时，直接复用上一帧 `m_indexBuffer`。
- `GaussianSplatRaster.hlsl::cs_sort_keys` 使用 `matWorldToClipNoOffset` 计算 depth key，避免 TAA sub-pixel jitter 让排序缓存每帧失效。
- 当前 3DGS 没有独立 object transform，PLY 中心点直接作为 world space 使用；如果后续加入 splat object transform，需要把该 transform 也加入排序缓存 key。

## Python Integration

Python 相关代码在 `caustica/Python/`：

| File | Role |
| --- | --- |
| `PythonBindingsCore.*` | C++ 类型和 API 的共享 binding。 |
| `PythonBindings_Embed.cpp` | 嵌入到 `caustica.exe` 的 module entry。 |
| `PythonBindings_Extension.cpp` | standalone `caustica_py.pyd` 的 module entry。 |
| `PythonScripting.*` | 嵌入式脚本 host，按需初始化 interpreter。 |
| `RenderSession.*` | 离线/脚本渲染 session 封装。 |
| `Python/Examples/*.py` | offline 和 interactive 示例。 |

启用 Python 时，CMake 会把共享 binding 放进 `causticaApp`，再分别为 exe embed mode 和 extension mode 添加不同的 `NB_MODULE` entry。

## Where To Start For Common Changes

| Task | Start Here |
| --- | --- |
| 修改主渲染帧流程 | `caustica/Sample.cpp` 的 `Sample::Render()`。 |
| 修改高级 path tracer | `caustica/AdvancedSample.h` 和 `caustica/Shaders/PathTracer/`。 |
| 新增/修改 shader variant | `caustica/shaders.cfg`、对应 `.hlsl/.hlsli`、pipeline baker 调用点。 |
| 修改 UI 开关 | `caustica/SampleUI.h/.cpp` 和 `SampleUIData`。 |
| 修改命令行参数 | `caustica/SampleCommon/CommandLine.h/.cpp`。 |
| 修改 render target 格式或尺寸依赖资源 | `caustica/SampleCommon/RenderTargets.*` 和 `Sample::BackBufferResizing()` / `CreateRenderPasses()`。 |
| 修改 3DGS 加载/排序/绘制 | `caustica/ProcessingPasses/GaussianSplatPass.*`、`GaussianSplatRaster.hlsl`、`GPUSort/`。 |
| 修改 lighting baking | `caustica/Lighting/` 和 `caustica/RTXDI/`。 |
| 修改 denoise/postprocess | `caustica/ProcessingPasses/`、`caustica/NRD/`、`caustica/ToneMapper/`。 |
| 修改 Python API | `caustica/Python/PythonBindingsCore.cpp` 和 extension/embed entry。 |

## Useful Mental Model

把当前工程理解成三层会比较顺：

1. **Application layer**：`SampleBaseApp`、`Sample`、`SampleUI`、command line、scene loading、camera/input。
2. **Render orchestration layer**：render targets、pipeline bakers、lighting update、accel structures、path tracing dispatch、postprocess、3DGS overlay。
3. **Shader/data layer**：`SampleConstantBuffer.h` 中的共享结构、`Shaders/PathTracer`、`Lighting`/`RTXDI` HLSL、`ProcessingPasses` HLSL、GPU buffers/bindings。

多数功能修改会跨第 2 层和第 3 层：C++ 负责资源、binding、dispatch 顺序，HLSL 负责实际计算。

