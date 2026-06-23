# caustica → 四层架构改造方案

## 一、现状分析

### 1.1 caustica 当前架构 vs DIVSHOT 四层架构对比

| 层级 | DIVSHOT (diverse) | caustica (当前) | 差距 |
|------|------------------|----------------|------|
| **第1层 平台抽象** | `OS`/`Window`/`FileSystem`/`Timer` 清晰抽象基类 + 平台实现 | `DeviceManager` 混合了窗口+GPU+消息循环；直接依赖 GLFW | ❌ 无独立平台层 |
| **第2层 核心系统** | `coresystem::init()` 统一初始化：日志→内存→JobSystem→命令行 | `log.cpp`/`chunk`/`json` 散落各处，无统一初始化 | ❌ 无统一核心层 |
| **第3层 应用层** | `Application` 拥有 Window/Timer/ImGuiManager/SceneManager/DeferedRenderer，`run()`→`frame()` 清晰主循环 | `SampleBaseApp` + `Application` + `DeviceManager::RunMessageLoop()` 职责分散 | ⚠️ 部分有，职责不清 |
| **第4层 渲染层** | `DeferedRenderer` + `rg::RenderGraph` + `drs_rhi`（自研RHI） | `nvrhi`（成熟RHI）+ `Sample::Render()` + 各 RenderPass | ✅ RHI层成熟，上层可改进 |

### 1.2 caustica 当前源码结构

```
caustica/
├── editor/
│   ├── app/
│   │   ├── AdvancedSample.cpp    ← WinMain/main 入口
│   │   ├── AdvancedSample.h
│   │   ├── caustica.cpp          ← Sample 类 (263KB 巨型文件!)
│   │   └── caustica.h            ← Sample 类声明
│   ├── SampleCommon/
│   │   ├── SampleBaseApp.cpp/h   ← 应用初始化+Device管理+主循环
│   │   ├── CommandLine.cpp/h
│   │   ├── PTPipelineBaker.cpp/h
│   │   └── ...
│   ├── SampleGame/               ← 游戏逻辑
│   └── ui/SampleUI.cpp/h         ← ImGui UI
│
├── caustica/
│   ├── include/
│   │   ├── core/                 ← chunk/json/log/string_utils/vfs
│   │   ├── math/                 ← vector/matrix/quat/box/sphere...
│   │   ├── engine/               ← Application/DeviceManager/Scene/ShaderFactory...
│   │   ├── render/               ← Bloom/RTXDI/NRD/TAA/GBuffer...
│   │   └── backend/rhi/          ← nvrhi 公共头
│   ├── src/
│   │   ├── core/                 ← core 实现
│   │   ├── math/                 ← math 实现
│   │   ├── engine/               ← engine 实现 + DeviceManager_DX11/DX12/VK
│   │   ├── render/               ← render 实现
│   │   └── backend/rhi/          ← nvrhi 后端实现 (vulkan/d3d11/d3d12)
│   └── shaders/                  ← HLSL 着色器
│
├── Python/                       ← Python 绑定
├── External/                     ← 第三方库 (nvrhi源码移到了backend)
└── thirdparty/                   ← DirectX-Headers, Vulkan-Headers
```

### 1.3 当前启动流程

```
WinMain() / main()                              [AdvancedSample.cpp]
  │
  ├─ DeviceManager::Create(api)                 [SampleBaseApp 构造函数]
  │
  ├─ SampleBaseApp::Init(argc, argv)
  │   ├─ ProcessCommandLine()
  │   ├─ InitDeviceAndWindow()
  │   │   └─ DeviceManager::CreateWindowDeviceAndSwapChain()
  │   │       ├─ glfwInit() + glfwCreateWindow()
  │   │       ├─ CreateInstanceInternal()  (Vulkan/D3D12实例)
  │   │       ├─ CreateDevice()            (GPU设备)
  │   │       └─ CreateSwapChain()
  │   ├─ CreateShaderFactory()
  │   ├─ CreateMainRenderPass() → new Sample(deviceManager)
  │   │   └─ Sample::Init()
  │   ├─ DeviceManager::AddRenderPassToBack(sample)
  │   └─ DeviceManager::AddRenderPassToBack(uiRender)
  │
  ├─ SampleBaseApp::RunMainLoop()
  │   └─ DeviceManager::RunMessageLoop()
  │       └─ while(!glfwWindowShouldClose)
  │           ├─ glfwPollEvents()
  │           ├─ Animate(dt)
  │           ├─ Render()
  │           │   └─ for each IRenderPass: pass->Render(framebuffer)
  │           └─ Present()
  │
  └─ SampleBaseApp::End()
```

