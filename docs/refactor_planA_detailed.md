# 方案A 详细实施计划：拆分 Sample 类 + 完善 Application 生命周期

> 对标 DIVSHOT 架构，将 caustica.cpp (6151行) 拆分为职责清晰的子系统，完善 Application 生命周期。

---

## 一、目标架构

### 1.1 目标 Application 结构

对标 DIVSHOT 的 `Application` 类（拥有 Window / Engine / SceneManager / DeferedRenderer / ImGuiManager）：

```
Application
├── Window*             (已有 - platform/)
├── Engine&             (新建 - Singleton, 帧率/时间步长/统计)
├── SceneManager*       (新建 - 场景加载/切换/队列)
├── Renderer*           (新建 - 主渲染器, 拥有所有RenderPass)
│   ├── LightingBaker  (光照烘焙)
│   ├── CameraController(相机管理)
│   └── GaussianSplatMgr(3DGS管理)
├── ImGuiManager*       (将建 - ImGui生命周期)
└── Input*              (已有 - platform/)
```

### 1.2 目标启动流程

```
main() / WinMain()                              [entry_point.h]
  │
  ├─ coresystem::init()                         ← 日志/命令行
  ├─ createApplication() → new CausticaApp()    ← 用户定义
  └─ app->run()
      ├─ app->init()
      │   ├─ Engine::create()
      │   ├─ Window::create(desc)               → 第1层
      │   ├─ GpuDevice::create(api)             → backend/
      │   ├─ Renderer::init(device, ...)         → 渲染管线初始化
      │   ├─ SceneManager::init()
      │   └─ ImGuiManager::init()
      │
      └─ while(app->frame())
          ├─ Engine::timestep.update()
          ├─ Input::poll()
          ├─ sceneManager->update(dt)
          ├─ imguiManager->newFrame()
          ├─ renderer->render(fb)               → 所有Pass调度
          ├─ imguiManager->render()
          └─ window->present()
```

### 1.3 目标目录变更

```
caustica/caustica/                       (库代码 - 已在分层)
  include/
  │ ├─ engine/
  │ │   ├─ Application.h          ← 扩展: 拥有所有子系统
  │ │   ├─ entry_point.h          ← 新增
  │ │   ├─ Engine.h               ← 新增: Singleton, 时间步长, 统计
  │ │   ├─ SceneManager.h         ← 新增: 场景生命周期
  │ │   └─ Renderer.h             ← 新增: 主渲染器接口
  │ ├─ imgui/
  │ │   └─ ImGuiManager.h         ← 新增: ImGui生命周期管理
  │ └─ ... (其余不变)
  src/
  │ ├─ engine/
  │ │   ├─ Application.cpp        ← 重写: 清晰的生命周期
  │ │   ├─ entry_point.cpp        ← 新增
  │ │   ├─ Engine.cpp             ← 新增
  │ │   ├─ SceneManager.cpp       ← 新增
  │ │   └─ Renderer.cpp           ← 新增 (核心拆分)
  │ └─ ... (其余不变)

caustica/editor/                       (应用层 - 大幅简化)
  ├─ caustica_app.h / .cpp             ← 新增: CausticaApp extends Application
  ├─ app/
  │   ├─ caustica.h / .cpp             ← 大幅精简：仅保留虚接口 + 粘合代码
  │   └─ AdvancedSample.h / .cpp       ← 不变(已很薄)
  ├─ SampleCommon/
  │   └─ SampleBaseApp.h / .cpp        ← 最终删除(合并到 CausticaApp)
  └─ ui/
      └─ SampleUI.h / .cpp             ← 精简: 只保留Panel, 渲染委托给ImGuiManager
```

---

## 二、详细拆分步骤

### Phase 6a: Engine 单例 — 时间步长与帧统计

**对标 DIVSHOT：** `engine/engine.h` — Singleton `Engine`，拥有 `TimeStep` 和 `Stats`

**新建文件：**

| 文件 | 行数估算 | 职责 |
|------|---------|------|
| `include/engine/Engine.h` | ~60 | Singleton 声明，Stats 结构体，帧率/FPS/时间访问器 |
| `src/engine/Engine.cpp` | ~40 | 单例创建，默认 targetFPS |

