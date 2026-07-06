#include <cassert>
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
#include <render/core/BindingCache.h>
#include <render/core/BindlessTable.h>
#include <render/core/SceneGpuUpdater.h>
#include <render/core/RenderDevice.h>
#include <render/PathTracerScenePasses.h>
#include <render/worldRenderer/WorldRenderer.h>
#include <render/worldRenderer/PathTracingContext.h>
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

bool GpuRenderSubsystem::initializeSession(const GpuRenderSubsystemInitParams& params)
{
    GpuDevice& gpuDevice = params.gpuDevice;

    createShaderFactory(gpuDevice);

    auto* device = gpuDevice.GetDevice();
    m_bindlessLayout = render::WorldRenderer::CreateBindlessLayout(device);

    m_renderDevice = std::make_unique<caustica::render::RenderDevice>(device, m_shaderFactory);
    m_bindingCache = std::make_unique<BindingCache>(device);
    m_bindlessTable = std::make_unique<BindlessTable>(device, m_bindlessLayout);
    m_descriptorTable = m_bindlessTable->getDescriptorTableManager();

    auto nativeFS = std::make_shared<NativeFileSystem>();
    AssetSystem::initialize(device, nativeFS, m_descriptorTable);
    m_textureCache = AssetSystem::get().getTextureLoader();

    m_accelStructs = AccelStructManager(device);
    m_camera.camera().SetRotateSpeed(.003f);

    m_sceneManager = std::make_unique<SceneManager>(
        gpuDevice,
        *m_shaderFactory,
        m_textureCache,
        m_descriptorTable,
        params.sceneTypeFactory);

    m_sceneManager->setLoadingCallbacks(
        std::move(params.sceneCallbacks.OnSceneLoaded),
        std::move(params.sceneCallbacks.OnSceneUnloading));

    m_gpuDevice = &params.gpuDevice;
    m_settings = &params.settings;
    m_runtimeState = &params.runtimeState;
    m_diagnostics = &params.diagnostics;
    m_sceneTime = &params.sceneTime;
    m_cmdLine = params.cmdLine;

    m_pathTracingContext = std::make_unique<render::PathTracingContext>(render::PathTracingContext{
        .gpuDevice = params.gpuDevice,
        .sceneManager = *m_sceneManager,
        .camera = m_camera,
        .accelStructs = m_accelStructs,
        .settings = params.settings,
        .runtimeState = params.runtimeState,
        .scenePasses = m_scenePasses,
        .shaderFactory = m_shaderFactory,
        .renderDevice = *m_renderDevice,
        .bindingCache = *m_bindingCache,
        .textureCache = m_textureCache,
        .descriptorTable = m_descriptorTable,
        .sceneTime = params.sceneTime,
        .diagnostics = params.diagnostics,
    });

    m_worldRenderer = std::make_unique<render::WorldRenderer>(*m_pathTracingContext);
    m_worldRenderer->createBindingLayouts(m_bindlessLayout);

    m_scenePasses.wireSession(render::ScenePassWireParams{
        .gpuDevice = params.gpuDevice,
        .sceneManager = *m_sceneManager,
        .accelStructs = m_accelStructs,
        .worldRenderer = *m_worldRenderer,
        .settings = params.settings,
        .invalidation = params.runtimeState.Invalidation,
        .gaussianSplatsSummary = params.runtimeState.GaussianSplats,
        .lighting = m_scenePasses.lighting,
        .bindingCache = *m_bindingCache,
        .shaderFactory = m_shaderFactory,
        .renderDevice = *m_renderDevice,
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
    m_accelStructs.releaseGpuResources();
    if (m_bindingCache)
        m_bindingCache->clear();
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
    if (!m_settings || !m_sceneManager)
        return;

    const auto scene = m_sceneManager->getScene();
    if (!scene)
        return;

    if (std::shared_ptr<SampleSettings> sampleSettings = scene->GetSampleSettingsNode())
    {
        m_settings->RealtimeMode = sampleSettings->realtimeMode.value_or(m_settings->RealtimeMode);
        m_settings->EnableAnimations = sampleSettings->enableAnimations.value_or(m_settings->EnableAnimations);
        if (sampleSettings->startingCamera.has_value())
            m_camera.setSelectedCameraIndex(sampleSettings->startingCamera.value() + 1);
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

    if (m_textureCache && m_renderDevice)
    {
        AssetSystem::get().processRenderingThreadCommands(*m_renderDevice, 0.f);
        AssetSystem::get().loadingFinished();
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
            render::SceneGpuUpdater::refreshAfterLoad(*scene, 0);
            m_scenePasses.lighting.notifySceneReloaded(*scene);
        }
    }
}

void GpuRenderSubsystem::onSceneLoadedGpuFinish()
{
    if (!m_sceneManager || !m_settings || !m_runtimeState)
        return;

    const auto scene = m_sceneManager->getScene();
    if (!scene)
        return;

    if (m_cmdLine)
        m_scenePasses.gaussianSplats.onSceneLoaded(*m_cmdLine);

    m_scenePasses.lighting.onSceneLoaded(*scene, *m_settings);

    SceneManager::onSceneLoadedGpuPrep(*scene, m_runtimeState->Invalidation.AccelerationStructRebuildRequested);
    m_accelStructs.resetSubInstanceCount();
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
    if (m_shutdown)
        return;

    m_shutdown = true;

    m_worldRenderer.reset();
    m_pathTracingContext.reset();
    m_sceneManager.reset();
    m_accelStructs = AccelStructManager{};

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
    m_renderDevice.reset();
    m_shaderFactory.reset();
    m_bindlessLayout = nullptr;

    AssetSystem::shutdown();
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
        && shaderPackFS->fileExists("caustica/caustica/shaders/render/misc/DebugLines_main_vs.bin")
        && shaderPackFS->fileExists("engine/fullscreen_vs.bin");

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

caustica::render::RenderDevice& GpuRenderSubsystem::renderDevice()
{
    assert(m_renderDevice != nullptr);
    return *m_renderDevice;
}

const caustica::render::RenderDevice& GpuRenderSubsystem::renderDevice() const
{
    assert(m_renderDevice != nullptr);
    return *m_renderDevice;
}

} // namespace caustica
