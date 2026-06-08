# 3DGS 作为发光体照亮 Mesh 的方案研讨

## 结论

可行，而且当前项目已经有不少可复用基础：3DGS PLY 加载、GPU sort/raster overlay、Gaussian TLAS/BLAS、以及让 Gaussian 作为遮挡体参与 mesh 光线可见性测试。现在缺的是另一半：把 Gaussian 的 radiance 当成可采样的 emitter，让 mesh surface 的 NEE、scatter ray 或 ReSTIR GI 能拿到来自 3DGS 的入射光。

建议不要一开始把每个 splat 都塞进现有 polymorphic light/RTXDI light list。百万级 splat 会让 light baking、代理列表、PDF、ReGIR 都变得很重。更稳的路线是：

1. **MVP：3DGS emitter proxy + ray-traced Gaussian evaluation**
   - CPU/GPU 聚类 splat 为几百到几千个发光 proxy，用 proxy 做重要性采样。
   - 真正的 radiance/透明度仍沿采样方向查询 Gaussian TLAS，得到更可信的命中 radiance。
   - 先接进 path tracer 的 mesh surface NEE，立即得到 3DGS 对 mesh 的近似照明。

2. **第二步：把 Gaussian hit 作为 path tracer 的 emissive hit**
   - 对 secondary/scatter/specular ray 查询 Gaussian TLAS。
   - 若 Gaussian hit 比 mesh hit 更近，则按 emissive surface/volume event 贡献 radiance。
   - ReSTIR GI 当前已经从 path tracer secondary surface/radiance 初始化 GI reservoir，因此这一步能比较自然地把 3DGS emission 带进 ReSTIR GI。

3. **第三步：与 RTXDI/ReGIR 深度集成**
   - 若需要大量动态 proxy 或更强 temporal/spatial reuse，再把 3DGS proxy 变成 RTXDI 可见的 light candidates，或单独做一个 Gaussian emitter reservoir。

这不是严格物理正确的 relighting。普通 3DGS 的颜色本来就是 baked radiance field；把它当 emission 使用，本质上是“用捕获/训练出的 radiance 去照亮新增 mesh”。视觉上会很像 GI 或环境光传递，但需要 emission scale、clamp、SH band 控制来避免过曝和双重计光。

## 当前项目现状

3DGS 相关代码主要在：

- `caustica/ProcessingPasses/GaussianSplatPass.h/.cpp`
  - 读取 PLY，构建 `GaussianSplatData`、颜色/SH buffer、sort key、index buffer。
  - `BuildAccelerationStructures()` 已能为 splat 构建 AABB 或 icosahedron 代理 AS。
  - `GetTopLevelAS()`、`GetSplatBuffer()` 已暴露给全局 binding set。

- `caustica/ProcessingPasses/GaussianSplatRaster.hlsl`
  - 负责 raster overlay、depth test、GPU sort key、SH view-dependent color。
  - hybrid shadow 模式下，splat pixel 可以向 mesh BVH 打 shadow ray。

- `caustica/Shaders/HybridGaussianShadow.hlsli`
  - 已有 Gaussian ray query 的近似相交、随机 opacity acceptance、hard/soft shadow helper。
  - 当前用途是“Gaussian 遮挡 mesh/light visibility”，不是“Gaussian 发光”。

- `caustica/Shaders/HybridGaussianCommon.hlsli`、`caustica/Shaders/HybridGaussianReflection.hlsli`
  - 这两个文件目前是未跟踪文件，但内容很接近我们要的 emission 查询：给定一条 ray，遍历 Gaussian BVH，累积/返回 splat radiance。
  - 后续可以把其中的 radiance evaluation 和 K-hit accumulation 改成正式 `HybridGaussianEmission.hlsli`。

- `caustica/Shaders/SampleConstantBuffer.h`
  - `SampleConstants` 已有 `GaussianSplatShadow*` 参数。
  - 还没有 emission 相关参数，例如 strength、sample count、SH format、radiance clamp、proxy count。

- `caustica/Shaders/Bindings/SceneBindings.hlsli`
  - `GaussianSplatBVH : t7` 与 `t_GaussianShadowSplats : t8` 已绑定。
  - 当前 path tracer 只能读 `GaussianSplatData`，还读不到 raster path 使用的 packed RGBA/SH buffer。

