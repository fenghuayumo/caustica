#include "PathTracerSessionBootstrap.h"

#include "SceneEditor.h"

#include <engine/EngineRenderer.h>
#include <render/Core/RenderSceneTypeFactory.h>

namespace caustica::editor
{

std::unique_ptr<caustica::EngineRenderer> bootstrapPathTracerSession(
    const PathTracerSessionBootstrapParams& params)
{
    auto engineRenderer = std::make_unique<caustica::EngineRenderer>();
    SceneEditor* sceneEditor = &params.sceneEditor;
    engineRenderer->initialize(params.gpuDevice,
        std::make_shared<caustica::render::RenderSceneTypeFactory>(),
        caustica::EngineSceneCallbacks{
            .OnSceneLoaded = [sceneEditor]() { sceneEditor->SceneLoaded(); },
            .OnSceneUnloading = [sceneEditor]() { sceneEditor->SceneUnloading(); },
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

    params.sceneEditor.AttachLightingPasses(engineRenderer->lightingPasses());
    params.sceneEditor.AttachRayTracingResources(engineRenderer->rayTracingResources());
    params.sceneEditor.AttachGaussianSplatPasses(engineRenderer->gaussianSplatPasses());

    engineRenderer->createPathTracer(caustica::PathTracerSessionParams{
        .gpuDevice = params.gpuDevice,
        .settings = params.sceneEditor.GetPathTracerSettings(),
        .runtimeState = params.sceneEditor.GetRenderRuntimeState(),
        .sceneTime = params.sceneEditor.GetSceneTimeRef(),
        .diagnostics = params.diagnostics,
        .frameExtensions = params.frameExtensions,
    });
    params.sceneEditor.AttachWorldRenderer(engineRenderer->worldRenderer());
    assert(engineRenderer->rayTracingResources().isAttached()
        && engineRenderer->gaussianSplatPasses().isAttached());

    params.sceneEditor.Init(params.preferredScene, engineRenderer->shaderFactory());
    return engineRenderer;
}

} // namespace caustica::editor