### 1.4 核心问题总结

1. **`caustica.cpp` 263KB** — Sample 类承载了几乎所有业务逻辑（渲染、场景、资源、相机、输入），严重违反单一职责
2. **无平台抽象层** — GLFW 直接暴露到应用层，`DeviceManager` 同时管理窗口+GPU+消息循环
3. **初始化流程分散** — 构造函数、`Init()`、`Sample::Init()` 三段式初始化，生命周期管理混乱
4. **主循环由 DeviceManager 掌控** — 而非 Application，不符合应用层控制主循环的模式
5. **`IRenderPass` 模式过重** — 应用逻辑和渲染逻辑都通过 RenderPass 接口混在一起

---

## 二、改造方案

### 2.1 目标目录结构

```
caustica/
├── source/                          ← 新的源码根目录
│   ├── platform/                    ← 第1层：平台抽象层
│   │   ├── os.h                     ← OS 抽象基类
│   │   ├── window.h                 ← Window 抽象基类
│   │   ├── file_system.h            ← 文件系统抽象
│   │   ├── timer.h                  ← 定时器抽象
│   │   ├── windows/
│   │   │   ├── windows_os.cpp/h
│   │   │   ├── windows_window.cpp/h
│   │   │   └── windows_timer.cpp/h
│   │   ├── unix/
│   │   │   ├── unix_os.cpp/h
│   │   │   └── ...
│   │   └── glfw/
│   │       └── glfw_window.cpp/h    ← GLFW 窗口实现
│   │
│   ├── core/                        ← 第2层：核心系统层
│   │   ├── core_system.h/cpp        ← 统一初始化/关闭
│   │   ├── job_system.h/cpp         ← 任务调度（已有 ThreadPool 可改造）
│   │   ├── memory_manager.h/cpp     ← 内存管理
│   │   ├── command_line.h/cpp       ← 命令行解析
│   │   ├── log.h/cpp                ← 日志（已有，迁移）
│   │   ├── arena.h/cpp              ← 线性分配器
│   │   ├── vfs/                     ← 虚拟文件系统（已有，迁移）
│   │   ├── chunk/                   ← chunk 序列化（已有，迁移）
│   │   └── json.h/cpp               ← JSON（已有，迁移）
│   │
│   ├── engine/                      ← 第3层：应用/引擎层
│   │   ├── application.h/cpp        ← Application 主控类（改造）
│   │   ├── entry_point.h            ← main 入口（平台分支）
│   │   ├── engine.h/cpp             ← 引擎单例 + 统计
│   │   ├── scene_manager.h/cpp      ← 场景管理（新增）
│   │   ├── scene.h/cpp              ← 场景（已有，迁移）
│   │   ├── scene_graph.h/cpp        ← 场景图（已有，迁移）
│   │   ├── input.h/cpp              ← 输入管理（新增）
│   │   ├── events/                  ← 事件系统（新增）
│   │   │   ├── event.h
│   │   │   ├── key_event.h
│   │   │   └── mouse_event.h
│   │   └── imgui_manager.h/cpp      ← ImGui 管理（从 SampleUI 提取）
│   │
│   ├── render/                      ← 第4层：渲染层
│   │   ├── renderer.h/cpp           ← 主渲染器（从 caustica.cpp 拆分）
│   │   ├── render_graph/            ← 渲染图系统（可选新增）
│   │   ├── rhi/                     ← RHI 封装（现有 nvrhi，加薄封装）
│   │   ├── passes/                  ← 各渲染 Pass
│   │   │   ├── rtxdi/
│   │   │   ├── nrd/
│   │   │   ├── gbuffer.cpp/h
│   │   │   ├── lighting.cpp/h
│   │   │   ├── shadow.cpp/h
│   │   │   ├── taa.cpp/h
│   │   │   ├── bloom.cpp/h
│   │   │   ├── post_process.cpp/h
│   │   │   └── ...
│   │   └── shader_factory.h/cpp     ← 着色器管理（已有，迁移）
│   │
│   ├── scene/                       ← 场景相关
│   │   ├── camera/
│   │   ├── components/
│   │   └── ...
│   │
│   ├── math/                        ← 数学库（已有，基本不变）
│   ├── assets/                      ← 资产系统（已有部分）
│   └── editor/                      ← 编辑器/应用入口（独立）
│       ├── caustica_app.cpp/h       ← 新 Application 子类
│       └── ui/
│
├── External/                        ← 第三方库（不变）
└── thirdparty/                      ← 第三方头文件（不变）
```

