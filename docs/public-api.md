# Public C++ API (frozen — P0)

New applications embed Caustica through **`EngineApp`** and the public headers
below. Prefer a single include:

```cpp
#include <caustica.h>
```

Walkthrough and lifecycle details: [embedding-cpp.md](embedding-cpp.md).

## Allowlist

| Header | Role |
| --- | --- |
| `caustica.h` | Umbrella include (preferred) |
| `engine/EngineApp.h` | Create / run / stepFrame / setScene / camera / settings |
| `engine/EntryPoint.h` | `initializeAppPlatform`, `runEngineApp` |
| `engine/EntityWorld.h` | System-param scene ECS (`spawn`, transform, `findEntity`, `setVisible`) |
| `engine/Plugin.h` | `addPlugin` extension point |
| `engine/SystemSets.h` / `engine/SystemLabel.h` | `Simulation` membership + labels |
| `engine/AppSchedules.h` | Via `App.h`: `AppSchedule`, `Query`, `Res` / `ResMut`, `SystemContext` |
| `engine/App.h` | Schedule runtime; prefer not to construct `App` yourself |
| `engine/SceneSpawn.h` | `load` / `spawn` / `spawnFromFile` / `despawn` |
| `engine/SceneTransform.h` | App-based transform / visibility |
| `engine/SceneQuery.h` | `entityWorld`, load status, `findEntity`, materials |
| `engine/ActiveScene.h` | Name/path/generation metadata only (no `Scene*` digs) |
| `engine/MeshDeformApi.h` | Entity + MeshHandle deform / geometry sequence |
| `engine/CameraApi.h` | Camera selection, pose, FOV, intrinsics |
| `engine/RenderSessionApi.h` | Accumulation, sizes, env map, screenshots (entity overloads) |
| `engine/SceneLifecycle.h` | `setCurrentScene` (prefer `EngineApp::setScene`) |
| `engine/EnqueueRenderCommand.h` | Occasional non-blocking RT work |
| `engine/EngineSceneCallbacks.h` / `engine/SceneViewState.h` | Optional `EngineAppDesc` hooks |

Transitive types apps may use: `scene::*Component` (`SceneEcs.h`), `ecs::Entity`,
`Query<>`, `math` / `dm::` types, `PathTracerSettings` via `EngineApp::settings()`,
`Handle` / `MeshHandle` / `MaterialHandle`.

## Not for applications

| Area | Headers / APIs |
| --- | --- |
| Bootstrap | `DefaultPlugins`, `ScenePlugins`, `SceneStartup`, `SceneScheduleRegistration` |
| Threading internals | `RenderThread`, `LoadSession` |
| GPU digs | `GpuSharedCaches`, `engine/internal/*`, `WorldRenderer`, `SceneManager` |
| Frame schedule guts | `RenderFrameApi` (`beginFrameScheduled` / `renderScene` / …) |
| Session guts | `SceneSession`, `SceneGaussianSplatLogic` |
| Editor / tooling | `UserInterfaceUtils`, `Console*`, `SplashScreen`, `CaptureSequencer`, … |

Editor SampleGame (`application/editor/game`, `demo::`) is **not** an embedding API.

## Coverage target (thin_client ≈ 80%)

| Need | API | Shown in thin_client |
| --- | --- | --- |
| Create + run | `EngineApp::create` / `runEngineApp` | yes |
| Simulation system | `addSystem` + `system_set::Simulation` | yes |
| Prefab spawn | `spawnFromFile` | yes |
| Bundle spawn | `EntityWorld::spawn` (e.g. light) | yes |
| Transform | `EntityWorld::setLocalTransform` | yes |
| Visibility | `EntityWorld::setVisible` | yes |
| Query | `Query<LocalTransformComponent, MeshInstanceComponent>` | yes |
| Find by path | `EntityWorld::findEntity` / `findEntity(app, …)` | yes |
| Despawn | `despawn(app, entity)` | yes |
| Camera | `EngineApp::setCameraVerticalFOV` | yes |
| Settings | `EngineApp::settings()` | yes |
| Load status | `isSceneLoaded` / `currentSceneName` | yes |
| RT enqueue | `EnqueueRenderCommand` | yes |
| Mesh deform | `MeshDeformApi` | no (advanced / remaining ~20%) |
| Headless step + readback | `stepFrame` / `ldrColorTexture` | no (see embedding-cpp) |
| Custom `Plugin` | `addPlugin` | no (same pattern as systems) |
