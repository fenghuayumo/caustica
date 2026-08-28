# ADR 0001: TaskRuntime + unified streaming / render threading


| Field    | Value                                                                                                                                                                                                                   |
| -------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Status   | **Accepted**; P1–P3 + R1/R2 + TaskRuntime domain pumps / LoadSession / sole Affinity::Render queue landed (R3 follow)                                                                                                   |
| Date     | 2026-08-07                                                                                                                                                                                                              |
| Deciders | Caustica engine                                                                                                                                                                                                         |
| Relates  | [ADR 0002](0002-frame-path-rhi-sync.md), [ADR 0003](0003-taskgraph-parallel-ecs.md), [architecture-render-proxy.md](../architecture-render-proxy.md), [architecture-rhi-threading.md](../architecture-rhi-threading.md) |


## Context

### Historical problem (why this ADR existed)

Before P1–P3 the engine already had Logic / Extract / RenderThread, triple-buffered `SceneRenderSnapshot`, and `FrameCommandContext` waves — but scheduling and Open Scene orchestration were not product-grade:

1. **`JobSystem` / `ThreadPool`** — mutex pool, no priorities / deps / generation / domains.
2. **Overlapping Logic→Render enqueue layers** — `RenderThread::dispatch*`, App “GpuWork” helpers, session free functions, plus idle aliases.
3. **Divergent full-load vs runtime structure** — `GpuBindPhase` + per-step waits under `sceneGpuSuspended` vs StructureGpu committed-serve.
4. **Sync escapes** — dual GC paths, `flushPendingStructureGpuSync`, per-upload `waitForIdle`.
5. **Overlapping “are we loading?” flags** — loader / suspended / bind phase / diag bools.

### Current state (post P1–P3 + follow-ups)


| Area              | Status                                                                                                                                              |
| ----------------- | --------------------------------------------------------------------------------------------------------------------------------------------------- |
| Scheduler         | `caustica::task` only; `JobSystem` / `ThreadPool` **deleted**                                                                                       |
| Logic→RT          | Sole public API `EnqueueRenderCommand*` → App private impl → **one** `Affinity::Render` queue (`RenderThread` pumps; `--syncRender` pumps on Logic) |
| Open Scene        | `LoadSession` + budgeted GpuStreaming; host busy = `LoadSession::isBusy()`                                                                          |
| Structure         | Enqueue-only + committed-serve; sync flush **removed**                                                                                              |
| Upload happy path | R1 `StreamingUploadBudget` (no per-batch `waitForIdle`)                                                                                             |
| Remaining sync    | Frame-path sync → **[ADR 0002](0002-frame-path-rhi-sync.md)** (fence/retire; not LoadSession glue)                                                  |
| Built on         | DAG authoring + concurrent ECS systems → **[ADR 0003](0003-taskgraph-parallel-ecs.md)**                                                             |
| Next              | ADR 0002 S1–S5; R3 free-threaded create (separate ADR + profiling)                                                                                  |


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


| Capability    | Requirement                                                                                |
| ------------- | ------------------------------------------------------------------------------------------ |
| Launch        | `launch(TaskDesc) → TaskHandle`                                                            |
| Dependencies  | prerequisite / subsequent (or equivalent fence chaining)                                   |
| Pipe          | tasks on the same `Pipe` run serially in submission order                                  |
| Priority      | at least `High` / `Normal` / `Background` (frame record must not starve under load decode) |
| Affinity      | `Any` | `Logic` (`pumpLogic`) | `Render` (`pumpRender`) | `IO` (dedicated worker)          |
| Wait / poll   | `wait(handle)`, `poll(handle)`, context/group wait                                         |
| Generation    | `FrameGen` / `LoadGen` so abandoned work can be discarded                                  |
| Observability | `snapshotStats()` queue depths + LoadSession phase in editor Debugging panel               |


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
    TaskFn fn = nullptr;           // P1.1 fixed job (preferred)
    void* user = nullptr;
    std::function<void()> body;    // capture-heavy / legacy
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

**Pipes:**


| Pipe                   | Status                                                               |
| ---------------------- | -------------------------------------------------------------------- |
| `LoadSession`          | Registered at `task::initialize`; Import + GpuStreaming steps        |
| `Logic` / `RHI.Submit` | Reserved names via `getPipe()` on demand (not eagerly registered)    |
| Affinity::Render       | Sole Logic→RT domain queue (frames, commands, LoadSession GPU steps) |


Workers never close / submit / present / GC. That stays the RHI threading contract.

### Logic → Render API freeze (P1)

Public / engine-facing surface collapses to **one** pair:

- `EnqueueRenderCommand(App&, Fn)` — non-blocking onto Render pipe
- `EnqueueRenderCommandAndWait(App&, Fn)` — sync point; rare; annotated

Private / internal (not a second public API):

