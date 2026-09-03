# Architecture: Render Data Pipeline + RHI Threading

Caustica splits **logic** and **render** via Extract + `RenderThread` (see
[architecture-render-proxy.md](architecture-render-proxy.md)). This document is
the data-path and RHI contract that makes that split, two-frame overlap, and
parallel command-list recording safe.

Do **not** reintroduce `ThreadPool` / `JobSystem` — `caustica::task` is the only
scheduler. Do **not** add another Logic→Render enqueue helper — extend
`EnqueueRenderCommand` / `EnqueueRenderCommandAndWait` only. Do **not** add
another “are we loading?” flag — extend `LoadSession`. Do **not** make resource
those labels mean the rules in this file.

## Pipeline (Logic → GPU)

```text
Logic (ThreadDomain::Logic)                         Render domain (ThreadDomain::Render)
───────────────────────────                         ───────────────────────────────────
App::runFrame
  pumpLogic
  First / preUpdate / update / PostUpdate           (ECS may run concurrent systems)
  Extract (serial)
    extractSceneRenderData / extractFrameRenderState
    Scene::extractAndPublishRenderSnapshot
         │
         ▼
  SceneRenderSnapshot[frame % 3]  ───────────────►  WorldRenderer::render
  (proxies + settings + camera)                     beginGpuReadFrame(frameIndex)
                                                    serve committed structure if AS in flight
  dispatchScheduledRender                           runFramePipeline
    RenderThread::dispatch  ── Affinity::Render ──►   setup / AS / needNewPasses
                                                      beginPrimary
                                                      scene + path-trace prepare
                                                      GraphBuilder::compile + execute
                                                        serial wave  → primary
                                                        parallel wave → flushPrimary
                                                                        fork() × N
                                                                        task::parallelFor record
                                                                        submitForks
                                                      endFrame (close + execute primary)
                                                    present
                                                    Device::runGarbageCollection
```

Headless and `--syncRender` skip the dedicated OS thread. Logic enters
`ThreadDomain::Render` for `executeRenderPhase` and pumps `Affinity::Render`
itself. `EngineApp` forces this when `headless` is true
(`dedicatedRenderThread && !headless`).

`caustica::isRenderThread()` means `ThreadDomain::Render`, not an OS thread id.

### Frame identity

| Item | Value | Why |
| --- | --- | --- |
| `RenderThread::kMaxInFlightFrames` | 2 | Logic may start N+1 while Render finishes N |
| `SceneRenderSnapshot::kSlotCount` | 3 | Writer slot + two in-flight readers; skipped Extract falls back to latest |
| Snapshot publish | Logic, after Extract | `publish(frameIndex)` release-stores the slot |
| Snapshot consume | Render, `WorldRenderer::render` | `beginGpuReadFrame` / `getRenderData()` / `committedRenderData()` |

A no-render gap can rotate the logic frame index onto a slot still owned by an
older GPU frame. `App::runFrame` drains the render thread once before the next
Extract (`// THREADING: Logic↔RT wait`). Consecutive render frames do not.

### What each layer owns

| Layer | Writes | Reads | Must not |
| --- | --- | --- | --- |
| Logic ECS + Extract | Live components → flat proxies in the frame slot | App resources, `ResolvedActiveCamera` | `executeCommandLists` / present / GC |
| `SceneRenderSnapshot` | Triple-buffer packet | Render reads the slot for its phase frame | Share a slot across two live GPU frames |
| `LoadSession` | Owned `shared_ptr<const SceneRenderData>` | Render stream steps | Borrow a triple-buffer slot |
| Structure GPU | Copied packet + `EnqueueRenderCommand` | RT builds meshes / AS / SBT | Bind unpublished structure proxies |
| `WorldRenderer` | Frame CBs, graph, primary + forks | Snapshot / committed proxies | Touch live ECS |
| `GraphBuilder` | Waves, transient alloc, local state copies | Imported + transient resources | Mutate shared `currentState` from workers |
| `FrameCommandContext` | Open / close / submit | Pool of deferred lists | Let workers close or submit |
| RHI device / queue | Submit, fence, `permanentState` writeback | Closed command lists | Record from two threads into one list |

### Two ways work enters the render domain

All of it is `task::Affinity::Render`. `RenderThread` only wakes, pumps, and
paces frames. There is no second `m_queue`.

| Entry | How | Counts toward `kMaxInFlightFrames` |
| --- | --- | --- |
| Interactive frame | `RenderThread::dispatch` → `executeRenderPhase` | Yes (`m_inFlight`) |
| `EnqueueRenderCommand` | Same `dispatch` | Yes — a structure-GPU job can stall the next frame enqueue |
| `EnqueueRenderCommandAndWait` | `dispatchAndWait` (idle, then Critical) | Blocks Logic until the domain is empty |
| `LoadSession` stream step | Direct `task::launch(Affinity::Render)` on the LoadSession pipe | No — present continues during GpuStreaming |
| `--syncRender` command | `task::launch` + `pumpRender` on Logic | N/A (no dedicated thread) |

