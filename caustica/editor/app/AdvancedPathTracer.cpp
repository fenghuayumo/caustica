#include "AdvancedPathTracer.h"

#include <SampleCommon/CaptureScriptManager.h>
#include <render/Passes/Debug/ZoomTool.h>
#include <render/Passes/Gaussian/GaussianSplatPass.h>

void AdvancedPathTracer::Render(nvrhi::IFramebuffer* framebuffer)
{
    if (GetSceneManager()->getScene() == nullptr)
    {
        assert(false);
        return;
    }
    PrepareEditorFrame();
    GetWorldRenderer()->render(framebuffer);
}

void AdvancedPathTracer::BackBufferResizing()
{
    if (auto* r = GetWorldRenderer())
        r->onBackBufferResizing();
}

void AdvancedPathTracer::PathTrace(nvrhi::IFramebuffer* framebuffer, const SampleConstants& constants)
{
    GetWorldRenderer()->pathTrace(framebuffer, constants);
}

void AdvancedPathTracer::Denoise(nvrhi::IFramebuffer* framebuffer)
{
    GetWorldRenderer()->denoise(framebuffer);
}

void AdvancedPathTracer::PostProcessAA(nvrhi::IFramebuffer* framebuffer, bool reset)
{
    GetWorldRenderer()->postProcessAA(framebuffer, reset);
}

void AdvancedPathTracer::SampleRenderCode(nvrhi::IFramebuffer* framebuffer, nvrhi::CommandListHandle commandList, const SampleConstants& constants)
{
    auto* r = GetWorldRenderer();
    if (!r)
        return;
    if (m_ui.ActualUseRTXDIPasses())
        r->getRtxdiPass()->BeginFrame(commandList, *r->getRenderTargets(), r->getBindingLayout(), r->getBindingSet());

    PathTrace(framebuffer, constants);
    Denoise(framebuffer);
}

void AdvancedPathTracer::CreateRTPipelines()
{
    auto pipelineBaker = GetRTPipelineBaker();
    using SM = caustica::ShaderMacro;

    PtPipelineReference()         = pipelineBaker->CreateVariant("PathTracerSample.hlsl", { SM("PATH_TRACER_MODE", "PATH_TRACER_MODE_REFERENCE") }, "REF");
    PtPipelineBuildStablePlanes() = pipelineBaker->CreateVariant("PathTracerSample.hlsl", { SM("PATH_TRACER_MODE", "PATH_TRACER_MODE_BUILD_STABLE_PLANES") }, "BUILD");
    PtPipelineFillStablePlanes()  = pipelineBaker->CreateVariant("PathTracerSample.hlsl", { SM("PATH_TRACER_MODE", "PATH_TRACER_MODE_FILL_STABLE_PLANES") }, "FILL");
    PtPipelineTestRaygenPPHDR()   = pipelineBaker->CreateVariant("TestRaygenPP.hlsl", { SM("PP_TEST_HDR", "1") }, "TESTRG", true);
    PtPipelineEdgeDetection()     = pipelineBaker->CreateVariant("TestRaygenPP.hlsl", { SM("PP_EDGE_DETECTION", "1") }, "EDGY", true);
}

void AdvancedPathTracer::DestroyRTPipelines()
{
    PtPipelineReference()         = nullptr;
    PtPipelineBuildStablePlanes() = nullptr;
    PtPipelineFillStablePlanes()  = nullptr;
    PtPipelineTestRaygenPPHDR()   = nullptr;
    PtPipelineEdgeDetection()     = nullptr;
}

std::string AdvancedPathTracer::GetMaterialSpecializationShader() const
{
    return "PathTracerMaterialSpecializations.hlsl";
}

bool AdvancedPathTracer::needsRasterPrecompute() { return NeedsRasterPrecompute(); }

std::string AdvancedPathTracer::getMaterialSpecializationShader() const { return GetMaterialSpecializationShader(); }

void AdvancedPathTracer::fillPTPipelineGlobalMacros(std::vector<caustica::ShaderMacro>& macros)
{
    FillPTPipelineGlobalMacros(macros);
}

void AdvancedPathTracer::sampleRenderCode(nvrhi::IFramebuffer* framebuffer, nvrhi::CommandListHandle commandList, const SampleConstants& constants)
{
    SampleRenderCode(framebuffer, commandList, constants);
}

void AdvancedPathTracer::addCustomBindings(nvrhi::BindingSetDesc& bindingSetDesc)
{
    AddCustomBindings(bindingSetDesc);
}

void AdvancedPathTracer::onRenderTargetsRecreated()
{
    OnRenderTargetsRecreated();
}

void AdvancedPathTracer::prepareGaussianSplatPasses()
{
    for (auto& object : m_gaussianSplatSceneObjects)
    {
        if (object.pass != nullptr && object.pass->HasSplats())
            PrepareGaussianSplatPass(*object.pass);
    }
}

