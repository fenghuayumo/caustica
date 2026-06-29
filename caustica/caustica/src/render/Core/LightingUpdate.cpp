#include <render/Core/LightingUpdate.h>
#include <render/Core/RenderCore.h>
#include <render/Core/BindingCache.h>
#include <render/Core/CommonRenderPasses.h>
#include <render/Passes/Lighting/Distant/EnvMapProcessor.h>
#include <render/Passes/Lighting/Distant/EnvMapImportanceSamplingCache.h>
#include <render/Passes/Lighting/LightSamplingCache.h>
#include <render/Passes/Lighting/MaterialGpuCache.h>
#include <render/Passes/OMM/OpacityMicromapBuilder.h>
#include <render/Passes/Gaussian/GaussianSplatEmissionProxy.h>
#include <scene/Scene.h>
#include <scene/SceneGraph.h>
#include <core/scope.h>
#include <shaders/light_cb.h>
#include <shaders/SampleConstantBuffer.h>

using namespace caustica::math;

namespace caustica
{

namespace
{

bool gaussianSplatEmissionEnabled(const PathTracerSettings& settings)
{
    return settings.EnableGaussianSplats
        && settings.GaussianSplatAsEmitter
        && settings.GaussianSplatEmissionIntensity > 0.0f
        && settings.GaussianSplatEmissionMaxProxyCount > 0;
}

} // namespace

void RenderCore::preUpdateLighting(PreUpdateLightingParams& params)
{
    nvrhi::ICommandList* commandList = params.commandList;
    if (commandList == nullptr || params.environment == nullptr)
        return;

    RAII_SCOPE(commandList->beginMarker("PreUpdateLighting");, commandList->endMarker(););

    nvrhi::TextureHandle preUpdateCube = params.environment->GetEnvMapCube();
    params.environment->PreUpdate(
        commandList, params.commonPasses, params.envMapActualPath, params.sceneDirectory);

    if (preUpdateCube != params.environment->GetEnvMapCube())
        params.needNewBindings = true;
}

void RenderCore::updateLighting(UpdateLightingParams& params)
{
    nvrhi::ICommandList* commandList = params.commandList;
    if (commandList == nullptr || params.environment == nullptr || params.lightSampling == nullptr
        || params.bindingCache == nullptr || params.lights == nullptr || !params.scene)
    {
        return;
    }

    RAII_SCOPE(commandList->beginMarker("UpdateLighting");, commandList->endMarker(););

    EMB_DirectionalLight dirLights[EnvMapProcessor::c_MaxDirLights];
    uint32_t dirLightCount = 0;
    {
        const float3 rotationInRadians = radians(params.settings.EnvironmentMapParams.RotationXYZ);
        const affine3 rotationTransform = dm::rotation(rotationInRadians);
        for (const auto& light : *params.lights)
        {
            std::shared_ptr<DirectionalLight> dirLight = std::dynamic_pointer_cast<DirectionalLight>(light);
            if (dirLight == nullptr)
                continue;

            LightConstants lightConstants;
            dirLight->FillLightConstants(lightConstants);

            const float minAngularSize = PI_f / (params.environment->GetTargetCubeResolution() / 2.0f);
            assert(lightConstants.angularSizeOrInvRange >= minAngularSize);

            dirLights[dirLightCount].AngularSize =
                std::max(lightConstants.angularSizeOrInvRange, minAngularSize);
            dirLights[dirLightCount].ColorIntensity =
                float4(lightConstants.color, lightConstants.intensity);
            dirLights[dirLightCount].Direction =
                rotationTransform.transformVector(lightConstants.direction);
            dirLightCount++;
        }
    }

    if (params.environment->Update(
            commandList,
            *params.bindingCache,
            params.commonPasses,
            EnvMapProcessor::UpdateSettings{ .EnvMapRadianceScale = params.envMapRadianceScale },
            params.sceneTime,
            dirLights,
            dirLightCount,
            !params.settings.RealtimeMode || !params.settings.EnableAnimations))
    {
        params.settings.ResetAccumulation = true;
    }

    LightSamplingCache::UpdateSettings settings;
    settings.ImportanceSamplingType = static_cast<uint>(params.settings.NEEType);
    settings.CameraPosition = m_camera.camera().GetPosition();
    settings.CameraDirection = m_camera.camera().GetDir();
    settings.ViewProjMatrix = m_camera.view()->GetViewProjectionMatrix();
    settings.MouseCursorPos = params.settings.MousePos;
    settings.GlobalTemporalFeedbackWeight = params.settings.NEEAT_GlobalTemporalFeedbackWeight;
    settings.LocalToGlobalSampleRatio = params.settings.ActualNEEAT_LocalToGlobalSampleRatio();
    settings.UseApproximateMIS = params.settings.ActualUseApproximateMIS();
    settings.DistantVsLocalImportanceScale = params.settings.NEEAT_Distant_vs_Local_Importance;
    settings.ResetFeedback = params.settings.ResetAccumulation && !params.settings.RealtimeMode
#if 1
        || params.settings.ResetRealtimeCaches
#endif
        ;
    settings.PrevViewportSize = float2(
        static_cast<float>(m_camera.viewPrevious()->GetViewExtent().width()),
        static_cast<float>(m_camera.viewPrevious()->GetViewExtent().height()));
    settings.ViewportSize = float2(
        static_cast<float>(m_camera.view()->GetViewExtent().width()),
        static_cast<float>(m_camera.view()->GetViewExtent().height()));
    settings.EnvMapParams = LightSamplingCacheEnvMapParams{
        .Transform = params.envMapSceneParams.Transform,
        .InvTransform = params.envMapSceneParams.InvTransform,
        .ColorMultiplier = params.envMapSceneParams.ColorMultiplier,
        .Enabled = params.envMapSceneParams.Enabled,
    };
    settings.FrameIndex = params.frameIndex;

    if (params.gaussianSplatEmissionProxies != nullptr
        && !params.gaussianSplatEmissionProxies->empty()
        && gaussianSplatEmissionEnabled(params.settings))
    {
        settings.GaussianSplatEmissionProxies = params.gaussianSplatEmissionProxies;
        settings.GaussianSplatEmissionObjectToWorld = float4x4::identity();
        settings.GaussianSplatEmissionIntensity = params.settings.GaussianSplatEmissionIntensity;
    }

    params.lightSampling->UpdateBegin(
        commandList,
        *params.bindingCache,
        settings,
        params.sceneTime,
        params.scene,
        params.materials,
        params.opacityMaps,
        m_accelStructs.getSubInstanceBuffer(),
        m_accelStructs.getSubInstanceData(),
        params.environment->GetImportanceSampling()->GetRadianceAndImportanceMap());
}

void syncEnvMapSceneParams(
    const PathTracerSettings& settings,
    EnvMapSceneParams& params,
    float envMapRadianceScale)
{
    if (settings.EnvironmentMapParams.Enabled)
    {
        const float intensity = settings.EnvironmentMapParams.Intensity / envMapRadianceScale;
        params.ColorMultiplier = settings.EnvironmentMapParams.TintColor * intensity;

        const float3 rotationInRadians = radians(settings.EnvironmentMapParams.RotationXYZ);
        const affine3 rotationTransform = dm::rotation(rotationInRadians);
        const affine3 inverseTransform = inverse(rotationTransform);
        affineToColumnMajor(rotationTransform, params.Transform);
        affineToColumnMajor(inverseTransform, params.InvTransform);
        params.Enabled = 1;
    }
    else
    {
        params.ColorMultiplier = { 0, 0, 0 };
        params.Enabled = 0;
    }
}

void RenderCore::updateLightingEnd(UpdateLightingEndParams& params)
{
    if (params.commandList == nullptr || params.lightSampling == nullptr || params.bindingCache == nullptr
        || !params.scene)
    {
        return;
    }

    params.lightSampling->UpdateEnd(
        params.commandList,
        *params.bindingCache,
        params.scene,
        params.materials,
        params.opacityMaps,
        params.subInstanceDataBuffer,
        params.depthBuffer,
        params.motionVectors);
}

} // namespace caustica
