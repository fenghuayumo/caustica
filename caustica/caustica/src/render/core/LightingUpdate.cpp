#include <render/core/LightingUpdate.h>
#include <render/core/AccelStructManager.h>
#include <render/core/CameraController.h>
#include <render/core/BindingCache.h>
#include <render/passes/lighting/distant/EnvMapProcessor.h>
#include <render/passes/lighting/distant/EnvMapImportanceSamplingCache.h>
#include <render/passes/lighting/LightSamplingCache.h>
#include <render/passes/lighting/MaterialGpuCache.h>
#include <render/passes/omm/OpacityMicromapBuilder.h>
#include <render/passes/gaussian/GaussianSplatEmissionProxy.h>
#include <scene/SceneRenderData.h>
#include <scene/SceneLightAccess.h>
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

void preUpdateLighting(PreUpdateLightingParams& params)
{
    caustica::rhi::ICommandList* commandList = params.commandList;
    if (commandList == nullptr || params.environment == nullptr)
        return;

    RAII_SCOPE(commandList->beginMarker("PreUpdateLighting");, commandList->endMarker(););

    caustica::rhi::TextureHandle preUpdateCube = params.environment->getEnvMapCube();
    params.environment->preUpdate(
        commandList, params.renderDevice, params.envMapActualPath, params.sceneDirectory);

    if (preUpdateCube != params.environment->getEnvMapCube())
        params.needNewBindings = true;
}

void updateLighting(CameraController& camera, AccelStructManager& accelStructs, UpdateLightingParams& params)
{
    caustica::rhi::ICommandList* commandList = params.commandList;
    if (commandList == nullptr || params.environment == nullptr || params.lightSampling == nullptr
        || params.bindingCache == nullptr || params.sceneData == nullptr || !params.gpuHandles.valid())
    {
        return;
    }

    RAII_SCOPE(commandList->beginMarker("UpdateLighting");, commandList->endMarker(););

    EMB_DirectionalLight dirLights[EnvMapProcessor::c_MaxDirLights];
    uint32_t dirLightCount = 0;
    {
        const float3 rotationInRadians = radians(params.settings.EnvironmentMapParams.RotationXYZ);
        const affine3 rotationTransform = dm::rotation(rotationInRadians);
        for (const scene::LightRenderProxy& lightProxy : params.sceneData->lights)
        {
            if (!scene::tryGetDirectionalLightData(lightProxy.data))
                continue;

            LightConstants lightConstants;
            scene::fillLightConstants(lightProxy, lightConstants);

            if (dirLightCount >= EnvMapProcessor::c_MaxDirLights)
                break;

            const float minAngularSize = PI_f / (params.environment->getTargetCubeResolution() / 2.0f);
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

    if (params.environment->update(
            commandList,
            *params.bindingCache,
            params.renderDevice,
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
    settings.CameraPosition = camera.camera().getPosition();
    settings.CameraDirection = camera.camera().getDir();
    settings.ViewProjMatrix = camera.view()->getViewProjectionMatrix();
    settings.MouseCursorPos = params.settings.MousePos;
    settings.GlobalTemporalFeedbackWeight = params.settings.NEEAT_GlobalTemporalFeedbackWeight;
    settings.LocalToGlobalSampleRatio = params.settings.ActualNEEAT_LocalToGlobalSampleRatio();
    settings.UseApproximateMIS = params.settings.actualUseApproximateMIS();
    settings.DistantVsLocalImportanceScale = params.settings.NEEAT_Distant_vs_Local_Importance;
    settings.ResetFeedback = params.settings.ResetAccumulation && !params.settings.RealtimeMode
#if 1
        || params.settings.ResetRealtimeCaches
#endif
        ;
    settings.PrevViewportSize = float2(
        static_cast<float>(camera.viewPrevious()->getViewExtent().width()),
        static_cast<float>(camera.viewPrevious()->getViewExtent().height()));
    settings.ViewportSize = float2(
        static_cast<float>(camera.view()->getViewExtent().width()),
        static_cast<float>(camera.view()->getViewExtent().height()));
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

    params.lightSampling->updateBegin(
        commandList,
        *params.bindingCache,
        settings,
        params.sceneTime,
        params.sceneData,
        params.gpuHandles,
        params.bindlessDescriptorTable,
        params.materials,
        params.opacityMaps,
        accelStructs.getSubInstanceBuffer(),
        accelStructs.getSubInstanceData(),
        params.environment->getImportanceSampling()->getRadianceAndImportanceMap());
}

void syncEnvMapSceneParams(
    const PathTracerSettings& settings,
    EnvMapSceneParams& params,
    float envMapRadianceScale)
{
    if (settings.EnvironmentMapParams.enabled)
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

void updateLightingEnd(UpdateLightingEndParams& params)
{
    if (params.commandList == nullptr || params.lightSampling == nullptr || params.bindingCache == nullptr
        || !params.gpuHandles.valid())
    {
        return;
    }

    params.lightSampling->updateEnd(
        params.commandList,
        *params.bindingCache,
        params.gpuHandles,
        params.materials,
        params.opacityMaps,
        params.subInstanceDataBuffer,
        params.depthBuffer,
        params.motionVectors);
}

} // namespace caustica