void AdvancedPathTracer::buildGaussianSplatEmissionProxyList()
{
    BuildGaussianSplatEmissionProxyList();
}

bool AdvancedPathTracer::isGaussianSplatEmissionEnabled() const
{
    return m_ui.EnableGaussianSplats
        && m_ui.GaussianSplatAsEmitter
        && m_ui.GaussianSplatEmissionIntensity > 0.0f
        && m_ui.GaussianSplatEmissionMaxProxyCount > 0;
}

bool AdvancedPathTracer::gaussianSplatObjectsEmpty() const
{
    return m_gaussianSplatSceneObjects.empty();
}

caustica::render::WorldRendererGaussianSplatBinding AdvancedPathTracer::getPrimaryGaussianSplatBinding() const
{
    caustica::render::WorldRendererGaussianSplatBinding binding;
    if (const auto* object = GetPrimaryGaussianSplatObject())
    {
        binding.pass = object->pass.get();
        binding.objectToWorld = GetGaussianSplatObjectToWorld(*object);
    }
    return binding;
}

void AdvancedPathTracer::renderSceneGaussianSplats(nvrhi::ICommandList* commandList,
    const caustica::PlanarView& splatView,
    RenderTargets& renderTargets,
    const GaussianSplatRenderSettings& settings,
    bool& renderedAny)
{
    for (auto& object : m_gaussianSplatSceneObjects)
    {
        if (object.splat == nullptr || !object.splat->enabled || object.pass == nullptr || !object.pass->HasSplats())
            continue;

        GaussianSplatRenderSettings objectSettings = settings;
        objectSettings.objectToWorld = GetGaussianSplatObjectToWorld(object);
        object.pass->Render(commandList, splatView, GetRenderCore()->accelStructs().getTopLevelAS().Get(), renderTargets, objectSettings);
        renderedAny = true;
    }
}

void AdvancedPathTracer::updateViews(nvrhi::IFramebuffer* framebuffer) { UpdateViews(framebuffer); }

void AdvancedPathTracer::recreateAccelStructs(nvrhi::ICommandList* commandList) { RecreateAccelStructs(commandList); }

void AdvancedPathTracer::uploadSubInstanceData(nvrhi::ICommandList* commandList) { UploadSubInstanceData(commandList); }

void AdvancedPathTracer::collectUncompressedTextures() { CollectUncompressedTextures(); }

dm::float2 AdvancedPathTracer::computeCameraJitter(uint frameIndex) { return ComputeCameraJitter(frameIndex); }

bool AdvancedPathTracer::consumeShaderReloadRequest()
{
    if (!m_editor.ShaderReloadRequested)
        return false;
    m_editor.ShaderReloadRequested = false;
    return true;
}

bool& AdvancedPathTracer::accelerationStructRebuildRequested() { return m_editor.AccelerationStructRebuildRequested; }

bool AdvancedPathTracer::hasActivePickRequest() const { return m_editor.hasActivePickRequest(); }

bool AdvancedPathTracer::showDeltaTree() const { return m_editor.ShowDeltaTree; }

bool AdvancedPathTracer::pickMaterialRequested() const { return m_editor.PickMaterialRequested; }

bool AdvancedPathTracer::pickInstanceRequested() const { return m_editor.PickInstanceRequested; }

void AdvancedPathTracer::clearPickRequests() { m_editor.clearPickRequests(); }

void AdvancedPathTracer::resolvePickFeedback(const DebugFeedbackStruct& feedback)
{
    if (m_editor.PickMaterialRequested)
        m_editor.SelectedMaterial = FindMaterial(int(feedback.pickedMaterialID));

    if (m_editor.PickInstanceRequested)
    {
        m_editor.SelectedNode = FindNodeByInstanceIndex(int(feedback.pickedInstanceIndex));
        if (m_editor.SelectedNode != nullptr)
            m_editor.SelectedGaussianSplat = false;
    }
}

bool AdvancedPathTracer::consumeExperimentalPhotoScreenshot()
{
    if (!m_editor.ExperimentalPhotoModeScreenshot)
        return false;
    m_editor.ExperimentalPhotoModeScreenshot = false;
    return true;
}

void AdvancedPathTracer::captureScriptPreRender()
{
    if (m_captureScriptManager)
        m_captureScriptManager->PreRender();
}

void AdvancedPathTracer::captureScriptPostRender(std::function<bool(const char* fileName)> saveTexture)
{
    if (m_captureScriptManager)
        m_captureScriptManager->PostRender(saveTexture);
}

ZoomTool* AdvancedPathTracer::getOrCreateZoomTool()
{
    return GetOrCreateZoomTool();
}
