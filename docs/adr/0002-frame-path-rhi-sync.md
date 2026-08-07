# ADR 0002: Frame-path RHI sync (fence / timeline / retire)

| Field | Value |
| --- | --- |
| Status | **Accepted** (implementation pending) |
| Date | 2026-08-07 |
| Deciders | Caustica engine |
| Relates | [ADR 0001](0001-task-runtime-multithreading.md), [architecture-rhi-threading.md](../architecture-rhi-threading.md), [architecture-render-proxy.md](../architecture-render-proxy.md) |

## Context

ADR 0001 landed TaskRuntime, LoadSession, StructureGpu committed-serve, R1
`StreamingUploadBudget`, and R2 binder work. Remaining stalls are **not** load
glue — they are frame-path / feature-toggle sync points that still call
device-wide `waitForIdle`, plus a few Logic-thread `EnqueueRenderCommandAndWait`
/ `waitForRenderThreadIdle` escapes that can make the app feel single-threaded
even when Extract and structure builds are already async.

Inventory (RT unless noted):

| Hotspot | Typical call site | Why it idles today |
| --- | --- | --- |
| `needNewPasses` / createRenderPasses | `WorldRendererFrame.cpp` | Destroy/recreate passes while GPU may still reference old PSO/resources |
| DLSS / Streamline init & teardown | `DLSS-*.cpp`, `StreamlineIntegration` | Feature / NGX state swap |
| OMM build | `OpacityMicromapBuilder`, `OmmBuildQueue` | Builder ↔ accel consume ordering |
| ToneMapping auto-exposure | `ToneMappingPasses` (`serialOnPrimary`) | Mid-pass close → execute → idle → open for readback |
| Debug / pick readback | `framePassFinalize` | Map CPU feedback buffers |
| Logic `AndWait` / RT idle | resize, splat load, precache presets, teardown | Host wants a completed result before continuing |

Cold start, device destroy, and exclusive LoadSession teardown may keep full
idles — those are infrequent and already gated.

## Decision

### Principles

1. **RT-only for GPU sync.** `Device::waitForIdle`, queue waits, GC, submit, and
   present stay on the Render domain. Never idle the device from Logic.
2. **Prefer fence / timeline / retire over device-wide idle** on the frame and
   feature-toggle happy paths. Pattern already proven by R1
   `StreamingUploadBudget` (EventQuery + in-flight caps + `retire` / `waitAll`).
3. **Logic never blocks a frame on RT work.** Prefer `EnqueueRenderCommand`
   (fire-and-forget) + next-frame / polled readiness. Keep
   `EnqueueRenderCommandAndWait` only for rare host ops that are not on the
   interactive frame path (and document each remaining call).
4. **Cold start / shutdown / device-lost may still `waitForIdle`.** Annotate
   `// THREADING: sync-point, RT-only` (or `shutdown`). Do not “fix” these in
   drive-by PRs that touch LoadSession.

### Target sync toolkit (reuse before inventing)

| Tool | Use when |
| --- | --- |
| `StreamingUploadBudget`-style EventQuery + retire | Batched GPU work that must finish before resource destroy/reuse |
| `queueWaitForCommandList` / timeline wait | Order queue A behind submit B without draining the whole device |
| Double-buffer / generation + deferred destroy | Passes, Streamline state, OMM blobs — serve N, build N+1, retire N after fence |
| Staging + async readback fence | AE histogram, debug feedback, picking (map only after query complete) |
| Full `waitForIdle` | Device create/destroy, fatal reset, LoadSession exclusive teardown |

Do **not** introduce a second public enqueue API. Do **not** widen free-threaded
RHI create here (that remains ADR 0001 R3 / a later create ADR).

### Phased work