**提取来源：** `caustica.cpp` 和 `SampleBaseApp.cpp` 中的：
- `m_benchStart`, `m_benchLast`, `m_benchFrames` → `Engine::Stats`
- `GetAvgTimePerFrame()`, `GetFPSInfo()`, `GetResolutionInfo()` → `Engine`
- `m_fpsInfo` → `Engine::Stats`
- `m_frameIndex` → `Engine::frameIndex()`
- `SampleBaseApp::m_FPSLimiter` → `Engine::TimeStep`

**API 设计：**
```cpp
class Engine : public ThreadSafeSingleton<Engine>
{
public:
    struct Stats {
        uint32_t FramesPerSecond = 0;
        uint32_t UpdatesPerSecond = 0;
        double   FrameTimeMs = 0.0;
        uint64_t FrameIndex = 0;
    };
    Stats& statistics();
    float  targetFrameRate() const;
    TimeStep& getTimeStep();
    static Engine& get();
};
```

**风险：** 极低。纯数据提取，不改变行为。

---

### Phase 6b: entry_point.h — 统一入口点

**对标 DIVSHOT：** `engine/entry_point.h` — 平台分支的 `main()`/`WinMain()`

**新建文件：**

| 文件 | 行数估算 | 职责 |
|------|---------|------|
| `include/engine/entry_point.h` | ~40 | `#ifdef _WIN32` → `WinMain()`, `#else` → `main()`，调用 `coresystem::init()` + `createApplication()` + `run()` |

**修改文件：**

| 文件 | 变更 |
|------|------|
| `editor/app/AdvancedSample.cpp` | 当前的 `WinMain` 逻辑移到 `createApplication()` 实现 |
| `editor/app/caustica.cpp` | 从 caustica.cpp 提取出 `CausticaApp::init()` 作为 createApplication 的一部分 |

**目标代码：**
```cpp
// entry_point.h (简化版)
#ifdef _WIN32
extern caustica::Application* caustica::createApplication();
int WINAPI WinMain(...) {
    coresystem::init(__argc, __argv);
    auto* app = createApplication();
    app->run();
    delete app;
    coresystem::shutDown();
}
#else
// 类似 Unix 版本
#endif
```

**风险：** 低。只是重组现有入口代码。

---

### Phase 6c: SceneManager — 场景生命周期管理

**对标 DIVSHOT：** `scene/scene_manager.h` — 场景队列、切换、加载

这是从 `caustica.cpp` 提取 **场景管理** 相关代码。预计从 6151 行中提取 ~600 行。

**新建文件：**

| 文件 | 行数估算 | 职责 |
|------|---------|------|
| `include/engine/SceneManager.h` | ~80 | 场景管理器声明 |
| `src/engine/SceneManager.cpp` | ~500 | 场景加载/切换/卸载实现 |

**从 caustica.cpp 提取的方法（→ SceneManager）：**

| 方法 | 行数(约) | 说明 |
|------|---------|------|
| `SetCurrentScene()` | 120 | 场景切换主逻辑 |
| `LoadScene()` | 80 | 从JSON加载场景 |
| `SceneLoaded()` | 180 | 场景加载后处理：创建AS、材质、光照 |
| `SceneUnloading()` | 60 | 场景卸载清理 |
| `LoadMeshFile()` | 40 | 加载.obj网格 |
| `LoadGltfMeshFile()` | 50 | 加载glTF网格 |
| `LoadObjMeshFile()` | 30 | 加载OBJ网格 |
| `FinalizeRuntimeSceneMutation()` | 80 | 运行时场景变更 |
| `DeleteSceneNode()` | 30 | 删除场景节点 |
| `HandleDroppedFiles()` | 20 | 拖放文件处理 |
| `FindNodeByInstanceIndex()` | 20 | 按实例索引查找节点 |

**从 caustica.h 提取的成员变量（→ SceneManager）：**

```
m_sceneFilesAvailable, m_currentSceneName, m_currentScenePath
m_scene, m_sceneTime, m_lastDeltaTime
m_inlineSceneJson, m_selectedCameraIndex
```

