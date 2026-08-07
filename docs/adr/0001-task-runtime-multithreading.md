# ADR 0001: TaskRuntime + unified streaming / render threading

| Field | Value |
| --- | --- |
| Status | **Accepted**; P1–P3 + R1 upload fences landed (R2/R3 follow) |
| Date | 2026-08-07 |
| Deciders | Caustica engine |
| Relates | [architecture-render-proxy.md](../architecture-render-proxy.md), [architecture-rhi-threading.md](../architecture-rhi-threading.md) |

## Context

Caustica already has a real Logic / Extract / RenderThread split, triple-buffered `SceneRenderSnapshot`, `FrameCommandContext` parallel waves, and async structure GPU handoff for runtime spawn. Those pieces are intentional and should be kept.

What is not product-grade:

1. **`JobSystem` / `ThreadPool`** — a mutex-per-worker pool with `std::function`, a counter-only “context”, no priorities, no dependencies, no cancellation / generation, no named domains. `ThreadPool` is an explicit legacy wrapper; GraphBuilder already bypasses it, while importers / textures / shaders still pass `ThreadPool*`.
2. **Logic → Render enqueue surface** — four overlapping layers (`RenderThread::dispatch*`, `App::enqueueGpuWork*` / `runGpuWork*`, `RenderSessionApi` free functions, `EnqueueRenderCommand*`) plus `waitForDedicatedRenderThreadIdle` aliases.
3. **Full scene load vs runtime structure** — two philosophies. Open Scene uses `GpuBindPhase` + per-step `dispatchAndWait` / `waitForIdle` under `sceneGpuSuspended`. Runtime spawn uses `enqueuePendingStructureGpu` + committed-serve. Same upload helpers, opposite orchestration.
4. **Sync escapes everywhere** — `--syncRender` duplicates GC / completion paths; structure keeps `flushPendingStructureGpuSync`; load path drains with device `waitForIdle` per texture / mesh.
5. **Overlapping load flags** — `isSceneLoading`, `sceneGpuSuspended`, `GpuBindPhase`, `asyncLoadingInProgress`, `SceneStructureGpuSync` all answer “are we loading?” differently.

Large-scene load feels like a blocking burst because Render stays suspended until `LogicFinish`, and each bind step synchronously waits the render thread. Fixing that without a single task model will only add more glue.

## Decision

Introduce a single **TaskRuntime** as the engine scheduling hub, then migrate **render frames** and **scene LoadSession** onto it. Keep the RHI Phase-1 contract (create / submit / present / GC on the render domain; workers may only record into forked deferred lists) until a later ADR explicitly widens it.

Do **not** grow another enqueue wrapper. Do **not** “optimize” `JobSystem` in place and call it done.

### Target layering

```
TaskRuntime                          ← sole scheduler (replace JobSystem + ThreadPool)
  ├── Pipe / Affinity: Logic
  ├── Pipe / Affinity: Render        ← may still own a dedicated OS thread
  ├── Affinity: IO (optional)
  └── Affinity: Any + Priority       ← decode, import, parallel CL record, compile
        ↑
Frame pipeline tasks                 ← Extract publish → RenderFrame(N)
LoadSession tasks                    ← budgeted GPU upload / structure / present gate
GraphBuilder waves                   ← task deps (not a private JobSystem call)
```

### TaskRuntime minimum contract (P1)

Required semantics (implementation may start with mutex queues; lock-free steal is a later optimization):

| Capability | Requirement |
| --- | --- |
| Launch | `launch(TaskDesc) → TaskHandle` |
| Dependencies | prerequisite / subsequent (or equivalent fence chaining) |
| Pipe | tasks on the same `Pipe` run serially in submission order |
| Priority | at least `High` / `Normal` / `Background` (frame record must not starve under load decode) |
| Affinity | `Any` \| `Logic` \| `Render` \| `IO` (IO optional in P1) |
| Wait / poll | `wait(handle)`, `poll(handle)`, context/group wait |
| Generation | `FrameGen` / `LoadGen` so abandoned work can be discarded |
| Observability | task name, queue depth, wait time (Tracy or internal counters) |

Conceptual API (names may shift; shape must not):