### 2.2 分层改造详细方案

---

#### 第1层改造：平台抽象层 `source/platform/`

**现状**：`DeviceManager` 直接调用 GLFW，平台API混在引擎代码中。

**改造**：创建清晰的分层抽象。

**需要新建的文件**：

| 文件 | 说明 |
|------|------|
| `platform/os.h` | OS 抽象基类：`run()`/`getExecutablePath()`/`openURL()` 等虚函数 |
| `platform/window.h` | Window 抽象基类：`WindowDesc` 结构体、`Window::create()` 工厂方法 |
| `platform/file_system.h` | FileSystem 单例（从 `core/vfs/` 迁移，去掉虚拟文件系统耦合） |
| `platform/timer.h` | 平台无关 Timer 抽象 |
| `platform/windows/windows_os.cpp/h` | Windows OS 实现 |
| `platform/glfw/glfw_window.cpp/h` | GLFW Window 实现（封装现有 GLFW 调用） |
| `platform/windows/windows_timer.cpp/h` | Windows 高性能定时器 |

**需要改造的文件**：

| 文件 | 改造内容 |
|------|---------|
| `caustica/src/engine/DeviceManager.cpp` | 拆分：窗口创建 → `platform/`，GPU设备管理 → `render/rhi/`，消息循环 → `engine/application.cpp` |
| `caustica/include/engine/DeviceManager.h` | 同上拆分 |
| `caustica/src/engine/dx12/DeviceManager_DX12.cpp` | GPU创建部分保留在渲染层 |
| `caustica/src/engine/vulkan/DeviceManager_VK.cpp` | GPU创建部分保留在渲染层 |
| `caustica/src/engine/dx11/DeviceManager_DX11.cpp` | GPU创建部分保留在渲染层 |

---

#### 第2层改造：核心系统层 `source/core/`

**现状**：核心工具分散在 `core/`、功能不完整、无统一初始化入口。

**改造**：创建 `CoreSystem` 统一初始化入口，补充缺失的基础设施。

**需要新建的文件**：

| 文件 | 说明 |
|------|------|
| `core/core_system.h/cpp` | 统一初始化：`coresystem::init(argc, argv)` → 日志→内存→JobSystem→命令行→FileSystem |
| `core/job_system.h/cpp` | 多线程任务系统（复用现有 `ThreadPool` 代码，改造成更通用的 JobSystem） |
| `core/memory_manager.h/cpp` | 内存跟踪/统计（新增） |
| `core/arena.h/cpp` | 线性内存分配器（新增，DIVSHOT参考实现） |
| `core/command_line.h/cpp` | 命令行解析封装（从 `SampleCommon/CommandLine` 提取核心部分） |

**需要迁移/改造的文件**：

