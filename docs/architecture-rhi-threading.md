# Architecture: RHI Threading Contract (Phase 1)

Caustica already splits **logic** and **render** via Extract + `RenderThread` (see [architecture-render-proxy.md](architecture-render-proxy.md)). This document defines the RHI rules that make that split (and later parallel command-list recording) safe.

## Thread roles

| Role | Responsibility |
| --- | --- |
| Logic thread | ECS, Extract, snapshot publish. No `executeCommandLists` / `present` / `runGarbageCollection`. |
| Render thread (`RenderThread`) | Sole owner of queue submit, present, and RHI GC. Records GPU work (Phase 1: still mostly one command list). |
| RHI workers (Phase 2+) | May record **deferred** command lists only. Must not submit, present, or GC. |

`caustica::isRenderThread()` is true on the dedicated render thread. When the dedicated thread is disabled, the logic thread temporarily acts as the render thread for GPU work.

## Phase 1 API rules

| Operation | Allowed on |
| --- | --- |
| `createCommandList` / open–record–close | Render thread (workers in Phase 2) |
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
- Mid-frame `close → execute → waitForIdle → open` on a shared list is a **sync point**. Keep it rare, RT-only, and annotated `// THREADING: sync-point, RT-only`.

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

## Out of scope for Phase 1

- Parallel pass recording / `GraphBuilder` waves
- Free-threaded resource create across many threads
- Renaming or replacing intrusive `AddRef`/`Release` (public types already omit the `I` prefix)
