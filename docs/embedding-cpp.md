# Embedding Caustica from C++

`EngineApp` is the supported high-level entry point for a C++ host. It owns or
accepts the graphics device, installs the default engine plugins, runs the
application schedules, and exposes focused scene, camera, and render-session
APIs. New hosts should not assemble `DefaultPlugins`, reach through
`GpuRenderSubsystem`, or drive `WorldRenderer` directly.

The complete in-tree example is
[`application/samples/thin_client/Main.cpp`](../application/samples/thin_client/Main.cpp);
its CMake target is `caustica_thin_client`.

## Lifecycle

For a host that only needs the default engine:

```cpp
#include <engine/EngineApp.h>

int main()
{
    caustica::initializeAppPlatform();

    auto engine = caustica::EngineApp::create({
        .width = 1280,
        .height = 720,
        .scene = "default.json",
        .windowTitle = "My Caustica Host",
    });

    if (!engine)
    {
        caustica::shutdownAppPlatform();
        return 1;
    }

    engine->run();
    caustica::shutdownAppPlatform();
    return 0;
}
```

`EngineApp::run()` enters the normal window loop and shuts the engine down when
the loop exits. For a host-owned loop, call `stepFrame()` and then `shutdown()`:

```cpp
auto engine = caustica::EngineApp::create({
    .headless = true,
    .scene = "default.json",
});

while (engine && keepRunning)
    engine->stepFrame(fixedDeltaSeconds);

if (engine)
    engine->shutdown();
```

Headless mode creates offscreen back buffers, passes no window to `App`, and
uses the single-threaded render path. `stepFrame()` with no argument uses the
engine clock; a non-negative argument supplies a fixed elapsed time.

## `EngineAppDesc`

Frequently used fields:

| Field | Default | Purpose |
| --- | --- | --- |
| `width`, `height` | `1920`, `1080` | Back-buffer size. |
| `headless` | `false` | Create an offscreen device without a window. |
| `dedicatedRenderThread` | `true` | Pipeline render work on a dedicated thread; ignored in headless mode. |
| `debugDevice` | `false` | Enable backend debug support. |
| `adapterIndex` | `-1` | GPU index; negative means automatic selection. |
| `useVulkan` | `false` | Select Vulkan when both Vulkan and DirectX 12 were compiled. |
| `fullscreen` | `false` | Start the owned window fullscreen. |
| `scene` | `default.json` | Initial scene. |
| `runtimeDirectory` | auto | Directory containing cooked shaders and runtime libraries. |
| `resourceRoot` | auto | Directory containing `Assets/`. |
| `device`, `window` | `nullptr` | Inject host-owned objects; `EngineApp` does not take ownership. |
| `finishStartup` | `true` | Set false to register host plugins/systems before startup schedules run. |

`viewState`, `diagnostics`, `renderState`, and `cmdLine` can point to host-owned
state. `preGpuDeviceInit` runs just before an owned device is created.

## Registering simulation systems

When adding systems or plugins during initialization, create with
`finishStartup=false`, register them, and call `finishStartup()` exactly once:

```cpp
#include <engine/EngineApp.h>
#include <engine/SceneSpawn.h>
#include <engine/SceneTransform.h>

struct HostSimulationLabel
{
    static constexpr const char* name = "Host.Simulation";
};

auto engine = caustica::EngineApp::create({
    .scene = "default.json",
    .finishStartup = false,
});

engine->app().addSystem<HostSimulationLabel>(
    caustica::AppSchedule::update,
    [](caustica::SystemContext& ctx) {
        if (!caustica::isSceneLoaded(ctx.app))
            return;

        // Mutate host state or the logic-side scene here.
        // Extract publishes the render-thread snapshot after update/PostUpdate.
    });

if (!engine->finishStartup())
    return 1;

engine->run();
```

An `update` system with no explicit set joins
`system_set::Simulation`. The per-frame order is:

```text
First → preUpdate → update → PostUpdate → Extract → dispatch render → postRender → Last
```

