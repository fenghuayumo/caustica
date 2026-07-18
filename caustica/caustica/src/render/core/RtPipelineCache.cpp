#include <render/core/RtPipelineCache.h>

#include <render/core/PathTracingShaderCompiler.h>

#include <core/log.h>

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

} // namespace

RtPipelineCache::RtPipelineCache(std::shared_ptr<PathTracingShaderCompiler> compiler)
    : m_compiler(std::move(compiler))
{
    m_bundles.resize(ptFeaturePresetCount());
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
    m_activePreset = PtFeaturePresetId::Default;
    m_buildingPreset = PtFeaturePresetId::Count;
    m_stats = {};
}

void RtPipelineCache::recordLookup(PtFeaturePresetId id, bool hit, std::string_view reason)
{
    if (hit)
    {
        ++m_stats.hits;
        const bool switched = (id != m_activePreset);
        if (m_verboseLogs || switched)
        {
            caustica::info(
                "RtPipelineCache: HIT preset='%s' reason=%.*s (hits=%llu misses=%llu)",
                ptFeaturePresetName(id).data(),
                static_cast<int>(reason.size()),
                reason.data(),
                static_cast<unsigned long long>(m_stats.hits),
                static_cast<unsigned long long>(m_stats.misses));
        }
        return;
    }

    ++m_stats.misses;
    caustica::info(
        "RtPipelineCache: MISS preset='%s' reason=%.*s — CreateStateObject "
        "(hits=%llu misses=%llu)",
        ptFeaturePresetName(id).data(),
        static_cast<int>(reason.size()),
        reason.data(),
        static_cast<unsigned long long>(m_stats.hits),
        static_cast<unsigned long long>(m_stats.misses));
}

void RtPipelineCache::logStatsSummary(bool force)
{
    const uint64_t total = m_stats.hits + m_stats.misses + m_stats.precached;
    if (!force && total == m_stats.lastLoggedTotal)
        return;

    m_stats.lastLoggedTotal = total;
    const double hitRate = (m_stats.hits + m_stats.misses) > 0
        ? (100.0 * static_cast<double>(m_stats.hits)
           / static_cast<double>(m_stats.hits + m_stats.misses))
        : 100.0;
    const RtPipelineWarmupStatus s = status();
    caustica::info(
        "RtPipelineCache: stats hits=%llu misses=%llu precached=%llu binds=%llu hitRate=%.1f%% ready=%u/%u",
        static_cast<unsigned long long>(m_stats.hits),
        static_cast<unsigned long long>(m_stats.misses),
        static_cast<unsigned long long>(m_stats.precached),
        static_cast<unsigned long long>(m_stats.binds),
        hitRate,
        s.completed,
        s.total);
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
        "RtPipelineCache: created variants for preset '%s' (cooked library macros)",
        ptFeaturePresetName(id).data());
}

void RtPipelineCache::ensurePresets(std::span<const PtFeaturePresetId> ids)
{
    for (PtFeaturePresetId id : ids)
        ensurePresetVariants(id);
}

bool RtPipelineCache::buildPreset(PtFeaturePresetId id, bool showProgress)
{
    if (!m_compiler)
        return false;
    if (isReady(id))
        return true;

    // REF/BUILD/FILL need a live hit-group export set from compiler update().
    // Creating state objects earlier yields empty/invalid PSOs and hard crashes.
    if (!m_compiler->hasUniqueHitGroups())
    {
        caustica::warning(
            "RtPipelineCache: defer CreateStateObject for '%s' — hit groups not ready yet",
            ptFeaturePresetName(id).data());
        ensurePresetVariants(id);
        return false;
    }

    ensurePresetVariants(id);
    Bundle* bundle = findBundle(id);
    if (!bundle)
        return false;

    std::vector<std::shared_ptr<PTPipelineVariant>> variants;
    if (bundle->reference && !bundle->reference->hasPipeline())
        variants.push_back(bundle->reference);
    if (bundle->buildStablePlanes && !bundle->buildStablePlanes->hasPipeline())
        variants.push_back(bundle->buildStablePlanes);
    if (bundle->fillStablePlanes && !bundle->fillStablePlanes->hasPipeline())
        variants.push_back(bundle->fillStablePlanes);

    if (!variants.empty())
    {
        m_buildingPreset = id;
        m_compiler->buildPipelines(variants, showProgress);
        m_buildingPreset = PtFeaturePresetId::Count;
    }

    return isReady(id);
}

bool RtPipelineCache::ensureReady(PtFeaturePresetId id, bool showProgress)
{
    if (isReady(id))
    {
        recordLookup(id, /*hit=*/true, "ensure");
        return true;
    }

    recordLookup(id, /*hit=*/false, "ensure");
    return buildPreset(id, showProgress);
}

uint32_t RtPipelineCache::precacheAll(bool showProgress)
{
    if (!m_compiler)
        return 0;

    caustica::info(
        "RtPipelineCache: precacheAll begin (%u presets) — CreateStateObject owned by cache",
        ptFeaturePresetCount());

    uint32_t ready = 0;
    for (uint32_t i = 0; i < ptFeaturePresetCount(); ++i)
    {
        const auto id = static_cast<PtFeaturePresetId>(i);
        m_buildingPreset = id;
        if (buildPreset(id, showProgress))
        {
            ++ready;
            ++m_stats.precached;
        }
        else
        {
            caustica::error(
                "RtPipelineCache: precache failed for preset '%s'",
                ptFeaturePresetName(id).data());
        }
    }
    m_buildingPreset = PtFeaturePresetId::Count;

    logStatsSummary(true);
    caustica::info("RtPipelineCache: precacheAll end ready=%u/%u", ready, ptFeaturePresetCount());
    return ready;
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

RtPipelineWarmupStatus RtPipelineCache::status() const
{
    RtPipelineWarmupStatus s;
    s.total = ptFeaturePresetCount();
    for (uint32_t i = 0; i < ptFeaturePresetCount(); ++i)
    {
        if (isReady(static_cast<PtFeaturePresetId>(i)))
            ++s.completed;
    }
    if (m_buildingPreset != PtFeaturePresetId::Count)
    {
        s.active = true;
        s.currentPreset = ptFeaturePresetName(m_buildingPreset);
    }
    return s;
}

} // namespace caustica::render
