#pragma once

#include <render/Core/PathTracerSettings.h>
#include <render/RenderRuntimeState.h>
#include <rhi/nvrhi.h>
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
class MeshInfo;
class RenderCore;
class Scene;
class ShaderFactory;
} // namespace caustica

namespace caustica::render
{
class WorldRenderer;
}

class SceneManager;

namespace caustica::render
{

class SceneLightingPasses;
struct ScenePassWireParams;

using AdditionalAccelStructBuilder = std::function<void(nvrhi::ICommandList*)>;

// RT pipeline variants, shader macros, and acceleration-structure lifecycle.
class SceneRayTracingResources
{
    friend struct PathTracerScenePasses;

public:
    void setAdditionalAccelStructBuilder(AdditionalAccelStructBuilder builder);

    void fillPTPipelineGlobalMacros(std::vector<caustica::ShaderMacro>& macros);
    bool createPTPipeline();
    void createRTPipelines();
    void ensureStablePlanePipelines();

    void createBlases(nvrhi::ICommandList* commandList);
    void createTlas(nvrhi::ICommandList* commandList);
    void uploadSubInstanceData(nvrhi::ICommandList* commandList);
    void createAccelStructs(nvrhi::ICommandList* commandList);
    void recreateAccelStructs(nvrhi::ICommandList* commandList);
    void requestMeshAccelRebuild(const std::shared_ptr<caustica::MeshInfo>& mesh);

    void requestFullRebuild();
    void invalidateBindingSet();
    void recreateBindingSet();

    void sampleRenderCode(nvrhi::IFramebuffer* framebuffer,
        nvrhi::CommandListHandle commandList,
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

    caustica::GpuDevice*                        m_gpuDevice = nullptr;
    SceneManager*                               m_sceneManager = nullptr;
    caustica::RenderCore*                       m_renderCore = nullptr;
    caustica::render::WorldRenderer* m_worldRenderer = nullptr;
    PathTracerSettings*                         m_settings = nullptr;
    caustica::render::RenderInvalidationState*  m_invalidation = nullptr;
    SceneLightingPasses*                        m_lightingPasses = nullptr;
    caustica::BindingCache*                     m_bindingCache = nullptr;
    AdditionalAccelStructBuilder                m_additionalAccelStructBuilder;
};

} // namespace caustica::render
