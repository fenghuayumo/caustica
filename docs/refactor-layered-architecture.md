# caustica 分层架构重构方案

> 范围：`caustica/caustica/`（引擎）+ `application/editor/`（桌面应用）+ Python 绑定。
> 不在范围内：External/ 下的第三方 SDK（NRD/RTXDI/OMM/Streamline…）、`backend/rhi`（nvrhi，第三方图形抽象，作为既有下层使用）。
> 当前分支：`refactor/phase1-foundation`（领先 main 87 个提交）。Phase 1 已交付**正确的 CMake 分层拓扑**（见下文"Phase 1 交付物"），但**未配套强制机制**，源码层 `#include` 越界仍可静默编译——这正是本方案要补齐的部分。

---

## 一、现状诊断

Phase 1 已经把单体拆成了按层的 CMake 静态库目标，方向正确。但"分层透明"还远未达成：**底层反向 include 上层**、**没有稳定公共 API**、**顶层分层倒置**。三条主线问题如下。

### 问题 A：底层 → 上层 的反向依赖（违反"上层依赖下层"）

| 位置 | 反向 include | 性质 |
| --- | --- | --- |
| `src/backend/GpuDevice.cpp:2` | `<engine/Application.h>` | backend → engine，最严重（潜在环） |
| `include/backend/GpuDevice.h`（条件） | `<engine/StreamlineInterface.h>` | backend → engine |
| `src/core/MediaFileSystem.cpp:3` | `<scene/scene_utils.h>` | core → scene |
| `src/assets/loader/TextureLoader.cpp:4,5` | `<render/Core/DescriptorTableManager.h>`、`<render/Core/CommonRenderPasses.h>` | assets → render |
| `src/assets/loader/TextureLoader.cpp:6` | `<engine/ConsoleObjects.h>` | assets → engine |
| `src/assets/loader/ShaderFactory.cpp:7` | `<engine/AftermathCrashDump.h>` | assets → engine |
| `include/scene/Scene.h:4` | `<render/Core/DescriptorTableManager.h>` | scene → render |
| `src/render/WorldRenderer/PathTracingWorldRenderer.cpp:39` | `<engine/StreamlineInterface.h>` | render → engine |
| `src/render/Core/PostProcessAA.cpp:13` | `<engine/StreamlineInterface.h>` | render → engine |
| `render/Passes/Lighting/MaterialsBaker.cpp`、`LightsBaker.cpp`、`Passes/OMM/OmmBaker.cpp` | `<engine/UserInterfaceUtils.h>` | render → engine |
| `src/imgui/imgui_console.cpp:4,5` | `<engine/ConsoleInterpreter.h>`、`<engine/ConsoleObjects.h>` | imgui → engine |

这些是"机器可判定"的越界，是 Phase 2 要逐条消灭的对象。

### 问题 B：没有公共 API（Facade 缺失）

- 桌面应用直接伸手进引擎内脏：`SampleUI.cpp` 一个文件就 include 了 `SceneLightingPasses / SceneGaussianSplatPasses / MaterialsBaker / OmmBaker / ToneMappingPasses / Korgi / ZoomTool / ShaderFactory / UserInterfaceUtils…` 十多个内部头；`game/*.cpp`、`app/SceneEditor.h` 同样如此。整个 editor 合计 **45+ 个引擎内部头**。
- `application/editor/app/SceneEditor.h` 是从 NVIDIA 原版 sample 继承下来的 **God-class "Sample"**：约 100 个方法、成员横跨 `GpuDevice / SceneManager / RenderCore / ShaderFactory / TextureLoader / CommonRenderPasses / BindingCache / DescriptorTableManager / PathTracingWorldRenderer / CaptureScriptManager / ZoomTool / GameScene…` 全部层级。它既是"编辑器控制器"，又是事实上的"引擎入口"，两者职责混在一起。
- 反观 Python 绑定里的 `PyRenderer`（`PythonBindings_Extension.cpp`）反而是干净的 Facade 形态：`Renderer(...) / load_scene / step / step_until_accumulated / save_screenshot / set_camera / set_camera_fov / close()`。**同一个引擎，两个消费者用了两套完全不同的接入方式**——这正是要统一的。

### 问题 C：顶层分层倒置

`application/editor/CMakeLists.txt` 把 `app/*.cpp` 等通过 `PARENT_SCOPE` 注入了引擎构建；而位于引擎内的 Python 绑定（`caustica/Python/RenderSession.cpp`）反过来 include `application/editor/` 的 `SceneEditor.h`、`EditorApplication.h`。即 **engine → editor**，方向反了。"editor" 名义上是顶层应用，实际被编译进引擎库，且引擎还依赖它。

