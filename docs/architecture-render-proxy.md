# Architecture: ECS + UE-style Render Proxies

Caustica combines a **Bevy-inspired logic-side ECS** with an **Unreal-style game/render thread split**. The two concerns are orthogonal:

| Layer | Responsibility | Thread |
| --- | --- | --- |
| `SceneEntityWorld` (ECS) | Entities, components, queries, animation, hierarchy, `Changed<>` | Logic / game |
| Extract | Copy ECS + session camera/settings → flat proxies | Logic (Extract schedule) |
| `SceneRenderData` / `SceneRenderSnapshot` | Triple-buffered, ECS-free frame packet | Logic writes, render reads |
| `WorldRenderer` + passes | AS build, path trace, denoise, present | Render thread |

## Intended contract

```
SceneWorld (ECS)          Extract                SceneRenderData[N%3]           WorldRenderer
─────────────────         ───────                ────────────────────          ─────────────
TransformComponent   ──►  Changed / dirty   ──►  MeshInstanceRenderProxy      read-only
*LightComponent      ──►  LightData pack    ──►  LightRenderProxy             no getEntityWorld()
| CameraController     ──►  pose / FOV        ──►  CameraSnapshot (valid)      apply then updateViews
PathTracerSettings   ──►  full copy         ──►  RenderSettingsSnapshot       activeSettings()
```

**Rules**

1. Render-thread frame work must consume `Scene::getRenderData()` (or typed proxies inside it).
2. Render thread must **not** call `Scene::getEntityWorld()` for per-frame lighting / emissive / mesh enumeration.
3. Light history (NEE-AT remapping) lives in render-side maps (`LightSamplingCache`), not in typed `*LightComponent::lightLink`.
4. Operate on `LightRenderProxy` / `LightData` directly — **no** `asComponent()` glue back to ECS.
5. ECS lights are UE-style typed components (`DirectionalLightComponent`, `SpotLightComponent`, `PointLightComponent`, `EnvironmentLightComponent`); Extract packs them into unified `LightRenderProxy` + `LightData` for the GPU thread.
6. Game-thread scene-load / editor mutation may still touch ECS; that is not the render path.
7. Structure-only republish (runtime drag-drop import) must not stomp `CameraSnapshot` — either pass `SessionRenderExtractInputs`, preserve a same-frame session extract, or leave `camera.valid == false` so WorldRenderer skips apply.

## What is extracted today

`extractSceneRenderData()` + `extractSessionRenderState()` (`scene/SceneRenderExtract.cpp`):

- `MeshInstanceRenderProxy` — transform, bounds, mesh, `proxiedAnalyticLight`, `parentLightEntity`
- `SkinnedMeshRenderProxy` — joint matrices / debug lines
- `LightRenderProxy` — color, `LightData`, world transform (no shadow maps)
- `CameraSnapshot` — position / dir / up / FOV / intrinsics
- `RenderSettingsSnapshot` — `PathTracerSettings` copy, invalidation, picking, splat temporal reset, scene time
- Entity id lists for cameras / animations

Published via `Scene::extractAndPublishRenderSnapshot(frameIndex, &sessionInputs)` into a **3-slot** snapshot.

## Game-thread-only ECS paths (intentional)

| Path | Why ECS is OK |
| --- | --- |
| `SceneMeshEditing` | Editor / Python deform / geometry sequences on logic thread |
| `SceneGaussianSplatPasses::loadFromSceneEntities` / `attachToScene` | Load/edit mutates entities then publishes snapshot |
| JSON / glTF / USD importers | Write typed `*LightComponent` / `CameraComponent` / `AnimationComponent` directly |

Frame rendering already uses light proxies + cached splat transforms; do not move these load/edit paths onto the render thread.

## RHI threading

Game/render split above is necessary but not sufficient for parallel GPU recording. Queue submit, GC, and deferred command-list rules live in [architecture-rhi-threading.md](architecture-rhi-threading.md) (Phase 1 MT foundation).

## Remaining gaps

| Item | Status |
| --- | --- |
| OO mesh/camera leaf classes | Removed; meshes/cameras are ECS components + Extract proxies |
| `SceneRenderCommandQueue` | Removed (Extract + RenderThread dispatch is the sync path) |
| Parallel RHI command-list recording | Phase 1: submit/GC locking + deferred CL default; Phase 2+: worker recording |
| SampleSettings / GameSettings / GaussianSplat | Value payloads on ECS; GPU splat passes keyed by entity in `SceneGaussianSplatPasses` |
| Scene API modules | Split from god-facade: `AppResources` / `SceneQuery` / `SceneSpawn` / `CameraApi` / `SceneLifecycle` / `RenderSessionApi` / `RenderFrameApi` (include the focused header you need) |
| Scene query path | Engine plugins + editor use `activeScene`/`entityWorld`; do not dig `gpu->sceneManager()->getScene()` |
| `EditorPlugin` | Composes `DefaultPlugins` (shared bootstrap + `ActiveScene`) |
| Scene plugins | `CameraPlugin` / `RenderExtractPlugin` / … are `Plugin` structs (via `registerSceneSchedules`) |
| Camera wrappers | `SceneCameraController` removed; interactive side effects live on `CameraController::bindSideEffects` |

## File map

| Piece | Path |
| --- | --- |
| Proxy + session snapshot types | `include/scene/SceneRenderData.h` |
| Extract | `src/scene/SceneRenderExtract.cpp` |
| Extract schedule | `src/engine/RenderExtractPlugin.cpp` |
| Snapshot buffer | `include/scene/SceneRenderSnapshot.h` |
| Frame settings binding | `PathTracingContext::activeSettings()` / `WorldRenderer::render()` |

## Why not a “render ECS”

A second EnTT world on the render thread would add sync cost without helping path tracing. Flat proxy arrays match bindless / light-buffer upload and match UE’s `F*SceneProxy` model. Keep ECS where queries and composition pay off (simulation); keep proxies where the GPU thread needs stable, read-only packets.
