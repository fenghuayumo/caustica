#include <engine/PathTracingRuntime.h>
#include <engine/GpuSharedCaches.h>

#include <render/core/BindingCache.h>
#include <render/core/RenderDevice.h>
#include <render/worldRenderer/WorldRenderer.h>

namespace caustica
{

PathTracingRuntime::PathTracingRuntime() = default;
PathTracingRuntime::~PathTracingRuntime()
{
    destroy();
}

bool PathTracingRuntime::create(const CreateParams& params)
{
    destroy();

    GpuSharedCaches& infra = params.gpuSharedCaches;
    m_accelStructs = AccelStructManager(params.gpuDevice.getDevice());

    m_pathTracingContext = std::make_unique<render::PathTracingContext>(render::PathTracingContext{
        .gpuDevice = params.gpuDevice,
        .camera = m_renderCamera,
        .accelStructs = m_accelStructs,
        .settings = params.settings,
        .runtimeState = params.runtimeState,
        .scenePasses = m_scenePasses,
        .shaderFactory = infra.shaderFactory,
        .renderDevice = *infra.renderDevice,
        .bindingCache = *infra.bindingCache,
        .textureCache = infra.textureLoader,
        .descriptorTable = infra.descriptorTable,
        .sceneTime = params.sceneTime,
        .diagnostics = params.diagnostics,
    });

    m_worldRenderer = std::make_unique<render::WorldRenderer>(*m_pathTracingContext);
    m_worldRenderer->createBindingLayouts(infra.bindlessLayout);

    render::ScenePassWireParams sceneWireParams{
        .gpuDevice = params.gpuDevice,
        .accelStructs = m_accelStructs,
        .worldRenderer = *m_worldRenderer,
        .settings = params.settings,
        .invalidation = params.runtimeState.Invalidation,
        .gaussianSplatsSummary = params.runtimeState.GaussianSplats,
        .lighting = m_scenePasses.lighting,
        .bindingCache = *infra.bindingCache,
        .shaderFactory = infra.shaderFactory,
        .renderDevice = *infra.renderDevice,
    };
    sceneWireParams.onGaussianSplatTemporalReset = [worldRenderer = m_worldRenderer.get()]() {
        worldRenderer->setGaussianSplatTemporalReset(true);
    };
    sceneWireParams.getRenderTargets = [worldRenderer = m_worldRenderer.get()]() {
        return worldRenderer->getRenderTargets();
    };
    sceneWireParams.getShaderDebug = [worldRenderer = m_worldRenderer.get()]() {
        return worldRenderer->getShaderDebug();
    };
    m_scenePasses.wireSession(sceneWireParams);
    return true;
}

void PathTracingRuntime::destroy()
{
    m_worldRenderer.reset();
    m_pathTracingContext.reset();
    m_accelStructs = AccelStructManager{};
    m_scenePasses = {};
}

void PathTracingRuntime::bindSessionScene(std::shared_ptr<Scene> scene, std::filesystem::path scenePath)
{
    m_scenePasses.bindSessionScene(std::move(scene), std::move(scenePath));
}

void PathTracingRuntime::clearSessionScene()
{
    m_scenePasses.clearSessionScene();
}

} // namespace caustica
