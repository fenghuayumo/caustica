# Architecture: RHI Threading Contract

Caustica already splits **logic** and **render** via Extract + `RenderThread` (see [architecture-render-proxy.md](architecture-render-proxy.md)). This document defines the RHI rules that make that split and parallel command-list recording safe.

## Thread roles

| Role | Responsibility |
| --- | --- |
| Logic thread | ECS, Extract, snapshot publish. No `executeCommandLists` / `present` / `runGarbageCollection`. |
| Render thread (`RenderThread`) | Sole owner of queue submit, present, and RHI GC. Owns `FrameCommandContext` fork/join/submit. |
| RHI workers | May **record** into deferred command lists that were forked on the render thread. Must not close/submit/present/GC. |

`caustica::isRenderThread()` is true on the dedicated render thread. When the dedicated thread is disabled, the logic thread temporarily acts as the render thread for GPU work.

## Phase 1 API rules

| Operation | Allowed on |
| --- | --- |
| `createCommandList` / open–record–close | Render thread (workers may record into already-open forked lists) |
| `executeCommandLists` / `executeCommandList` | Render thread only (backends also mutex-serialize) |
| `queueWaitForCommandList` | Render thread only |
| `runGarbageCollection` | Render thread only |
| `waitForIdle` | Render thread only; treat as a sync point |
| `createTexture` / `createBuffer` / other creates | Render thread only (Phase 1; no device-wide create lock yet) |
| `mapBuffer` / `mapStagingTexture` | Render thread; may CPU-wait a fence |
| Present / swapchain resize | Render thread (`dispatchAndWait` for resize) |

## Deferred vs immediate command lists

- **DX12 / Vulkan:** Prefer `CommandListParameters::enableImmediateExecution = false` (the default). Multiple deferred lists may be open at once (validation allows this).
- **DX11:** Immediate only. The D3D11 backend upgrades deferred requests to immediate.
- Mid-frame `close → execute → waitForIdle → open` on a shared list is a **sync point**. Keep it rare, RT-only, and annotated `// THREADING: sync-point, RT-only`. Mark such graph passes `PassOptions::serialOnPrimary`.

## Phase 2: CommandListPool / FrameCommandContext

Headers: `rhi/command_list_pool.h`.

| Type | Role |
| --- | --- |
| `CommandListPool` | Thread-safe acquire/release of deferred `CommandList` objects per queue. |
| `FrameCommandContext` | Sole owner of the frame primary list + forked deferred lists. |

`WorldRenderer` owns one pool and one `FrameCommandContext`. There is no separate recycled `m_commandList` — use `beginPrimary` / `flushPrimary` / `endFrame` / `ensurePrimary`.

### Usage sketch

```cpp
// Render thread — frame open
frameCtx.beginPrimary();
// ... scene / path-trace prep on frameCtx.primary() ...

// GraphBuilder (Phase 3): parallel waves fork + JobSystem record + submitForks
graph.execute(frameCtx);

frameCtx.endFrame(); // close + execute primary (and any leftover forks)
```

### Rules

- `fork` / `closeFork` / `submitForks` / `endFrame` / `flushPrimary` / `beginPrimary` / `ensurePrimary`: **render thread only**
- Workers: record only into lists already returned by `fork()`
- Do not issue conflicting `permanentState` transitions across forked lists in one submit batch

## Phase 3: GraphBuilder waves

`GraphBuilder::compile` builds dependency waves (including WAR edges). `execute(FrameCommandContext&, ExecuteParams)`:

- Serial / single-pass / `serialOnPrimary` waves record on the open primary list.
- Multi-pass waves (when `ExecuteParams::parallelWaves`) fork one deferred list per pass, record on JobSystem workers with **local** resource-state snapshots (no shared `currentState` mutation), then `submitForks` on the RT before the next wave.

Passes that close/execute/wait mid-body (e.g. ToneMapping auto-exposure) must set `PassOptions::serialOnPrimary = true`.

### Volatile constant buffers

NVRHI tracks volatile CB GPU addresses **per command-list open session**. `close` / `flushPrimary` clears that map. A `writeBuffer` on list instance A does **not** satisfy a bind on list instance B (or on A after reopen).

Implications:

- `ExecuteParams::parallelWaves` defaults to **true**. GraphBuilder rewrites registered
  volatile CPU shadows (`addVolatileConstantRewrite`) at the start of every `recordPass`
  so flush/fork open sessions stay valid. WorldRenderer registers `FrameConstants`.
- WorldRenderer also registers the RTXDI bridge CB CPU shadow (`RtxdiPass::bridgeConstantsCpu`)
  so ReSTIR consumers stay valid across parallel waves after `FillConstants`.
- `FrameConstants` is graph-owned (`UploadFrameConstants`, refreshed again by `UploadSubInstanceData` / after serial sync-points like ToneMapping and ReferenceOIDN).

## Resource state

- Transient barrier state is **per command list**.
- `permanentState` on a resource is global; backends write it during submit under the queue/device submit lock.
- Do not issue conflicting permanent transitions for the same resource from two command lists in one submit batch.

## Lifetime / GC

- In-flight command-list instances keep referenced resources alive until `runGarbageCollection` retires them past the queue fence / timeline (primary lifetime mechanism).
- `DeferredDeletionQueue` defers native resource release when a buffer/staging object still has a last-use fence/timeline (destroy path must not CPU-wait). Flushed from `runGarbageCollection`.
- `mapBuffer` / `mapStagingTexture` may still CPU-wait a fence (RT-only).
- Logic-thread `finalizeFrameTiming` must **not** call `runGarbageCollection` when the dedicated render thread is active; GC runs at the end of `executeRenderPhase` on the render thread.

## Backend guarantees (Phase 1)

- **D3D12:** `Device::m_Mutex` serializes `executeCommandLists`, `queueWaitForCommandList`, `waitForIdle`, and `runGarbageCollection` (including `permanentState` writeback in `CommandList::executed`).
- **Vulkan:** `Device::m_Mutex` serializes `executeCommandLists` + `executed` writeback and GC; `Queue::m_Mutex` serializes `submit`, wait/signal semaphore staging, and `retireCommandBuffers` (pool acquire in `getOrCreateCommandBuffer` remains free-threaded under the same queue lock).

## Out of scope (next)

- Free-threaded resource create across many threads
- Replacing intrusive `AddRef`/`Release` with non-COM lifetime
