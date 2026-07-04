#include <engine/GpuRenderSubsystem.h>

#include <assets/AssetSystem.h>
#include <assets/loader/ShaderFactory.h>
#include <assets/loader/ShaderPackFileSystem.h>
#include <backend/GpuDevice.h>
#include <backend/ShaderUtils.h>
#include <core/command_line.h>
#include <core/log.h>
#include <core/path_utils.h>
#include <core/vfs/VFS.h>
#include <render/Core/BindingCache.h>
#include <render/Core/BindlessTable.h>
#include <render/Core/CommonRenderPasses.h>
#include <render/Core/RenderCore.h>
#include <render/Core/SceneGpuUpdater.h>
#include <render/PathTracerScenePasses.h>
#include <render/WorldRenderer/WorldRenderer.h>
#include <render/WorldRenderer/PathTracingContext.h>
#include <scene/Scene.h>
#include <scene/SceneEcs.h>
#include <scene/SceneManager.h>

namespace caustica
{

GpuRenderSubsystem::GpuRenderSubsystem() = default;

GpuRenderSubsystem::~GpuRenderSubsystem()
{
    shutdown();
}

void GpuRenderSubsystem::initialize(EngineInitContext& /*context*/)
{
}

void GpuRenderSubsystem::onRenderEnd(GpuDevice& /*gpuDevice*/)
{
    endFrame();
}

bool GpuRenderSubsystem::initializeSession(const GpuRenderSubsystemInitParams& params)
{
    GpuDevice& gpuDevice = params.gpuDevice;

    createShaderFactory(gpuDevice);

    auto* device = gpuDevice.GetDevice();
    m_bindlessLayout = render::WorldRenderer::CreateBindlessLayout(device);

    m_commonPasses = std::make_shared<CommonRenderPasses>(device, m_shaderFactory);
    m_bindingCache = std::make_unique<BindingCache>(device);
    m_bindlessTable = std::make_unique<BindlessTable>(device, m_bindlessLayout);
    m_descriptorTable = m_bindlessTable->GetDescriptorTableManager();

    auto nativeFS = std::make_shared<NativeFileSystem>();
    AssetSystem::Initialize(device, nativeFS, m_descriptorTable);
    m_textureCache = AssetSystem::Get().GetTextureLoader();
    AssetSystem::Get().EnableHotReload(true);
    AssetSystem::Get().WatchAssetDirectory("Assets");

    m_renderCore = std::make_unique<RenderCore>(device);
    m_renderCore->camera().camera().SetRotateSpeed(.003f);

    m_sceneManager = std::make_unique<SceneManager>(
        gpuDevice,
        *m_shaderFactory,
        m_textureCache,
        m_descriptorTable,
        params.sceneTypeFactory);

    m_sceneManager->setLoadingCallbacks(
        std::move(params.sceneCallbacks.OnSceneLoaded),
        std::move(params.sceneCallbacks.OnSceneUnloading));

    m_renderCore->initializeRenderPipeline(m_shaderFactory);

    m_gpuDevice = &params.gpuDevice;
    m_settings = &params.settings;
    m_runtimeState = &params.runtimeState;
    m_diagnostics = &params.diagnostics;
    m_sceneTime = &params.sceneTime;
    m_cmdLine = params.cmdLine;

    auto& lighting = m_scenePasses.lighting;
    auto& rayTracing = m_scenePasses.rayTracing;
    auto& gaussianSplats = m_scenePasses.gaussianSplats;

    m_pathTracingContext = std::make_unique<render::PathTracingContext>(render::PathTracingContext{
        .gpuDevice = params.gpuDevice,
        .sceneManager = *m_sceneManager,
        .renderCore = *m_renderCore,
        .settings = params.settings,
        .runtimeState = params.runtimeState,
        .rayTracing = rayTracing,
        .gaussianSplats = gaussianSplats,
        .shaderFactory = m_shaderFactory,
        .commonPasses = m_commonPasses,
        .bindingCache = *m_bindingCache,
        .textureCache = m_textureCache,
        .descriptorTable = m_descriptorTable,
        .environment = lighting.environment(),
        .lightSampling = lighting.lightSampling(),
        .materials = lighting.materials(),
        .opacityMaps = lighting.opacityMaps(),
        .computePipelines = lighting.computePipelines(),
        .envMapSceneParams = lighting.envMapSceneParams(),
        .envMapLocalPath = lighting.envMapLocalPath(),
        .envMapOverride = lighting.envMapOverride(),
        .sceneTime = params.sceneTime,
        .gaussianSplatEmissionProxies = gaussianSplats.emissionProxies(),
        .diagnostics = params.diagnostics,
    });

    m_worldRenderer = std::make_unique<render::WorldRenderer>(*m_pathTracingContext);
    m_worldRenderer->createBindingLayouts(m_bindlessLayout);

    m_scenePasses.wireSession(render::ScenePassWireParams{
        .gpuDevice = params.gpuDevice,
        .sceneManager = *m_sceneManager,
        .renderCore = *m_renderCore,
        .worldRenderer = *m_worldRenderer,
        .settings = params.settings,
        .invalidation = params.runtimeState.Invalidation,
        .gaussianSplatsSummary = params.runtimeState.GaussianSplats,
        .lighting = m_scenePasses.lighting,
        .bindingCache = *m_bindingCache,
        .shaderFactory = m_shaderFactory,
        .commonPasses = m_commonPasses,
    });
    return true;
}

void GpuRenderSubsystem::endFrame()
{
    if (m_bindlessTable)
        m_bindlessTable->FlushDeferredFrees();
}

void GpuRenderSubsystem::onSceneUnloading()
{
    if (m_worldRenderer)
        m_worldRenderer->onSceneUnloading();
    if (m_renderCore)
        m_renderCore->onSceneUnloading();
    if (m_bindingCache)
        m_bindingCache->Clear();
    m_scenePasses.lighting.sceneUnloading();
    m_scenePasses.gaussianSplats.sceneUnloading();
}

void GpuRenderSubsystem::refreshEnvironmentMapMediaList(const std::filesystem::path& assetsRoot,
    const std::filesystem::path& scenePath)
{
    m_scenePasses.lighting.refreshEnvironmentMapMediaList(assetsRoot, scenePath);
}

void GpuRenderSubsystem::applySampleSettingsFromScene()
{
    if (!m_settings || !m_sceneManager || !m_renderCore)
        return;

    const auto scene = m_sceneManager->getScene();
    if (!scene)
        return;

    if (std::shared_ptr<SampleSettings> sampleSettings = scene->GetSampleSettingsNode())
    {
        m_settings->RealtimeMode = sampleSettings->realtimeMode.value_or(m_settings->RealtimeMode);
        m_settings->EnableAnimations = sampleSettings->enableAnimations.value_or(m_settings->EnableAnimations);
        if (sampleSettings->startingCamera.has_value())
            m_renderCore->camera().setSelectedCameraIndex(sampleSettings->startingCamera.value() + 1);
        if (sampleSettings->realtimeFireflyFilter.has_value())
        {
            m_settings->RealtimeFireflyFilterThreshold = sampleSettings->realtimeFireflyFilter.value();
            m_settings->RealtimeFireflyFilterEnabled = true;
        }
        m_settings->BounceCount = sampleSettings->maxBounces.value_or(m_settings->BounceCount);
        m_settings->DiffuseBounceCount = sampleSettings->maxDiffuseBounces.value_or(m_settings->DiffuseBounceCount);
        m_settings->TexLODBias = sampleSettings->textureMIPBias.value_or(m_settings->TexLODBias);
    }
}

void GpuRenderSubsystem::onSceneLoadedBegin()
{
    if (!m_settings || !m_sceneTime)
        return;

    *m_sceneTime = 0.0;
    m_settings->EnableAnimations = false;
    m_settings->RealtimeMode = false;

    applySampleSettingsFromScene();

    if (m_cmdLine && m_cmdLine->stopAnimations)
        m_settings->EnableAnimations = false;

    if (m_cmdLine)
    {
        if (m_cmdLine->OverrideToRealtimeMode)
            m_settings->RealtimeMode = true;
        if (m_cmdLine->OverrideToReferenceMode)
            m_settings->RealtimeMode = false;
    }

    m_settings->ToneMappingParams.exposureCompensation = 2.0f;
    m_settings->ToneMappingParams.exposureValue = 0.0f;
}

void GpuRenderSubsystem::onSceneLoadedGpuPrep()
{
    if (m_worldRenderer)
        m_worldRenderer->onSceneLoaded();

    if (m_textureCache && m_commonPasses)
    {
        m_textureCache->ProcessRenderingThreadCommands(*m_commonPasses, 0.f);
        m_textureCache->LoadingFinished();
    }

    if (m_sceneManager)
    {
        if (auto scene = m_sceneManager->getScene())
        {
            if (auto* entityWorld = scene->GetEntityWorld())
            {
                entityWorld->refreshHierarchy(scene::PreviousTransformPolicy::CaptureCurrent);
                entityWorld->syncPreviousTransformsFromCurrent();
            }
            render::SceneGpuUpdater::RefreshAfterLoad(*scene, 0);
            m_scenePasses.lighting.notifySceneReloaded(*scene);
        }
    }
}

void GpuRenderSubsystem::onSceneLoadedGpuFinish()
{
    if (!m_sceneManager || !m_settings || !m_renderCore || !m_runtimeState)
        return;

    const auto scene = m_sceneManager->getScene();
    if (!scene)
        return;

    if (m_cmdLine)
        m_scenePasses.gaussianSplats.onSceneLoaded(*m_cmdLine);

    m_scenePasses.lighting.onSceneLoaded(*scene, *m_settings);

    m_renderCore->onSceneLoaded(*scene, m_runtimeState->Invalidation.AccelerationStructRebuildRequested);
    m_runtimeState->Invalidation.ShaderReloadRequested = true;

    m_settings->MaterialVariantIndex = 0;

    if (m_diagnostics)
        m_diagnostics->asyncLoadingInProgress = true;
}

void GpuRenderSubsystem::applyCmdLinePostLoadOverrides()
{
    if (!m_settings || !m_cmdLine)
        return;

    if (m_cmdLine->OverrideAutoexposureOff)
    {
        m_settings->ToneMappingParams.autoExposure = false;
        m_settings->ToneMappingParams.exposureValue = 0.0f;
    }
    if (m_cmdLine->OverrideExposureOffset != FLT_MAX)
        m_settings->ToneMappingParams.exposureCompensation = m_cmdLine->OverrideExposureOffset;
    if (m_cmdLine->DisableFireflyFilters)
    {
        m_settings->RealtimeFireflyFilterEnabled = false;
        m_settings->ReferenceFireflyFilterEnabled = false;
    }
    if (m_cmdLine->DisablePostProcessFilters)
        m_settings->EnableBloom = false;
}

void GpuRenderSubsystem::shutdown()
{
    m_worldRenderer.reset();
    m_pathTracingContext.reset();
    m_sceneManager.reset();
    m_renderCore.reset();

    m_gpuDevice = nullptr;
    m_settings = nullptr;
    m_runtimeState = nullptr;
    m_diagnostics = nullptr;
    m_sceneTime = nullptr;
    m_cmdLine = nullptr;

    m_textureCache.reset();
    m_descriptorTable.reset();
    m_bindlessTable.reset();
    m_bindingCache.reset();
    m_commonPasses.reset();
    m_shaderFactory.reset();
    m_bindlessLayout = nullptr;

    AssetSystem::Shutdown();
}

void GpuRenderSubsystem::createShaderFactory(GpuDevice& gpuDevice)
{
    const char* shaderTypeName = GetShaderTypeName(gpuDevice.GetGraphicsAPI());
    const std::filesystem::path appDirectory = GetRuntimeDirectory();
    const std::filesystem::path engineShaderPath = appDirectory / "ShaderPrecompiled/engine" / shaderTypeName;
    const std::filesystem::path appShaderPath = appDirectory / "ShaderPrecompiled/caustica" / shaderTypeName;
    const std::filesystem::path nrdShaderPath = appDirectory / "ShaderPrecompiled/nrd" / shaderTypeName;
    const std::filesystem::path ommShaderPath = appDirectory / "ShaderPrecompiled/omm" / shaderTypeName;

    std::shared_ptr<RootFileSystem> rootFS = std::make_shared<RootFileSystem>();
    const std::filesystem::path shaderPackPath = appDirectory / (std::string("caustica.shaders.") + shaderTypeName + ".pack");
    auto shaderPackFS = std::make_shared<ShaderPackFileSystem>(shaderPackPath, "ShaderPrecompiled");
    const bool shaderPackHasCurrentLayout = shaderPackFS->isOpen()
        && shaderPackFS->fileExists("caustica/caustica/shaders/render/Misc/DebugLines_main_vs.bin");

    if (shaderPackFS->isOpen() && !shaderPackHasCurrentLayout)
    {
        warning("Shader pack '%s' does not match the current shader layout; falling back to ShaderPrecompiled directories",
            shaderPackPath.string().c_str());
    }

    if (shaderPackHasCurrentLayout)
    {
        rootFS->mount("/ShaderPrecompiled", shaderPackFS);
    }
    else
    {
        rootFS->mount("/ShaderPrecompiled/engine", engineShaderPath);
        rootFS->mount("/ShaderPrecompiled/caustica", appShaderPath);
        rootFS->mount("/ShaderPrecompiled/nrd", nrdShaderPath);
        rootFS->mount("/ShaderPrecompiled/omm", ommShaderPath);
    }

    m_shaderFactory = std::make_shared<ShaderFactory>(gpuDevice.GetDevice(), rootFS, "/ShaderPrecompiled");
}

} // namespace caustica
