#include <render/core/PostProcessAA.h>
#include <render/core/CameraController.h>
#include <render/core/RenderTargets.h>
#include <scene/View.h>
#include <render/passes/postProcess/PostProcess.h>
#include <backend/GpuDevice.h>
#include <core/scope.h>
#include <shaders/SampleConstantBuffer.h>

#if CAUSTICA_WITH_STREAMLINE
#include <backend/StreamlineInterface.h>
#endif

using namespace caustica::math;

namespace caustica
{

void postProcessAA(CameraController& camera, PostProcessAAParams& params)
{
    postProcessAAPlatform(camera, params);
}

void postProcessAAPlatform(CameraController& camera, PostProcessAAParams& params)
{
    auto& settings = params.settings;
    nvrhi::ICommandList* commandList = params.commandList;
    RenderTargets* renderTargets = params.renderTargets;

    if (!settings.RealtimeMode)
        return;

#if CAUSTICA_WITH_STREAMLINE
    const bool useStreamlineThisFrame = params.gpuDevice != nullptr
        && !params.gpuDevice->IsHeadless();
    if (useStreamlineThisFrame)
    {
        {
            affine3 viewReprojection = camera.view()->getChildView(ViewType::PLANAR, 0)->getInverseViewMatrix()
                * camera.viewPrevious()->getViewMatrix();
            float4x4 reprojectionMatrix = inverse(camera.view()->getProjectionMatrix(false))
                * affineToHomogeneous(viewReprojection)
                * camera.viewPrevious()->getProjectionMatrix(false);
            const float outputAspectRatio = params.displayAspectRatio;
            float4x4 projection = perspProjD3DStyleReverse(
                dm::radians(camera.verticalFOV()), outputAspectRatio, camera.zNear());

            StreamlineInterface::Constants slConstants = {};
            slConstants.cameraAspectRatio = outputAspectRatio;
            slConstants.cameraFOV = dm::radians(camera.verticalFOV());
            slConstants.cameraFar = camera.zFar();
            slConstants.cameraMotionIncluded = true;
            slConstants.cameraNear = camera.zNear();
            slConstants.cameraPinholeOffset = { 0.f, 0.f };
            slConstants.cameraPos = camera.camera().GetPosition();
            slConstants.cameraFwd = camera.camera().GetDir();
            slConstants.cameraUp = camera.camera().GetUp();
            slConstants.cameraRight = normalize(
                cross(camera.camera().GetDir(), camera.camera().GetUp()));
            slConstants.cameraViewToClip = projection;
            slConstants.clipToCameraView = inverse(projection);
            slConstants.clipToPrevClip = reprojectionMatrix;
            slConstants.depthInverted = camera.view()->isReverseDepth();
            slConstants.jitterOffset = params.cameraJitter;
            slConstants.mvecScale = { 1.0f / params.renderSize.x, 1.0f / params.renderSize.y };
            slConstants.prevClipToClip = inverse(reprojectionMatrix);
            slConstants.reset = params.reset;
            slConstants.motionVectors3D = false;
            slConstants.motionVectorsInvalidValue = FLT_MIN;

            params.gpuDevice->GetStreamline().SetConstants(slConstants);

            if (settings.RealtimeAA == 3 && params.dlssRROptions != nullptr)
            {
                params.dlssRROptions->worldToCameraView =
                    dm::affineToHomogeneous(camera.view()->getViewMatrix());
                params.dlssRROptions->cameraViewToWorld =
                    dm::affineToHomogeneous(camera.view()->getInverseViewMatrix());
                params.gpuDevice->GetStreamline().SetDLSSRROptions(*params.dlssRROptions);
            }
        }

        commandList->commitBarriers();

        params.gpuDevice->GetStreamline().TagResourcesGeneral(
            commandList,
            camera.view()->getChildView(ViewType::PLANAR, 0),
            renderTargets->screenMotionVectors,
            renderTargets->depth,
            renderTargets->preUIColor);

        params.gpuDevice->GetStreamline().TagResourcesDLSSNIS(
            commandList,
            camera.view()->getChildView(ViewType::PLANAR, 0),
            renderTargets->processedOutputColor,
            renderTargets->outputColor);

        if (settings.RealtimeAA == 2)
        {
            RAII_SCOPE(commandList->beginMarker("DLSS");, commandList->endMarker(););
            params.gpuDevice->GetStreamline().EvaluateDLSS(commandList);
            commandList->clearState();
        }
    }

    if (useStreamlineThisFrame && settings.RealtimeAA == 3)
    {
        RAII_SCOPE(commandList->beginMarker("DLSS-RR");, commandList->endMarker(););

        SampleMiniConstants miniConstants = { uint4(0, 0, 0, 0) };
        nvrhi::TextureDesc tdesc = renderTargets->outputColor->getDesc();
        commandList->beginMarker("DLSSRR_PrepareInputs");
        params.postProcess->Apply(
            commandList,
            PostProcess::ComputePassType::DLSSRRDenoiserPrepareInputs,
            params.constantBuffer,
            miniConstants,
            params.bindingSet,
            params.bindingLayout,
            tdesc.width,
            tdesc.height);
        commandList->endMarker();

        static bool useSpecHitT = false;
        params.gpuDevice->GetStreamline().TagResourcesDLSSRR(
            commandList,
            camera.view()->getChildView(ViewType::PLANAR, 0),
            (int2)params.renderSize,
            (int2)params.displaySize,
            renderTargets->outputColor,
            renderTargets->rrDiffuseAlbedo,
            renderTargets->rrSpecAlbedo,
            renderTargets->rrNormalsAndRoughness,
            nullptr,
            useSpecHitT ? renderTargets->specularHitT : nullptr,
            useSpecHitT ? nullptr : renderTargets->rrSpecMotionVectors,
            renderTargets->processedOutputColor);

        commandList->commitBarriers();
        params.gpuDevice->GetStreamline().EvaluateDLSSRR(commandList);
        commandList->clearState();
    }
    else if (!settings.ActualUseStandaloneDenoiser())
    {
        SampleMiniConstants miniConstants = { uint4(0, 0, 0, 0) };
        nvrhi::TextureDesc tdesc = renderTargets->outputColor->getDesc();
        commandList->beginMarker("NoDenoiserFinalMerge");
        params.postProcess->Apply(
            commandList,
            PostProcess::ComputePassType::NoDenoiserFinalMerge,
            params.constantBuffer,
            miniConstants,
            params.bindingSet,
            params.bindingLayout,
            tdesc.width,
            tdesc.height);
        commandList->endMarker();
    }
#endif
}

} // namespace caustica
