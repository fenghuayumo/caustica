# Embedding Caustica from C++

`EngineApp` is the supported high-level entry point for a C++ host. It owns the
graphics device (or borrows one), installs the default runtime plugins, runs the
application schedules, and exposes focused scene, camera, and render-session
APIs. Hosts should not reach through internal GPU/WR headers or drive
`WorldRenderer` directly.

**Public API (P0):** prefer `#include <caustica.h>` and stay on the
allowlist in [public-api.md](public-api.md). The combined C++ / Python
`EngineApp` reference is [caustica.md](../caustica.md) — Python is snake_case
(`engine.set_scene`, `engine.step_frame`, `engine.spawn_from_file`). The
complete in-tree C++ example is
[`examples/cpp/thin_client/Main.cpp`](../examples/cpp/thin_client/Main.cpp)
(`caustica_thin_client`). The allowlist is a convention, not build-enforced.

Do **not** copy `application/editor/game` (`demo::Prop*`, `GameModel`, `LightController`) —
that folder is an editor game script layer, not an embedding API.

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
        .scene = "default.scene.json",
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
    .scene = "default.scene.json",
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
| `useVulkan` | `false` | Select Vulkan when both Vulkan and DirectX 12 were compiled. Linux builds are Vulkan-only; a `false` value still uses Vulkan. |
| `fullscreen` | `false` | Start the owned window fullscreen. |
| `scene` | `default.scene.json` | Initial scene. |
| `runtimeDirectory` | auto | Directory containing cooked shaders and runtime libraries. |
| `resourceRoot` | auto | Directory containing the `Assets/` pack folder. |
| `assetPackRoot` | auto | Direct path to the asset pack (`CAUSTICA_ASSETS_DIR` / `--assets`). |
| `device`, `window`, `surface` | `nullptr` | Borrow an existing GPU trio (Python Device-outlives-App). All three must be set together; `EngineApp` does not take ownership. |
| `cli` | `{}` | Snapshot of parsed CLI. Copied into the engine; do not keep a second live copy on the host. |
| `fromArgv(argc, argv)` | — | Parse argv into a desc. CLI apps should start here instead of filling overlapping structs. |

`EngineApp` owns `viewState`, `diagnostics`, and `renderAppState`. Bind host references after `create` (the editor does this with `SceneEditor::bindEngine`). `preGpuDeviceInit` runs just before an owned device is created.

Windowed create order is Window → Device(bind) → Surface. `GpuDevice::create` does not create a window.

## Registering simulation systems

Bevy-style: `create` → `addSystem` / `addPlugin` → `run()` (Startup is automatic):

```cpp
#include <caustica.h>

struct AppSimulationLabel
{
    static constexpr const char* name = "App.Simulation";
};

auto engine = caustica::EngineApp::create({ .scene = "default.scene.json" });

engine->addSystem<AppSimulationLabel>(
    caustica::AppSchedule::update,
    [](caustica::Res<caustica::Time> time,
       caustica::SceneTransforms transforms,
       caustica::Query<const MySpinner> spinners) {
        spinners.each([&](caustica::ecs::Entity entity, const MySpinner& spin) {
            transforms.setRotation(entity, spin.rotationAt(time->elapsedSeconds));
        });
    });

engine->run();
```

Typed system parameters: `Res<T>`, `ResMut<T>`, `Commands`, `Query<Components...>`,
`SceneTransforms`, `EntityWorld`, and `SystemContext&`. Prefer them over digging
through internal GPU/WR headers, `WorldRenderer`, or a raw `Scene*` (extract / GPU
APIs). `EngineApp` does not expose `scene()`; use `setScene` + `entityWorld` /
`isSceneLoaded`. CameraController and path-tracer settings are App resources — use
`RenderSessionApi` / `CameraApi`.

After a scene commits, the live graph is grafted into `App::world()` and
`SceneEntityWorld` borrows that registry. `Query`, `Commands`, `Res`, and
`SceneTransforms` therefore see the same entities and resources. Importers still
load into a scratch World on the load thread; that is not a second scheduled ECS.
`EntityWorld::spawn` / `spawnNamed` (or `SceneSpawn.h`) create scene nodes with
hierarchy and transforms. `Commands.spawn()` is a raw `World::spawn` and does not.

### Parameters decide parallelism

The parameter list is also what makes systems run in parallel: the scheduler derives each
system's reads and writes from it and overlaps systems that cannot conflict, with no threading
code on your side. `Query<const T, U>` reads `T` and writes `U`; `Res<T>` / `ResMut<T>` do the
same for resources; `SceneTransforms` declares exactly one write (`LocalTransformComponent`);
`Commands` is deferred and gets its own buffer.

