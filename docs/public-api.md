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
| `engine/EntityWorld.h` | System-param scene graph (`spawn` / `spawnNamed`, `emplace`, transform, `findEntity`, `setVisible`) |
| `engine/SceneTransforms.h` | System-param transform writes that keep a system parallel |
| `engine/Time.h` | `Time` resource (`deltaSeconds`, `elapsedSeconds`, `frameCount`) |
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

Simulation systems run concurrently when their parameter lists prove they cannot conflict — you
never declare access by hand and there is no `addSystem` overload that accepts it. `Query` /
`Res` / `ResMut` / `Commands` / `SceneTransforms` all target `App::world()` after the live
scene is committed (`SceneEntityWorld` borrows that registry). `EntityWorld` and
`SystemContext&` run exclusively because they reach the whole engine. Keep those two in
structural, ideally one-shot systems and give per-frame systems narrow parameters —
`Res<Time>` instead of `SystemContext&` for the clock, `SceneTransforms` instead of
`EntityWorld` for movement. Scene nodes go through `EntityWorld::spawn`, not
`Commands.spawn()`. See
[architecture-render-proxy.md](architecture-render-proxy.md#one-live-logic-world).

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
