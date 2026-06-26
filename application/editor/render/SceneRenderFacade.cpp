#include "render/SceneRenderFacade.h"

#include "SceneEditor.h"

#include <backend/GpuDevice.h>
#include <render/Core/BindingCache.h>
#include <render/Core/RenderCore.h>
#include <render/WorldRenderer/PathTracingWorldRenderer.h>
#include <scene/SceneManager.h>

namespace caustica::editor
{

void SceneRenderFacade::attachSceneEditor(SceneEditor& editor)
{
    editor.AttachLightingPasses(m_lighting);
    editor.AttachRayTracingResources(m_rayTracing);
    editor.AttachGaussianSplatPasses(m_gaussianSplats);
}

void SceneRenderFacade::refreshEnvironmentMapMediaList(const std::filesystem::path& assetsFolder,
    const std::filesystem::path& currentScenePath)
{
    m_lighting.refreshEnvironmentMapMediaList(assetsFolder, currentScenePath);
}

void SceneRenderFacade::initWorldRenderer(
    caustica::GpuDevice& gpuDevice,
    SceneManager& sceneManager,
    caustica::RenderCore& renderCore,
    caustica::render::PathTracingWorldRenderer& worldRenderer,
    PathTracerSettings& settings,
    EditorUIState& editor,
    const std::shared_ptr<caustica::ShaderFactory>& shaderFactory,
    const std::shared_ptr<caustica::CommonRenderPasses>& commonPasses,
    caustica::BindingCache& bindingCache)
{
    m_rayTracing.attach(
        gpuDevice,
        sceneManager,
        renderCore,
        worldRenderer,
        settings,
        editor,
        m_lighting,
        bindingCache);

    m_gaussianSplats.attach(
        gpuDevice,
        sceneManager,
        renderCore,
        worldRenderer,
        settings,
        editor,
        shaderFactory,
        commonPasses);

    m_gaussianSplats.setOnRequestFullRebuild([this] { m_rayTracing.requestFullRebuild(); });
    m_rayTracing.setAdditionalAccelStructBuilder(
        [this](nvrhi::ICommandList* commandList) { m_gaussianSplats.buildAccelStructs(commandList); });
}

caustica::render::WorldRendererPipelineHooks SceneRenderFacade::buildHooks(SceneEditor& editor)
{
    SceneRayTracingResources& rt = m_rayTracing;
    SceneGaussianSplatPasses& splats = m_gaussianSplats;
    return caustica::render::WorldRendererPipelineHooks{
        .needsRasterPrecompute = [] { return false; },
        .getMaterialSpecializationShader = [&editor] { return editor.GetMaterialSpecializationShader(); },
        .fillPTPipelineGlobalMacros = [&rt](auto& macros) { rt.fillPTPipelineGlobalMacros(macros); },
        .sampleRenderCode = [&rt](auto* framebuffer, auto commandList, auto& constants) {
            rt.sampleRenderCode(framebuffer, commandList, constants);
        },
        .addCustomBindings = [&editor](auto& desc) { editor.AddCustomBindings(desc); },
        .createRTPipelines = [&rt] { rt.createRTPipelines(); },
        .onRenderTargetsRecreated = [&editor] { editor.OnRenderTargetsRecreated(); },
        .prepareGaussianSplatPasses = [&splats] { splats.preparePasses(); },
        .buildGaussianSplatEmissionProxyList = [&splats] { splats.buildEmissionProxyList(); },
        .isGaussianSplatEmissionEnabled = [&splats] { return splats.isEmissionEnabled(); },
        .gaussianSplatObjectsEmpty = [&splats] { return splats.objectsEmpty(); },
        .getPrimaryGaussianSplatBinding = [&splats] { return splats.getPrimaryBinding(); },
        .renderSceneGaussianSplats = [&splats](auto* commandList, auto& view, auto& targets, auto& settings, bool& renderedAny) {
            splats.renderSceneGaussianSplats(commandList, view, targets, settings, renderedAny);
        },
        .updateViews = [&editor](auto* framebuffer) { editor.UpdateViews(framebuffer); },
        .recreateAccelStructs = [&rt](auto* commandList) { rt.recreateAccelStructs(commandList); },
        .uploadSubInstanceData = [&rt](auto* commandList) { rt.uploadSubInstanceData(commandList); },
        .collectUncompressedTextures = [&editor] { editor.CollectUncompressedTextures(); },
        .computeCameraJitter = [&editor](uint frameIndex) { return editor.ComputeCameraJitter(frameIndex); },
        .consumeShaderReloadRequest = [&rt] { return rt.consumeShaderReloadRequest(); },
        .accelerationStructRebuildRequested = [&rt]() -> bool& { return rt.accelerationStructRebuildRequested(); },
        .hasActivePickRequest = [&editor] { return editor.hasActivePickRequest(); },
        .showDeltaTree = [&editor] { return editor.showDeltaTree(); },
        .pickMaterialRequested = [&editor] { return editor.pickMaterialRequested(); },
        .pickInstanceRequested = [&editor] { return editor.pickInstanceRequested(); },
        .clearPickRequests = [&editor] { editor.clearPickRequests(); },
        .resolvePickFeedback = [&editor](auto& feedback) { editor.resolvePickFeedback(feedback); },
        .consumeExperimentalPhotoScreenshot = [&editor] { return editor.consumeExperimentalPhotoScreenshot(); },
        .captureScriptPreRender = [&editor] { editor.captureScriptPreRender(); },
        .captureScriptPostRender = [&editor](auto saveTexture) { editor.captureScriptPostRender(saveTexture); },
        .getOrCreateZoomTool = [&editor] { return editor.getOrCreateZoomTool(); },
    };
}

caustica::render::WorldRendererServices SceneRenderFacade::buildWorldRendererServices(
    const SceneRenderFacadeServicesParams& params)
{
    SceneEditor& editor = params.editor;
    SceneLightingPasses& lighting = m_lighting;
    SceneGaussianSplatPasses& splats = m_gaussianSplats;
    return caustica::render::WorldRendererServices{
        .gpuDevice = params.gpuDevice,
        .sceneManager = params.sceneManager,
        .renderCore = params.renderCore,
        .settings = params.settings,
        .shaderFactory = params.shaderFactory,
        .commonPasses = params.commonPasses,
        .bindingCache = params.bindingCache,
        .textureCache = params.textureCache,
        .descriptorTable = params.descriptorTable,
        .envMapBaker = lighting.envMapBaker(),
        .lightsBaker = lighting.lightsBaker(),
        .materialsBaker = lighting.materialsBaker(),
        .ommBaker = lighting.ommBaker(),
        .computePipelineBaker = lighting.computePipelineBaker(),
        .lights = lighting.lights(),
        .envMapSceneParams = lighting.envMapSceneParams(),
        .envMapLocalPath = lighting.envMapLocalPath(),
        .envMapOverride = lighting.envMapOverride(),
        .sceneTime = editor.GetSceneTimeRef(),
        .gaussianSplatEmissionProxies = splats.emissionProxies(),
        .progressInitializingRenderer = editor.GetProgressInitializingRenderer(),
        .asyncLoadingInProgress = editor.GetAsyncLoadingInProgressRef(),
        .benchStart = editor.GetBenchStart(),
        .benchLast = editor.GetBenchLast(),
        .benchFrames = editor.GetBenchFrames(),
        .hooks = buildHooks(editor),
    };
}

} // namespace caustica::editor