- `App::enqueueRenderCommandImpl` / `enqueueRenderCommandAndWaitImpl` — backing for the public pair
- `RenderThread::dispatch*` — only TaskRuntime / App internals
- Scene/editor code must not call `RenderThread` or invent new enqueue helpers

`--syncRender`: **Affinity::Render is pumped on the Logic thread** (same `executeRenderPhase` + GC tail). No second completion / GC algorithm.

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


| Stage               | Scope                                                                                                       |
| ------------------- | ----------------------------------------------------------------------------------------------------------- |
| **R0** (with P1–P2) | Keep Phase-1: create/submit/present/GC on Render pipe; parallel record OK                                   |
| **R1**              | Fence / timeline instead of `device->waitForIdle` for upload batches — **landed** (`StreamingUploadBudget`) |
| **R2**              | First-class `rg::VolatileConstantBinder` — **landed** (`GraphBuilder::volatileConstants()`)                 |
| **R3**              | Future ADR: free-threaded create / multi-queue — **out of scope here**                                      |


## Where it landed


| Piece              | Path                                                                                            |
| ------------------ | ----------------------------------------------------------------------------------------------- |
| Scheduler          | `include/core/task/TaskRuntime.h`                                                               |
| Render-domain pump | `include/engine/RenderThread.h` (pumps `Affinity::Render` only)                                 |
| Logic→RT enqueue   | `include/engine/EnqueueRenderCommand.h` (sole public pair)                                      |
| RT drain           | `GpuDevice::waitForRenderThreadIdle`                                                            |
| Frame packet       | `include/scene/SceneRenderSnapshot.h`                                                           |
| Structure handoff  | `include/scene/SceneStructureGpu.h`, `enqueuePendingStructureGpu` (`src/engine/SceneSpawn.cpp`) |
| Parallel record    | `include/render/graph/GraphBuilder.h` + `FrameCommandContext`                                   |
| Volatile CBs       | `rg::VolatileConstantBinder` / `GraphBuilder::volatileConstants()`                              |
| CPU import         | `include/scene/SceneLoader.h` (Background tasks)                                                |
| Load state machine | `LoadSession` / `tickLoadSession`; budgets on `SceneLifecycle` / `SceneGpuUpdater`              |
| Teardown window    | `sceneGpuSuspended` (`SceneViewState` / `App::skipRenderPhase`)                                 |


### Remaining frame-path sync (out of R1 scope)

Upload streaming no longer per-batch idles. Frame-path RT sync is owned by
**[ADR 0002](0002-frame-path-rhi-sync.md)** (fence / timeline / retire; Logic no
frame-path `AndWait`). Cold start / shutdown may keep `waitForIdle`.

## Non-goals

- Rewriting NVRHI or replacing COM refcounts
- Fiber runtime. (The UE-style DAG / completion-event *shape* later landed as a thin layer over
  this runtime in [ADR 0003](0003-taskgraph-parallel-ecs.md); still no fibers.)
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


| Risk                                               | Mitigation                                                                    |
| -------------------------------------------------- | ----------------------------------------------------------------------------- |
| Desktop hard-lock without per-upload `waitForIdle` | R1 fence budgets + capped in-flight upload bytes before removing idles        |
| Streamline / AS overlap hangs                      | Keep exclusive teardown window; FirstPresent only after safe committed packet |
| Priority inversion                                 | Document Pipe + Priority rules; add queue-depth stats before P3               |
| Sync mode regressions                              | Single pipe pump path; PIX/headless CI on `--syncRender`                      |


## Implementation plan

### P0 — Freeze (this ADR)

- [x] ADR accepted
- [x] Link from render-proxy + RHI threading docs
- [x] Engineering rule: no new `ThreadPool` APIs, no new load bools, no new enqueue wrappers

1. TaskRuntime skeleton landed (`include/core/task/TaskRuntime.h`)
2. P1 — TaskRuntime replaces JobSystem
3. [x] Add `caustica::task` (`include/core/task/TaskRuntime.h`, `src/core/task/TaskRuntime.cpp`).
4. [x] Implement initialize/shutdown, launch, pipe serial, priority queues, wait/poll, parallelFor, generations.
5. [x] Migrate `GraphBuilder` off `JobSystem` → `task::parallelFor` (`Priority::High`).
6. [x] EntryPoint calls `task::initialize` (JobSystem/ThreadPool removed).
7. [x] Migrate shader / compute compile + texture async + audio to `task::`.
8. [x] Collapse Logic→Render to `EnqueueRenderCommand*` (`App` enqueue methods private + friend).
9. [x] Delete `ThreadPool` / `JobSystem` files; importers use `bool asyncTextures` gate.

**Exit (P1):** feature-equivalent editor / samples; `parallelWaves` on; sole public RT enqueue pair; zero `ThreadPool`/`JobSystem` in code.

### P2 — Render frame as tasks