`EntityWorld` and `SystemContext&` are the two that opt out. Both can reach anything — spawn,
despawn, `App&`, the whole engine — so the scheduler has no choice but to run those systems
alone. That is the right trade for setup and occasional structural work, and the wrong one for
a system that runs every frame.

So split by what the work actually needs:

| Work | Parameters | Runs |
| --- | --- | --- |
| Spawn, despawn, attach components, reach `App&` | `EntityWorld`, `SystemContext&` | Alone |
| Move / rotate / scale existing entities | `SceneTransforms` + `Query<>` | In parallel |
| Read components, update your own resources | `Query<const T>`, `Res<T>`, `ResMut<T>` | In parallel |
| Timing | `Res<Time>` | In parallel |

Two habits carry most of the benefit. Take `Res<Time>` instead of `SystemContext&` when all you
wanted was `deltaTimeSeconds` — that alone is the most common reason a system ends up exclusive.
And keep structural work in a separate, ideally one-shot system, tagging entities with a marker
component (`EntityWorld::emplace`) so the per-frame system can find them with a `Query` instead
of asking for the world.

`SceneTransforms` will not create a transform: adding a component is a structural change, so an
entity without `LocalTransformComponent` is skipped and the call returns `false`. Spawn with a
transform, or attach one from the setup system.

Ordering between conflicting systems is fixed by the plan, so results do not depend on thread
timing; run with `--serialSystems` to rule parallelism out while debugging.
`examples/cpp/thin_client/Main.cpp` is this split end to end. Details in
[architecture-render-proxy.md](architecture-render-proxy.md#concurrent-systems).

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
| `EntityWorld` (system param) | Hierarchy-aware `spawn` / `spawnNamed`, `emplace` tags, `setLocalTransform`. Exclusive. |
| `Commands` (system param) | Deferred edits on `App::world()`. `spawn()` is raw ECS — not a scene node. |
| `SceneTransforms` (system param) | Local transform writes only, safe to run in parallel. |
| `Query<...>` (system param) | Bevy-style `each` over `App::world()` (`Changed<>` / `With<>` supported). |
| `engine/SceneQuery.h` | `entityWorld`, load status (`sceneLoadStatus`), materials, entity lookup, `gameSettings` / `importedModels` / `sceneTypeFactory` (no diggable `Scene*`). |
| `engine/SceneSpawn.h` | Prefab `load`, `spawn`, `spawnFromFile`, `despawn`, plus spawn lights / rect-light visuals. |
| `engine/SceneTransform.h` | Free-function local transform / visibility (App-based). |
| `scene/ScenePoseAccess.h` | Typed entity TRS (`EntityPose`). Camera aiming stays on `CameraApi` / `look_to`. |
| `engine/MeshDeformApi.h` | Vertex reads/deformation and geometry-sequence playback (entity + MeshHandle only). |
| `MeshHandle` / `MaterialHandle` / `MeshInstanceComponent` | App asset identity; do not dig mesh/material GPU ids. |
| `findMaterial(app, pickId)` | Path-tracer pick id (`gpuDataIndex`), not dense `materialID`. |
| `engine/CameraApi.h` | Camera selection, pose, FOV, intrinsics, scene camera entities. |
| `engine/SceneLifecycle.h` | `setCurrentScene` / `retargetCurrentScene`. |
| `engine/RenderSessionApi.h` | Accumulation, sizes, env map, screenshots, picking, lighting/material handles, `shaderFactory`. |
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
    caustica::spawnFromFile(app, "models/GlassSphere/GlassSphere.gltf");

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

These functions mutate the live scene graph in `App::world()`. Extract handles
proxy publication and schedules structure-related GPU work; callers should not
wait for the GPU or rebuild acceleration structures directly. Look up the live
graph through `SystemContext::entityWorld()` or `SceneQuery.h`. After commit,
`sceneEcs()` is the same registry as `App::world()` (or null if no scene is
active).

## Camera and frame control

`EngineApp` forwards the common operations. `setCameraVerticalFOV` / `setCameraIntrinsics` / `setCameraPosDirUp` write the **active** camera (free controller at index 0, or the selected scene camera). Per-camera FOV, pose, and pinhole live on the scene camera entity via `CameraApi.h` (`setSceneCameraVerticalFOV`, `setSceneCameraLookTo`, `setActiveCamera`).

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

`causEngine` does not link ImGui. The editor (`causticaApp`) links `causImgui`;
a headless or `EngineApp` host should not.

It also depends on the shader targets and copies optional runtime payloads.
Use the `caustica_thin_client` block in `caustica/CMakeLists.txt` as the
authoritative template when adding another executable inside this repository.
For build options and runtime layout, see
[Building and running Caustica](build-and-run.md).