`LoadSession` is the only long-running GPU producer that must not take a frame
slot. Occasional Logic→RT work (structure build, resize, splat) uses
`EnqueueRenderCommand*` and therefore shares the two-frame budget.

### WorldRenderer frame path

`AppSchedule::render` is serial. `PathTracingPlugin` calls `renderScene` →
`WorldRenderer::render` → `runFramePipeline`:

1. **Bind the packet** — camera / settings always from the latest Extract slot.
   While `structureGpuBuildInFlight()`, geometry/SBT come from
   `committedRenderData()`; lights overlay from the latest slot so lighting
   stays live during AS rebuild.
2. **Setup** — `recreateAccelStructs` on `ensurePrimary()` (closed helper list).
   `needNewPasses` still fences the graphics queue around
   `createRenderPasses` (close / execute / fence / reopen).
3. **`beginPrimary`** — open the frame list; start the GPU frame timer.
4. **CPU prepare on the open primary** — camera, scene GPU refresh, path-trace
   constants. This is still serial on the render thread.
5. **Graph** — `compile` (cached plan when topology is stable), bind
   `VolatileConstantBinder` (`FrameConstants` + RTXDI bridge CB), `execute`.
6. **`endFrame`** — submit leftover forks, then close + execute primary.
7. **Present + GC** — `GpuSurface::presentFrame`, then
   `Device::runGarbageCollection` at the end of `executeRenderPhase` (not in
   Logic `finalizeFrameTiming`).

`ParallelRenderGraphRecording` (default true) and the cost knobs live on
`PathTracerSettings`.

## Thread roles

| Role | Responsibility |
| --- | --- |
| Logic thread | ECS, Extract, snapshot publish. No `executeCommandLists` / `present` / `runGarbageCollection`. |
| Render thread (`RenderThread`) | Sole owner of queue submit, present, and RHI GC. Owns `FrameCommandContext` fork/join/submit. Pumps `task::Affinity::Render`. |
| RHI workers (`Affinity::Any`) | May **record** into deferred lists that were forked and opened on the render thread. Must not close/submit/present/GC or enter `ThreadDomain::Render`. |

## Core API rules

| Operation | Allowed on |
| --- | --- |
| `createCommandList` / open–record–close | Render thread (workers may record into already-open forked lists) |
| `executeCommandLists` / `executeCommandList` | Render thread only (backends also mutex-serialize) |
| `queueWaitForCommandList` | Render thread only |
| `runGarbageCollection` | Render thread only |
| `waitForIdle` | Render thread only; treat as a sync point |
| `createTexture` / `createBuffer` / other creates | Render thread only; there is no general device-wide free-threaded create contract. |
| `mapBuffer` / `mapStagingTexture` | Render thread; may CPU-wait a fence |
| Present / swapchain resize | Render thread (`dispatchAndWait` for resize) |

`CommandListPool::acquire` is mutex-serialized and may call `createCommandList`
on a cache miss. Only the render thread calls `beginPrimary` / `fork`, so create
stays on the render thread. Do not call `acquire` from a worker.

## Deferred vs immediate command lists

- **DX12 / Vulkan:** Prefer `CommandListParameters::enableImmediateExecution = false`
  (the default). Multiple deferred lists may be open at once. The pool forces this.
- **DX11:** Immediate only. The D3D11 backend upgrades deferred requests to
  immediate; `executeCommandLists` is a no-op. Parallel waves do not apply.
- Mid-frame `close → execute → waitForIdle → open` on a shared list is a
  **sync point**. Keep it rare, RT-only, and annotated
  `// THREADING: sync-point, RT-only`. Mark such graph passes
  `PassOptions::serialOnPrimary`.
- Interactive waits use a graphics `EventQuery` (`syncGraphicsQueueFence`), not
  device-wide idle. `runGarbageCollection` runs once at the end of
  `App::executeRenderPhase`.
- Streaming texture/mesh uploads use `StreamingUploadBudget` (EventQuery +
  in-flight byte/submit caps) instead of per-batch `waitForIdle`. Teardown /
  device-destroy paths may still idle.

## CommandListPool / FrameCommandContext

Headers: `caustica/caustica/include/backend/rhi/command_list_pool.h`.

| Type | Role |
| --- | --- |
| `CommandListPool` | Mutex-protected acquire/release of deferred `CommandList` objects per queue. |
| `FrameCommandContext` | Sole owner of the frame primary list + forked deferred lists. |