```cpp
namespace caustica::task {

enum class Priority : uint8_t { Critical, High, Normal, Low, Background };
enum class Affinity : uint8_t { Any, Logic, Render, IO };

struct Pipe;           // serial domain, e.g. "RHI.Submit", "LoadSession"
struct TaskHandle;
struct TaskDesc {
    const char* name = nullptr;
    Priority priority = Priority::Normal;
    Affinity affinity = Affinity::Any;
    Pipe* pipe = nullptr;          // if set, serializes with other tasks on this pipe
    uint64_t generation = 0;       // FrameGen / LoadGen; 0 = immortal
    std::function<void()> body;    // P1 may keep std::function; P1.1 can add fixed jobs
};

TaskHandle launch(TaskDesc);
void       then(TaskHandle prerequisite, TaskHandle subsequent);
void       wait(TaskHandle);
bool       poll(TaskHandle);
void       waitPipe(Pipe&);        // drain that pipe

// Parallel-for convenience (GraphBuilder migration)
void parallelFor(uint32_t count, Priority, Affinity, /*callable*/);

void initialize(/*thread counts*/);
void shutdown();

} // namespace caustica::task
```

**Pipes that must exist early:**

| Pipe | Owner / meaning |
| --- | --- |
| `Logic` | Game / ECS mutations that must not race Extract |
| `Render` / `RHI.Submit` | submit, present, GC, create, `FrameCommandContext` fork/join |
| `LoadSession` | orders LoadSession phase advances (may share Render pipe for GPU work) |

Workers never close / submit / present / GC. That stays the RHI threading contract.

### Logic → Render API freeze (P1)

Public / engine-facing surface collapses to **one** pair:

- `EnqueueRenderCommand(App&, Fn)` — non-blocking onto Render pipe
- `EnqueueRenderCommandAndWait(App&, Fn)` — sync point; rare; annotated

Delete or inline as private after migration:

- `App::enqueueGpuWorkOnRenderThread` / `runGpuWorkOnRenderThread` (become implementation details or aliases marked deprecated then removed)
- `RenderSessionApi` GPU-work free functions (same)
- `waitForDedicatedRenderThreadIdle` alias
- Direct `RenderThread::dispatch*` from scene/editor code (only TaskRuntime / App internals)

`--syncRender` becomes: **Render pipe is pumped on the Logic thread** (same `executeRenderPhase` + GC tail). No second completion / GC algorithm.

### LoadSession (P3; design locked now)

Replace overlapping flags with one explicit session:

```
Idle
 → Importing          // CPU tasks, Priority::Background
 → GpuStreaming       // budgeted upload/bind on Render pipe
 → FirstPresent       // optional early present with partial / committed AS
 → Finalizing
 → Ready
 → (Teardown exclusive window only)
```

Rules:

- `sceneGpuSuspended` shrinks to teardown / AS cutover, not the entire Open Scene.
- Per-frame budgets: texture finalize ms, mesh upload bytes/count, no per-resource `waitForIdle` on the happy path.
- Open Scene and runtime spawn share **StructureGpu committed-serve**; delete the long-term need for `flushPendingStructureGpuSync` except a documented cold-start fallback (then remove).
- Progress UI reads `LoadSession` only — not `asyncLoadingInProgress` + phase + suspended.

### RHI evolution (separate phases; this ADR does not widen create)

| Stage | Scope |
| --- | --- |
| **R0** (with P1–P2) | Keep Phase-1: create/submit/present/GC on Render pipe; parallel record OK |
| **R1** | Fence / timeline instead of `device->waitForIdle` for upload batches — **landed** (`StreamingUploadBudget`) |
| **R2** | First-class volatile CB binder (replace scattered `addVolatileConstantRewrite`) |
| **R3** | Future ADR: free-threaded create / multi-queue — **out of scope here** |

## Current inventory (as of this ADR)

### Schedulers

| Symbol | Path | Fate |
| --- | --- | --- |
| `JobSystem::*` | `caustica/caustica/include/core/JobSystem.h`, `src/core/JobSystem.cpp` | Replace with TaskRuntime; keep thin shim only during migration |
| `ThreadPool` / `ThreadPoolTask` | `include/core/ThreadPool.h`, `src/core/ThreadPool.cpp` | **Delete** after call sites migrate |
| `JobSystem::Initialize` | `src/engine/EntryPoint.cpp` | → `task::initialize` |

### Logic → Render