- `caustica/Shaders/PathTracerBridgeDonut.hlsli` 和 `caustica/RTXDI/RtxdiApplicationBridge.hlsli`
  - mesh visibility ray 已能额外测试 Gaussian shadow。
  - 这说明 Gaussian AS 已进入 path tracing/RTXDI shader 可见范围，后续加 emission 不需要从零搭桥。

- `caustica/RTXDI/GITemporalResampling.hlsl`、`caustica/RTXDI/GIFinalShading.hlsl`
  - ReSTIR GI 的 initial reservoir 来自 path tracer 写出的 secondary position/radiance。
  - 如果 path tracer 能把 Gaussian 当作 secondary emissive hit，ReSTIR GI 可以复用这类样本。

一个小注意：现在 `CreateAccelStructs()` 只在 `GaussianSplatShadows` 打开时构建 Gaussian AS。如果 emission 独立于 shadow 开关，需要把条件改成 `shadowsEnabled || emissionEnabled || gaussianRayHitEnabled`。

## EGSR 2025 论文是否可用

论文：Xin Sun, Iliyan Georgiev, Yun Fei, Milos Hasan, **Stochastic Ray Tracing of Transparent 3D Gaussians**, EGSR 2025。项目页见 <https://iliyan.com/publications/GaussianRayTracing/>，本地 PDF 为 `D:/GaussianRayTracing_EGSR2025.pdf`。

这篇论文非常适合用在“3DGS 作为 emissive/radiance field 被 path tracer 查询”的部分，尤其是 secondary ray、reflection ray、shadow ray、NEE 验证 ray：

- 它把透明 Gaussian 的 alpha 当成 stochastic binary opacity，ray 只需要一次 BVH traversal，随机接受一个或 N 个交点，避免沿 ray 收集和排序所有 Gaussian。
- 它的估计与 Monte Carlo path tracing 很合拍：噪声可以由现有 accumulation/denoiser/ReSTIR GI 分担，而不是在单帧里硬做完整透明排序。
- 论文示例明确包含 mesh 与 Gaussian 混合渲染，包含 shadow、glossy reflection、refraction，以及“Gaussian scene/environment 照亮 mesh asset”的效果。
- 论文也说明了一个重要边界：Gaussian radiance 被视为已知，默认不受周围 mesh/light 反过来影响。这正好匹配我们现在想做的“3DGS 作为 emission 物体”的近似。

但它不能单独解决全部问题：

- 它解决的是“ray 穿过透明 Gaussian 云时如何高效得到 radiance/opacity event”，不是“从 mesh surface 如何重要性采样到 3DGS”。
- 若只做 cosine hemisphere sampling 去撞 3DGS，方差会很高。仍然需要 proxy light、CDF/alias table、ReSTIR reservoir 或 probe 引导。
- DXR/RayQuery 里要注意不要在 emission 查询里简单使用 `RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH` 接受第一个 traversal candidate。对 radiance 来说我们要的是最近 accepted event，或者 K 个近似前向合成 hit。

因此建议：**用 EGSR 的 stochastic traversal 作为 Gaussian radiance evaluator，用 proxy/ReSTIR 解决采样分布。**

## nvpro lighting_and_shadows 的参考意义

参考页：<https://nvpro-samples.github.io/vk_gaussian_splatting/deep-dives/lighting_and_shadows/>

这个页面有参考价值，但它的目标和这里不完全一样。它主要讲“如何给 3DGS 自身做 lighting/shading/shadow”，并明确说明不是 relighting 方法；它默认可以把 splat set 当成 fully emissive radiance field 来显示，也可以降低 emissive、提高 diffuse/specular，让合成灯光影响 3DGS。

对本项目最有用的是这些思想：

- **把 3DGS 当 radiance field/emissive material**：这支持我们把 base color/SH radiance 用作 emission。
- **iso-opacity depth picking**：它用 front-to-back transmittance 跨过阈值来选一个稳定“表面深度”。如果我们做 screen-space/probe 版本，或要给 Gaussian hit 生成 debug depth/normal，可以采用。
- **hit distance 选择**：AABB parametric 模式使用 ray 到 Gaussian center 的最大密度点，和当前 `HybridGaussian_IntersectSplat()` 的思路接近；icosahedron shell 模式则更像边界表面。
- **normal 近似**：max-density plane 或 kernel ellipsoid normal 可用于 debug、directional emission、或让 proxy 更像表面而不是各向同性体积。
- **shadow transmittance threshold / particle shadow offset / colored shadows**：可迁移到 Gaussian emission 的 self-shadow 与 mesh occlusion 控制。
- **hybrid pipeline**：raster 给 first bounce 的 color/depth/normal，ray tracing 接后续 bounce。当前 caustica 是 path tracing 后再 overlay 3DGS，若未来想让 camera-primary Gaussian 也进入统一 GI/denoising，可参考它的 hybrid 合成策略。

