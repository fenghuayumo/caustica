#include "ui/RenderSettingsConsole.h"

#include "ui/EditorUIData.h"
#include "ui/EditorUIInternal.h"

#include <core/console/ConsoleInterpreter.h>
#include <core/console/ConsoleObjects.h>
#include <core/log.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace caustica::editor
{
namespace
{

enum class Invalidation : uint32_t
{
    None = 0,
    Accumulation = 1u << 0,
    RealtimeCaches = 1u << 1,
    AccelerationStructure = 1u << 2,
    Shaders = 1u << 3,
};

constexpr Invalidation operator|(Invalidation a, Invalidation b)
{
    return static_cast<Invalidation>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

bool HasFlag(Invalidation value, Invalidation flag)
{
    return (static_cast<uint32_t>(value) & static_cast<uint32_t>(flag)) != 0;
}

void ApplyInvalidation(EditorUIData& ui, Invalidation invalidation)
{
    if (HasFlag(invalidation, Invalidation::Accumulation))
        ui.render.settings.ResetAccumulation = true;
    if (HasFlag(invalidation, Invalidation::RealtimeCaches))
        ui.render.settings.ResetRealtimeCaches = true;
    if (HasFlag(invalidation, Invalidation::AccelerationStructure))
        ui.render.runtime.Invalidation.AccelerationStructRebuildRequested = true;
    if (HasFlag(invalidation, Invalidation::Shaders))
        ui.render.runtime.Invalidation.ShaderReloadRequested = true;
}

std::string Lower(std::string_view value)
{
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

struct Registry
{
    EditorUIData* active = nullptr;
    bool initialized = false;
};

Registry& GetRegistry()
{
    static Registry registry;
    return registry;
}

caustica::console::VariableFlags FlagsFor(const char* name)
{
    using Flags = caustica::console::VariableFlags;
    Flags flags = Flags::ARCHIVE;
    const std::string_view value(name);
    if (value.starts_with("r.Debug.") || value.starts_with("r.StablePlanes.")
        || value.starts_with("r.AS."))
    {
        flags = flags | Flags::DEVELOPER;
    }
    return flags;
}

template <typename Access>
void AddScalar(
    const char* name,
    const char* description,
    Access access,
    Invalidation invalidation,
    double minimum = std::numeric_limits<double>::quiet_NaN(),
    double maximum = std::numeric_limits<double>::quiet_NaN(),
    std::function<void(EditorUIData&)> afterChange = {})
{
    using Field = std::remove_cv_t<std::remove_reference_t<
        std::invoke_result_t<Access, EditorUIData&>>>;
    using CVar = std::conditional_t<std::is_same_v<Field, bool>, bool,
        std::conditional_t<std::is_floating_point_v<Field>, float, int>>;
    static_assert(std::is_arithmetic_v<Field>, "Render CVar fields must be scalar values");

    Registry& registry = GetRegistry();
    if (!registry.active)
        return;

    caustica::console::BoundVariableDesc<CVar> desc;
    desc.name = name;
    desc.description = description;
    desc.defaultValue = static_cast<CVar>(access(*registry.active));
    desc.flags = FlagsFor(name);
    desc.getter = [access, defaultValue = desc.defaultValue]()
    {
        if (EditorUIData* ui = GetRegistry().active)
            return static_cast<CVar>(access(*ui));
        return defaultValue;
    };
    desc.setter = [access, invalidation, afterChange](const CVar& value)
    {
        if (EditorUIData* ui = GetRegistry().active)
        {
            Field& destination = access(*ui);
            const Field converted = static_cast<Field>(value);
            if (destination != converted)
            {
                destination = converted;
                if (afterChange)
                    afterChange(*ui);
                ApplyInvalidation(*ui, invalidation);
            }
        }
    };
    desc.validator = [minimum, maximum](CVar& value, std::string&)
    {
        if constexpr (!std::is_same_v<CVar, bool>)
        {
            if (std::isfinite(minimum))
                value = std::max(value, static_cast<CVar>(minimum));
            if (std::isfinite(maximum))
                value = std::min(value, static_cast<CVar>(maximum));
        }
        return true;
    };
    if constexpr (std::is_same_v<CVar, bool>)
        desc.choices = { { "false", false }, { "true", true } };
    caustica::console::registerBoundVariable<CVar>(desc);
}

template <typename Access>
void AddEnum(
    const char* name,
    const char* description,
    Access access,
    std::vector<std::string> labels,
    Invalidation invalidation,
    std::function<void(EditorUIData&)> afterChange = {})
{
    using Field = std::remove_cv_t<std::remove_reference_t<
        std::invoke_result_t<Access, EditorUIData&>>>;
    Registry& registry = GetRegistry();
    if (!registry.active)
        return;

    caustica::console::BoundVariableDesc<int> desc;
    desc.name = name;
    desc.description = description;
    desc.defaultValue = static_cast<int>(access(*registry.active));
    desc.flags = FlagsFor(name);
    desc.getter = [access, defaultValue = desc.defaultValue]()
    {
        if (EditorUIData* ui = GetRegistry().active)
            return static_cast<int>(access(*ui));
        return defaultValue;
    };
    desc.setter = [access, invalidation, afterChange](const int& value)
    {
        if (EditorUIData* ui = GetRegistry().active)
        {
            Field& destination = access(*ui);
            const Field converted = static_cast<Field>(value);
            if (destination != converted)
            {
                destination = converted;
                if (afterChange)
                    afterChange(*ui);
                ApplyInvalidation(*ui, invalidation);
            }
        }
    };
    desc.validator = [count = static_cast<int>(labels.size())](int& value, std::string& error)
    {
        if (value < 0 || value >= count)
        {
            error = "enum value is outside the registered range";
            return false;
        }
        return true;
    };
    for (size_t i = 0; i < labels.size(); ++i)
        desc.choices.emplace_back(labels[i], static_cast<int>(i));
    caustica::console::registerBoundVariable<int>(desc);
}

void AddAction(
    const char* name,
    const char* description,
    std::function<void(EditorUIData&)> action)
{
    caustica::console::CommandDesc desc;
    desc.name = name;
    desc.description = description;
    desc.on_execute = [action, nameString = std::string(name)](
                          const caustica::console::Command::Args&)
        -> caustica::console::Command::Result
    {
        Registry& current = GetRegistry();
        if (!current.active)
            return { false, "No active editor render settings." };
        action(*current.active);
        return { true, nameString + " executed" };
    };
    caustica::console::registerCommand(desc);
}

void InitializeEntries()
{
    Registry& registry = GetRegistry();
    if (registry.initialized)
        return;
    registry.initialized = true;

    constexpr Invalidation reset = Invalidation::Accumulation;
    constexpr Invalidation resetCaches =
        Invalidation::Accumulation | Invalidation::RealtimeCaches;
    constexpr Invalidation rebuildAS =
        Invalidation::Accumulation | Invalidation::AccelerationStructure;

#define SETTING_ACCESS(field) [](EditorUIData& ui) -> auto& { return ui.render.settings.field; }

    AddScalar("r.Animation.Enabled", "Enable imported / skeletal animation playback.",
        SETTING_ACCESS(EnableAnimations), Invalidation::None);
    AddScalar("r.Animation.Keyframes.Enabled", "Enable editor keyframe timeline playback.",
        SETTING_ACCESS(EnableKeyframes), Invalidation::None);

    AddScalar("r.RenderGraph.ParallelRecording",
        "Record independent render-graph passes in parallel.",
        SETTING_ACCESS(ParallelRenderGraphRecording), Invalidation::None);
    AddScalar("r.RenderGraph.MinParallelRecordingCost",
        "Minimum estimated wave cost before parallel command recording is used.",
        SETTING_ACCESS(RenderGraphMinParallelRecordingCost), Invalidation::None, 1, 1024);
    AddScalar("r.RenderGraph.MaxRecordingJobs",
        "Maximum parallel recording jobs; zero uses the task worker count.",
        SETTING_ACCESS(RenderGraphMaxRecordingJobs), Invalidation::None, 0, 64);

    AddEnum("r.PathTracing.Mode", "Path tracing operating mode.",
        SETTING_ACCESS(RealtimeMode), { "reference", "realtime" }, resetCaches);
    AddScalar("r.PathTracing.TargetSamples", "Reference accumulation target.",
        SETTING_ACCESS(AccumulationTarget), reset, 1, 4 * 1024 * 1024);
    AddScalar("r.PathTracing.SamplesPerPixel", "Realtime samples per pixel.",
        SETTING_ACCESS(RealtimeSamplesPerPixel), resetCaches, 1, 64);
    AddScalar("r.PathTracing.MaxBounces", "Maximum path depth.",
        SETTING_ACCESS(BounceCount), reset, 0, MAX_BOUNCE_COUNT);
    AddScalar("r.PathTracing.DiffuseBounces", "Maximum diffuse path depth.",
        SETTING_ACCESS(DiffuseBounceCount), reset, 0, MAX_BOUNCE_COUNT);
    AddScalar("r.PathTracing.TextureMipBias", "Global texture sampling MIP bias.",
        SETTING_ACCESS(TexLODBias), reset, -16, 16);
    AddScalar("r.PathTracing.EnvironmentMip", "Environment map diffuse sampling MIP.",
        SETTING_ACCESS(EnvironmentMapDiffuseSampleMIPLevel), reset, 0, 16);
    AddScalar("r.PathTracing.RussianRoulette", "Enable stochastic path termination.",
        SETTING_ACCESS(EnableRussianRoulette), reset);
    AddEnum("r.PathTracing.NestedDielectrics", "Nested dielectric quality.",
        SETTING_ACCESS(NestedDielectricsQuality), { "off", "fast", "quality" }, reset);
    AddScalar("r.PathTracing.ExplicitFP16", "Enable explicit FP16 shader types.",
        SETTING_ACCESS(UseFp16Types), reset);
    AddScalar("r.PathTracing.LDBSDFSampler", "Enable low-discrepancy BSDF sampling.",
        SETTING_ACCESS(EnableLDSamplerForBSDF), reset);
    AddScalar("r.PathTracing.Firefly.Realtime", "Enable realtime firefly filtering.",
        SETTING_ACCESS(RealtimeFireflyFilterEnabled), reset);
    AddScalar("r.PathTracing.Firefly.RealtimeThreshold", "Realtime firefly threshold.",
        SETTING_ACCESS(RealtimeFireflyFilterThreshold), reset, 0.00001, 1000);
    AddScalar("r.PathTracing.Firefly.Reference", "Enable reference firefly filtering.",
        SETTING_ACCESS(ReferenceFireflyFilterEnabled), reset);
    AddScalar("r.PathTracing.Firefly.ReferenceThreshold", "Reference firefly threshold.",
        SETTING_ACCESS(ReferenceFireflyFilterThreshold), reset, 0.01, 1000);

    AddEnum("r.AA.Mode", "Realtime anti-aliasing, super-resolution and denoising mode.",
        SETTING_ACCESS(RealtimeAA), { "off", "taa", "dlss", "dlss-rr" }, resetCaches);
    AddScalar("r.Denoiser.Standalone", "Enable standalone NRD denoising.",
        SETTING_ACCESS(StandaloneDenoiser), resetCaches);
    AddEnum("r.NRD.Method", "Standalone NRD method.",
        SETTING_ACCESS(NRDMethod), { "reblur", "relax" }, resetCaches,
        [](EditorUIData& ui) { ui.render.settings.NRDModeChanged = true; });
    AddScalar("r.NRD.DisocclusionThreshold", "NRD disocclusion threshold.",
        SETTING_ACCESS(NRDDisocclusionThreshold), resetCaches, 0, 1);
    AddScalar("r.NRD.AlternateDisocclusion", "Use alternate NRD disocclusion threshold.",
        SETTING_ACCESS(NRDUseAlternateDisocclusionThresholdMix), resetCaches);
    AddScalar("r.NRD.AlternateDisocclusionThreshold", "Alternate NRD disocclusion threshold.",
        SETTING_ACCESS(NRDDisocclusionThresholdAlternate), resetCaches, 0, 1);
    AddScalar("r.NRD.RadianceClamp", "NRD input radiance clamp.",
        SETTING_ACCESS(DenoiserRadianceClampK), resetCaches, 0, 65536);

    AddScalar("r.TAA.HistoryClamping", "Enable TAA history clamping.",
        [](EditorUIData& ui) -> auto& { return ui.render.settings.TemporalAntiAliasingParams.enableHistoryClamping; },
        resetCaches);
    AddScalar("r.TAA.NewFrameWeight", "TAA current-frame blend weight.",
        [](EditorUIData& ui) -> auto& { return ui.render.settings.TemporalAntiAliasingParams.newFrameWeight; },
        resetCaches, 0.001, 1);
    AddScalar("r.TAA.ClampRelax", "Use the TAA history clamp-relax input.",
        [](EditorUIData& ui) -> auto& { return ui.render.settings.TemporalAntiAliasingParams.useHistoryClampRelax; },
        resetCaches);
    AddEnum("r.TAA.Jitter", "Camera jitter sequence.",
        SETTING_ACCESS(TemporalAntiAliasingJitter), { "msaa", "halton", "r2", "white-noise" },
        resetCaches);

    AddScalar("r.Lighting.NEE.Enabled", "Enable next-event estimation.",
        SETTING_ACCESS(UseNEE), reset);
    AddEnum("r.Lighting.NEE.Method", "NEE light sampling method.",
        SETTING_ACCESS(NEEType), { "uniform", "power", "adaptive" }, reset);
    AddScalar("r.Lighting.NEE.Candidates", "NEE candidate sample count.",
        SETTING_ACCESS(NEECandidateSamples), reset, 1, CAUSTICA_LIGHTING_MAX_SAMPLE_COUNT);
    AddScalar("r.Lighting.NEE.FullSamples", "NEE shadow-tested sample count.",
        SETTING_ACCESS(NEEFullSamples), reset, 0, CAUSTICA_LIGHTING_MAX_SAMPLE_COUNT);
    AddEnum("r.Lighting.NEE.MIS", "Path/light multiple-importance-sampling mode.",
        SETTING_ACCESS(NEEMISType), { "full", "realtime-approx", "approx" }, reset);
    AddScalar("r.Lighting.NEEAT.GlobalFeedback", "NEE-AT temporal feedback weight.",
        SETTING_ACCESS(NEEAT_GlobalTemporalFeedbackWeight), reset, 0, 0.95);
    AddScalar("r.Lighting.NEEAT.LocalToGlobal", "NEE-AT local/global sample ratio.",
        SETTING_ACCESS(NEEAT_LocalToGlobalSampleRatio), reset, 0, 0.95);
    AddScalar("r.Lighting.NEEAT.DistantToLocal", "NEE-AT distant/local importance ratio.",
        SETTING_ACCESS(NEEAT_Distant_vs_Local_Importance), reset, 0.01, 100);

    AddScalar("r.ReSTIR.DI.Enabled", "Enable ReSTIR direct illumination.",
        SETTING_ACCESS(UseReSTIRDI), resetCaches);
    AddScalar("r.ReSTIR.GI.Enabled", "Enable ReSTIR global illumination.",
        SETTING_ACCESS(UseReSTIRGI), resetCaches, std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::quiet_NaN(),
        [](EditorUIData& ui)
        {
            if (ui.render.settings.UseReSTIRGI)
                ui.render.settings.UseReSTIRPT = false;
        });
    AddScalar("r.ReSTIR.PT.Enabled", "Enable ReSTIR path tracing.",
        SETTING_ACCESS(UseReSTIRPT), resetCaches, std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::quiet_NaN(),
        [](EditorUIData& ui)
        {
            if (ui.render.settings.UseReSTIRPT)
                ui.render.settings.UseReSTIRGI = false;
        });
    AddEnum("r.ReSTIR.DI.Preset", "ReSTIR DI/GI quality preset.",
        SETTING_ACCESS(RTXDIRestirPreset),
        { "custom", "fast", "medium", "unbiased", "ultra", "reference" }, resetCaches,
        [](EditorUIData& ui) { ui.render.settings.applyRTXDIRestirPreset(); });
    AddEnum("r.ReSTIR.PT.Preset", "ReSTIR PT quality preset.",
        SETTING_ACCESS(RTXDIRestirPTPreset),
        { "custom", "fast", "medium", "ultra" }, resetCaches,
        [](EditorUIData& ui) { ui.render.settings.applyRTXDIRestirPTPreset(); });

    AddScalar("r.StablePlanes.Count", "Active denoising stable-plane count.",
        SETTING_ACCESS(StablePlanesActiveCount), resetCaches, 1, cStablePlaneCount);
    AddScalar("r.StablePlanes.MaxVertexDepth", "Maximum stable-plane vertex depth.",
        SETTING_ACCESS(StablePlanesMaxVertexDepth), resetCaches, 2, cStablePlaneMaxVertexIndex);
    AddScalar("r.StablePlanes.SplitThreshold", "Stable-plane path split stop threshold.",
        SETTING_ACCESS(StablePlanesSplitStopThreshold), resetCaches, 0, 2);
    AddScalar("r.StablePlanes.PrimarySurfaceReplacement", "Allow primary-surface replacement.",
        SETTING_ACCESS(AllowPrimarySurfaceReplacement), resetCaches);
    AddScalar("r.StablePlanes.SuppressPrimarySpecular", "Suppress noisy primary-plane indirect specular.",
        SETTING_ACCESS(StablePlanesSuppressPrimaryIndirectSpecular), resetCaches);
    AddScalar("r.StablePlanes.SuppressPrimarySpecularAmount", "Primary-plane specular suppression amount.",
        SETTING_ACCESS(StablePlanesSuppressPrimaryIndirectSpecularK), resetCaches, 0, 1);
    AddScalar("r.StablePlanes.AAFallthrough", "Non-primary stable-plane AA fallthrough.",
        SETTING_ACCESS(StablePlanesAntiAliasingFallthrough), resetCaches, 0, 1);

    AddScalar("r.PostProcess.ToneMapping", "Enable tone mapping.",
        SETTING_ACCESS(EnableToneMapping), reset);
    AddScalar("r.PostProcess.AutoExposure", "Enable automatic exposure.",
        [](EditorUIData& ui) -> auto& {
            return ui.render.settings.ToneMappingParams.autoExposure;
        },
        reset);
    AddScalar("r.PostProcess.EdgeDetection", "Enable the LDR edge-detection test.",
        SETTING_ACCESS(PostProcessEdgeDetection), reset);
    AddScalar("r.PostProcess.EdgeThreshold", "Edge-detection threshold.",
        SETTING_ACCESS(PostProcessEdgeDetectionThreshold), reset, 0, 1);

    AddEnum("r.Gaussian.RenderingMode", "Gaussian splat primary rendering method.",
        SETTING_ACCESS(GaussianSplatPrimaryMethod), { "3dgs", "3dgut" }, reset);
    AddEnum("r.Gaussian.Shadows", "Gaussian splat mesh-BVH shadow mode.",
        SETTING_ACCESS(GaussianSplatShadowsMode), { "off", "hard", "soft" }, reset,
        [](EditorUIData& ui)
        {
            ui.render.settings.GaussianSplatShadows =
                ui.render.settings.GaussianSplatShadowsMode != 0;
        });
    AddEnum("r.Gaussian.Sorting", "Gaussian raster sorting method.",
        SETTING_ACCESS(GaussianSplatSortingMode), { "gpu", "stochastic" }, reset);
    AddScalar("r.Gaussian.MipAntialiasing", "Enable mip-splatting antialiasing.",
        SETTING_ACCESS(GaussianSplatMipAntialiasing), reset);
    AddScalar("r.Gaussian.QuantizeNormals", "Quantize Gaussian normal storage.",
        SETTING_ACCESS(GaussianSplatQuantizeNormals), reset);
    AddEnum("r.Gaussian.Culling.Mode", "Gaussian frustum-culling stage.",
        SETTING_ACCESS(GaussianSplatFrustumCulling), { "off", "distance", "raster" }, reset);
    AddScalar("r.Gaussian.Culling.FrustumDilation", "Gaussian frustum dilation.",
        SETTING_ACCESS(GaussianSplatFrustumDilation), reset, 0, 1);
    AddScalar("r.Gaussian.Culling.ScreenSize", "Enable Gaussian screen-size culling.",
        SETTING_ACCESS(GaussianSplatScreenSizeCulling), reset);
    AddScalar("r.Gaussian.Culling.MinPixelCoverage", "Minimum Gaussian pixel coverage.",
        SETTING_ACCESS(GaussianSplatMinPixelCoverage), reset, 0.1, 20);
    AddScalar("r.Gaussian.Shadow.SoftRadius", "Gaussian soft-shadow radius.",
        SETTING_ACCESS(GaussianSplatShadowSoftRadius), reset, 0, 0.5);
    AddScalar("r.Gaussian.Shadow.Samples", "Gaussian soft-shadow sample count.",
        SETTING_ACCESS(GaussianSplatShadowSoftSampleCount), reset, 1, 16);
    AddScalar("r.Gaussian.Shadow.KernelDegree", "Gaussian shadow proxy kernel degree.",
        SETTING_ACCESS(GaussianSplatRtxKernelDegree), rebuildAS, 0, 5);
    AddScalar("r.Gaussian.Shadow.UseAABBs", "Use AABB Gaussian shadow proxies.",
        SETTING_ACCESS(GaussianSplatUseAABBs), rebuildAS);
    AddScalar("r.Gaussian.Shadow.AdaptiveClamp", "Enable adaptive Gaussian shadow clamping.",
        SETTING_ACCESS(GaussianSplatRtxAdaptiveClamp), rebuildAS);
    AddScalar("r.Gaussian.Shadow.RayOffset", "Gaussian shadow-ray offset.",
        SETTING_ACCESS(GaussianSplatRtxParticleShadowOffset), reset, 0, 1);

    AddScalar("r.AS.ForceOpaque", "Force ray-tracing instances opaque.",
        [](EditorUIData& ui) -> auto& { return ui.render.settings.AS.ForceOpaque; }, reset);
    AddScalar("r.AS.ExcludeTransmissive", "Exclude transmissive geometry from the BVH.",
        [](EditorUIData& ui) -> auto& { return ui.render.settings.AS.ExcludeTransmissive; },
        rebuildAS);

    AddScalar("r.Debug.FreezeNoise", "Freeze the realtime noise seed.",
        SETTING_ACCESS(DbgFreezeRealtimeNoiseSeed), reset);
    AddScalar("r.Debug.DisableSERTerminationHint", "Disable the SER path-termination hint.",
        SETTING_ACCESS(DbgDisableSERTerminationHint), reset);
    AddScalar("r.Debug.DiscardNonNEE", "Discard non-NEE path lighting.",
        SETTING_ACCESS(DbgDiscardNonNEELighting), reset);
    AddScalar("r.Debug.DiscardNEE", "Discard NEE lighting.",
        SETTING_ACCESS(DbgDiscardNEELighting), reset);
    AddScalar("r.Debug.ContinuousFeedback", "Continuously sample shader debug feedback.",
        SETTING_ACCESS(ContinuousDebugFeedback), Invalidation::None);
    AddScalar("r.Debug.ShowLines", "Show shader debug lines.",
        SETTING_ACCESS(ShowDebugLines), Invalidation::None);
    AddScalar("r.Debug.LineScale", "Shader debug-line scale.",
        SETTING_ACCESS(DebugLineScale), Invalidation::None, 0.0001, 1000);

    AddAction("r.ResetAccumulation", "Reset path-tracing accumulation.",
        [](EditorUIData& ui) { ui.render.settings.ResetAccumulation = true; });
    AddAction("r.ResetRealtimeCaches", "Reset temporal realtime caches.",
        [](EditorUIData& ui) { ui.render.settings.ResetRealtimeCaches = true; });
    AddAction("r.ReloadShaders", "Request shader and RT pipeline reload.",
        [](EditorUIData& ui) { ui.render.runtime.Invalidation.ShaderReloadRequested = true; });

    auto currentProfile = [](EditorUIData& ui)
    {
        for (const PerformancePreset& preset : s_performancePresets)
        {
            if (MatchesPreset(ui, preset))
            {
                std::string name = Lower(preset.Name);
                std::replace(name.begin(), name.end(), ' ', '-');
                return name;
            }
        }
        return std::string("custom");
    };

    caustica::console::BoundVariableDesc<std::string> profile;
    profile.name = "r.Profile";
    profile.description = "Get or apply the global render quality profile.";
    profile.defaultValue = currentProfile(*registry.active);
    profile.getter = [currentProfile, defaultValue = profile.defaultValue]()
    {
        if (EditorUIData* ui = GetRegistry().active)
            return currentProfile(*ui);
        return defaultValue;
    };
    profile.validator = [](std::string& requested, std::string& error)
    {
        requested = Lower(requested);
        std::replace(requested.begin(), requested.end(), '_', '-');
        if (requested == "custom")
            return true;
        for (const PerformancePreset& preset : s_performancePresets)
        {
            std::string candidate = Lower(preset.Name);
            std::replace(candidate.begin(), candidate.end(), ' ', '-');
            if (candidate == requested)
                return true;
        }
        error = "unknown render profile";
        return false;
    };
    profile.setter = [](const std::string& requested)
    {
        EditorUIData* ui = GetRegistry().active;
        if (!ui || requested == "custom")
            return;
        for (const PerformancePreset& preset : s_performancePresets)
        {
            std::string candidate = Lower(preset.Name);
            std::replace(candidate.begin(), candidate.end(), ' ', '-');
            if (candidate == requested)
            {
                ApplyPreset(*ui, preset);
                ui->render.settings.ResetRealtimeCaches = true;
                return;
            }
        }
    };
    profile.choices = {
        { "custom", "custom" },
        { "ultra-performance", "ultra-performance" },
        { "performance", "performance" },
        { "balanced", "balanced" },
        { "quality", "quality" },
        { "ultra-quality", "ultra-quality" },
    };
    caustica::console::registerBoundVariable<std::string>(profile);

#undef SETTING_ACCESS
}

} // namespace

RenderSettingsConsoleBinding::RenderSettingsConsoleBinding(EditorUIData& ui)
    : m_ui(&ui)
    , m_interpreter(std::make_shared<caustica::console::Interpreter>())
{
    GetRegistry().active = &ui;
    InitializeEntries();
}

RenderSettingsConsoleBinding::~RenderSettingsConsoleBinding()
{
    if (GetRegistry().active == m_ui)
        GetRegistry().active = nullptr;
}

bool RenderSettingsConsoleBinding::execute(
    std::string_view commandLine,
    std::string* output,
    caustica::console::VariableState::SetBy origin) const
{
    const caustica::console::Interpreter::Result result =
        m_interpreter->execute(commandLine, origin);
    if (output)
        *output = result.output;
    return result.status;
}

} // namespace caustica::editor
