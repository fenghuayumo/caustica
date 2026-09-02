# Public C++ API (frozen — P0)

New applications embed Caustica through **`EngineApp`** and the public headers
below. Prefer a single include:

```cpp
#include <caustica.h>
```

Host API for C++ and Python (same `EngineApp` operations): [caustica.md](../caustica.md).
C++ schedule / system-parameter walkthrough: [embedding-cpp.md](embedding-cpp.md).

## Allowlist

| Header | Role |
| --- | --- |
| `caustica.h` | Umbrella include (preferred) |
| `engine/EngineApp.h` | Create / run / stepFrame / setScene / camera / spawn / settings / screenshot |
| `engine/EntryPoint.h` | `initializeAppPlatform`, `runEngineApp` |
| `engine/AppSchedules.h` | `AppSchedule`, `Query`, `Res` / `ResMut`, `SystemContext` |
| `engine/EntityWorld.h` | System-param scene graph (`spawn` / `spawnNamed`, `emplace`, transform, `findEntity`, `setVisible`) |
| `engine/SceneTransforms.h` | System-param transform writes that keep a system parallel |
| `engine/Time.h` | `Time` resource (`deltaSeconds`, `elapsedSeconds`, `frameCount`) |
| `engine/Plugin.h` | `addPlugin` extension point (`Plugin::build` takes the owned runtime) |
| `engine/SystemSets.h` / `engine/SystemLabel.h` | `Simulation` membership + labels |
| `engine/SceneSpawn.h` | `load` / `spawn` / `spawnFromFile` / `despawn` / spawn lights |
| `engine/SceneTransform.h` | App-based transform / visibility |
| `scene/ScenePoseAccess.h` | Entity TRS (`EntityPose`, get/set local/world). Not camera view space. |
| `engine/SceneQuery.h` | `entityWorld`, load status, `findEntity`, materials, `sceneLoadStatus`, `gameSettings` / `importedModels` / `sceneTypeFactory` |
| `engine/ActiveScene.h` | Name/path/generation metadata only (no `Scene*` digs) |
| `engine/MeshDeformApi.h` | Entity + MeshHandle deform / geometry sequence |
| `engine/CameraApi.h` | Camera selection, pose, FOV, intrinsics, scene camera entities. `EngineApp` camera methods write the active/main camera; per-entity helpers take `ecs::Entity`. Usage: [caustica.md — Camera](../caustica.md#camera). |
| `engine/RenderSessionApi.h` | Accumulation, sizes, env map, screenshots, picking, lighting/material handles, `shaderFactory` |
| `engine/SceneLifecycle.h` | `setCurrentScene` / `retargetCurrentScene` (prefer `EngineApp::setScene`) |
| `engine/EnqueueRenderCommand.h` | Occasional non-blocking RT work |
| `engine/EngineSceneCallbacks.h` / `engine/SceneViewState.h` | Optional `EngineAppDesc` hooks |

Transitive types apps may use: `scene::*Component` (`SceneEcs.h`), `ecs::Entity`,
`Query<>`, `math` / `dm::` types, `PathTracerSettings` via `EngineApp::settings()`,
`Handle` / `MeshHandle` / `MaterialHandle`, `LdrFramebuffer`.

Python (`import caustica`) is the same `EngineApp` surface with snake_case names
(`step_frame`, `set_scene`, `spawn_from_file`). `GpuDevice` injects an existing
GPU into `EngineApp.create(device=...)`. Full tables: [caustica.md](../caustica.md).

Simulation systems run concurrently when their parameter lists prove they cannot conflict — you
never declare access by hand and there is no `addSystem` overload that accepts it. `Query` /
`Res` / `ResMut` / `Commands` / `SceneTransforms` all target the live logic
registry after the scene is committed (`SceneEntityWorld` borrows that registry).
`EntityWorld` and `SystemContext&` run exclusively because they reach the whole
engine. Keep those two in structural, ideally one-shot systems and give per-frame
systems narrow parameters — `Res<Time>` instead of `SystemContext&` for the clock,
`SceneTransforms` instead of `EntityWorld` for movement. Scene nodes go through
`EntityWorld::spawn`, not `Commands.spawn()`. See
[architecture-render-proxy.md](architecture-render-proxy.md#one-live-logic-world).

## Not for applications

| Area | Headers / APIs |
| --- | --- |
| Bootstrap | `engine/App.h` (owned runtime), `engine/internal/DefaultPlugins.h`, `ScenePlugins`, `SceneStartup` |
| Threading internals | `RenderThread`, `LoadSession` |
| GPU digs | `GpuSharedCaches`, `WorldRenderer`, `SceneManager` |
| Frame schedule guts | `RenderFrameApi` (`beginFrameScheduled` / `renderScene` / …) |
| Session guts | `SceneSession`, `SceneGaussianSplatLogic` |
| Editor / tooling | `Console*`, `SplashScreen`, `CaptureSequencer`, … |

Editor game (`application/editor/game`, `demo::`) is **not** an embedding API.

## Coverage target (thin_client ≈ 80%)

| Need | API | Shown in thin_client |
| --- | --- | --- |
| Create + run | `EngineApp::create` / `runEngineApp` | yes |
| Simulation system | `addSystem` + `system_set::Simulation` | yes |
| Parallel-friendly split | exclusive setup vs. `Res<Time>` + `SceneTransforms` per-frame | yes |
| Prefab spawn | `spawnFromFile` | yes |
| Bundle spawn | `EntityWorld::spawn` (e.g. light) | yes |
| Marker tagging | `EntityWorld::emplace` | yes |
| Transform (setup) | `EntityWorld::setLocalTransform` | yes |
| Transform (per frame) | `SceneTransforms::setRotation` | yes |
| Frame timing | `Res<Time>` | yes |
| Visibility | `EntityWorld::setVisible` | yes |
| Query | `Query<const ThinClientSpun>` / `Query<const MeshInstanceComponent>` | yes |
| Find by path | `EntityWorld::findEntity` / `findEntity(app, …)` | yes |
| Despawn | `despawn(app, entity)` | yes |
| Camera | `EngineApp::setCameraVerticalFOV` | yes |
| Settings | `EngineApp::settings()` | yes |
| Load status | `isSceneLoaded` / `currentSceneName` | yes |
| RT enqueue | `EnqueueRenderCommand` | yes |
| Mesh deform | `MeshDeformApi` | no (advanced / remaining ~20%) |
| Headless step + readback | `stepFrame` / `ldrColorTexture` | no (see embedding-cpp) |
| Custom `Plugin` | `addPlugin` | no (same pattern as systems) |