它不直接给出“3DGS 对 mesh 产生 GI”的完整方案。我们需要在 caustica 的 path tracer/RTXDI 路径里新增 Gaussian emitter sampling。

## 推荐 MVP：Proxy 引导的 Gaussian Emission NEE

### 目标

让 mesh surface 在 path tracer 的直接光采样阶段，额外采样一次或多次 3DGS emission，使周围 mesh 获得来自 3DGS 的 diffuse/specular 入射光。

这个阶段不要求 3DGS 自身被 mesh 反照亮，也不要求每个 splat 成为真实面积光。目标是低成本、可调、稳定。

### 数据结构

新增一个轻量 proxy buffer，而不是把每个 splat 当光源：

```cpp
struct GaussianEmitterProxy
{
    float3 center;
    float radius;
    float3 radiance;
    float weight;
    uint firstSplat;
    uint splatCount;
};
```

proxy 构建方式可以从简单到复杂：

- **Grid clustering**：按 world-space cell 聚合 splat，适合快速实现。
- **BVH leaf clustering**：复用 AS 构建或离线层级，空间分布更自然。
- **Top-K energy splats + grid background**：高亮区域保留细节，其余聚合。

proxy 权重可先用近似：

```text
energy = luminance(linear(baseColor) * emissionScale)
       * opacity
       * approximateGaussianAreaOrVolume
       * userImportanceBoost
```

其中 area/volume 可以先取 `sqrt(det(covariance))` 的简化版本并 clamp，避免巨大 splat 过度支配。第一版甚至可以不使用体积，只用 `opacity * luminance(color)` 验证视觉链路。

### Shader 查询

新增 `caustica/Shaders/HybridGaussianEmission.hlsli`，从现有 `HybridGaussianCommon.hlsli` / `HybridGaussianReflection.hlsli` 提炼：

```hlsl
bool HybridGaussian_TraceEmission(
    RaytracingAccelerationStructure gaussianBVH,
    StructuredBuffer<GaussianSplatData> splats,
    ByteAddressBuffer shBuffer,              // phase 2 可选
    uint splatCount,
    RayDesc worldRay,
    float4x4 worldToObject,
    float4x4 objectToWorld,
    GaussianEmissionParams params,
    out float hitT,
    out float3 Le);
```

第一版可以只用 `GaussianSplatData.color.rgb`，不读 SH。这样无需立刻把 `m_shBuffer` 接到全局 path tracer binding。第二版再暴露 `m_colorBuffer/m_shBuffer/shDegree/shFormat/brightness`，用 receiver 到 splat 的方向评价 view-dependent SH：

```text
viewDirForSH = normalize(worldCenter - receiverPosition)
```

### NEE 贡献

在 `caustica/Shaders/PathTracer/PathTracerNEE.hlsli` 附近新增 Gaussian emitter sample：

1. 从 proxy CDF/alias table 采样一个 proxy。
2. 在 proxy sphere/ellipsoid 上采一个目标点或方向。
3. 从 mesh shading point 发 ray。
4. 先用 `SceneBVH` 做 mesh occlusion。
5. 再沿同一 ray 查询 `GaussianSplatBVH`，得到实际 `Le` 和 `hitT`。
6. 按 BSDF、cosine、pdf 加权加入 `NEEResult`。

伪代码：

```hlsl
GaussianProxySample p = SampleGaussianProxy(proxyBuffer, rng);
RayDesc ray = MakeVisibilityRay(shadingData.posW, p.samplePosition);

if (!TraceMeshOcclusion(SceneBVH, ray))
{
    float hitT;
    float3 Le;
    if (HybridGaussian_TraceEmission(GaussianSplatBVH, t_GaussianShadowSplats, ..., ray, hitT, Le))
    {
        float3 wi = ray.Direction;
        float3 f = bsdf.eval(shadingData, wi, ...).rgb;
        radiance += pathThroughput * f * Le * abs(dot(shadingData.N, wi)) / p.pdf;
    }
}
```