`WorldRenderer` owns one pool and one `FrameCommandContext`. There is no
separate recycled `m_commandList` — use `beginPrimary` / `flushPrimary` /
`endFrame` / `ensurePrimary`.

### Usage sketch

```cpp
// Render thread — frame open
frameCtx.beginPrimary();
// ... scene / path-trace prep on frameCtx.primary() ...

// GraphBuilder: serial waves record on primary.
// A parallel wave flushes the primary (so GPU order matches wave order),
// forks N deferred lists, records on Affinity::Any, then submitForks.
graph.execute(frameCtx);

frameCtx.endFrame(); // leftover forks + close/execute primary
```

### Rules

- `fork` / `closeFork` / `submitForks` / `endFrame` / `flushPrimary` /
  `beginPrimary` / `ensurePrimary`: **render thread only**
- Workers: record only into lists already returned by `fork()`
- Do not issue conflicting `permanentState` transitions across forked lists in
  one submit batch
- `flushPrimary` is `close → execute → open` on the same handle. It does **not**
  `waitForIdle`. It **does** clear that list’s NVRHI volatile-CB address map.

## GraphBuilder waves

`GraphBuilder::compile` builds dependency waves from **resource read/write**
(including textures, buffers, accel structs, and WAR) plus optional
`PassOptions::after`. Ready passes are split by
`PassOptions::queue` (Copy, then Compute, then Graphics) so async work can
submit before later graphics recording. A consumer on another queue records a
`waitWaves` edge; `execute` turns that into
`Device::queueWaitForCommandList`. DX11 (no compute/copy queue) falls back to
Graphics. After all waves, `execute` joins leftover async queues back to
graphics unless a graphics wait inserted during the frame already covers that
queue's latest submitted instance (extract / present still get a join whenever
trailing async work was never consumed by graphics).

Public HDR / LDR / depth identity is `FrameSlots` (seeded once per frame
with write version 0). `read()` / `write()` resolve those handles to the
latest version, so register functions do not re-import or sync public
images. Feature switches stay `if`s in `registerDefaultFrameGraphPasses`.

Resource ownership: `createTexture` / `createBuffer` are **graph-owned**
transients (aliased at compile). A second `createTexture` with the same
non-empty name returns the first handle so multiple register functions can
declare one scratch. `importTexture` / `importBuffer` / `importAccelStruct`
are **external** (history, present, vendor, TLAS). `extract` is only for
resources that must outlive `execute()`. Handles carry a generation;
`reset()` invalidates the previous frame's handles. Texture handles also
carry a sequential **write version**: `write()` / `readWrite()` produce
`latest+1`. Version 0 is identity and binds to the current latest; a
non-zero handle is a pinned `write()` result (`currentTexture()` if it
went stale). `generation` is still only the `reset()` epoch.

`serialOnPrimary` passes never share a wave and always run on the graphics
primary. `execute(FrameCommandContext&, ExecuteParams)`:

- Compute/copy waves acquire a list from `CommandListPool` on that queue,
  record, and `executeCommandList` immediately.
- Serial / single-pass / `serialOnPrimary` graphics waves record on the open
  primary.
- Multi-pass graphics waves (when `ExecuteParams::parallelWaves`) pack
  independent passes into a **bounded number of recording batches** (declared
  `recordingCost`, EMA of measured microseconds, `minParallelRecordingCost`,
  `maxParallelRecordingJobs`; 0 jobs = `task::workerCount()`). Below the cost
  threshold the wave stays on the primary.
- A parallel graphics wave then:
  1. `flushPrimary` so earlier serial work is submitted before the forks
  2. Snapshot `currentState` into per-batch local vectors (no shared mutation)
  3. `fork` one deferred list per batch; emit aliasing barriers on the RT
  4. `task::parallelFor` (`Priority::High`, `Affinity::Any`) records
  5. `submitForks` on the RT; `syncPassEndStates` updates the shared snapshot

Passes that close/execute/wait mid-body (e.g. ToneMapping auto-exposure) must
set `PassOptions::serialOnPrimary = true`.

### Volatile constant buffers

NVRHI tracks volatile CB GPU addresses **per command-list open session**.
`close` / `flushPrimary` clears that map. A `writeBuffer` on list instance A
does **not** satisfy a bind on list instance B (or on A after reopen).

Implications:

- `ExecuteParams::parallelWaves` defaults to **true**. `rg::VolatileConstantBinder`
  (`GraphBuilder::volatileConstants()`) rewrites registered CPU shadows at the
  start of every `recordPass` so flush/fork open sessions stay valid.
- WorldRenderer binds `FrameConstants` and the RTXDI bridge CB
  (`RtxdiPass::bridgeConstantsCpu`) on the binder before `execute`.