### 问题 D：分层"声明了"但"没强制"（最关键的缺口）

Phase 1 在 CMake 层面其实做对了——`target_link_libraries` 的方向是**严格向下**的（已核对 `caustica/caustica/CMakeLists.txt` 实际值）：

```
causMath → (无)
causCore → causMath
causEvents → causCore          causAnimation → causCore,causMath      causAudio → causCore,causMath
causPlatform → causCore,causEvents
causBackend  → causCore,causMath,causPlatform, nvrhi        ← 不含 causEngine ✓
causImgui    → causCore,causBackend
causAssets   → causCore,causBackend                            ← 不含 render/engine ✓
causScene    → causCore,causMath,causAnimation,causAssets,causBackend  ← 不含 render ✓
causRender   → causScene,causBackend,causImgui,causAssets     ← 不含 engine ✓
causEngine   → causEvents,causPlatform,causBackend,causRender,causImgui
causticaLib  = INTERFACE 聚合以上全部
causticaApp  → causticaLib  （= editor 源码编成的静态库）
caustica_py  → causticaApp   （python 绑定 ← 顶层倒置点）
```

方向全对。**那为什么问题 A 的越界还能编译通过？** 两个机制让分层沦为"约定"：

1. **共享扁平 include 根**：`caus_configure_static()` 给**每个** target 都 `target_include_directories(<tgt> PUBLIC ${CAUSTICA_COMMON_INCLUDE_PUBLIC})`——12 个 target 共用同一棵 `include/` 树。于是 causBackend 里写 `#include <engine/Application.h>` 在头搜索路径上完全可见，CMake 根本不知道这是越界。
2. **静态库只在最终可执行文件链接时解析符号**：`causticaApp → causticaLib` 把所有模块一起拉进来，所以即便 causBackend 的 `target_link_libraries` 没列 causEngine，GpuDevice.cpp 对 Application 符号的引用照样在最终链接时被满足——链接器不报错。

结论：**Phase 1 把"层"声明对了，但没有给"层"上锁。** 上锁需要的是 include-lint（源码层禁越界）+ public/internal 目录隔离（让下层私有头物理上不可见）+ 最终链接顺序的依赖闭合校验。这三件就是本方案"护栏"章节的内容，也是真正"完成 Phase 1 意图"的最后一步。

---

## 二、目标分层架构

依赖规则：**箭头只朝下**。每一层只暴露一个小的"公共 API 头"，实现头对上层不可见。

```
┌──────────────────────────────────────────────────────────────┐
│ application/editor (桌面 app)  +  caustica/Python (绑定)       │  只依赖 Engine Facade
├──────────────────────────────────────────────────────────────┤   （+ 各自的 UI/编辑器专用类型）
│ ENGINE  causEngine                                            │  组合根 + 唯一公共 API
│  • Engine (Facade) / Application / CaptureSequencer / EntryPoint│
├──────────────────────────────────────────────────────────────┤
│ RENDER  causRender                                            │
│  • PathTracingWorldRenderer / RenderCore / Passes/*           │
├──────────────────────────────────────────────────────────────┤
│ SCENE causScene          │  ASSETS causAssets                 │  对等层
│  SceneGraph/Camera/Material │  Loader / TextureLoader / Shader│
├──────────────────────────────────────────────────────────────┤
│ BACKEND causBackend  ——  GpuDevice（封装 nvrhi RHI）            │
├──────────────────────────────────────────────────────────────┤
│ PLATFORM causPlatform  ——  window / OS / input                │
├──────────────────────────────────────────────────────────────┤
│ CORE  causCore · causMath · causEvents · causAnimation · causAudio │  零依赖地基
└──────────────────────────────────────────────────────────────┘
```

说明几点关键判断：

1. **scene 与 assets 是对等层，都在 render 之下**。render 依赖 scene/assets（向下，OK）；scene/assets **不得**依赖 render。当前 `scene/Scene.h → render/Core/DescriptorTableManager.h` 必须拆除。
2. **scene 现阶段保留持有 GPU 资源句柄**（`nvrhi::BufferHandle / TextureHandle / AccelStructHandle`）。这是务实选择——它是"可渲染场景"而非纯数据。因此 scene 合法地依赖 backend/RHI（向下），但绝不能依赖 render。纯数据拆分留作 Phase 5（可选）。
3. **Streamline/Aftermath/Console 等横切能力下沉或反转**，不能留在 engine 被下层 include（详见下文）。
4. **public/ 与 internal/ 分离**：每个 target 的 `include/<layer>/` 只放公共 API 头；实现头放在 `src/<layer>/` 或 `include/<layer>/internal/`，且**不**导出给上层。