**SceneManager API：**
```cpp
class SceneManager
{
public:
    void init(const std::shared_ptr<ShaderFactory>&);
    
    // Scene switching
    void setCurrentScene(const std::string& name, bool forceReload = false);
    void enqueueScene(std::shared_ptr<Scene> scene);
    void applySceneSwitch();
    bool isSwitchingScene() const;
    
    // Scene queries
    std::shared_ptr<Scene> getCurrentScene() const;
    const std::vector<std::string>& getAvailableScenes() const;
    std::string getCurrentSceneName() const;
    uint getCameraCount() const;
    
    // Scene mutation
    bool loadMeshFile(const std::filesystem::path&);
    bool loadGltfMeshFile(const std::filesystem::path&);
    void finalizeRuntimeMutation(std::shared_ptr<SceneGraphNode> root);
    bool deleteNode(std::shared_ptr<SceneGraphNode> node);
    void requestFullRebuild();
    
    // Scene time
    void setSceneTime(double t);
    double getSceneTime() const;
    double getLastDeltaTime() const;
    
    // Access
    std::shared_ptr<ExtendedScene>& scene() { return m_scene; }
    
private:
    std::shared_ptr<ExtendedScene> m_scene;
    std::vector<std::string> m_sceneFilesAvailable;
    std::string m_currentSceneName;
    std::filesystem::path m_currentScenePath;
    double m_sceneTime = 0.0;
    double m_lastDeltaTime = 0.0;
    // ... callback for SceneRender overrides
};
```

**依赖注入：** SceneManager 需要
- `GpuDevice*` — 用于创建加速结构
- `ShaderFactory*` — 用于着色器
- `BindingCache*` — 用于绑定
- `DescriptorTableManager*` — 用于描述符表

通过构造函数传入，而非通过 Sample 基类访问。

**风险：** 中等。场景加载是 caustica 最复杂的部分之一，涉及的 GPU 资源创建较多。需要仔细保持依赖注入的顺序。

---

### Phase 6d: Renderer — 主渲染器（最大拆分）

**对标 DIVSHOT：** `renderer/defered_renderer.h` — 拥有所有 Pass，管理渲染线程

这是最大的单项拆分。预计从 6151 行中提取 ~3000 行。

**新建文件：**

| 文件 | 行数估算 | 职责 |
|------|---------|------|
| `include/engine/Renderer.h` | ~150 | 主渲染器声明 |
| `src/engine/Renderer.cpp` | ~2500 | 渲染循环、Pass调度、资源管理 |
| `include/render/LightingBaker.h` | ~50 | 光照烘焙子系统（可选子模块） |
| `src/render/LightingBaker.cpp` | ~500 | 光照烘焙实现 |
| `include/render/CameraController.h` | ~40 | 相机管理子系统（可选子模块） |
| `src/render/CameraController.cpp` | ~200 | 相机管理实现 |
| `include/render/GaussianSplatManager.h` | ~50 | 3DGS管理子系统 |
| `src/render/GaussianSplatManager.cpp` | ~600 | 3DGS加载/排序/渲染 |

**从 caustica.cpp 提取到 Renderer 的方法：**

#### A. 渲染主循环 (→ Renderer)
| 方法 | 说明 |
|------|------|
| `Render(framebuffer)` | **主渲染入口**：更新view → 更新光照 → PathTrace → GaussianSplats → PostProcessAA |
| `PreRender()` | 帧前准备 |
| `PostProcessAA()` | 后处理反走样 |
| `PathTrace()` | 路径追踪调度 |
| `Denoise()` | 降噪调度 |
| `CreateRenderPasses()` | 创建所有渲染Pass |

#### B. 光照 (→ Renderer::LightingBaker 或直接留在 Renderer)
| 方法 | 说明 |
|------|------|
| `PreUpdateLighting()` | 光照预处理 |
| `UpdateLighting()` | 光照更新 |
| `FindMaterial()` | 材质查找 |
| `SetEnvMapOverrideSource()` | 环境贴图覆盖 |
| `RefreshEnvironmentMapMediaList()` | 环境贴图列表刷新 |

#### C. 相机 (→ Renderer::CameraController)
| 方法 | 说明 |
|------|------|
| `SaveCurrentCamera()`, `LoadCurrentCamera()` | 相机保存/加载 |
| `GetCurrentCameraPosDirUp()`, `SetCurrentCameraPosDirUp()` | 相机位置序列化 |
| `SetCameraVerticalFOV()`, `GetCameraVerticalFOV()` | FOV管理 |
| `SetCameraIntrinsics()`, `ClearCameraIntrinsics()` | 相机内参 |
| `UpdateCameraFromScene()` | 场景相机同步 |
| `UpdateViews()` | 视图矩阵更新 |
| `ComputeCameraJitter()` | TAA抖动计算 |