- `FrameConstants` is graph-owned (`UploadFrameConstants`, refreshed again by
  `UploadSubInstanceData` / after serial sync-points like ToneMapping and
  ReferenceOIDN).

## Resource state

- Transient barrier state is **per command list**. Parallel batches start from a
  copy of the graph snapshot taken on the RT; they never write
  `GraphTexture::currentState` / `GraphBuffer::currentState`.
- `permanentState` on a resource is global; backends write it during submit
  under the queue/device submit lock (`CommandListResourceStateTracker::commandListSubmitted`).
- Do not issue conflicting permanent transitions for the same resource from two
  command lists in one submit batch.

## Lifetime / GC

- In-flight command-list instances keep referenced resources alive until
  `runGarbageCollection` retires them past the queue fence / timeline (primary
  lifetime mechanism).
- `DeferredDeletionQueue` defers native resource release when a buffer/staging
  object still has a last-use fence/timeline (destroy path must not CPU-wait).
  Flushed from `runGarbageCollection`.
- `mapBuffer` / `mapStagingTexture` may still CPU-wait a fence (RT-only).
- Logic-thread `finalizeFrameTiming` must **not** call `runGarbageCollection`
  when the dedicated render thread is active; GC runs at the end of
  `executeRenderPhase` on the render thread.

## Backend guarantees

- **D3D12:** `Device::m_Mutex` serializes `executeCommandLists`,
  `queueWaitForCommandList`, `waitForIdle`, and `runGarbageCollection`
  (including `permanentState` writeback in `CommandList::executed`).
  `createCommandList` / `createTexture` / `createBuffer` are **not** under this
  lock — they stay RT-only by contract. Recording into a given list is
  single-threaded (one worker or the RT).
- **Vulkan:** `Device::m_Mutex` serializes `executeCommandLists` + `executed`
  writeback, `waitForIdle`, and GC. `Queue::m_Mutex` serializes `submit`,
  wait/signal semaphore staging, `getOrCreateCommandBuffer` (`CommandList::open`),
  and `retireCommandBuffers`. Pool acquire of a recycled command buffer is
  free-threaded under that queue lock.

## Frame-path sync

Prefer graphics `EventQuery` / queue fence / retire over device-wide
`waitForIdle` on the interactive frame (AE, pick/feedback, `needNewPasses`,
OMM, DLSS/SL `freeResources`). Frame resize is non-blocking
`EnqueueRenderCommand`.

Still allowed to idle **on the render thread**: app/device shutdown, LoadSession
exclusive teardown, device-lost recovery, documented one-shot tools.

Remaining Logic↔RT waits (not the editor frame loop): skip-render gap idle,
splat Pass create/remove, precache `AndWait`, editor undo/redo. Realtime
animation no longer locksteps: Extract publishes an immutable joint palette
per slot, and GPU skinning / skinned BLAS reuse is ordered by the graphics
queue. Do not delete annotated sync-points in drive-by PRs.

```text
// THREADING: sync-point, RT-only — (short reason)
// THREADING: Logic↔RT wait — (short reason)
// THREADING: sync-point, shutdown — allowed
```

## Assessment

The split is the right one: Logic never submits, workers never own a list’s
lifetime, and the GPU sees wave order because the RT flushes the primary before
forks. Triple-buffer Extract plus a two-frame render queue is enough overlap
for a path tracer without a second ECS.

What is working as designed:

- One scheduler, one render-domain queue, one public enqueue API.
- Snapshot / committed-serve so spawn and LoadSession do not
  `waitForRenderThreadIdle` on the structure path.
- Local resource-state snapshots + `VolatileConstantBinder` so parallel
  recording does not race NVRHI per-list state.
- Backend submit locks as a second line of defense, not as a substitute for
  the RT-only contract.

Known costs, not bugs:

- Every parallel wave is an extra `executeCommandLists` (the primary flush).
  Cheap waves stay on the primary via the cost threshold.
- `EnqueueRenderCommand` shares `kMaxInFlightFrames` with the interactive
  frame. A structure-GPU job can delay the next `dispatch`. That is safer
  than an unbounded RT queue; LoadSession is the exception because it must
  overlap present.
- `needNewPasses` still fences the graphics queue. Rare, RT-only.
- Skinned vertex buffers and BLAS stay single-buffered. That is safe while
  every write stays on the graphics queue after the previous frame’s submits.
  Double-buffer them only if skinning or BLAS update moves off that queue.

## Out of scope

- Free-threaded resource create across many threads
- Replacing intrusive `AddRef`/`Release` with non-COM lifetime
- A second EnTT “render ECS” (see [architecture-render-proxy.md](architecture-render-proxy.md))
