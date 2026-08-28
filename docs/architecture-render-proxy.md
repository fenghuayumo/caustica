# Architecture: ECS + UE-style Render Proxies

Caustica combines a **Bevy-inspired logic-side ECS** with an **Unreal-style game/render thread split**. The two concerns are orthogonal:

| Layer | Responsibility | Thread |
| --- | --- | --- |
| `SceneEntityWorld` (ECS) | Entities, components, queries, animation, hierarchy, `Changed<>` | Logic / game |
| Extract | Copy ECS + active camera/settings → flat proxies | Logic (Extract schedule) |
| `SceneRenderData` / `SceneRenderSnapshot` | Triple-buffered, ECS-free frame packet | Logic writes, render reads |
| `WorldRenderer` + passes | AS build, path trace, denoise, present | Render thread |

## Intended contract

```
SceneWorld (ECS)     PostUpdate resolve      Extract (copy)         SceneRenderData[N%3]      WorldRenderer
─────────────────    ──────────────────      ──────────────         ────────────────────     ─────────────
TransformComponent   ResolvedActiveCamera ─► ActiveCamera copy  ──► ActiveCameraRenderProxy  read-only
*LightComponent      (App resource)          Light/mesh proxies ──► LightRenderProxy          no getEntityWorld()
CameraController      after TransformPropagate PathTracerSettings ─► RenderSettingsSnapshot    activeSettings()
PathTracerSettings   (already App resource)  one-shot clear
```

**Rules**

1. Render-thread frame work must consume `Scene::getRenderData()` (or typed proxies inside it).
2. Render thread must **not** call `Scene::getEntityWorld()` for per-frame lighting / emissive / mesh enumeration.
3. Light history (NEE-AT remapping) lives in render-side maps (`LightSamplingCache`), keyed by entity / instance hash — not on ECS light/mesh components.
4. Operate on `LightRenderProxy` / `LightData` directly — **no** `asComponent()` glue back to ECS.
5. ECS lights are UE-style typed components (`DirectionalLightComponent`, `SpotLightComponent`, `PointLightComponent`, `EnvironmentLightComponent`); Extract packs them into unified `LightRenderProxy` + `LightData` for the GPU thread.
6. Game-thread scene-load / editor mutation may still touch ECS; that is not the render path.
7. Structure-only republish (runtime drag-drop import) must not stomp `ActiveCameraRenderProxy` — either pass `FrameExtractInputs`, preserve a same-frame frame extract, or leave `camera.valid == false` so WorldRenderer skips apply.

## What is extracted today

`extractSceneRenderData()` + `extractFrameRenderState()`
(`caustica/caustica/src/scene/SceneRenderExtract.cpp`):

- `MeshInstanceRenderProxy` — transform, bounds, mesh, `proxiedAnalyticLight`, `parentLightEntity`
- `SkinnedMeshRenderProxy` — joint matrices / debug lines
- `LightRenderProxy` — color, `LightData`, world transform (no shadow maps)
- `CameraRenderProxy` — every scene camera and its projection/exposure data
- `ActiveCameraRenderProxy` — resolved free/selected camera pose, FOV, clipping, and intrinsics
- `GaussianSplatRenderProxy` — entity, enable state, and object-to-world transform
- `RenderSettingsSnapshot` — `PathTracerSettings` copy, invalidation, picking, splat temporal reset, scene time
- Immutable material, geometry, and mesh resource snapshots when scene structure changes
- Entity id lists for cameras / animations

Published via `Scene::extractAndPublishRenderSnapshot(frameIndex, &frameInputs)` into a **3-slot** snapshot.

## Game-thread-only ECS paths (intentional)

| Path | Why ECS is OK |
| --- | --- |
| `MeshDeformGpu` | Editor / Python deform / geometry sequences on logic thread |
| `SceneGaussianSplatPasses::loadFromSceneEntities` / `attachToScene` | Load/edit mutates entities then publishes snapshot |
| JSON / glTF / USD importers | Write typed `*LightComponent` / `CameraComponent` / `AnimationComponent` directly |

Frame rendering already uses light proxies + cached splat transforms; do not move these load/edit paths onto the render thread.

## RHI threading

Game/render split above is necessary but not sufficient for parallel GPU recording. Queue submit, GC, and deferred command-list rules live in [architecture-rhi-threading.md](architecture-rhi-threading.md).

TaskRuntime + LoadSession streaming: [ADR 0001](adr/0001-task-runtime-multithreading.md).
TaskGraph + concurrent system execution: [ADR 0003](adr/0003-taskgraph-parallel-ecs.md).

## App scene-edit API

Prefer these for application / Python / editor scene edits (no WorldRenderer / AS words):

- `SceneSpawn.h` - `load` / `spawn` / `despawn`
- `SceneTransform.h` - local transform / translation / visibility
- `MeshDeformApi.h` - mesh deform / geometry sequence (`MeshDeformOptions`) via **entity** only
- `MeshHandle` / `MaterialHandle` + `MeshInstanceComponent::meshHandle()` — app asset identity