| Symbol | Path | Fate |
| --- | --- | --- |
| `RenderThread` | `include/engine/RenderThread.h` | Keep as Render-pipe executor (dedicated thread optional) |
| `EnqueueRenderCommand*` | `include/engine/EnqueueRenderCommand.h` | **Keep as sole public API** |
| `App::enqueueGpuWork*` / `runGpuWork*` | `include/engine/App.h` | Deprecate → remove |
| `enqueueGpuWorkOnRenderThread(App&)` | `RenderSessionApi` | Deprecate → remove |
| `GpuDevice::waitForRenderThreadIdle` | device / frame driver | Route through one wait helper |

### Frame / structure

| Symbol | Path | Fate |
| --- | --- | --- |
| `SceneRenderSnapshot` | `include/scene/SceneRenderSnapshot.h` | Keep |
| `SceneStructureGpuSync` | `include/scene/SceneStructureGpu.h` | Keep; become default for full load too |
| `enqueuePendingStructureGpu` / `flushPendingStructureGpuSync` | `src/engine/SceneSpawn.cpp` | Prefer enqueue; retire sync flush |
| `GraphBuilder` + `parallelWaves` | `include/render/graph/GraphBuilder.h` | Keep; dispatch via TaskRuntime |
| `FrameCommandContext` | `include/backend/rhi/command_list_pool.h` | Keep |
| Volatile CB rewrite | `GraphBuilder::addVolatileConstantRewrite` | Keep until R2 binder |

### Load path

| Symbol | Path | Fate |
| --- | --- | --- |
| `SceneLoader` | `include/scene/SceneLoader.h` | Keep as CPU import worker host; jobs become Background tasks |
| `loadSceneToPending` / `promotePendingScene` | `SceneManager` | Keep |
| `GpuBindPhase` / `tickSceneGpuBind` | `SceneViewState`, `SceneLifecycle.cpp` | Fold into `LoadSession` |
| `sceneGpuSuspended` | `SceneViewState` / `App::skipRenderPhase` | Narrow window |
| `GpuRenderSubsystem` trampoline | lifecycle / GPU bind | Thin or absorb into LoadSession owner |
| `flushTextures(8.f)` / `uploadMeshes(..., 1)` | `SceneLifecycle` / `SceneGpuUpdater` | Budget knobs on LoadSession |

### ThreadPool call-site families (migrate in P1)

- Scene: `Scene::load` / `loadWithThreadPool`, glTF / OBJ / USD / URDF importers (`ThreadPool*` args)
- Assets: `TextureLoader` async path, `AssetSystem`
- Render: `PathTracingShaderCompiler`, `ComputePipelineRegistry`
- Audio: `AudioCache`
- Graph: `GraphBuilder.cpp` (`JobSystem::dispatch` / `wait`)

## Non-goals

- Rewriting NVRHI or replacing COM refcounts
- Full UE TaskGraph / fiber runtime clone
- Free-threaded `createTexture` / `createBuffer` in this ADR
- Streaming levels / disk mip streaming (future; LoadSession budgets come first)
- New public enqueue API layers
- Big-bang single PR across TaskRuntime + LoadSession + RHI binder

## Consequences

### Positive

- One mental model for “run this work”
- Background load cannot silently stall parallel CL recording (priorities)
- Scene open and runtime spawn stop diverging
- `--syncRender` stops forking engine semantics
- Documentation and asserts can name Pipes instead of tribal knowledge

### Negative / cost

- Broad mechanical migration (importer signatures, shader compile, App APIs)
- Temporary shims during P1
- Early TaskRuntime may not be faster than today’s JobSystem until steal/job SOO work — **correctness and API unity are the P1 wins**

### Risks

| Risk | Mitigation |
| --- | --- |
| Desktop hard-lock without per-upload `waitForIdle` | R1 fence budgets + capped in-flight upload bytes before removing idles |
| Streamline / AS overlap hangs | Keep exclusive teardown window; FirstPresent only after safe committed packet |
| Priority inversion | Document Pipe + Priority rules; add queue-depth stats before P3 |
| Sync mode regressions | Single pipe pump path; PIX/headless CI on `--syncRender` |

## Implementation plan

### P0 — Freeze (this ADR)

- [x] ADR accepted
- [x] Link from render-proxy + RHI threading docs
- [x] Engineering rule: no new `ThreadPool` APIs, no new load bools, no new enqueue wrappers
- [x] TaskRuntime skeleton landed (`include/core/task/TaskRuntime.h`)