---

## 三、逐项重构动作

针对问题 A 的每一条反向依赖，给出具体拆法。统一采用两种手法：**下沉**（把被多方需要的类型/接口移到更底层）与**依赖反转**（下层定义接口，上层实现并注入）。

### A1. backend → engine（`GpuDevice` ↔ `Application` / `Streamline`）

这是最危险的一条（潜在环）。`GpuDevice` 需要 `Application` 的什么，就把它**抽到 `platform/` 或 `core/`**：
- 若是窗口/生命周期回调参数：定义一个 `platform/` 下的纯结构（如 `WindowEventSink` 接口），`Application` 实现它并注入 `GpuDevice`。
- `backend/GpuDevice.h → engine/StreamlineInterface.h`：`StreamlineInterface` 本质是图形 SDK 集成，**应下沉到 `backend/`（或新增 `backend/integration/`）**，由 backend 拥有；engine 不再被 backend 引用。

> 目标：`causBackend` 的依赖只剩 `causCore / causMath / causPlatform`（+ nvrhi）。CMake `target_link_libraries(causBackend PRIVATE causCore causMath causPlatform)`，**不含 causEngine**。

### A2. core → scene（`MediaFileSystem` ↔ `scene_utils`）

把 `scene_utils.h` 中 `MediaFileSystem` 真正需要的那一小块**下沉到 `core/`**（如路径/媒体枚举工具）；`scene_utils` 反过来改成 include core。core 永远不认识 scene。

### A3. assets → render + engine（`TextureLoader` / `ShaderFactory`）

这是 assets 越界最密集处。三处分别处理：
- **DescriptorTableManager**：`DescriptorHandle` 已抽到 `core/DescriptorHandle.h`（Phase 1 已做）。把"描述符分配器"抽象成一个接口（`IDescriptorAllocator`，放 `backend/` 或 `core/`），`render/Core/DescriptorTableManager` 实现它；`TextureLoader` 依赖接口，不再 include render。
- **CommonRenderPasses**（用于纹理上传/mip 生成 blit）：定义 `ITextureUploader` 接口（assets 或 core），由 render 实现、engine 注入。或更彻底：把"GPU 上传"从 `TextureLoader` 拆出去，TextureLoader 只产 CPU 端 `TextureData`，上传交给 render 层。
- **engine/ConsoleObjects.h**、**engine/AftermathCrashDump.h**：assets 需要的"控制台变量 / 崩溃上报"是横切能力，**下沉到 `core/`**（`core/console/`、`core/diagnostics/`），engine 层只是聚合。TextureLoader/ShaderFactory 改依赖 core。

### A4. scene → render（`Scene.h → DescriptorTableManager.h`）

`Scene` 只需要持有"描述符句柄"来登记 bindless 资源，并不需要 `DescriptorTableManager` 这个 render 类。配合 A3 的 `IDescriptorAllocator`：`Scene` 依赖 `core/DescriptorHandle` + 接口，include `render/...` 删除。

### A5. render → engine（`StreamlineInterface` / `UserInterfaceUtils`）

- **StreamlineInterface（DLSS-FG/Reflex 等帧合成钩子）**：与 A1 一致，下沉到 backend/integration；render 通过接口调用。
- **UserInterfaceUtils（MaterialsBaker/LightsBaker/OmmBaker 里画 UI）**：Baker 不应该画 UI。把 UI 绘制从 Baker 里**剥离**——Baker 只暴露数据/进度回调，由 editor/UI 层负责绘制。彻底消除 `render → engine`。

### A6. imgui → engine（console）

`ConsoleInterpreter / ConsoleObjects` 下沉到 `core/console/`，imgui 与 engine 都改依赖 core。

### A7. engine → editor（顶层倒置，配合 Facade 一起做）

- 把 `SceneEditor` 中"引擎级编排"（持有 device/render core/scene manager/world renderer/loaders、驱动一帧）的部分**上移进 engine 层的 Facade**；把"编辑器级"部分（Inspector、mesh 编辑、capture script、ZoomTool、GameScene）**留在 editor**，且只通过 Facade 访问引擎。
- `caustica/Python` 不再 include `application/editor/`；它绑定 Facade（与 `PyRenderer` 合并，见下）。

---

## 四、引入 Engine Facade（核心结构变更）