这里有两个可选估计器：

- **Deterministic K-hit composite**：沿 ray 收集近处 K 个 Gaussian hit，按 `T * alpha * Le` 累积。噪声低，可能有 bias，适合实时预览。
- **EGSR stochastic hit**：每个候选按 alpha 随机接受，返回最近 accepted event。理论更干净，噪声交给 temporal accumulation/ReSTIR/denoiser。

MVP 建议先实现 K-hit composite，调通后加 stochastic 模式作为质量/性能选项。

## 与 ReSTIR GI 的结合方式

当前 ReSTIR GI 的 initial reservoir 来自 path tracer 写出的：

- `u_SecondarySurfacePositionNormal`
- `u_SecondarySurfaceRadiance`

`caustica/RTXDI/GITemporalResampling.hlsl` 会把这些 secondary samples 包成 GI reservoir，再做 temporal/spatial reuse。利用这一点有两条路：

### 路线 A：让 scatter ray 能命中 Gaussian emission

在 path tracer 的 scatter ray 查询中，同时查询 mesh `SceneBVH` 和 `GaussianSplatBVH`：

- 如果 mesh hit 更近：按现有 mesh surface 处理。
- 如果 Gaussian accepted hit 更近：把 Gaussian 当 emissive hit，累积 `path.throughput * Le`，并在需要时写 secondary radiance。

优点是最接近 EGSR 论文，也会自然影响 glossy reflection、refraction、diffuse bounce 和 ReSTIR GI。缺点是改 path tracer 核心路径，风险比 NEE MVP 高。

### 路线 B：给 ReSTIR GI 增加 Gaussian initial candidate

在 ReSTIR GI temporal pass 前，或 path tracer secondary sample 输出处，额外生成一个 Gaussian emitter candidate：

- `samplePosition = Gaussian hit position` 或 proxy center。
- `sampleRadiance = Le / pdf`。
- `sampleNormal = estimated Gaussian normal`，没有稳定 normal 时可用 `-wi` 或跳过 normal 相关项。

`RAB_GetGISampleTargetPdfForSurface()` 已经用 `surface.Eval(L) * sampleRadiance` 做 target pdf，这和 Gaussian emitter sample 是兼容的。

优点是更容易利用 RTXDI 的 temporal/spatial reuse。缺点是要处理 candidate 与原始 path secondary sample 的 MIS/权重混合。

建议顺序：先做路线 A 的基础命中或 NEE MVP，再考虑路线 B。

## 是否接入现有 RTXDI light list

可以，但不建议作为第一步。

现有 RTXDI/LightSampler 针对 scene lights、emissive triangles、environment quad light 做了完整 light preparation、proxy counters、local sampling、ReGIR。把 3DGS proxy 注入进去的好处是可以直接获得 ReSTIR DI/ReGIR 的 sampling/reuse；代价是会碰到：

- proxy 数量动态变化，需要改 `PrepareLightsPass` 和 `RtxdiResources` 的容量估算。
- `PolymorphicLightType` 没有 Gaussian 类型；可以伪装成 sphere light，但实际 radiance 仍应通过 Gaussian BVH 校正。
- RTXDI light sample 的 visibility 当前是到一个几何/解析光位置，不能自动处理 Gaussian field 的半透明 self-transmittance。
- 如果 proxy 太多，light sampling proxy/counter 会膨胀。

更好的中间方案是：**proxy 只负责采样方向，最终 radiance 一律由 Gaussian TLAS query 校正**。等视觉与性能稳定后，再考虑把 proxy list 作为 RTXDI candidates。

## 关键工程改动点

### C++ 层

1. `SampleUI.h/.cpp`
   - 增加开关和参数：
     - `GaussianSplatEmissionEnabled`
     - `GaussianSplatEmissionStrength`
     - `GaussianSplatEmissionSamples`
     - `GaussianSplatEmissionUseSH`
     - `GaussianSplatEmissionProxyCount / GridCellSize`
     - `GaussianSplatEmissionRadianceClamp`

2. `GaussianSplatPass.h/.cpp`
   - 构建并暴露 `GaussianEmitterProxy` buffer、CDF/alias buffer。
   - 暴露 packed SH/RGBA buffer getter，或者第一版只暴露 base `GaussianSplatData.color`。
   - AS build 条件从 shadow 扩展为 shadow/emission/reflection 任一启用。