`MeshInfo` / `MeshGeometry` / `Material` GPU keys (`m_renderResourceId`) are private; only Extract /
GPU updater touch them via `scene/internal/RenderResourceAccess.h`. Pick materials with
`findMaterial(app, gpuDataIndex)` — not dense `Material::materialID`. Apps use entity +
`MeshHandle` only — not `shared_ptr<MeshInfo>`.
`MeshDeformGpuParams` / `engine/internal/MeshDeformGpu.h` remain engine-internal.
Import attach/detach is `SceneApply.h` (the old `SceneRuntimeMutation` shim was removed).
Editor `demo::PropComponentBase` / `GameModel` live only under `application/editor/game`
(not in `causScene`). Sample scripts over ECS — not a second component system.
For a host-side walkthrough, see [C++ embedding](embedding-cpp.md).

## SystemSet + thin client

Default SystemSets (`SystemSets.h`):

- `system_set::Simulation` — gameplay / host systems on `AppSchedule::update` (default membership)
- `system_set::TransformPropagate` — hierarchy refresh in `PostUpdate` (after other PostUpdate systems)
- `system_set::Extract` — Extract publish path

App systems can take Bevy-style parameters: `EntityWorld`, `Query<...>`, `Res` / `ResMut`,
`Commands`. Bundle spawn is plain ECS emplace via `EntityWorld::spawn(...)` /
`SceneEntityWorld::spawnNamed`. Mesh/light/camera resource lists and leaf bounds
are rebuilt in `SceneEntityWorld::syncSceneResourcesFromEcs()` from `Added<>` /
`Changed<>` (runs in `beginRefreshFrame` / `ensureSceneResourcesSynced`). Sticky
`m_structureDirty` / `m_transformDirty` are refresh/Extract caches hydrated from
ChangeDetection — not a second host-facing dirty API. `EngineApp::run` /
`stepFrame` auto-run `finishStartup` after the host registers systems.

Logic-thread `CameraController` and settings stay **App resources** (`CameraController`, `ResolvedActiveCamera`,
`PathTracerSettings`). Extract copies them via `FrameExtractInputs`. Free vs scene camera resolve runs in
PostUpdate (`SceneResolveActiveCamera` after `TransformPropagate`).
Applications must not dig `worldRenderer()` — use `RenderSessionApi` / `CameraApi` /
`SceneLifecycle`. `WorldRenderer` access is `engine/internal/WorldRendererAccess.h`.

`Scene` owns `SceneEntityWorld` + `SceneRenderSnapshot` + `SceneStructureGpuSync` (async AS
handoff). Editor SampleGame (`application/editor/game`, `demo::`) is demo-only and editor-linked.

Occasional render-thread work from Logic: `EnqueueRenderCommand` / `EnqueueRenderCommandAndWait`
(`EnqueueRenderCommand.h`) — thin wrappers over the existing RT dispatch (non-blocking by default).

Official sample (no editor): `examples/cpp/thin_client` → target `caustica_thin_client`
(`#include <caustica.h>` + Simulation systems). Frozen surface: [public-api.md](public-api.md).

## Concurrent systems

`First` / `preUpdate` / `update` / `PostUpdate` / `Last` run their systems concurrently on
TaskRuntime workers when it is provably safe. `Startup`, `Extract` and `render` stay serial.
Hosts write no threading code: the scheduler reads each system's **parameter list** and lets two
systems overlap only when neither writes what the other reads or writes. See
[ADR 0003](adr/0003-taskgraph-parallel-ecs.md).

| Parameter | Declares |
| --- | --- |
| `Query<const T, U>` | read `T`, write `U` (filters `With` / `Without` / `Changed` / `Added` are reads) |
| `Res<T>` / `ResMut<T>` | read / write resource `T` |
| `Commands` | deferred; gets its own buffer, merged in plan order after the phase |
| `SceneTransforms` | write `LocalTransformComponent` — nothing else |
| `EntityWorld` or `SystemContext&` | **exclusive** — reaches everything, so it runs alone |

`EntityWorld` is the convenient way to spawn and move things, but it opts a system out of
parallelism, and that is correct: it can touch anything. The fix is not to avoid it but to
split the system in two. Structural work (spawn, despawn, attaching components) keeps
`EntityWorld` and is usually one-shot; the per-frame work takes narrow parameters and runs in
parallel. `Res<Time>` and `SceneTransforms` exist for exactly this — they cover the two reasons
a per-frame system used to need the whole world, namely reading the clock and moving something.
`examples/cpp/thin_client/Main.cpp` shows the split.

`SceneTransforms` is safe to run concurrently because the conflict rule already guarantees only
one system writes `LocalTransformComponent` at a time, and the change-tick storage for every
written component is created before dispatch. It refuses to add a missing transform, since that
would be a structural edit against systems iterating the registry.

Conflicting systems are ordered by the plan, so parallel and `--serialSystems` runs produce
identical results — turning parallelism off only changes timing.

Inspect a phase with `app.schedules().describePlan(AppSchedule::update)`; `planInfo()` reports
system / exclusive / edge counts and max parallel width.

