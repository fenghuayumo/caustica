# Embedding Caustica from C++

`EngineApp` is the supported high-level entry point for a C++ host. It owns or
accepts the graphics device, installs the default engine plugins, runs the
application schedules, and exposes focused scene, camera, and render-session
APIs. New hosts should not assemble `DefaultPlugins`, reach through
internal GPU/WR headers, or drive `WorldRenderer` directly.

**Public API (P0):** prefer `#include <caustica.h>` and stay on the
allowlist in [public-api.md](public-api.md). The complete in-tree example is
[`examples/cpp/thin_client/Main.cpp`](../examples/cpp/thin_client/Main.cpp)
(`caustica_thin_client`); CMake also runs `tools/check_public_api_includes.py`.

Do **not** copy `application/editor/game` (`demo::Prop*`, `GameModel`, `LightController`) —
that folder is an editor SampleGame script layer, not an embedding API.

## Lifecycle

For a host that only needs the default engine:

```cpp
#include <caustica.h>

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
| `adapter` | `AdapterSelector::automatic()` | GPU selector. Automatic mode chooses the highest-scoring suitable hardware adapter; explicit modes support index, name, UUID, and LUID. |
| `useVulkan` | `false` | Select Vulkan when both Vulkan and DirectX 12 were compiled. |
| `fullscreen` | `false` | Start the owned window fullscreen. |
| `scene` | `default.json` | Initial scene. |
| `runtimeDirectory` | auto | Directory containing cooked shaders and runtime libraries. |
| `resourceRoot` | auto | Directory containing `Assets/`. |
| `device`, `window` | `nullptr` | Inject host-owned objects; `EngineApp` does not take ownership. |
| `finishStartup` | `false` | Deprecated. `create()` leaves Startup pending; `run()` / `stepFrame()` finish automatically. Set true only if you need Startup inside `create()`. |

`viewState`, `diagnostics`, `renderState`, and `cmdLine` can point to host-owned
state. `preGpuDeviceInit` runs just before an owned device is created.

## Registering simulation systems

Bevy-style: `create` → `addSystem` / `addPlugin` → `run()` (Startup is automatic):

```cpp
#include <caustica.h>

struct AppSimulationLabel
{
    static constexpr const char* name = "App.Simulation";
};

auto engine = caustica::EngineApp::create({ .scene = "default.json" });

engine->addSystem<AppSimulationLabel>(
    caustica::AppSchedule::update,
    [](caustica::EntityWorld scene, caustica::SystemContext& ctx) {
        if (!scene || !caustica::isSceneLoaded(ctx.app))
            return;

        // Mutate host state or the logic-side scene here.
        // Extract publishes the render-thread snapshot after update/PostUpdate.
    });

engine->run();
```

Typed system parameters: `Res<T>`, `ResMut<T>`, `Commands`, `EntityWorld`,
`Query<Components...>`, and `SystemContext&`. Prefer `EntityWorld` /
`Query<>` over digging through internal GPU/WR headers, `WorldRenderer`, or a raw
`Scene*` (extract / GPU APIs). `EngineApp` does not expose `scene()`; use
`setScene` + `entityWorld` / `isSceneLoaded`. CameraController and path-tracer
settings are App resources — use `RenderSessionApi` / `CameraApi`.

An `update` system with no explicit set joins
`system_set::Simulation`. The per-frame order is:

```text
First → preUpdate → update → PostUpdate → Extract → dispatch render → postRender → Last
```

`Startup` runs once on the first `run()` / `stepFrame()` / explicit
`finishStartup()`. `shutdown` runs during engine shutdown. Transform propagation
is the tail of `PostUpdate`; Extract publishes the ECS-free render snapshot
afterward. The `render` schedule executes in the render-thread domain when the
dedicated thread is active. `postRender` and `Last` continue on the logic thread
after dispatch and do not imply that the asynchronous render work has completed.

Use `AppSystemOrdering::runBefore`, `runAfter`, and `inSet` for explicit ordering.
Apps that must run code after Startup but before the loop (e.g. the editor) can
still call `finishStartup()` explicitly.

## Scene access and mutation

Prefer system parameters, then focused application headers:

| API | Supported operations |
| --- | --- |
| `EntityWorld` (system param) | `spawn` / `spawnNamed` bundles, `setLocalTransform`, scene ECS access. |
| `Query<...>` (system param) | Bevy-style `each` over scene components (`Changed<>` / `With<>` supported). |
| `engine/SceneQuery.h` | `entityWorld`, load status, materials, entity lookup (no diggable `Scene*`). |
| `engine/SceneSpawn.h` | Prefab `load`, `spawn`, `spawnFromFile`, and `despawn`. |
| `engine/SceneTransform.h` | Free-function local transform / visibility (App-based). |
| `engine/MeshDeformApi.h` | Vertex reads/deformation and geometry-sequence playback (entity + MeshHandle only). |
| `MeshHandle` / `MaterialHandle` / `MeshInstanceComponent` | App asset identity; do not dig mesh/material GPU ids. |
| `findMaterial(app, pickId)` | Path-tracer pick id (`gpuDataIndex`), not dense `materialID`. |
| `engine/CameraApi.h` | Camera selection state, pose, FOV, and intrinsics. |
| `engine/SceneLifecycle.h` | Scene selection/reload operations. |
| `engine/RenderSessionApi.h` | Session-level render controls. |
| `engine/RenderFrameApi.h` | Accumulation and rendered-frame access. |

Bundle spawn example (plain component emplace; Extract/refresh syncs resource lists):

```cpp
scene.spawn(
    caustica::scene::NameComponent{ "Spinner" },
    caustica::scene::LocalTransformComponent::fromTRS(
        dm::double3{ 2.0, 1.0, 0.0 },
        dm::dquat::identity(),
        dm::double3{ 0.5, 0.5, 0.5 }),
    caustica::scene::PointLightComponent{ .intensity = 5.f });
```

Prefab spawn example:

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