| 文件 | 改造内容 |
|------|---------|
| `caustica/src/core/log.cpp` → `core/log.cpp` | 移动到新位置，接口不变 |
| `caustica/src/core/json.cpp` → `core/json.cpp` | 移动到新位置 |
| `caustica/src/core/chunk/*` → `core/chunk/` | 移动到新位置 |
| `caustica/src/core/vfs/*` → `core/vfs/` | 移动到新位置 |
| `caustica/include/engine/ThreadPool.h/cpp` | 提取公共部分到 `JobSystem`，渲染专用部分保留 |
| `caustica/include/engine/Timer.h` | 拆分：平台无关部分 → `platform/timer.h`，平台实现 → `platform/windows/windows_timer.cpp` 等 |

---

#### 第3层改造：应用/引擎层 `source/engine/`

**现状**：`SampleBaseApp` + `Application` + `DeviceManager` 职责混乱，263KB 的 `caustica.cpp` 承载一切。

**改造**：创建清晰的 `Application` 主控类，拥有所有子系统，掌控主循环。

**需要新建的文件**：

| 文件 | 说明 |
|------|------|
| `engine/entry_point.h` | 平台统一的 `main()` 入口（参考 DIVSHOT `entry_point.h`） |
| `engine/engine.h/cpp` | 引擎单例：帧率/时间步长/统计（参考 DIVSHOT `Engine` 类） |
| `engine/application.h/cpp` | 改造后的 Application：拥有 Window/Timer/ImGuiManager/Renderer/SceneManager |
| `engine/scene_manager.h/cpp` | 场景管理器：场景队列、切换、加载（新增独立类） |
| `engine/input.h/cpp` | 输入管理器：键盘/鼠标状态轮询（新增） |
| `engine/events/event.h` | 事件基类 |
| `engine/events/key_event.h` | 键盘事件 |
| `engine/events/mouse_event.h` | 鼠标事件 |
| `engine/imgui_manager.h/cpp` | ImGui 管理器：初始化/渲染/事件处理（从 SampleUI 提取） |

**需要重构的文件**：

| 文件 | 改造内容 |
|------|---------|
| `editor/app/caustica.cpp` (263KB) | **大幅拆分**：渲染逻辑 → `render/renderer.cpp`，相机逻辑 → `scene/camera/`，场景加载 → `engine/scene_manager.cpp`，资源管理 → `assets/`，Sample 类保留为 Application 子类 |
| `editor/app/caustica.h` | Sample 类改为继承新 `Application`，只保留应用特定逻辑 |
| `editor/SampleCommon/SampleBaseApp.cpp/h` | **可以删除**，初始化逻辑合并到 `Application::init()` + `entry_point.h` |
| `caustica/src/engine/Application.cpp` | 改造为主体框架，添加 `run()` → `frame()` 主循环 |
| `caustica/include/engine/Application.h` | 添加 `Window`/`Timer`/`ImGuiManager`/`Renderer`/`SceneManager` 成员 |
| `caustica/src/engine/Scene.cpp/h` | 保持基本结构，与 `SceneManager` 整合 |
| `caustica/src/engine/SceneGraph.cpp/h` | 保持不变 |

---

#### 第4层改造：渲染层 `source/render/`

**现状**：NVRHI 已经是成熟的 RHI 抽象，RenderPass 模式可用但需要整理。

**改造**：从 `caustica.cpp` 拆分出独立的主渲染器，RenderPass 保持模块化。

**需要新建的文件**：

| 文件 | 说明 |
|------|------|
| `render/renderer.h/cpp` | 主渲染器：拥有渲染线程、管理 Pass 管线、提供 `build_frame_packet()` / `enqueue_frame_packet()` 接口 |
| `render/rhi/device_manager.h/cpp` | 从 `DeviceManager` 拆分出的纯 GPU 设备管理（不含窗口/消息循环） |
| `render/render_graph/graph.h/cpp` | (可选) 渲染图系统，管理 Pass 依赖和资源生命周期 |

**需要迁移/改造的文件**：

