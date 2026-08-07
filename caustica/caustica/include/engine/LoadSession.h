#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace caustica
{

namespace scene
{
class SceneRenderData;
}

// ADR 0001 P3 — sole "are we loading / streaming?" session for Open Scene.
// Progress UI and switch gates should read this (isBusy), not overlapping bools.
enum class LoadSessionPhase : uint8_t
{
    Idle = 0,
    Teardown,      // exclusive GPU unload; sceneGpuSuspended held until RT completes
    Importing,     // CPU worker (SceneLoader)
    GpuStreaming,  // budgeted texture/mesh/bind on Render domain
    Finalizing,    // logic-thread post-bind; requests StructureGpu AccelOnly
    FirstPresent,  // wait until StructureGpu committed serve is ready
    Ready,         // terminal → cleared to Idle same tick
};

// Internal GpuStreaming substeps (not part of the public phase enum).
enum class LoadStreamStep : uint8_t
{
    Textures = 0,
    World,
    Meshes,
    Finalize,
};

struct LoadSession
{
    LoadSessionPhase phase = LoadSessionPhase::Idle;
    LoadStreamStep streamStep = LoadStreamStep::Textures;

    const scene::SceneRenderData* renderData = nullptr;
    size_t texturesTotal = 0;
    size_t meshBegin = 0;
    size_t meshTotal = 0;

    // One non-blocking Render step at a time (Logic never EnqueueRenderCommandAndWait).
    bool stepInFlight = false;
    bool textureDrainPending = false; // true while flushTextures(0) is the in-flight/next step
    std::atomic<uint8_t> stepStatus{0}; // 0=pending, 1=ok, 2=fail
    std::atomic<size_t> stepMeshNext{0};
    std::atomic<size_t> stepTexturesRemaining{0};

    // Teardown: RT sets true when Streamline/AS release + GC finished.
    std::atomic<bool> teardownGpuDone{false};

    // After Teardown, tickLoadSession calls beginLoadingScene (avoids racing GPU unload).
    bool deferredImportPending = false;

    // OMM / opacity builds and similar work outside the Open Scene phase machine.
    // Mirrored from RT diagnostics each Logic tick; do not add a second load bool.
    std::atomic<bool> secondaryStreaming{false};

    // Logic-tick budgets; RT upload pressure is gated by StreamingUploadBudget (ADR 0001 R1).
    static constexpr float kTextureBudgetMs = 8.f;
    static constexpr size_t kMeshesPerStep = 1;

    [[nodiscard]] bool isActive() const noexcept
    {
        return phase != LoadSessionPhase::Idle;
    }

    [[nodiscard]] bool isBusy() const noexcept
    {
        return isActive() || secondaryStreaming.load(std::memory_order_acquire);
    }

    void reset() noexcept
    {
        phase = LoadSessionPhase::Idle;
        streamStep = LoadStreamStep::Textures;
        renderData = nullptr;
        texturesTotal = 0;
        meshBegin = 0;
        meshTotal = 0;
        stepInFlight = false;
        textureDrainPending = false;
        stepStatus.store(0, std::memory_order_relaxed);
        stepMeshNext.store(0, std::memory_order_relaxed);
        stepTexturesRemaining.store(0, std::memory_order_relaxed);
        teardownGpuDone.store(false, std::memory_order_relaxed);
        deferredImportPending = false;
        secondaryStreaming.store(false, std::memory_order_relaxed);
    }

    // Rough 0..100 for ProgressBar while the session is active.
    [[nodiscard]] int progressPercent() const noexcept
    {
        switch (phase)
        {
        case LoadSessionPhase::Idle:
            return 0;
        case LoadSessionPhase::Teardown:
            return 15;
        case LoadSessionPhase::Importing:
            return 30;
        case LoadSessionPhase::GpuStreaming:
        {
            switch (streamStep)
            {
            case LoadStreamStep::Textures:
                if (texturesTotal > 0)
                {
                    const size_t remaining = stepTexturesRemaining.load(std::memory_order_relaxed);
                    const size_t done = texturesTotal > remaining ? texturesTotal - remaining : 0;
                    return 55 + int(float(done) / float(texturesTotal) * 15.f);
                }
                return 55;
            case LoadStreamStep::World:
                return 72;
            case LoadStreamStep::Meshes:
                if (meshTotal > 0)
                    return 72 + int(float(meshBegin) / float(meshTotal) * 18.f);
                return 72;
            case LoadStreamStep::Finalize:
                return 90;
            }
            return 55;
        }
        case LoadSessionPhase::Finalizing:
            return 93;
        case LoadSessionPhase::FirstPresent:
            return 97;
        case LoadSessionPhase::Ready:
            return 100;
        }
        return 0;
    }
};

} // namespace caustica