3. `SampleConstantBuffer.h`
   - 新增 emission params，注意 C++/HLSL 对齐。
   - 新增 `GaussianSplatEmissionWorldToObject/ObjectToWorld`，或复用当前 transform。

4. `Sample.cpp`
   - `CreateAccelStructs()`：emission enabled 时也构建 Gaussian AS。
   - `RecreateBindingSet()`：绑定 proxy buffer、CDF/alias buffer、可选 SH buffer。
   - `SampleRenderCode()` 前写入 emission constants。

### HLSL 层

1. `HybridGaussianCommon.hlsli`
   - 作为相交/协方差/alpha kernel 的唯一公共实现。
   - 避免 `HybridGaussianShadow.hlsli` 和 reflection/emission 里重复定义不同签名的函数。

2. 新增 `HybridGaussianEmission.hlsli`
   - `EvaluateSplatRadiance()`
   - `TraceGaussianEmissionKHit()`
   - `TraceGaussianEmissionStochastic()`
   - 可选 `EstimateGaussianNormal()`

3. `PathTracerNEE.hlsli`
   - 增加 `HandleGaussianEmissionNEE()`。
   - 用独立开关编译，避免关掉时增加寄存器压力。

4. `PathTracerBridgeDonut.hlsli`
   - 如做 scatter hit 路线，需要在 mesh hit 查询旁边加 Gaussian hit 查询和最近 hit 比较。

5. `RtxdiApplicationBridge.hlsli`
   - 如果要让 RTXDI/ReSTIR GI 的 final visibility 也考虑 Gaussian 自遮挡/遮光，复用现有 Gaussian shadow branch。

## 质量与性能控制

必要参数：

- `Emission Strength`：3DGS radiance 不是物理单位，必须给艺术/调参控制。
- `Radiance Clamp`：防止少量高亮 splat 造成 firefly。
- `Max Hit Count` 或 `Stochastic Mode`：在 K-hit 稳定性与 EGSR stochastic 性能之间切换。
- `Proxy Count / Grid Size`：控制采样质量与内存。
- `Use SH`：默认关闭，先用 DC/base color；打开后更符合 view-dependent 3DGS，但也更容易方向错误或过拟合。
- `Self Shadow / Transmittance Threshold`：决定 3DGS 内部遮光强度。
- `Mesh Occlusion`：通常应打开，否则 3DGS 会穿墙照亮 mesh。

性能预期：

- 直接把每个 splat 当 light 不现实。
- 每个 shaded mesh point 1 个 Gaussian NEE sample 通常可以先跑起来；配合 accumulation/ReSTIR GI 后再提高到 2-4。
- K-hit composite 的 per-ray 成本取决于 BVH candidate 数和 payload/register。先限制 `maxHitsPerPass` 为 4 或 8。
- EGSR stochastic single-hit 适合高 spp/temporal accumulation；静帧收敛更慢但成本更低。

## 视觉风险

- **双重照明**：3DGS base color/SH 已经包含原场景光照，再让它照亮 mesh 会把 baked lighting 当作真实 emission。视觉上可接受，但不守恒。
- **方向性错误**：3DGS SH 是 view-dependent appearance，不一定等价于物理 BRDF/发光分布。第一版用 base color 更稳定。
- **floaters 贡献光照**：孤立 splat 可能产生脏光。用 alpha threshold、energy clamp、proxy 过滤、iso-depth/normal 规则缓解。
- **primary camera 双显示**：当前 camera-primary 3DGS 是 path tracing 后 overlay。如果又让 primary ray 命中 Gaussian，可能重复显示。第一版只让 mesh shading/secondary/NEE 查询 Gaussian，不改 camera-primary overlay。
- **透明排序 bias/noise**：K-hit composite 稳定但近似；EGSR stochastic 更正确但有噪声。保留两种模式。

## 推荐实现顺序

1. **文档/参数落地**
   - 加 UI/CLI/Python 参数和 `SampleConstants` 字段。
   - emission enabled 时构建 Gaussian AS。

2. **Base color 版 NEE**
   - 构建 grid proxy buffer。
   - path tracer mesh hit 处每个 shading point 采 1 个 proxy。
   - ray 到 Gaussian TLAS，返回 base color radiance。
   - mesh BVH visibility 打开。