`Startup` runs once during `finishStartup()` and `shutdown` runs during engine
shutdown. Transform propagation is the tail of `PostUpdate`; Extract publishes
the ECS-free render snapshot afterward. The `render` schedule executes in the
render-thread domain when the dedicated thread is active. `postRender` and
`Last` continue on the logic thread after dispatch and do not imply that the
asynchronous render work has completed.

Systems can request typed parameters (`Res<T>`, `ResMut<T>`, `Commands`, or
`SystemContext`) or take a `SystemContext&`. Use
`AppSystemOrdering::runBefore`, `runAfter`, and `inSet` for explicit ordering.

## Scene access and mutation

Use the focused application headers:

| Header | Supported operations |
| --- | --- |
| `engine/SceneQuery.h` | Active scene, scene ECS, load status, materials, and entity lookup. |
| `engine/SceneSpawn.h` | `load`, `spawn`, `spawnFromFile`, and `despawn`. |
| `engine/SceneTransform.h` | Local transform, translation, and visibility. |
| `engine/SceneMeshEdit.h` | Vertex reads/deformation and geometry-sequence playback. |
| `engine/CameraApi.h` | Camera selection state, pose, FOV, and intrinsics. |
| `engine/SceneLifecycle.h` | Scene selection/reload operations. |
| `engine/RenderSessionApi.h` | Session-level render controls. |
| `engine/RenderFrameApi.h` | Accumulation and rendered-frame access. |

Example runtime spawn:

```cpp
caustica::ecs::Entity entity =
    caustica::spawnFromFile(app, "Models/GlassSphere/GlassSphere.gltf");

if (caustica::ecs::isValid(entity))
{
    caustica::setEntityLocalTransform(
        app,
        entity,
        dm::double3{2.0, 1.0, 0.0},
        std::nullopt,
        dm::double3{0.5, 0.5, 0.5});
}
```

These functions mutate the logic-side `SceneEntityWorld`. Extract handles proxy
publication and schedules structure-related GPU work; callers should not wait
for the GPU or rebuild acceleration structures directly. Query the live scene
through `SystemContext::entityWorld()`, `SystemContext::sceneEcs()`, or the
functions in `SceneQuery.h`.

## Camera and frame control

`EngineApp` forwards the common operations:

```cpp
engine->setScene("kitchen.scene.json");
engine->setCameraVerticalFOV(dm::radians(60.f));
engine->setCameraIntrinsics(fx, fy, cx, cy, imageWidth, imageHeight);

while (!engine->accumulationCompleted())
    engine->stepFrame();

caustica::rhi::Texture* ldr = engine->ldrColorTexture();
```

`setCameraPosDirUp()` accepts the same nine-comma-separated-value format as the
`--cameraPosDirUp` command-line option. `ldrColorTexture()` returns an
engine-owned texture; do not retain it across shutdown or resource recreation.

## Render-thread work

Most hosts do not need to enqueue render work. When unavoidable:

```cpp
#include <engine/EnqueueRenderCommand.h>

caustica::EnqueueRenderCommand(app, [] {
    // Render-thread-only work. Do not access App schedules or live scene ECS.
});
```

`EnqueueRenderCommand` is non-blocking unless queue backpressure applies.
`EnqueueRenderCommandAndWait` is a blocking synchronization point and should be
reserved for work whose completion must be observed immediately.

The render callable must not capture transient logic-thread references, mutate
the live ECS, submit/present outside the engine, or bypass the RHI threading
contract. See [ECS and render proxies](architecture-render-proxy.md) and
[RHI threading](architecture-rhi-threading.md).

## In-tree CMake integration

The official sample links the engine and RHI targets:

```cmake
add_executable(my_caustica_host Main.cpp)
target_link_libraries(my_caustica_host PRIVATE causEngine caustica_rhi)
```

It also depends on the shader targets and copies optional runtime payloads.
Use the `caustica_thin_client` block in `caustica/CMakeLists.txt` as the
authoritative template when adding another executable inside this repository.
For build options and runtime layout, see
[Building and running Caustica](build-and-run.md).