目标：**一个 Facade，两个消费者（editor / python）**。把 Python 已有的 `PyRenderer` 干净形态提升为引擎官方 API，editor 也改用它。

### 4.1 Facade 形态（示意，`engine/Engine.h`）

```cpp
namespace caustica {

struct EngineDesc {
    GraphicsAPI api = GraphicsAPI::D3D12;
    WindowDesc  window;
    bool        debug = false;
};

class Engine {
public:
    static std::unique_ptr<Engine> create(EngineDesc);   // 唯一入口：内部完成所有子系统装配

    bool loadScene(const std::filesystem::path& file);   // 异步/同步加载
    bool isSceneLoaded() const;
    void renderFrame();                                  // 推进 + 渲染一帧
    void shutdown();

    // 相机
    void setCameraView(float3 pos, float3 dir, float3 up);
    void setCameraFov(float vfovDeg);
    void setCameraIntrinsics(float fx, float fy, float cx, float cy, float w, float h);

    // 输出/捕获
    bool captureScreenshot(const std::filesystem::path& out);

    // 窄而稳定的"设置视图"（不直接暴露整个 PathTracerSettings 内部）
    PathTracerSettingsView settings();

    // 只读场景查询（给 Inspector 用），经稳定接口
    SceneQuery& scene();

    // 扩展钩子（依赖反转）：editor 注入 RT/材质特化/拾取等回调
    void setRenderHooks(RenderHooks hooks);

private:
    std::unique_ptr<EngineImpl> m_impl;   // pImpl：GpuDevice/ShaderFactory/CommonRenderPasses/
};                                        // BindingCache/DescriptorTableManager/SceneManager/
                                          // RenderCore/WorldRenderer/Loaders 全部藏在里面
} // namespace caustica
```

要点：
- **pImpl** 把所有下层子系统的具体类型藏在 `EngineImpl` 里，`Engine.h` 不再 include `render/`、`backend/`、`scene/`、`assets/` 的任何内部头——这就是"上层看不到下层细节"的物理保证。
- `Engine::create()` 集中完成现在 `EditorApplication::startup()` 里那段手工装配（GpuDevice → ShaderFactory → CommonRenderPasses → BindingCache → DescriptorTableManager → SceneManager → RenderCore → WorldRenderer）。装配知识从此**只存在于引擎内部**。
- 通过 `RenderHooks`（依赖反转）把 editor 需要的扩展点（材质特化 shader、拾取、stable-planes、gaussian splat 等）回调化——这正是现在 `WorldRendererPipelineHooks` 的 30+ 回调想做的事，把它从 editor 侧迁到 Facade 对外契约。

### 4.2 editor 收敛

```cpp
// EditorApplication（重构后）
void startup() {
    m_engine = Engine::create({.api = m_api, .window = ..., .debug = m_debug});
    m_engine->loadScene(m_sceneFile);
    m_editor = std::make_unique<SceneEditor>(*m_engine); // 只拿 Facade 引用
}
void onRender() {
    m_engine->renderFrame();
    m_editor->drawInspector();   // 编辑器专用 UI
}
```

`SceneEditor` 瘦身后只保留编辑器专属职责，通过 `Engine&` 访问一切，**不再持有裸的下层指针**。`application/editor/` 的 include 从 45+ 收敛到 Facade + 编辑器自身 UI 类型。

### 4.3 python 收敛

`PyRenderer` 改为 `Engine` 的薄封装；删除对 `Sample`/`SceneEditor` 的深度绑定（或仅保留极少数只读查询）。`caustica/Python` 不再 include `application/editor/`，A7 倒置随之消失。

---

## 五、分阶段实施

> 命名说明：本仓库提交历史里已经有团队自己的 "Phase 1 / 2 / 2e / 3 / 4 / 4b / C" 标号（见下节"Phase 1 交付物"）。为避免混淆，下面用 **R1–R4（Refactor）** 标记本方案的后续阶段，**接续**当前分支已做的工作，不覆盖团队既有标号。

| 阶段 | 内容 | 风险 | 验收 |
| --- | --- | --- | --- |
| **R1** 拆反向依赖 | 逐条消灭 A1–A6（下沉 + 依赖反转）。每条独立提交、可回滚。 | 低，机械重构 | include-lint 全绿（见下） |
| **R2** Facade + 顶层归位 | 引入 `Engine` Facade 与 `EngineImpl`（pImpl）；把装配从 `EditorApplication` 迁入；`SceneEditor` 拆分为 Facade 编排 + editor 控制器；修 A7（python 不再依赖 editor）。 | 中，结构性 | editor/python 只 include Facade；`causEngine` 不依赖 editor |
| **R3** 收敛上层 include | editor/python 删除剩余内部头；固化 Facade 公共契约（`RenderHooks`/`SceneQuery`/`PathTracerSettingsView`）。 | 中 | editor include 数 ≤ 个位数 |
| **R4（可选）** scene 数据/资源分离 | 纯数据 `SceneModel`（CPU）+ render 持有的 `SceneResources`（GPU）。让 assets/loader 也能脱离 GPU 离线跑。 | 高 | scene 无 nvrhi 句柄 |

