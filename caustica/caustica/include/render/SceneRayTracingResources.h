#pragma once

#include <assets/loader/ShaderMacro.h>
#include <render/core/PtPipelineFeaturePresets.h>
#include <render/RenderRuntimeState.h>
#include <rhi/rhi.h>

#include <functional>
#include <memory>
#include <vector>

class PathTracingShaderCompiler;
class PTPipelineVariant;
struct PathTracerSettings;

namespace caustica
{
class BindingCache;
class GpuDevice;
struct MeshInfo;
class AccelStructManager;
class Scene;
class ShaderFactory;
namespace scene { class SceneRenderData; }
} // namespace caustica

namespace caustica::render
{
class PathTraceSceneBindings;
class RtPipelineCache;
struct RtPipelineCacheStats;
struct RtPipelineWarmupStatus;
}

namespace caustica::render
{

class SceneLightingPasses;

using AdditionalAccelStructBuilder = std::function<void(caustica::rhi::CommandList*)>;
using AccelBuildProgress = std::function<void(
    const char* stage, size_t completedMeshes, size_t totalMeshes, uint64_t scratchBytes)>;

// RT pipeline variants, shader macros, and acceleration-structure lifecycle.
class SceneRayTracingResources
{
    friend struct PathTracerScenePasses;

public:
    struct Dependencies
    {
        caustica::GpuDevice& gpuDevice;
        caustica::AccelStructManager& accelStructs;
        RenderInvalidationState& invalidation;
        caustica::BindingCache& bindingCache;
        PathTraceSceneBindings& sceneBindings;
    };

    void setAdditionalAccelStructBuilder(AdditionalAccelStructBuilder builder);

    void fillPTPipelineGlobalMacros(
        std::vector<caustica::ShaderMacro>& macros,
        const PathTracerSettings& settings);
    void initializePipelineRuntime(
        caustica::rhi::BindingLayoutHandle bindingLayout,
        caustica::rhi::BindingLayoutHandle bindlessLayout,
        const PathTracerSettings& settings);
    [[nodiscard]] bool hasPipelineRuntime() const { return m_shaderCompiler != nullptr; }
    void updatePipelineRuntime(
        const caustica::scene::SceneRenderData* sceneData,
        uint32_t subInstanceCount,
        bool forceShaderReload,
        const PathTracerSettings& settings);
    [[nodiscard]] RtPipelineWarmupStatus pipelineWarmupStatus() const;
    [[nodiscard]] RtPipelineCacheStats pipelineCacheStats() const;
    [[nodiscard]] PTPipelineVariant* pipelineReference() const { return m_pipelineReference.get(); }
    [[nodiscard]] PTPipelineVariant* pipelineBuildStablePlanes() const { return m_pipelineBuildStablePlanes.get(); }
    [[nodiscard]] PTPipelineVariant* pipelineFillStablePlanes() const { return m_pipelineFillStablePlanes.get(); }
    [[nodiscard]] PTPipelineVariant* pipelineEdgeDetection() const { return m_pipelineEdgeDetection.get(); }
    void clearPipelineBindings();
    uint32_t precacheAllFeaturePresets(bool showProgress = true);
    void ensureStablePlanePipelines();

    void uploadSubInstanceData(caustica::rhi::CommandList* commandList);
    // Session Scene is owned by PathTracingContext; pass it in for mesh/AS mutation.
    [[nodiscard]] bool createAccelStructs(
        caustica::rhi::CommandList* commandList,
        caustica::Scene& scene,
        const PathTracerSettings& settings,
        const caustica::scene::SceneRenderData* renderData = nullptr);
    [[nodiscard]] bool recreateAccelStructs(
        caustica::rhi::CommandList* commandList,
        caustica::Scene& scene,
        const PathTracerSettings& settings,
        const caustica::scene::SceneRenderData* renderData = nullptr);
    // Exclusive load path: form BLAS submissions from backend-reported scratch
    // bytes and apply bounded in-flight fence backpressure.
    [[nodiscard]] bool recreateAccelStructsForLoad(
        caustica::Scene& scene,
        const caustica::scene::SceneRenderData& renderData,
        const PathTracerSettings& settings,
        uint64_t targetScratchBytesPerSubmit = 256ull * 1024ull * 1024ull,
        AccelBuildProgress progress = {});
    void requestMeshAccelRebuild(const std::shared_ptr<caustica::MeshInfo>& mesh, bool resetAccumulation = true);

    // Structure-only invalidation (no shader reload). Prefer this after runtime scene graph edits.
    void requestAccelerationStructureRebuild();
    void requestFullRebuild();

    bool consumeShaderReloadRequest();
    bool consumeAccumulationResetRequest();
    bool& accelerationStructRebuildRequested();

private:
    void initialize(const Dependencies& dependencies, SceneLightingPasses& lighting);
    [[nodiscard]] PtFeaturePresetId resolveFeaturePreset(const PathTracerSettings& settings) const;
    void createRTPipelines(const PathTracerSettings& settings);
    bool bindFeaturePreset(PtFeaturePresetId id);
    bool ensureFeaturePresetReady(PtFeaturePresetId id, bool showProgress = false);
    [[nodiscard]] bool createBlases(
        caustica::rhi::CommandList* commandList,
        const caustica::scene::SceneRenderData& renderData,
        const PathTracerSettings& settings);
    [[nodiscard]] bool createTlas(
        caustica::rhi::CommandList* commandList,
        const caustica::scene::SceneRenderData& renderData);

    caustica::GpuDevice*                        m_gpuDevice = nullptr;
    caustica::AccelStructManager*               m_accelStructs = nullptr;
    caustica::render::RenderInvalidationState*  m_invalidation = nullptr;
    SceneLightingPasses*                        m_lightingPasses = nullptr;
    caustica::BindingCache*                     m_bindingCache = nullptr;
    PathTraceSceneBindings*                     m_sceneBindings = nullptr;
    AdditionalAccelStructBuilder                m_additionalAccelStructBuilder;

    // Single owner for the complete RT pipeline runtime. WorldRenderer only
    // consumes raw per-frame views exposed above.
    std::shared_ptr<PathTracingShaderCompiler>  m_shaderCompiler;
    std::shared_ptr<RtPipelineCache>             m_pipelineCache;
    std::shared_ptr<PTPipelineVariant>           m_pipelineReference;
    std::shared_ptr<PTPipelineVariant>           m_pipelineBuildStablePlanes;
    std::shared_ptr<PTPipelineVariant>           m_pipelineFillStablePlanes;
    std::shared_ptr<PTPipelineVariant>           m_pipelineEdgeDetection;
};

} // namespace caustica::render
