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

// ADR 0001 P3 — sole "are we loading?" session for Open Scene.
// Progress UI and switch gates should read this, not overlapping bools.
enum class LoadSessionPhase : uint8_t
{
    Idle = 0,
    Importing,     // CPU worker (SceneLoader)
    GpuStreaming,  // budgeted texture/mesh/bind on Render domain
    FirstPresent,  // present already restored; structure enqueue / gate
    Finalizing,    // logic-thread post-bind
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

    // Logic-tick budgets; RT upload pressure is gated by StreamingUploadBudget (ADR 0001 R1).
    static constexpr float kTextureBudgetMs = 8.f;
    static constexpr size_t kMeshesPerStep = 1;

    [[nodiscard]] bool isActive() const noexcept
    {
        return phase != LoadSessionPhase::Idle;
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
    }

    // Rough 0..100 for ProgressBar while the session is active.
    [[nodiscard]] int progressPercent() const noexcept
    {
        switch (phase)
        {
        case LoadSessionPhase::Idle:
            return 0;
        case LoadSessionPhase::Importing:
            return 20;
        case LoadSessionPhase::GpuStreaming:
        {
            int base = 55;
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
                return 93;
            }
            return base;
        }
        case LoadSessionPhase::FirstPresent:
            return 96;
        case LoadSessionPhase::Finalizing:
            return 98;
        case LoadSessionPhase::Ready:
            return 100;
        }
        return 0;
    }
};

} // namespace caustica
