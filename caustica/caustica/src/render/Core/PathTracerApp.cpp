#include <render/Core/PathTracerApp.h>
#include <render/Core/RenderPipeline.h>
#include <engine/SceneManager.h>
#include <scene/Scene.h>

namespace caustica
{

PathTracerRenderCore::PathTracerRenderCore(nvrhi::IDevice* device)
    : m_device(device)
    , m_accelStructs(device)
{
}

void PathTracerRenderCore::initializeRenderPipeline(std::shared_ptr<ShaderFactory> shaderFactory)
{
    if (!shaderFactory || m_pipeline)
        return;
    m_pipeline = std::make_unique<RenderPipeline>(m_device, std::move(shaderFactory));
}

void PathTracerRenderCore::onSceneLoaded(Scene& scene, bool& accelRebuildRequested)
{
    SceneManager::onSceneLoadedGpuPrep(scene, accelRebuildRequested);
    m_accelStructs.resetSubInstanceCount();
}

void PathTracerRenderCore::onSceneUnloading()
{
    m_accelStructs.releaseGpuResources();
}

} // namespace caustica
