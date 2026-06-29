#include "PathTracerSessionBootstrap.h"

#include "SceneEditor.h"

#include <engine/EngineRenderer.h>
#include <render/Core/RenderSceneTypeFactory.h>
#include <render/SceneGaussianSplatPasses.h>
#include <render/SceneLightingPasses.h>
#include <render/SceneRayTracingResources.h>

namespace caustica::editor
{

std::unique_ptr<caustica::EngineRenderer> bootstrapPathTracerSession(
    const PathTracerSessionBootstrapParams& params)
{
    if (params.onAfterAttachPasses)
        params.onAfterAttachPasses();

    auto engineRenderer = std::make_unique<caustica::EngineRenderer>();
    engineRenderer->initialize(params.gpuDevice,
        std::make_shared<caustica::render::RenderSceneTypeFactory>(),
        caustica::EngineSceneCallbacks{
            .OnSceneLoaded = [&]() { params.sceneEditor.SceneLoaded(); },
            .OnSceneUnloading = [&]() { params.sceneEditor.SceneUnloading(); },
        });

    params.sceneEditor.AttachRenderResources(
        engineRenderer->shaderFactory(),
        engineRenderer->commonPasses(),
        engineRenderer->bindingCache(),
        engineRenderer->descriptorTable(),
        engineRenderer->textureLoader());
    params.sceneEditor.AttachSceneServices(
        *engineRenderer->sceneManager(),
        *engineRenderer->renderCore());

    engineRenderer->createPathTracer(caustica::PathTracerSessionParams{
        .gpuDevice = params.gpuDevice,
        .settings = params.sceneEditor.GetPathTracerSettings(),
        .runtimeState = params.sceneEditor.GetRenderRuntimeState(),
        .rayTracing = params.rayTracing,
        .gaussianSplats = params.gaussianSplats,
        .lighting = params.lighting,
        .sceneTime = params.sceneEditor.GetSceneTimeRef(),
        .gaussianSplatEmissionProxies = params.gaussianSplats.emissionProxies(),
        .diagnostics = params.diagnostics,
        .frameExtensions = params.frameExtensions,
    });
    params.sceneEditor.AttachWorldRenderer(engineRenderer->worldRenderer());
    assert(params.rayTracing.isAttached() && params.gaussianSplats.isAttached());

    params.sceneEditor.Init(params.preferredScene, engineRenderer->shaderFactory());
    return engineRenderer;
}

} // namespace caustica::editor