### P1 — TaskRuntime replaces JobSystem

1. [x] Add `caustica::task` (`include/core/task/TaskRuntime.h`, `src/core/task/TaskRuntime.cpp`).
2. [x] Implement initialize/shutdown, launch, pipe serial, priority queues, wait/poll, parallelFor, generations.
3. [x] Migrate `GraphBuilder` off `JobSystem` → `task::parallelFor` (`Priority::High`).
4. [x] EntryPoint calls `task::initialize` (JobSystem/ThreadPool removed).
5. [x] Migrate shader / compute compile + texture async + audio to `task::`.
6. [x] Collapse Logic→Render to `EnqueueRenderCommand*` (`App` enqueue methods private + friend).
7. [x] Delete `ThreadPool` / `JobSystem` files; importers use `bool asyncTextures` gate.

**Exit (P1):** feature-equivalent editor / samples; `parallelWaves` on; sole public RT enqueue pair; zero `ThreadPool`/`JobSystem` in code.

### P2 — Render frame as tasks

1. [x] `RenderThread` pumps `task::Affinity::Render` (dedicated thread remains default; `--syncRender` pumps in `executeRenderPhase`).
2. [x] One GC path: end of `executeRenderPhase` only (removed sync-only GC from `finalizeFrameTiming`).
3. [x] Structure cold start enqueue-only + empty present; `flushPendingStructureGpuSync` `[[deprecated]]` and unused.
4. [x] Unify `isRenderThread` → `ThreadDomain::Render` (`RenderThread::isRenderThread` delegates to free function).

**Exit:** no dual GC; structure sync flush deprecated/unused.

### P3 — LoadSession amortized streaming

1. [x] Implement `LoadSession` state machine; migrate `tickSceneGpuBind` → `tickLoadSession`.
2. [x] Budgeted texture/mesh upload via non-blocking `EnqueueRenderCommand` (+ R1 `StreamingUploadBudget` fences).
3. [x] Narrow `sceneGpuSuspended` to teardown window; present resumes for Importing/GpuStreaming.
4. [x] FirstPresent → StructureGpu `AccelOnly` (shared committed-serve path; no mesh re-upload).
5. [x] Progress / switch gates read `LoadSession`; `asyncLoadingInProgress` only for opacity/shader refresh outside session.

**Exit:** large scene open keeps UI/render ticking; no multi-second hard freeze from bind steps.

### P4 — RHI deepen (may split ADRs)

1. [x] R1 upload fences / timeline — `StreamingUploadBudget` (EventQuery + 256MB / 8-submit cap) on TextureLoader + mesh upload; `waitForIdle` kept for teardown / fallback only.
2. R2 volatile CB binder.
3. R3 only with new ADR + profiling justification.

## Frozen rules (effective immediately)

1. **Do not** reintroduce `ThreadPool` / `JobSystem` — use `caustica::task` only.
2. **Do not** add a second Logic→Render enqueue helper — extend `EnqueueRenderCommand` or wait for TaskRuntime pipes.
3. **Do not** add another “are we loading?” bool — extend `GpuBindPhase` only if LoadSession is not yet available; prefer a comment pointing at this ADR.
4. **Do not** widen RHI create to AnyThread without a new ADR.
5. Full scene GPU bind changes should move **toward** StructureGpu / budgets, not more `dispatchAndWait` steps.

## Success metrics

| Metric | Signal |
| --- | --- |
| API | One public RT enqueue pair; zero `ThreadPool` references |
| Load UX | Logic + present continue during GpuStreaming (P3) |
| Correctness | No Streamline/AS hard-hang on scene switch; PIX `--syncRender` green |
| Perf | P1 may be neutral; P3 reduces hitch length / peak stall ms during large open |
| Observability | Queue depth + load session phase visible in debug UI or log |

## References

- Current RHI contract: [architecture-rhi-threading.md](../architecture-rhi-threading.md)
- Extract / proxy / structure handoff: [architecture-render-proxy.md](../architecture-render-proxy.md)
- Embedding note for `EnqueueRenderCommand`: [embedding-cpp.md](../embedding-cpp.md)
- Industry direction (informational, not a mandate): UE named threads → `UE::Tasks` + Pipe; keep submit-domain serialization until RHI is proven free-threaded