| 文件 | 改造内容 |
|------|---------|
| `caustica/src/render/*.cpp` → `render/passes/` | 各 RenderPass 文件移动到统一目录，接口标准化 |
| `caustica/include/render/*.h` → `render/passes/` | 头文件同步移动 |
| `caustica/src/engine/DeviceManager.cpp` | 保留 GPU 设备创建/交换链管理，窗口/消息循环移出 |
| `caustica/src/engine/dx12/DeviceManager_DX12.cpp` | 保留在渲染层 |
| `caustica/src/engine/vulkan/DeviceManager_VK.cpp` | 保留在渲染层 |
| `caustica/src/engine/ShaderFactory.cpp/h` | 迁移到 `render/shader_factory.cpp/h` |
| `caustica/src/engine/CommonRenderPasses.cpp/h` | 迁移到 `render/passes/common.cpp/h` |
| `caustica/src/engine/BindingCache.cpp/h` | 迁移到 `render/binding_cache.cpp/h` |
| `caustica/src/engine/DescriptorTableManager.cpp/h` | 迁移到 `render/descriptor_table.cpp/h` |
| `caustica/src/engine/FramebufferFactory.cpp/h` | 迁移到 `render/framebuffer_factory.cpp/h` |
| `editor/SampleCommon/RenderTargets.cpp/h` | 迁移到 `render/render_targets.cpp/h` |

---

### 2.3 改造后的启动流程

```
main() / WinMain()                              [platform/entry_point.h]
  │
  ├─[1] coresystem::init(argc, argv)            ← 第2层：日志/内存/JobSystem/命令行
  │
  ├─[2] new WindowsOS()                         ← 第1层：平台OS
  ├─[3] OS::setInstance(windowsOS)
  ├─[4] windowsOS->init()
  │
  ├─[5] createApplication()                     ← 用户定义，返回 Application 子类
  │     └─ new CausticaApp()                    [editor/caustica_app.cpp]
  │
  ├─[6] windowsOS->run()
  │     ├─ app.init()                           ← 第3层初始化
  │     │   ├─ SceneManager 创建
  │     │   ├─ Engine::get() 创建
  │     │   ├─ Window::create(desc)             → 第1层
  │     │   ├─ rhi::DeviceManager::create(api)  → 第4层
  │     │   ├─ Renderer 创建 + 渲染线程启动
  │     │   ├─ ImGuiManager 初始化
  │     │   └─ 场景加载
  │     │
  │     └─ app.run()                            ← 第3层主循环
  │         └─ while(frame())
  │             ├─ update() → Scene::on_update()
  │             ├─ imgui_render()
  │             ├─ render() → Renderer → 第4层
  │             └─ window->on_update()
  │
  └─[7] coresystem::shut_down()                 ← 清理
```

### 2.4 新增 CMake 目标结构

```cmake
# 第1层：平台抽象库
add_library(causticaPlatform STATIC
    source/platform/window.cpp
    source/platform/file_system.cpp
    source/platform/timer.cpp
    source/platform/windows/windows_os.cpp
    source/platform/windows/windows_timer.cpp
    source/platform/glfw/glfw_window.cpp
)
target_link_libraries(causticaPlatform PUBLIC glfw)

# 第2层：核心系统库
add_library(causticaCore STATIC
    source/core/core_system.cpp
    source/core/job_system.cpp
    source/core/memory_manager.cpp
    source/core/command_line.cpp
    source/core/log.cpp
    source/core/json.cpp
    source/core/arena.cpp
    source/core/chunk/*.cpp
    source/core/vfs/*.cpp
)
target_link_libraries(causticaCore PUBLIC causticaPlatform)

# 第3层：引擎库
add_library(causticaEngine STATIC
    source/engine/engine.cpp
    source/engine/application.cpp
    source/engine/scene_manager.cpp
    source/engine/scene.cpp
    source/engine/scene_graph.cpp
    source/engine/input.cpp
    source/engine/imgui_manager.cpp
    source/math/*.cpp
    source/scene/camera/*.cpp
    source/scene/components/*.cpp
)
target_link_libraries(causticaEngine PUBLIC causticaCore imgui)

# 第4层：渲染库
add_library(causticaRender STATIC
    source/render/renderer.cpp
    source/render/rhi/device_manager.cpp
    source/render/shader_factory.cpp
    source/render/binding_cache.cpp
    source/render/passes/*.cpp
)
target_link_libraries(causticaRender PUBLIC causticaEngine nvrhi NRD Rtxdi)

# 应用可执行文件
add_executable(caustica
    source/editor/caustica_app.cpp
    source/editor/ui/*.cpp
)
target_link_libraries(caustica PRIVATE causticaRender)
```