#### D. 加速结构 (→ Renderer)
| 方法 | 说明 |
|------|------|
| `CreateAccelStructs()` | 创建所有加速结构 |
| `RecreateAccelStructs()` | 重建加速结构 |
| `CreateBlases()` | 创建BLAS |
| `CreateTlas()` | 创建TLAS |
| `BuildTLAS()` | 构建TLAS |
| `RequestMeshAccelRebuild()` | 请求重建 |
| `RebuildDirtyMeshAccelStructs()` | 重建脏数据 |
| `UpdateSkinnedBLASs()` | 蒙皮BLAS更新 |
| `TransitionMeshBuffersToReadOnly()` | 缓冲区转换 |
| `DestroyOpacityMicromaps()` | OMM销毁 |
| `CreateOpacityMicromaps()` | OMM创建 |

#### E. 路径追踪管线 (→ Renderer)
| 方法 | 说明 |
|------|------|
| `FillPTPipelineGlobalMacros()` | PT着色器宏 |
| `CreatePTPipeline()` | PT管线创建 |
| `UpdatePathTracerConstants()` | 常量更新 |
| `PreUpdatePathTracing()` | PT前处理 |
| `PostUpdatePathTracing()` | PT后处理 |
| `RtxdiSetupFrame()` | RTXDI帧设置 |

#### F. 后处理/降噪 (→ Renderer)
| 方法 | 说明 |
|------|------|
| `StreamlinePreRender()` | Streamline预处理 |
| `NativeDLSSPreRender()` | DLSS预处理 |
| `DenoisedScreenshot()` | 降噪截图 |
| `ResetReferenceOIDN()`, `ApplyReferenceOIDN()` | OIDN处理 |
| `PostProcessPreToneMapping()` | 色调映射前处理 |
| `PostProcessPostToneMapping()` | 色调映射后处理 |
| `EvaluateNativeDLSS()` | DLSS评估 |
| `BackBufferResizing()` | 后台缓冲调整 |

#### G. 子实例管理 (→ Renderer)
| 方法 | 说明 |
|------|------|
| `UpdateSubInstanceContents()` | 子实例内容更新 |
| `UploadSubInstanceData()` | 子实例上传 |

#### H. 3D高斯泼溅 (→ Renderer 或 GaussianSplatManager)
| 方法 | 说明 |
|------|------|
| `LoadGaussianSplatFile()` | PLY加载 |
| `LoadGaussianSplatsFromScene()` | 场景中的3DGS加载 |
| `AttachGaussianSplatToScene()` | 附加到场景 |
| `PrepareGaussianSplatPass()` | Pass准备 |
| `RenderGaussianSplats()` | 3DGS渲染 |
| `AccumulateGaussianSplats()` | 3DGS累积 |
| `BuildGaussianSplatEmissionProxyList()` | 发射代理 |
| `UpdateGaussianSplatUIState()` | UI状态 |
| `GetGaussianSplatCount()` 系列 | 查询 |

**从 caustica.h 提取的成员变量：**