| Phase | Scope | Exit criteria |
| --- | --- | --- |
| **S0** | Inventory + annotations | Every frame-path idle tagged; Logic `AndWait` / `waitForRenderThreadIdle` listed with owner |
| **S1** | Readback paths (AE, debug/pick) | No mid-frame device-wide idle for AE/feedback; fence then map (may lag 1 frame) |
| **S2** | `needNewPasses` | Recreate uses generation + retire-after-fence (or scoped queue wait), not pre/post full idle on the common toggle path |
| **S3** | OMM | Builder completion via query/fence; accel consume waits that fence only |
| **S4** | DLSS / Streamline | Init/teardown double-buffer or fence-gated swap; document unavoidable NGX sync if any |
| **S5** | Logic escapes | Resize / splat load / precache: enqueue + poll or deferred callback; frame loop has zero `AndWait` |

Phases may land as separate PRs. Prefer S1 → S2 (highest interactive cost) before
vendor-heavy S4.

### Allowed remaining sync-points

After this ADR, these may still idle **on RT**:

- App / `GpuRenderSubsystem` shutdown and device destroy
- LoadSession exclusive scene teardown window
- Device-removed / fatal recovery
- One-shot editor or tool ops explicitly documented as blocking (not the editor
  frame loop)

Everything else on the render frame should use fence/retire or be `serialOnPrimary`
with a **queue** wait, not a device-wide drain.

## Non-goals

- Free-threaded `createTexture` / `createBuffer` (ADR 0001 R3)
- Rewriting NVRHI / replacing COM refcounts
- Removing `serialOnPrimary` (ordering can stay; the idle inside it should not)
- Making every readback zero-latency (1-frame lag is acceptable for AE/debug)
- Big-bang single PR across DLSS + OMM + needNewPasses

## Consequences

### Positive

- Interactive frames stop paying full-GPU drains on settings / denoiser / AE toggles
- Logic UI / input stay responsive (no frame-path `AndWait`)
- One sync vocabulary shared with R1 uploads
- Clear boundary vs LoadSession work (no more “delete waitForIdle” drive-bys)

### Cost / risk

| Risk | Mitigation |
| --- | --- |
| Use-after-free when retiring passes early | Generation + fence before destroy; keep committed serve pattern from StructureGpu |
| Streamline / NGX still requires idle | Isolate behind RT fence helper; document; do not block Logic |
| 1-frame AE/debug lag | Accept; document; optional sync path behind debug flag |
| `--syncRender` semantics drift | Same RT-domain rules; Logic enters Render domain only for GPU pump |

## S0 inventory (2026-08-07)

Call sites tagged `// THREADING: … — ADR 0002 S#`. Backend `Device::waitForIdle`
implementations are out of scope (API definition only).

### Frame-path RT `waitForIdle` (fix in S1–S4)

| ID | Site | Phase | Notes |
| --- | --- | --- | --- |
| F1 | `WorldRendererFrame` AE N/A — `ToneMappingPasses` first-frame AE | **S1** | mid-pass close/execute/idle/open |
| F2 | `WorldRendererFrame::framePassFinalize` debug/pick map | **S1** | every pick / ContinuousDebugFeedback frame |
| F3 | `EnvMapProcessor` load + debug bake | **S1**-adj | readback helpers |
| F4 | `DenoisePass` OIDN reference readback | **S1**-adj | rare reference path |
| F5 | `TextureLoaderGpu` CPU readback helper | **S1**-adj | not upload happy path (R1) |
| F6 | `LightSamplingCache` LLB_ENABLE_VALIDATION | **S1**-adj | debug only |
| F7 | `WorldRendererFrame` needNewPasses pre/post/stage/final | **S2** | settings / pipeline recreate |
| F8 | `WorldRendererFrame` / `WorldRenderer` RT recreate + resize | **S2** | render-target resize |
| F9 | `LightSamplingCache` / `GPUSort` scratch recreate | **S2**-adj | resolution / count change |
| F10 | `EditorViewport` retire/resize | **S2**-adj | editor-only |
| F11 | `OpacityMicromapBuilder::destroyOpacityMicromaps` | **S3** | destroy before clear |
| F12 | `OmmBuildQueue` baker init | **S3** | TODO already noted EventQuery |
| F13 | `DLSS-DX12/VK/DX11` NGX ReleaseFeature | **S4** | feature recreate |
| F14 | `StreamlineIntegration` freeResources / DLSS-G fallback | **S4** | `wfi` paths; shutdown separate |