1. [x] `RenderThread` pumps `task::Affinity::Render` (dedicated thread remains default; `--syncRender` pumps in `executeRenderPhase`).
2. [x] One GC path: end of `executeRenderPhase` only (removed sync-only GC from `finalizeFrameTiming`).
3. [x] Structure cold start enqueue-only + empty present; `flushPendingStructureGpuSync` removed.
4. [x] Unify `isRenderThread` → `ThreadDomain::Render` (`RenderThread::isRenderThread` delegates to free function).
5. [x] Sole Logic→RT queue: `RenderThread::dispatch` / `--syncRender` enqueue → `Affinity::Render` (no parallel `m_queue`).

**Exit:** no dual GC; one Affinity::Render domain for frames + LoadSession + commands.

### P3 — LoadSession amortized streaming

1. [x] Implement `LoadSession` state machine; migrate `tickSceneGpuBind` → `tickLoadSession`.
2. [x] Budgeted texture/mesh upload via non-blocking `EnqueueRenderCommand` (+ R1 `StreamingUploadBudget` fences).
3. [x] Narrow `sceneGpuSuspended` to teardown window; present resumes for Importing/GpuStreaming.
4. [x] FirstPresent → StructureGpu `AccelOnly` (shared committed-serve path; no mesh re-upload).
5. [x] Progress / switch gates read `LoadSession::isBusy()`; OMM uses `secondaryStreaming` (diag scratch mirrored); FirstPresent waits StructureGpu commit; Teardown is async enqueue + poll (no AndWait).

**Exit:** large scene open keeps UI/render ticking; no multi-second hard freeze from bind steps.

### P1 follow-up — domain pumps / pipes / observability (landed)

1. [x] `Affinity::Logic` / `Affinity::IO` own queues; `pumpLogic()` each App update; dedicated IO worker(s).
2. [x] Well-known pipe at initialize: `LoadSession` only (`Logic` / `RHI.Submit` on demand via `getPipe`).
3. [x] LoadSession import + GpuStreaming steps on `loadSessionPipe()`; load jobs use `TaskFn` + LoadGen check.
4. [x] P1.1 `TaskFn` + `void* user` (body kept for capture-heavy compile / GraphBuilder waves).
5. [x] Host load/edit gates read `LoadSession::isBusy()`; Open Scene uses the narrower `isSceneSwitchBusy()` so teardown can cancel secondary OMM streaming.
6. [x] Editor Debugging → TaskRuntime / LoadSession queue depths + phase (success metric).

### P4 — RHI deepen (may split ADRs)

1. [x] R1 upload fences / timeline — `StreamingUploadBudget` (EventQuery + 256MB / 8-submit cap) on TextureLoader + mesh upload; `waitForIdle` kept for teardown / fallback only.
2. [x] R2 volatile CB binder — `rg::VolatileConstantBinder`; `GraphBuilder::volatileConstants()`; `WorldRendererFrame` registers FrameConstants + RTXDI bridge.
3. R3 only with new ADR + profiling justification.

## Frozen rules (effective immediately)

1. **Do not** reintroduce `ThreadPool` / `JobSystem` — use `caustica::task` only.
2. **Do not** add a second Logic→Render enqueue helper — extend `EnqueueRenderCommand`* only.
3. **Do not** add another “are we loading?” bool — extend `LoadSession` (phase / `secondaryStreaming` / `isBusy`) only.
4. **Do not** widen RHI create to AnyThread without a new ADR (R3).
5. Full scene GPU bind changes stay on StructureGpu / LoadSession budgets — no new Logic-thread `dispatchAndWait` / device `waitForIdle` on the happy path.
6. Frame-path sync follows [ADR 0002](0002-frame-path-rhi-sync.md); do not “fix” annotated RT idles drive-by in LoadSession PRs.

## Success metrics


| Metric        | Signal                                                                                                           |
| ------------- | ---------------------------------------------------------------------------------------------------------------- |
| API           | One public RT enqueue pair; zero `ThreadPool` references                                                         |
| Load UX       | Logic + present continue during GpuStreaming (P3)                                                                |
| Correctness   | No Streamline/AS hard-hang on scene switch; PIX `--syncRender` green                                             |
| Perf          | P1 may be neutral; P3 reduces hitch length / peak stall ms during large open                                     |
| Observability | Queue depth + load session phase visible in debug UI or log — **landed** (Debugging → TaskRuntime / LoadSession) |


## References

- Current RHI contract: [architecture-rhi-threading.md](../architecture-rhi-threading.md)
- Extract / proxy / structure handoff: [architecture-render-proxy.md](../architecture-render-proxy.md)
- Embedding note for `EnqueueRenderCommand`: [embedding-cpp.md](../embedding-cpp.md)
- Industry direction (informational, not a mandate): UE named threads → `UE::Tasks` + Pipe; keep submit-domain serialization until RHI is proven free-threaded