到大类划分：
- **Renderer** 核心: `m_renderTargets`, `m_commandList`, `m_constantBuffer`, `m_bindingLayout/Set`, `m_DescriptorTable`, `m_frameIndex`, `m_sampleIndex`, `m_accumulationSampleIndex`, `m_currentConstants`, `m_renderSize`, `m_displaySize`, `m_nrd[]`, `m_accumulationPass`, `m_oidnDenoiser`, `m_denoisingGuidesBaker`, `m_postProcess`, `m_temporalAntiAliasingPass`, `m_bloomPass`, `m_toneMappingPass`, `m_ptPipelineBaker`, `m_computePipelineBaker`, `m_ptPipeline*`, `m_rtxdiPass`, `m_subInstanceData/Buffer/Count`, `m_topLevelAS`, `m_meshesPendingAccelRebuild`
- **LightingBaker**: `m_lights`, `m_envMapBaker`, `m_lightsBaker`, `m_materialsBaker`, `m_ommBaker`, `m_envMapLocalPath`, `m_envMapOverride`, `m_envMapMediaFolder`, `m_envMapMediaList`, `m_envMapSceneParams`
- **CameraController**: `m_camera`, `m_view`, `m_viewPrevious`, `m_cameraVerticalFOV`, `m_cameraZNear`, `m_cameraZFar`, `m_cameraUseCustomIntrinsics`, `m_cameraIntrinsics`, `m_cameraIntrinsicsViewport`
- **GaussianSplatManager**: `m_gaussianSplatSceneObjects`, `m_gaussianSplatEmissionProxies`, `m_gaussianSplatFileNameSummary`, `m_initialGaussianSplatAttached`, `m_gaussianSplatCurrentColor`, `m_gaussianSplatAccumulatedColor`, `m_gaussianSplatAccumulationPass`, `m_gaussianSplatTemporalSampleIndex`, `m_gaussianSplatTemporalReset`, `m_gpuSort`
- **Debug drawing** (→ ImGuiManager 或 DebugRenderer): `m_feedback_Buffer_Gpu/Cpu`, `m_debugLineBuffer*`, `m_lines*`, `m_pick*`, `m_feedbackData`, `m_debugDeltaPathTree*`, `m_shaderDebug`, `m_zoomTool`, `m_exportVBufferCS/PSO`

**Renderer API 设计：**
```cpp
class Renderer
{
public:
    // Initialization
    bool init(GpuDevice& device, ShaderFactory& shaderFactory, 
              const CommandLineOptions& cmdLine);
    void shutdown();
    
    // Main render entry
    void render(nvrhi::IFramebuffer* framebuffer);
    
    // Scene integration
    void setScene(std::shared_ptr<Scene> scene);
    void onSceneLoaded();
    void onSceneUnloading();
    
    // Backbuffer resize
    void handleResize(uint32_t width, uint32_t height);
    
    // Sub-system access
    LightingBaker& getLighting() { return *m_lighting; }
    CameraController& getCamera() { return *m_camera; }
    GaussianSplatManager& getGaussianSplat() { return *m_gaussianSplat; }
    
    // Pipeline access
    PTPipelineBaker& getPTPipelineBaker() { return *m_ptPipelineBaker; }
    
private:
    void updateViews(nvrhi::IFramebuffer* fb);
    void pathTrace(nvrhi::IFramebuffer* fb, const SampleConstants& constants);
    void denoise(nvrhi::IFramebuffer* fb);
    void postProcessAA(nvrhi::IFramebuffer* fb, bool reset);
    void renderGaussianSplats(bool renderToOutput);
    
    GpuDevice* m_device;
    std::unique_ptr<RenderTargets> m_renderTargets;
    std::unique_ptr<LightingBaker> m_lighting;
    std::unique_ptr<CameraController> m_camera;
    std::unique_ptr<GaussianSplatManager> m_gaussianSplat;
    // ... 所有渲染成员
};
```

**LightingBaker / CameraController / GaussianSplatManager：**

这三个可以根据复杂度和时间决定是否独立文件，还是作为 Renderer 的 private 内嵌类。建议先作为独立文件，因为：
1. 修改时不需要重新编译 Renderer 的整个文件
2. 可以单独测试
3. 未来可以独立演进

**关于 SampleRenderCode 虚函数：**

当前的 `SampleRenderCode()` 在 AdvancedPathTracer 中被重写。这个虚函数需要保留，但改为通过回调或策略模式注入到 Renderer：

```cpp
// Option 1: 回调
using RenderCodeCallback = std::function<void(
    nvrhi::IFramebuffer*, nvrhi::CommandListHandle, const SampleConstants&)>;
Renderer::setRenderCodeCallback(RenderCodeCallback cb);

// Option 2: 保留虚函数在 Renderer 子类
class CausticaRenderer : public Renderer {
    virtual void renderCode(...) = 0;
};
```

**推荐 Option 1（回调注入）**，因为 DIVSHOT 也用类似的 frame packet 模式，且更灵活。

**风险：** 较高。这是最大的拆分，涉及 GPU 资源管理、渲染循环时序、Streamline/DLSS 集成。需要分小步进行，每步验证编译和运行。

**拆分策略：** 采用"套娃"方式
1. 先在 Renderer 中实现空壳 + 方法转发
2. Sample 调用 `m_renderer->render()` 而非直接执行
3. 逐步将方法实现从 Sample 移到 Renderer
4. 每次移动后编译验证