### Logic ↔ RT waits (fix in S5 unless Allowed)

| ID | Site | Kind | Phase | Notes |
| --- | --- | --- | --- | --- |
| L1 | `App::updateWindowSize` | `dispatchAndWait` | **S5** | only when size changed |
| L2 | `App::runFrame` skip-render gap | `waitForRenderThreadIdle` | **S5** | resume Extract after no-render |
| L3 | `SceneGaussianSplatLogic` load/remove/onSceneLoaded | `waitForRenderThreadIdle` | **S5** | Pass create still on Logic |
| L4 | `RenderSessionApi::precacheRtFeaturePresets` | `AndWait` | **S5** | tool/precache; not frame loop |
| L5 | `SceneEditor` undo/redo | RT idle + `AndWait` | **S5** | editor tool; not frame loop |

### Allowed (keep idle; annotated shutdown / teardown)

| ID | Site | Kind |
| --- | --- | --- |
| A1 | `App` main-loop exit / device-loss exit | RT idle + device idle |
| A2 | `GpuRenderSubsystem::shutdown` | RT idle + device idle ×2 |
| A3 | `SceneLifecycle::onSceneUnloading` | Logic RT idle + RT teardown idle (LoadSession) |
| A4 | `StreamlineIntegration` full SL shutdown | device idle before plugin unload |
| A5 | `StreamingUploadBudget` EventQuery create failure | R1 fallback only |
| A6 | `EditorLaunch` automated-run afterPresent drain | tool path |

### Annotation convention

```text
// THREADING: sync-point, RT-only — ADR 0002 S# (short reason).
// THREADING: Logic↔RT wait — ADR 0002 S5 (short reason).
// THREADING: sync-point, shutdown — ADR 0002 allowed.
```

## Checklist (implementation)

1. [x] S0: annotate + document remaining Logic `AndWait` call sites
2. [x] S1: ToneMapping AE + debug/pick readback → EventQuery / fence then map
   - AE: lagged ring; removed mid-pass close/execute/`waitForIdle`; `serialOnPrimary=false`
   - Feedback: graphics `EventQuery` after `endFrame`; ContinuousDebug 1-frame lag; pick waits queue fence
   - F3–F6 (env/OIDN/texture readback helpers) remain S1-adjacent follow-ups
3. [x] S2: `needNewPasses` / RT recreate → `waitGraphicsQueueFence` / `syncGraphicsQueueFence`
   - pre/post createRenderPasses, init stage flush, final submit, ensureRenderTargets, onBackBufferResizing
   - F9 light-cache / GPUSort recreate remain S2-adjacent
4. [x] S3: OMM baker init + `destroyOpacityMicromaps` → `syncGraphicsQueueFence`
5. [x] S4: DLSS `waitBeforeReleaseFeature` + Streamline `waitGraphicsBeforeFreeResources`
   - Full SL plugin shutdown may still device-idle (allowed)
6. [x] S5 (partial): frame-path resize → non-blocking `EnqueueRenderCommand`
   - Remaining tool/rare Logic waits (documented): skip-render gap RT idle, splat Pass
     create/remove drain, precache `AndWait`, editor undo — need RT-owned Pass factory
     / async host APIs before removal
7. [x] Update [architecture-rhi-threading.md](../architecture-rhi-threading.md) when each phase lands

## References

- R1 pattern: `include/render/core/StreamingUploadBudget.h`
- Frame idles: `src/render/WorldRendererFrame.cpp` (`needNewPasses`, finalize readback)
- Contract today: [architecture-rhi-threading.md](../architecture-rhi-threading.md)
- Prior deferral: ADR 0001 “Remaining frame-path sync”