## Extract schedule (P2)

Leaf Extract is Bevy-style schedule composition on `AppSchedule::Extract`
(`RenderExtractPlugin`) — no renderable type registry:

1. `Scene.ExtractCore` — mesh/light/camera/skinned into the logic cache
2. `Scene.ExtractGaussianSplats` — splat proxies (`extractGaussianSplatProxies`)
3. `Scene.PublishRenderSnapshot` — copy cache + frame inputs into the triple buffer

Add new leaf Extract as systems ordered after Core and before Publish (or after
sibling leaf systems). Load / GpuSetup still calls `Scene::extractAndPublishRenderSnapshot`
(core + splat + publish in one shot). GPU prepare for leaf types stays type-local
in `WorldRenderer` / pass owners (or a future Prepare schedule), not a C++ type table.
Mesh TLAS / instance indices / committed-serve stay central.

## Async structure handoff

Runtime spawn/despawn no longer `waitForRenderThreadIdle`. Extract freezes the previous
proxy packet as `Scene::committedRenderData()`, publishes the new generation, and
`enqueuePendingStructureGpu` builds meshes/AS/SBT/bindings on the render thread via
`EnqueueRenderCommand` (non-blocking for Logic).

While `structureGpuBuildInFlight()`:

- `WorldRenderer` serves committed proxies for path tracing (latest slot only for camera/settings)
- `SceneGpuUpdater::refresh` skips so it cannot race prune/upload against the old TLAS
- RT commits with `finishStructureGpuBuild` after binding-set recreate

First structure publish with no committed packet enqueues async build; WorldRenderer serves
null structure (empty/placeholder present) until commit. Full scene open uses `LoadSession`
budgeted GpuStreaming + StructureGpu `AccelOnly` (same committed-serve path as runtime spawn).
Logic→RT work shares one `Affinity::Render` domain queue (RenderThread pumps it).

## Current status and boundaries

| Item | Status |
| --- | --- |
| Async structure GPU build | Committed serve + RT enqueue + double-buffered TLAS/BLAS/SBT (retired handles; no structure `waitForIdle`) |
| Parallel RHI command-list recording | `FrameCommandContext` + GraphBuilder waves; see [architecture-rhi-threading.md](architecture-rhi-threading.md) |
| TaskRuntime + Logic→RT enqueue | `caustica::task`; sole `EnqueueRenderCommand*` → `Affinity::Render` — [ADR 0001](adr/0001-task-runtime-multithreading.md) |
| LoadSession amortized streaming | `LoadSession` / `tickLoadSession`; present continues during GpuStreaming — [ADR 0001](adr/0001-task-runtime-multithreading.md) |
| Concurrent ECS systems | Access derived from system parameters; conflict-aware parallel executor on the simulation phases — [ADR 0003](adr/0003-taskgraph-parallel-ecs.md) |
| TaskGraph | `task::TaskEvent` / `task::TaskGraph` DAG authoring over TaskRuntime — [ADR 0003](adr/0003-taskgraph-parallel-ecs.md) |
| SceneSettings / GameSettings / GaussianSplat | Value payloads on ECS; GPU splat passes keyed by entity in `SceneGaussianSplatPasses` |
| Scene API modules | Split from god-facade: `AppResources` / `SceneQuery` / `SceneSpawn` / `SceneTransform` / `MeshDeformApi` / `CameraApi` / `SceneLifecycle` / `RenderSessionApi` / `RenderFrameApi` (include the focused header you need) |
| Scene query path | Apps use `entityWorld` / lifecycle only; engine+editor use `internal/ActiveSceneAccess` (`activeScene`) — not `gpu->sceneManager()->getScene()` |
| `EditorPlugin` | Composes `DefaultPlugins` (shared bootstrap + `ActiveScene`) |
| Scene plugins | `CameraPlugin` / `RenderExtractPlugin` / … are `Plugin` structs (via `registerSceneSchedules`) |
| Camera wrappers | Interactive side effects live on `CameraController::bindSideEffects` |

## File map

| Piece | Path |
| --- | --- |
| Proxy + frame extract types | `caustica/caustica/include/scene/SceneRenderData.h` |
| Extract | `caustica/caustica/src/scene/SceneRenderExtract.cpp` |
| Extract schedule | `caustica/caustica/src/engine/RenderExtractPlugin.cpp` |
| Snapshot buffer | `caustica/caustica/include/scene/SceneRenderSnapshot.h` |
| Frame settings binding | `PathTracingContext::activeSettings()` / `WorldRenderer::render()` |
| Schedules + parallel plan | `caustica/caustica/include/engine/AppSchedules.h` |
| System access derivation | `caustica/caustica/include/ecs/SystemAccess.h` |
| TaskGraph | `caustica/caustica/include/core/task/TaskGraph.h` |

## Why not a “render ECS”

A second EnTT world on the render thread would add sync cost without helping path tracing. Flat proxy arrays match bindless / light-buffer upload and match UE’s `F*SceneProxy` model. Keep ECS where queries and composition pay off (simulation); keep proxies where the GPU thread needs stable, read-only packets.