---

### Phase 6e: ImGuiManager — ImGui 生命周期管理

**对标 DIVSHOT：** `imgui/imgui_manager.h` — ImGui 上下文初始化、字体、后端渲染

**新建文件：**

| 文件 | 行数估算 | 职责 |
|------|---------|------|
| `include/imgui/ImGuiManager.h` | ~50 | ImGui管理器声明 |
| `src/imgui/ImGuiManager.cpp` | ~300 | ImGui初始化/渲染/事件 |

**提取来源：**
- `SampleUI.cpp` 中的 ImGui 初始化代码
- `SampleUI.h` 中的 `ImguiInit()`, `ImguiBackendInit()`, 字体加载
- `caustica.cpp` 中的 Debug drawing 基础设施

**ImGuiManager API：**
```cpp
class ImGuiManager
{
public:
    bool init(GpuDevice& device, nvrhi::IFramebuffer* fb);
    void shutdown();
    
    void newFrame();
    void render(ImGuiRenderCallback uiCallback);
    void handleEvent(Event& e);
    
    // Debug drawing (从Sample提取)
    // 3D picking
};
```

**从 SampleUI 提取（保留在 SampleUI）：**
- SampleUI 保留所有 Panel 定义和布局
- 只是 `ImguiInit()` / `ImguiBackendInit()` 移到 ImGuiManager
- SampleUI::Render() 变为纯 Panel 渲染回调

**风险：** 低。ImGui 初始化逻辑清晰独立。

---

### Phase 6f: 扩展 Application — 拥有所有子系统

**对标 DIVSHOT：** `application.h/cpp` — 清晰的 `init()` → `run()` → `frame()` 生命周期

**核心改动：** Application 不再通过 `friend` 访问 GpuDevice 内部。

**Application 新声明：**
```cpp
class Application
{
public:
    Application();
    virtual ~Application();
    
    // Lifecycle (virtual for subclasses)
    virtual bool init(int argc, const char* const* argv);
    virtual void shutdown();
    void run();
    
    // Single frame
    bool frame();
    
    // Subsystem access
    Window*          getWindow() const;
    Engine&          getEngine() const;
    SceneManager*    getSceneManager() const;
    Renderer*        getRenderer() const;
    ImGuiManager*    getImGuiManager() const;
    Input&           getInput() const;
    GpuDevice*       getGpuDevice() const;
    
protected:
    // Override points
    virtual void update(const TimeStep& dt);
    virtual void render();
    virtual void imguiRender();
    virtual bool handleEvent(Event& e);
    
    // Owned subsystems
    std::unique_ptr<GpuDevice>      m_gpuDevice;
    std::unique_ptr<Window>         m_window;
    std::unique_ptr<SceneManager>   m_sceneManager;
    std::unique_ptr<Renderer>       m_renderer;
    std::unique_ptr<ImGuiManager>   m_imguiManager;
    
    AppState m_state = AppState::Uninitialized;
};
```

**Application::frame() 实现：**
```cpp
bool Application::frame()
{
    auto& ts = Engine::get().getTimeStep();
    ts.update();
    
    m_window->onUpdate();
    updateWindowSize();
    
    if (m_window->isMinimized())
        return m_state != AppState::Closing;
    
    // Scene switching
    if (m_sceneManager->isSwitchingScene()) {
        m_renderer->waitIdle();
        m_sceneManager->applySceneSwitch();
        m_renderer->onSceneLoaded();
    }
    
    // Input
    m_input->poll();
    
    // ImGui new frame
    ImGui::NewFrame();
    m_imguiManager->newFrame();
    
    // Update
    update(ts);
    
    // Scene update
    if (auto* scene = m_sceneManager->getCurrentScene())
        scene->onUpdate(ts);
    
    // Render
    if (!m_window->isMinimized()) {
        render();
        imguiRender();
        m_imguiManager->render();
    }
    
    // Present
    m_window->updateCursorImgui();
    m_window->onUpdate();
    
    // Stats
    Engine::get().statistics().FrameTimeMs = ts.getMillis();
    
    return m_state != AppState::Closing;
}
```

**Application::run()：**
```cpp
void Application::run()
{
    while (frame()) { }
    shutdown();
}
```

