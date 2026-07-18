#pragma once

#include <render/core/PtPipelineFeaturePresets.h>

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

class PathTracingShaderCompiler;
class PTPipelineVariant;

namespace caustica::render
{

// Diagnostics / HUD: how many cooked-preset RT PSO bundles are device-ready.
struct RtPipelineWarmupStatus
{
    bool active = false; // true while an explicit precache/build is in progress
    uint32_t completed = 0;
    uint32_t total = 0;
    std::string_view currentPreset = {};
};

struct RtPipelineCacheStats
{
    uint64_t hits = 0;    // ensure found a ready PSO bundle
    uint64_t misses = 0;  // ensure had to CreateStateObject
    uint64_t binds = 0;
    uint64_t precached = 0;
    uint64_t lastLoggedTotal = 0;
};

// UE-style RT pipeline cache (two layers):
//
//  1) Offline cook (cook_shaders.py): DXC shader *libraries* for the closed
//     feature-preset matrix → ShaderDynamic/Bin (+ optional .pack).
//  2) Runtime: CreateStateObject is owned *only* by this cache, and only when a
//     preset is actually needed (active bind) or explicitly precached at
//     load/cook time. Interactive frames never background-warm other presets.
//
// Hot path: resolve preset → ensureReady(active) → bind. Switching to a cold
// preset pays one CreateStateObject; that is the intended UE tradeoff.
class RtPipelineCache
{
public:
    struct Bundle
    {
        std::shared_ptr<PTPipelineVariant> reference;
        std::shared_ptr<PTPipelineVariant> buildStablePlanes;
        std::shared_ptr<PTPipelineVariant> fillStablePlanes;
    };

    explicit RtPipelineCache(std::shared_ptr<PathTracingShaderCompiler> compiler);

    void clear();

    // Create variant objects for a preset (loads cooked libraries later; no CreateStateObject).
    void ensurePresetVariants(PtFeaturePresetId id);
    void ensurePresets(std::span<const PtFeaturePresetId> ids);

    // Sole CreateStateObject path for a preset's REF/BUILD/FILL bundle.
    // No-op if already ready. Returns true when the bundle is ready afterward.
    bool buildPreset(PtFeaturePresetId id, bool showProgress = false);

    // buildPreset + isReady. Used by the interactive active-preset path.
    bool ensureReady(PtFeaturePresetId id, bool showProgress = false);

    // Explicit load-screen / offline-cook precache of the full matrix.
    // Must run on the render thread after the compiler has a live hit-group set
    // (typically after the first successful PT update). Returns ready count.
    uint32_t precacheAll(bool showProgress = true);

    bool bind(
        PtFeaturePresetId id,
        std::shared_ptr<PTPipelineVariant>& outReference,
        std::shared_ptr<PTPipelineVariant>& outBuild,
        std::shared_ptr<PTPipelineVariant>& outFill);

    [[nodiscard]] PtFeaturePresetId activePreset() const { return m_activePreset; }
    [[nodiscard]] bool hasBundle(PtFeaturePresetId id) const;
    [[nodiscard]] bool isReady(PtFeaturePresetId id) const;
    [[nodiscard]] Bundle* findBundle(PtFeaturePresetId id);
    [[nodiscard]] const Bundle* findBundle(PtFeaturePresetId id) const;

    [[nodiscard]] RtPipelineWarmupStatus status() const;
    [[nodiscard]] const RtPipelineCacheStats& stats() const { return m_stats; }

    void recordLookup(PtFeaturePresetId id, bool hit, std::string_view reason);
    void logStatsSummary(bool force = false);

private:
    [[nodiscard]] static bool bundleReady(const Bundle& bundle);

    std::shared_ptr<PathTracingShaderCompiler> m_compiler;
    std::vector<Bundle> m_bundles;
    PtFeaturePresetId m_activePreset = PtFeaturePresetId::Default;
    PtFeaturePresetId m_buildingPreset = PtFeaturePresetId::Count;
    bool m_verboseLogs = false;
    RtPipelineCacheStats m_stats{};
};

} // namespace caustica::render
