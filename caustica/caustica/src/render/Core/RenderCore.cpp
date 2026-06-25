#include <render/Core/RenderCore.h>
#include <render/Core/RenderPipeline.h>
#include <scene/SceneManager.h>
#include <scene/Scene.h>

namespace caustica
{

RenderCore::RenderCore(nvrhi::IDevice* device)
    : m_device(device)
    , m_accelStructs(device)
{
}

void RenderCore::initializeRenderPipeline(std::shared_ptr<ShaderFactory> shaderFactory)
{
    if (!shaderFactory || m_pipeline)
        return;
    m_pipeline = std::make_unique<RenderPipeline>(m_device, std::move(shaderFactory));
}

void RenderCore::onSceneLoaded(Scene& scene, bool& accelRebuildRequested)
{
    SceneManager::onSceneLoadedGpuPrep(scene, accelRebuildRequested);
    m_accelStructs.resetSubInstanceCount();
}

void RenderCore::onSceneUnloading()
{
    m_accelStructs.releaseGpuResources();
}

} // namespace caustica