**从 GpuDevice 中移除的 friend 依赖：**
- `m_windowVisible`, `m_windowIsInFocus` → Window 状态
- `m_DPIScaleFactorX/Y` → Window 管理
- `m_vRenderPasses` → Renderer 管理
- `m_FrameIndex` → Engine 管理
- `m_PreviousFrameTimestamp` → Engine::TimeStep 管理
- `m_Input` → Input 独立访问（已经是静态单例）

**风险：** 中等。GpuDevice 的 friend 访问需要逐个替换为新的子系统访问接口。

---

### Phase 6g: CausticaApp + 吸收 SampleBaseApp

**新建文件：**

| 文件 | 行数估算 | 职责 |
|------|---------|------|
| `editor/caustica_app.h` | ~50 | CausticaApp 声明 |
| `editor/caustica_app.cpp` | ~200 | caustica 特定的 Application 初始化 |

**删除/精简文件：**

| 文件 | 变更 |
|------|------|
| `editor/SampleCommon/SampleBaseApp.h` | 删除，合并到 CausticaApp |
| `editor/SampleCommon/SampleBaseApp.cpp` | 删除，合并到 CausticaApp |

**CausticaApp 实现：**
```cpp
class CausticaApp : public Application
{
public:
    bool init(int argc, const char* const* argv) override
    {
        // 1. Parse command line (from SampleBaseApp::ProcessCommandLine)
        // 2. Create GpuDevice (from SampleBaseApp::InitDeviceAndWindow)
        // 3. Create ShaderFactory (from SampleBaseApp::CreateShaderFactory)
        // 4. Create Window → GpuDevice::CreateDeviceAndSwapChain
        // 5. Create Renderer with pipeline config
        // 6. Create SceneManager → load initial scene
        // 7. Create ImGuiManager
        // 8. Register GLFW callbacks
        return true;
    }
    
    void update(const TimeStep& dt) override;
    void render() override;
    void imguiRender() override;
    
private:
    CommandLineOptions m_cmdLine;
    SampleUIData m_uiData;
    std::unique_ptr<SampleUI> m_ui; // Panel definitions only
};

// Defined in AdvancedSample.cpp
Application* createApplication() { return new CausticaApp(); }
```

**风险：** 低。主要是代码整合，不改变逻辑。

---

### Phase 6h: 精简 Sample 类

经过上面拆分后，Sample 类从 6151 行减少到约 500 行，变成一个薄层：

```cpp
class Sample : public caustica::SceneRender, public caustica::IInputHandler
{
public:
    Sample(CausticaApp& app);
    
    // Delegate to subsystems
    void Init(...)     { /* 委托给 CausticaApp */ }
    void Render(...)   { m_app.getRenderer()->render(fb); }
    
    // Virtual interface (kept for AdvancedPathTracer)
    virtual void SampleRenderCode(...) = 0;
    virtual void CreateRTPipelines() = 0;
    virtual void DestroyRTPipelines() = 0;
    virtual std::string GetMaterialSpecializationShader() const = 0;
    virtual bool NeedsRasterPrecompute() { return false; }
    
    // Input → delegates to CameraController
    bool KeyboardUpdate(...) override;
    bool MousePosUpdate(...) override;
    
    // Convenience accessors
    GpuDevice& getDevice();
    Renderer& getRenderer();
    SceneManager& getSceneManager();
    
    // UI data (shared with SampleUI)
    SampleUIData& getUIData() { return m_ui; }
    
protected:
    CausticaApp& m_app;
    SampleUIData& m_ui;
};
```

**风险：** 低。此时所有复杂逻辑已移走。

---

## 三、分步执行顺序与验证点

