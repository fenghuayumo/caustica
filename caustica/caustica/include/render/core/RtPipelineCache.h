#pragma once

#include <render/core/PtPipelineFeaturePresets.h>

#include <cstdint>
#include <deque>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class PathTracingShaderCompiler;
class PTPipelineVariant;

namespace caustica::render
{

struct RtPipelineWarmupStatus
{
    bool active = false;
    uint32_t completed = 0;
    uint32_t total = 0;
    std::string_view currentPreset = {};
};

// Interactive bind lookups + idle warmup counters (lifetime of the cache instance).
struct RtPipelineCacheStats
{
    uint64_t hits = 0;          // ensure/bind found a ready PSO bundle
    uint64_t misses = 0;        // interactive path had to CreateStateObject
    uint64_t idleWarms = 0;     // presets built by tickIdleWarmup
    uint64_t binds = 0;         // successful bind() calls
    uint64_t lastLoggedTotal = 0;
};

// Prebuilt RT pipeline bundles keyed by cooked feature presets.
// Startup: bind active preset immediately. Remaining presets warm on the render
// thread with a per-frame budget (CreateStateObject is not freely multithreaded).
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

    // Create variant objects for presets (no CreateStateObject yet).
    void ensurePresetVariants(PtFeaturePresetId id);
    void ensurePresets(std::span<const PtFeaturePresetId> ids);

    // Point WorldRenderer active pipeline slots at a bundle (variants must exist).
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

    // Queue every preset except `exclude` for idle-time CreateStateObject.
    void scheduleBackgroundWarmup(PtFeaturePresetId exclude);
    // Ensure + build at most `maxPresets` pending entries. Call on render thread.
    // Returns true if any work was done.
    bool tickIdleWarmup(uint32_t maxPresets = 1);
    void prioritizeWarmup(PtFeaturePresetId id);

    [[nodiscard]] RtPipelineWarmupStatus warmupStatus() const;
    [[nodiscard]] const RtPipelineCacheStats& stats() const { return m_stats; }
    [[nodiscard]] bool isIdleWarmupEnabled() const { return m_idleWarmupEnabled; }
    void setIdleWarmupEnabled(bool enabled) { m_idleWarmupEnabled = enabled; }

    // Record an interactive lookup (ready PSO = hit, CreateStateObject = miss).
    void recordLookup(PtFeaturePresetId id, bool hit, std::string_view reason);
    void logStatsSummary(bool force = false);

private:
    [[nodiscard]] static bool bundleReady(const Bundle& bundle);
    void enqueueWarmup(PtFeaturePresetId id);

    std::shared_ptr<PathTracingShaderCompiler> m_compiler;
    std::vector<Bundle> m_bundles;
    std::deque<PtFeaturePresetId> m_warmupQueue;
    PtFeaturePresetId m_activePreset = PtFeaturePresetId::Default;
    PtFeaturePresetId m_warmingPreset = PtFeaturePresetId::Count;
    uint32_t m_warmupTotal = 0;
    bool m_idleWarmupEnabled = true;
    bool m_verboseLogs = false;
    RtPipelineCacheStats m_stats{};
};

} // namespace caustica::render