---

## 三、改造阶段规划

### 阶段A：基础设施搭建（1-2周）
```
□ 创建 source/platform/ 目录结构
□ 实现 OS/Window/FileSystem/Timer 四个抽象基类
□ 实现 WindowsOS / GlfwWindow / WindowsTimer
□ 创建 source/core/core_system.h/cpp 统一初始化入口
□ 实现 JobSystem（复用 ThreadPool 代码）
□ 实现 Arena 线性分配器
□ 实现 CommandLine 封装
□ 编写 entry_point.h
```

### 阶段B：引擎层重组（1-2周）
```
□ 重构 Application 类：添加 Window/Timer/ImguiManager/Renderer/SceneManager 成员
□ 创建 SceneManager 类（从 caustica.cpp 拆分场景管理逻辑）
□ 创建 Input 管理类
□ 创建 Event 事件系统
□ 创建 ImGuiManager 类（从 SampleUI 提取）
□ 迁移 Scene/SceneGraph 到 source/engine/
□ 迁移 math 到 source/math/
```

### 阶段C：渲染层拆分（2-3周）
```
□ 从 DeviceManager 分离出纯 GPU 设备管理 → render/rhi/
□ 创建主 Renderer 类（从 caustica.cpp 拆分渲染主循环）
□ 按 Pass 类型整理 render/passes/ 目录结构
□ 迁移所有 RenderPass 到统一接口
□ 迁移 ShaderFactory / BindingCache / CommonRenderPasses
□ 可选的 RenderGraph 系统
```

### 阶段D：应用层收尾（1周）
```
□ 删除/重构 SampleBaseApp（合并到 Application）
□ 重构 Sample 类为 Application 子类（只保留应用逻辑）
□ 将 caustica.cpp 从 263KB 拆分到对应的引擎/渲染模块
□ 更新 CMakeLists.txt 为四层目标结构
□ 编译验证 + 运行测试
```

---

## 四、风险与注意事项

| 风险 | 应对 |
|------|------|
| **caustica.cpp 263KB 拆分风险大** | 按功能模块逐步提取：先渲染→再场景→再UI，每步验证编译 |
| **DeviceManager 职责拆分可能影响 Streamline/DLSS 集成** | 保留 Streamline 相关代码在渲染层，平台层只抽象窗口/消息循环 |
| **nvrhi 原先在 External 后来移到 backend，路径依赖复杂** | 保持 nvrhi include 路径不变，渲染层 `render/rhi/` 是对 nvrhi 的薄封装而非替代 |
| **Python 绑定依赖现有类结构** | 每个阶段完成后更新 Python 绑定 |
| **GLFW 回调直接耦合 DeviceManager** | 在 GlfwWindow 中处理 GLFW 回调，转换为内部 Event，再分发给 Application |

---

## 五、不需要改动的部分

以下部分质量较高，无需在本次重构中改动：

- `External/` 所有第三方库
- `thirdparty/` DirectX-Headers, Vulkan-Headers
- `source/math/` 数学库（仅移动位置）
- `source/core/vfs/` 虚拟文件系统（仅移动位置）
- `source/core/chunk/` chunk 序列化（仅移动位置）
- `source/core/json.cpp` JSON 处理（仅移动位置）
- `source/core/log.cpp` 日志系统（仅移动位置）
- `source/backend/rhi/` NVRHI 后端实现（仅移动位置到 render/rhi/）
- `caustica/shaders/` 着色器源码
- `source/scene/components/` 场景组件
- Python 绑定（更新引用路径即可）
