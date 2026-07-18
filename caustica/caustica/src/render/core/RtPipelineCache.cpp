#include <render/core/RtPipelineCache.h>

#include <render/core/PathTracingShaderCompiler.h>

#include <core/log.h>

#include <algorithm>
#include <cstdlib>
#include <utility>

namespace caustica::render
{
namespace
{

bool envFlagEnabled(const char* name)
{
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}

bool envDisableIdleWarmup()
{
    return envFlagEnabled("CAUSTICA_DISABLE_RT_IDLE_WARMUP");
}

} // namespace

RtPipelineCache::RtPipelineCache(std::shared_ptr<PathTracingShaderCompiler> compiler)
    : m_compiler(std::move(compiler))
{
    m_bundles.resize(ptFeaturePresetCount());
    m_idleWarmupEnabled = !envDisableIdleWarmup();
    m_verboseLogs = envFlagEnabled("CAUSTICA_RT_PIPELINE_CACHE_VERBOSE");
}

void RtPipelineCache::clear()
{
    if (!m_compiler)
        return;

    for (Bundle& bundle : m_bundles)
    {
        m_compiler->releaseVariant(bundle.reference);
        m_compiler->releaseVariant(bundle.buildStablePlanes);
        m_compiler->releaseVariant(bundle.fillStablePlanes);
        bundle = {};
    }
    m_warmupQueue.clear();
    m_activePreset = PtFeaturePresetId::Default;
    m_warmingPreset = PtFeaturePresetId::Count;
    m_warmupTotal = 0;
    m_stats = {};
}

void RtPipelineCache::recordLookup(PtFeaturePresetId id, bool hit, std::string_view reason)
{
    if (hit)
    {
        ++m_stats.hits;
        // Avoid per-frame spam: log HIT only on preset switch, or when verbose.
        const bool switched = (id != m_activePreset);
        if (m_verboseLogs || switched)
        {
            caustica::info(
                "RtPipelineCache: HIT preset='%s' reason=%.*s (hits=%llu misses=%llu warms=%llu)",
                ptFeaturePresetName(id).data(),
                static_cast<int>(reason.size()),
                reason.data(),
                static_cast<unsigned long long>(m_stats.hits),
                static_cast<unsigned long long>(m_stats.misses),
                static_cast<unsigned long long>(m_stats.idleWarms));
        }
    }
    else
    {
        ++m_stats.misses;
        caustica::warning(
            "RtPipelineCache: MISS preset='%s' reason=%.*s — CreateStateObject (hits=%llu misses=%llu warms=%llu)",
            ptFeaturePresetName(id).data(),
            static_cast<int>(reason.size()),
            reason.data(),
            static_cast<unsigned long long>(m_stats.hits),
            static_cast<unsigned long long>(m_stats.misses),
            static_cast<unsigned long long>(m_stats.idleWarms));
        logStatsSummary(true);
    }
}

void RtPipelineCache::logStatsSummary(bool force)
{
    const uint64_t total = m_stats.hits + m_stats.misses + m_stats.idleWarms;
    if (!force && total == m_stats.lastLoggedTotal)
        return;
    if (!force && (total - m_stats.lastLoggedTotal) < 8 && m_stats.misses == 0)
        return;

    m_stats.lastLoggedTotal = total;
    const double hitRate = (m_stats.hits + m_stats.misses) > 0
        ? (100.0 * static_cast<double>(m_stats.hits)
           / static_cast<double>(m_stats.hits + m_stats.misses))
        : 100.0;
    caustica::info(
        "RtPipelineCache: stats hits=%llu misses=%llu idleWarms=%llu binds=%llu hitRate=%.1f%%",
        static_cast<unsigned long long>(m_stats.hits),
        static_cast<unsigned long long>(m_stats.misses),
        static_cast<unsigned long long>(m_stats.idleWarms),
        static_cast<unsigned long long>(m_stats.binds),
        hitRate);
}

bool RtPipelineCache::bundleReady(const Bundle& bundle)
{
    return bundle.reference
        && bundle.buildStablePlanes
        && bundle.fillStablePlanes
        && bundle.reference->hasPipeline()
        && bundle.buildStablePlanes->hasPipeline()
        && bundle.fillStablePlanes->hasPipeline();
}

void RtPipelineCache::ensurePresetVariants(PtFeaturePresetId id)
{
    const uint32_t index = static_cast<uint32_t>(id);
    if (!m_compiler || index >= m_bundles.size())
        return;

    Bundle& bundle = m_bundles[index];
    if (bundle.reference)
        return;

    std::vector<caustica::ShaderMacro> globalMacros;
    fillPtFeaturePresetMacros(id, globalMacros);

    using SM = caustica::ShaderMacro;
    bundle.reference = m_compiler->createVariant(
        "PathTracerSample.hlsl",
        { SM("PATH_TRACER_MODE", "PATH_TRACER_MODE_REFERENCE") },
        "REF",
        false,
        globalMacros);
    bundle.buildStablePlanes = m_compiler->createVariant(
        "PathTracerSample.hlsl",
        { SM("PATH_TRACER_MODE", "PATH_TRACER_MODE_BUILD_STABLE_PLANES") },
        "BUILD",
        false,
        globalMacros);
    bundle.fillStablePlanes = m_compiler->createVariant(
        "PathTracerSample.hlsl",
        { SM("PATH_TRACER_MODE", "PATH_TRACER_MODE_FILL_STABLE_PLANES") },
        "FILL",
        false,
        globalMacros);

    caustica::info(
        "RtPipelineCache: created variants for preset '%s'",
        ptFeaturePresetName(id).data());
}

void RtPipelineCache::ensurePresets(std::span<const PtFeaturePresetId> ids)
{
    for (PtFeaturePresetId id : ids)
        ensurePresetVariants(id);
}

bool RtPipelineCache::bind(
    PtFeaturePresetId id,
    std::shared_ptr<PTPipelineVariant>& outReference,
    std::shared_ptr<PTPipelineVariant>& outBuild,
    std::shared_ptr<PTPipelineVariant>& outFill)
{
    Bundle* bundle = findBundle(id);
    if (!bundle || !bundle->reference || !bundle->buildStablePlanes || !bundle->fillStablePlanes)
        return false;

    outReference = bundle->reference;
    outBuild = bundle->buildStablePlanes;
    outFill = bundle->fillStablePlanes;
    m_activePreset = id;
    ++m_stats.binds;
    return true;
}

bool RtPipelineCache::hasBundle(PtFeaturePresetId id) const
{
    const Bundle* bundle = findBundle(id);
    return bundle != nullptr && bundle->reference != nullptr;
}

bool RtPipelineCache::isReady(PtFeaturePresetId id) const
{
    const Bundle* bundle = findBundle(id);
    return bundle != nullptr && bundleReady(*bundle);
}

RtPipelineCache::Bundle* RtPipelineCache::findBundle(PtFeaturePresetId id)
{
    const uint32_t index = static_cast<uint32_t>(id);
    if (index >= m_bundles.size())
        return nullptr;
    return &m_bundles[index];
}

const RtPipelineCache::Bundle* RtPipelineCache::findBundle(PtFeaturePresetId id) const
{
    const uint32_t index = static_cast<uint32_t>(id);
    if (index >= m_bundles.size())
        return nullptr;
    return &m_bundles[index];
}

void RtPipelineCache::enqueueWarmup(PtFeaturePresetId id)
{
    if (isReady(id))
        return;
    if (std::find(m_warmupQueue.begin(), m_warmupQueue.end(), id) != m_warmupQueue.end())
        return;
    m_warmupQueue.push_back(id);
}

void RtPipelineCache::scheduleBackgroundWarmup(PtFeaturePresetId exclude)
{
    if (!m_idleWarmupEnabled)
        return;

    m_warmupQueue.clear();
    m_warmingPreset = PtFeaturePresetId::Count;
    m_warmupTotal = ptFeaturePresetCount();

    for (uint32_t i = 0; i < ptFeaturePresetCount(); ++i)
    {
        const auto id = static_cast<PtFeaturePresetId>(i);
        if (id == exclude || isReady(id))
            continue;
        m_warmupQueue.push_back(id);
    }

    if (!m_warmupQueue.empty())
    {
        caustica::info(
            "RtPipelineCache: scheduled idle warmup for %zu presets (active '%s' bound first)",
            m_warmupQueue.size(),
            ptFeaturePresetName(exclude).data());
    }
}

void RtPipelineCache::prioritizeWarmup(PtFeaturePresetId id)
{
    if (isReady(id))
        return;

    ensurePresetVariants(id);
    m_warmupQueue.erase(
        std::remove(m_warmupQueue.begin(), m_warmupQueue.end(), id),
        m_warmupQueue.end());
    m_warmupQueue.push_front(id);
}

bool RtPipelineCache::tickIdleWarmup(uint32_t maxPresets)
{
    if (!m_idleWarmupEnabled || !m_compiler || maxPresets == 0)
        return false;

    bool didWork = false;
    uint32_t built = 0;
    while (built < maxPresets && !m_warmupQueue.empty())
    {
        const PtFeaturePresetId id = m_warmupQueue.front();
        m_warmupQueue.pop_front();

        if (isReady(id))
            continue;

        m_warmingPreset = id;
        ensurePresetVariants(id);

        std::vector<std::shared_ptr<PTPipelineVariant>> variants;
        if (Bundle* bundle = findBundle(id))
        {
            if (bundle->reference && !bundle->reference->hasPipeline())
                variants.push_back(bundle->reference);
            if (bundle->buildStablePlanes && !bundle->buildStablePlanes->hasPipeline())
                variants.push_back(bundle->buildStablePlanes);
            if (bundle->fillStablePlanes && !bundle->fillStablePlanes->hasPipeline())
                variants.push_back(bundle->fillStablePlanes);
        }

        if (!variants.empty())
        {
            // Silent: no modal ProgressBar on the interactive path.
            m_compiler->buildPipelines(variants, /*showProgress=*/false);
            ++m_stats.idleWarms;
            const RtPipelineWarmupStatus status = warmupStatus();
            caustica::info(
                "RtPipelineCache: WARM preset='%s' (idle %u/%u) hits=%llu misses=%llu warms=%llu",
                ptFeaturePresetName(id).data(),
                status.completed,
                status.total,
                static_cast<unsigned long long>(m_stats.hits),
                static_cast<unsigned long long>(m_stats.misses),
                static_cast<unsigned long long>(m_stats.idleWarms));
            didWork = true;
        }

        ++built;
    }

    if (m_warmupQueue.empty())
    {
        if (m_warmingPreset != PtFeaturePresetId::Count || didWork)
            logStatsSummary(true);
        m_warmingPreset = PtFeaturePresetId::Count;
    }

    return didWork;
}

RtPipelineWarmupStatus RtPipelineCache::warmupStatus() const
{
    RtPipelineWarmupStatus status;
    status.total = ptFeaturePresetCount();
    uint32_t ready = 0;
    for (uint32_t i = 0; i < ptFeaturePresetCount(); ++i)
    {
        if (isReady(static_cast<PtFeaturePresetId>(i)))
            ++ready;
    }
    status.completed = ready;
    status.active = m_idleWarmupEnabled
        && ready < status.total
        && (!m_warmupQueue.empty() || m_warmingPreset != PtFeaturePresetId::Count);
    if (m_warmingPreset != PtFeaturePresetId::Count)
        status.currentPreset = ptFeaturePresetName(m_warmingPreset);
    else if (!m_warmupQueue.empty())
        status.currentPreset = ptFeaturePresetName(m_warmupQueue.front());
    return status;
}

} // namespace caustica::render