3. **K-hit 与 stochastic 两个 evaluator**
   - 先 K-hit 调稳定。
   - 再加 EGSR stochastic single-hit，给实时模式测试性能。

4. **ReSTIR GI 复用**
   - 让 Gaussian hit 写入 secondary radiance，或增加 Gaussian GI initial candidate。
   - 调整 reservoir target pdf 与 final visibility。

5. **SH / normals / colored transmittance**
   - 暴露 SH buffer。
   - 加 max-density plane normal 估计。
   - 增加 colored shadow/transmittance 可选项。

## 最小验证场景

1. 只保留一个白色 diffuse plane 和一个彩色 3DGS，对比开关前后 plane 上是否出现 color bleeding。
2. 加一个 opaque wall，确认 mesh occlusion 会挡住 3DGS emission。
3. 关闭所有传统 lights/env，确认 mesh 仍可被 3DGS 照亮。
4. 提高 spp/accumulation，比较 K-hit 与 EGSR stochastic 的噪声/偏差。
5. 打开 ReSTIR GI，确认 temporal/spatial reuse 后 indirect 结果更稳。
6. 测试高亮小 splat，确认 clamp 能压住 firefly。

## 最终建议

短期目标不要追求“完整物理 GI”。先把 3DGS 当作可采样的 emissive radiance field，让 mesh 通过 NEE 和 path tracer secondary hit 获得近似入射光。这一层最符合当前 caustica 架构，能复用 Gaussian AS、现有 visibility、accumulation、ReSTIR GI 和 denoising。

EGSR 论文的 stochastic ray tracing 技术非常适合做底层 Gaussian emission evaluator；nvpro lighting/shadows 页面更适合借鉴 surface/depth/normal/shadow/transmittance 这些工程近似。两者结合起来，就是本项目最现实的路线：**proxy 负责采样，EGSR-style Gaussian ray traversal 负责评价，caustica/ReSTIR GI 负责复用与收敛。**

## 参考

- Stochastic Ray Tracing of Transparent 3D Gaussians, EGSR 2025: <https://iliyan.com/publications/GaussianRayTracing/>
- 本地论文 PDF: `D:/GaussianRayTracing_EGSR2025.pdf`
- NVIDIA nvpro-samples Gaussian Splatting Lighting and Shadows: <https://nvpro-samples.github.io/vk_gaussian_splatting/deep-dives/lighting_and_shadows/>

## 2026-05-17 落地版本：SH0 proxy emission

根据当前实现选择，先不做完整 Gaussian ray evaluator，而是把 3DGS proxy 直接注入现有 light sampling：

- 每个候选 splat 生成一个 `GaussianSplatEmissionProxy`，中心来自 splat center，半径来自已有 Gaussian AABB footprint。
- emission 使用 PLY 里的 SH 0 阶/DC 颜色，也就是当前 `GaussianSplatData.color.rgb`，按 raster 路径一致的 sRGB-to-linear 转换后乘 base opacity。
- 用户参数 `GaussianSplatAsEmitter` / `scene.splats.asEmitter` 是显式开关；默认 `false`，因此旧场景行为不变，也不会生成/注入额外 emissive proxy。
- 用户参数 `GaussianSplatEmissionIntensity` / `scene.splats.emissionIntensity` 作为最终辐射亮度强度倍率；默认 `1`，仅在 `As Emitter` 开启时生效。
- `GaussianSplatEmissionMaxProxyCount` / `scene.splats.emissionProxyLimit` 控制最多注入多少个 proxy，默认 `8192`，也仅在 `As Emitter` 开启时生效。当 splat 数量更多时，按 `luminance(radiance) * radius^2` 选 top-K。
- 非 RTXDI 路径在 `LightsBaker` 中把这些 proxy 作为 analytic sphere lights 注入 `PolymorphicLightInfo`。
- RTXDI/ReSTIR 路径在 `PrepareLightsPass` 中把这些 proxy 作为 finite primitive lights 注入，位置排在 infinite/environment light 之前，保证 local light region 连续。

这版更像“把 3DGS 的重要 splat 近似成自发光 sphere proxy mesh”，优点是能立刻复用现有 NEE、RTXDI、ReSTIR GI 的采样和 mesh visibility；缺点是不会表达透明 Gaussian 云内部的多层遮挡和 view-dependent 高阶 SH。后续如果需要更真实，可以把本节实现作为 sampling guide，再接 EGSR stochastic traversal 做实际 radiance evaluation。