建议 R1 先行、可并行多人；R2 是单点结构变更，需整体评审。

---

## 五附、Phase 1 交付物（当前分支已完成的部分）

从 `main..HEAD` 的 87 个提交与实际 CMake 核对，Phase 1（foundation）已交付：

- **身份清洗**：`RTXPT → CAUSTICA`、`DONUT_ → CAUSTICA_` 宏替换、去除 Donut 痕迹、`TextureCache → TextureLoader`、editor `SampleCommon/SampleGame → common/game`。
- **地基库抽取（P0/P1）**：colocate `causticaBase`（core/math 收拢到 `caustica/caustica/`）；抽取 base utilities；合并 ExtendedScene 的 "Ex" 类型、消除 Ex 重复；新增 `causticaEvents`（DIVSHOT 风格事件层）；新增 `AssetSystem / AssetRegistry / AssetCache<T>`（带 LRU 淘汰的 typed CPU 资源缓存）。
- **分层 target 拓扑**：单体拆成 12 个按层静态库（causMath/Core/Events/Animation/Audio/Platform/Backend/Imgui/Assets/Scene/Render/Engine + causticaLib 聚合），`target_link_libraries` 严格向下，并按 Foundation/Service/Feature 三带注释划分（见问题 D 的依赖图）。
- **应用边界**：editor 迁到 `application/`，建立 `EntryPoint` + 组合根（Phase 0/1 app boundary）；`scene/` 从 `engine/` 抽出独立层。
- **分支内继续推进的子阶段**（提交里的 Phase 2/2e/3/4/4b/C）：抽 `SceneRenderFacade` 与 render pass 模块；`PTPipelineBaker/ComputePipelineBaker → render/Core/`；删 `SampleCommon`、内容下沉到各 engine 层；`GameTypes/GameModel → scene/game/`；从 RenderCore 拆出 lighting/bloom；`slim SceneEditor.h`、drop Baker includes；`WorldRendererHost` 杂物袋换成强类型 `WorldRendererServices`；`IWorldRendererPipelineHooks` 换成 `WorldRendererPipelineHooks` 回调。

**Phase 1 没有做完的（= 本方案 R1–R3）**：源码层 `#include` 越界（A1–A7）仍可静默编译（见问题 D 的机制）；没有 Facade、editor 仍 include 45+ 内部头、`SceneEditor` 仍是 God-class；顶层倒置 `caustica_py → causticaApp(editor)` 未解。

---

## 六、验收标准与护栏（让"分层透明"可机器执行）

1. **include 规则脚本（CI）**：建立"层 → 允许 include 的前缀"白名单，构建期扫描 `#include`，越界即失败。例：
   - `backend/**` 只允许 `<core/> <math/> <platform/> <rhi/>`
   - `assets/**`、`scene/**` 只允许 `<core/> <math/> <backend/>`
   - `render/**` 只允许 `<core/> <math/> <backend/> <scene/> <assets/>`
   - `engine/**` 允许 `<core/> <math/> <platform/> <backend/> <render/> <scene/> <assets/>`
   - `application/editor/**`、`caustica/Python/**` 只允许 `<engine/Engine.h>` + 自身目录
   一次性把"透明"从口号变成 CI 红绿灯。
2. **public/internal 物理隔离**：每个 target 的公共头单独目录并 `target_include_directories(<tgt> PUBLIC include/<tgt>)`；私有实现头不进入 PUBLIC 路径。
3. **target 依赖闭合**：`target_link_libraries` 只链接下方层；`causBackend` 不得含 `causEngine`，`causRender` 不得含 `causEngine`。
4. **Facade 唯一入口**：editor/python 对引擎的 include 仅 `engine/Engine.h`（及其显式导出的几个公共类型头）。

---

## 七、一句话总结

> Phase 1 完成了"物理拆库"；本方案要做的是**拆反向依赖（A1–A7）+ 引入 Engine Facade（pImpl）+ include 规则机器化**，让 editor/python 只看见一个稳定窄接口，下层细节对上层不可见——这才是"分层透明"。
