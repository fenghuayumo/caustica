#include <render/Core/PostProcessAA.h>
#include <render/Core/CameraController.h>
#include <render/Core/RenderTargets.h>
#include <scene/View.h>
#include <render/Passes/Geometry/TemporalAntiAliasingPass.h>
#include <render/Passes/PostProcess/AccumulationPass.h>
#include <render/Passes/PostProcess/PostProcess.h>
#include <backend/GpuDevice.h>
#include <core/scope.h>
#include <shaders/SampleConstantBuffer.h>

#if CAUSTICA_WITH_STREAMLINE
#include <backend/StreamlineInterface.h>
#endif

#include <algorithm>

using namespace caustica::math;

namespace caustica
{

void postProcessAA(CameraController& camera, PostProcessAAParams& params)
{
    auto& settings = params.settings;
    nvrhi::ICommandList* commandList = params.commandList;
    RenderTargets* renderTargets = params.renderTargets;

    if (settings.RealtimeMode)
    {
        if (settings.RealtimeAA == 0)
        {
            commandList->copyTexture(
                renderTargets->ProcessedOutputColor, nvrhi::TextureSlice(),
                renderTargets->OutputColor, nvrhi::TextureSlice());
        }
        else if (settings.RealtimeAA == 1 && params.temporalAAPass != nullptr)
        {
            const bool stochasticSplats = settings.EnableGaussianSplats && settings.GaussianSplatSortingMode == 1;
            const bool stochasticReset = stochasticSplats && params.gaussianSplatTemporalReset != nullptr
                && (params.reset || settings.ResetAccumulation || settings.ResetRealtimeCaches
                    || *params.gaussianSplatTemporalReset);
            if (stochasticReset && params.gaussianSplatTemporalSampleIndex != nullptr)
                *params.gaussianSplatTemporalSampleIndex = 0;

            const bool previousViewValid = !params.reset && !stochasticReset && (params.frameIndex != 0);
            render::TemporalAntiAliasingParameters taaParams = settings.TemporalAntiAliasingParams;
            if (stochasticSplats && params.gaussianSplatTemporalSampleIndex != nullptr)
            {
                taaParams.enableHistoryClamping = false;
                taaParams.useHistoryClampRelax = false;
                taaParams.newFrameWeight = 1.0f / float(*params.gaussianSplatTemporalSampleIndex + 1);
            }

            commandList->beginMarker("TAA");
            params.temporalAAPass->TemporalResolve(
                commandList, taaParams, previousViewValid, *camera.view(), *camera.view());
            commandList->endMarker();

            if (stochasticSplats && params.gaussianSplatTemporalSampleIndex != nullptr
                && params.gaussianSplatTemporalReset != nullptr)
            {
                *params.gaussianSplatTemporalSampleIndex =
                    std::min(*params.gaussianSplatTemporalSampleIndex + 1, 1024 * 1024);
                *params.gaussianSplatTemporalReset = false;
            }
        }

#if CAUSTICA_WITH_STREAMLINE
        const bool useStreamlineThisFrame = params.gpuDevice != nullptr
            && !params.gpuDevice->IsHeadless();
        if (useStreamlineThisFrame)
        {
            {
                affine3 viewReprojection = camera.view()->GetChildView(ViewType::PLANAR, 0)->GetInverseViewMatrix()
                    * camera.viewPrevious()->GetViewMatrix();
                float4x4 reprojectionMatrix = inverse(camera.view()->GetProjectionMatrix(false))
                    * affineToHomogeneous(viewReprojection)
                    * camera.viewPrevious()->GetProjectionMatrix(false);
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
                slConstants.depthInverted = camera.view()->IsReverseDepth();
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
                        dm::affineToHomogeneous(camera.view()->GetViewMatrix());
                    params.dlssRROptions->cameraViewToWorld =
                        dm::affineToHomogeneous(camera.view()->GetInverseViewMatrix());
                    params.gpuDevice->GetStreamline().SetDLSSRROptions(*params.dlssRROptions);
                }
            }

            commandList->commitBarriers();

            params.gpuDevice->GetStreamline().TagResourcesGeneral(
                commandList,
                camera.view()->GetChildView(ViewType::PLANAR, 0),
                renderTargets->ScreenMotionVectors,
                renderTargets->Depth,
                renderTargets->PreUIColor);

            params.gpuDevice->GetStreamline().TagResourcesDLSSNIS(
                commandList,
                camera.view()->GetChildView(ViewType::PLANAR, 0),
                renderTargets->ProcessedOutputColor,
                renderTargets->OutputColor);

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
            nvrhi::TextureDesc tdesc = renderTargets->OutputColor->getDesc();
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
                camera.view()->GetChildView(ViewType::PLANAR, 0),
                (int2)params.renderSize,
                (int2)params.displaySize,
                renderTargets->OutputColor,
                renderTargets->RRDiffuseAlbedo,
                renderTargets->RRSpecAlbedo,
                renderTargets->RRNormalsAndRoughness,
                nullptr,
                useSpecHitT ? renderTargets->SpecularHitT : nullptr,
                useSpecHitT ? nullptr : renderTargets->RRSpecMotionVectors,
                renderTargets->ProcessedOutputColor);

            commandList->commitBarriers();
            params.gpuDevice->GetStreamline().EvaluateDLSSRR(commandList);
            commandList->clearState();
        }
        else if (!settings.ActualUseStandaloneDenoiser())
        {
            SampleMiniConstants miniConstants = { uint4(0, 0, 0, 0) };
            nvrhi::TextureDesc tdesc = renderTargets->OutputColor->getDesc();
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
    else if (params.accumulationPass != nullptr)
    {
        const float accumulationWeight = (params.accumulationSampleIndex < settings.AccumulationTarget)
            ? (1.f / float(std::max(0, params.accumulationSampleIndex) + 1))
            : (0.0f);

        params.accumulationPass->Render(
            commandList, *camera.view(), *camera.view(), accumulationWeight);
    }
}

} // namespace caustica
