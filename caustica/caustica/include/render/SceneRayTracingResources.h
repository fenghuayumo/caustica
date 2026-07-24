#pragma once

#include <assets/loader/ShaderMacro.h>
#include <render/core/PathTracerSettings.h>
#include <render/core/PtPipelineFeaturePresets.h>
#include <render/RenderRuntimeState.h>
#include <rhi/rhi.h>
#include <shaders/SampleConstantBuffer.h>

#include <functional>
#include <memory>
#include <vector>

class PathTracingShaderCompiler;
class PTPipelineVariant;

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
class WorldRenderer;
}

namespace caustica::render
{

class SceneLightingPasses;
struct ScenePassWireParams;

using AdditionalAccelStructBuilder = std::function<void(caustica::rhi::ICommandList*)>;

// RT pipeline variants, shader macros, and acceleration-structure lifecycle.
class SceneRayTracingResources
{
    friend struct PathTracerScenePasses;

public:
    void setAdditionalAccelStructBuilder(AdditionalAccelStructBuilder builder);

    void fillPTPipelineGlobalMacros(std::vector<caustica::ShaderMacro>& macros);
    [[nodiscard]] PtFeaturePresetId resolveFeaturePreset() const;
    bool createPTPipeline();
    void createRTPipelines();
    void ensureStablePlanePipelines();
    // Bind cooked preset pipelines (lightweight). Variants must already exist.
    bool bindFeaturePreset(PtFeaturePresetId id);
    // Ensure CreateStateObject for a preset (blocking for that preset only), then bind.
    bool ensureFeaturePresetReady(PtFeaturePresetId id, bool showProgress = false);

    void uploadSubInstanceData(caustica::rhi::ICommandList* commandList);
    // Session Scene is owned by PathTracingContext; pass it in for mesh/AS mutation.
    void createAccelStructs(
        caustica::rhi::ICommandList* commandList,
        caustica::Scene& scene,
        const caustica::scene::SceneRenderData* renderData = nullptr);
    void recreateAccelStructs(
        caustica::rhi::ICommandList* commandList,
        caustica::Scene& scene,
        const caustica::scene::SceneRenderData* renderData = nullptr);
    void requestMeshAccelRebuild(const std::shared_ptr<caustica::MeshInfo>& mesh, bool resetAccumulation = true);

    // Structure-only invalidation (no shader reload). Prefer this after runtime scene graph edits.
    void requestAccelerationStructureRebuild();
    void requestFullRebuild();
    void invalidateBindingSet();
    void recreateBindingSet(const caustica::scene::SceneRenderData* renderData = nullptr);

    void sampleRenderCode(caustica::rhi::IFramebuffer* framebuffer,
        caustica::rhi::CommandListHandle commandList,
        const SampleConstants& constants);

    bool consumeShaderReloadRequest();
    bool& accelerationStructRebuildRequested();

    std::shared_ptr<PathTracingShaderCompiler> pathTracingShaderCompiler() const;
    std::shared_ptr<PTPipelineVariant>& pipelineReference();
    std::shared_ptr<PTPipelineVariant>& pipelineBuildStablePlanes();
    std::shared_ptr<PTPipelineVariant>& pipelineFillStablePlanes();
    std::shared_ptr<PTPipelineVariant>& pipelineTestRaygenPPHDR();
    std::shared_ptr<PTPipelineVariant>& pipelineEdgeDetection();

private:
    void wireSession(const ScenePassWireParams& params);
    void createBlases(
        caustica::rhi::ICommandList* commandList,
        const caustica::scene::SceneRenderData& renderData);
    void createTlas(
        caustica::rhi::ICommandList* commandList,
        const caustica::scene::SceneRenderData& renderData);

    caustica::GpuDevice*                        m_gpuDevice = nullptr;
    caustica::AccelStructManager*               m_accelStructs = nullptr;
    caustica::render::WorldRenderer* m_worldRenderer = nullptr;
    PathTracerSettings*                         m_settings = nullptr;
    caustica::render::RenderInvalidationState*  m_invalidation = nullptr;
    SceneLightingPasses*                        m_lightingPasses = nullptr;
    caustica::BindingCache*                     m_bindingCache = nullptr;
    AdditionalAccelStructBuilder                m_additionalAccelStructBuilder;
};

} // namespace caustica::render