| 步骤 | 内容 | 预计行数变化 | 验证方式 |
|------|------|-------------|---------|
| **6a** | 新建 Engine 单例 | +100, -50 | 编译 + FPS 显示不变 |
| **6b** | 新建 entry_point.h | +40, -10 | 编译 + 启动正常 |
| **6c** | 新建 SceneManager | +600, -600 | 编译 + 场景加载正常 |
| **6d.1** | 新建 Renderer(空壳+转发) | +200, -50 | 编译 + 渲染不变 |
| **6d.2** | 迁移光照到 Renderer | +500, -500 | 编译 + 光照不变 |
| **6d.3** | 迁移相机到 Renderer | +200, -200 | 编译 + 相机操作不变 |
| **6d.4** | 迁移加速结构 | +400, -400 | 编译 + RT正常 |
| **6d.5** | 迁移路径追踪 | +500, -500 | 编译 + PT正常 |
| **6d.6** | 迁移后处理/降噪 | +400, -400 | 编译 + 降噪正常 |
| **6d.7** | 迁移3DGS | +600, -600 | 编译 + 3DGS正常 |
| **6e** | 新建 ImGuiManager | +300, -200 | 编译 + UI正常 |
| **6f** | 扩展 Application | +300, -150 | 编译 + 完整流程 |
| **6g** | CausticaApp + 吸收 SampleBaseApp | +250, -700 | 编译 + 启动正常 |
| **6h** | 精简 Sample | +100, -400 | 编译 + 所有功能正常 |
| | **总计** | ~+4490, ~-4760 | caustica.cpp: 6151→~1000行 |

---

## 四、关键设计决策

### 4.1 依赖注入 vs 继承

**当前：** Sample 通过继承 `SceneRender` 获得 `GpuDevice&` 访问。
**目标：** Renderer / SceneManager 通过构造函数注入 `GpuDevice&`、`ShaderFactory&`。

```cpp
// 旧 (继承链)
Sample : SceneRender : IRenderPass {
    GetGpuDevice(); // 来自基类
}

// 新 (依赖注入)
Renderer::Renderer(GpuDevice& device, ShaderFactory& sf, 
                   DescriptorTableManager& dtm, BindingCache& bc)
    : m_device(&device), m_shaderFactory(&sf) { ... }
```

### 4.2 虚函数 vs 回调

对于 `SampleRenderCode()` 这类需要子类重写的方法，改用回调注入：

```cpp
// 旧
virtual void SampleRenderCode(fb, cmdList, constants) = 0;

// 新
using RenderCodeFn = std::function<void(nvrhi::IFramebuffer*, 
    nvrhi::CommandListHandle, const SampleConstants&)>;
void Renderer::setRenderCode(RenderCodeFn fn) { m_renderCodeFn = std::move(fn); }
```

### 4.3 子系统的生命周期

```
Application::~Application()
    → m_renderer.reset()       // 等待GPU闲置, 销毁Pass
    → m_imguiManager.reset()   // 销毁ImGui上下文
    → m_sceneManager.reset()   // 卸载场景
    → m_shaderFactory.reset()  // 释放着色器
    → m_gpuDevice.reset()      // 销毁GPU设备
    → m_window.reset()         // 销毁窗口 (必须在GPU之后)
```

### 4.4 是否需要渲染线程？

DIVSHOT 的 `DeferedRenderer` 在独立线程上运行渲染。对于 caustica 而言：
- **收益：** 渲染不阻塞主线程（ImGui交互更流畅）
- **风险：** 需要线程安全的所有 GPU 资源访问，改动巨大
- **建议：** 本次暂不引入渲染线程，保持单线程渲染。Renderer 提供 `waitIdle()` 接口但不实际切换线程。

---

## 五、不改变的部分

- **着色器系统** — `shaders/`、`shaders.cfg`、`PTPipelineBaker`、`ComputePipelineBaker`
- **NVRHI** — `backend/rhi/` 保持不变
- **GpuDevice** — `backend/GpuDevice` 核心逻辑不变，只移除窗口/消息循环相关代码
- **RenderPass 接口** — 各 Pass 保持 `IRenderPass` 接口
- **外部依赖** — External/、thirdparty/
- **Python 绑定** — 更新引用路径
- **SampleGame** — 保留在 editor/

---

## 六、风险总览

| 风险 | 等级 | 缓解策略 |
|------|------|---------|
| Renderer 拆分导致渲染循环时序变化 | 高 | 分步迁移，每步验证渲染输出 |
| GpuDevice friend 访问移除后编译失败 | 中 | 先补齐 GpuDevice 的 public API |
| Streamline/DLSS 集成断裂 | 中 | 保留现有 StreamlineIntegration 调用路径 |
| 场景加载流程断裂 | 中 | SceneManager 提取时保持相同的加载顺序 |
| ImGui 字体/后端初始化遗漏 | 低 | ImGuiManager 提取只需移动代码 |
| Python 绑定编译失败 | 低 | 更新 include 路径 |
| 调试绘图（Debug Lines）断裂 | 低 | 作为独立子模块迁移 |
