#include <engine/EngineRenderer.h>

#include <assets/AssetSystem.h>
#include <assets/loader/ShaderFactory.h>
#include <assets/loader/ShaderPackFileSystem.h>
#include <backend/GpuDevice.h>
#include <backend/ShaderUtils.h>
#include <core/log.h>
#include <core/path_utils.h>
#include <core/vfs/VFS.h>
#include <render/Core/BindingCache.h>
#include <render/Core/BindlessTable.h>
#include <render/Core/CommonRenderPasses.h>
#include <render/Core/RenderCore.h>
#include <render/SceneGaussianSplatPasses.h>
#include <render/SceneLightingPasses.h>
#include <render/SceneRayTracingResources.h>
#include <render/WorldRenderer/PathTracingWorldRenderer.h>
#include <render/WorldRenderer/PathTracingContext.h>
#include <scene/SceneManager.h>

namespace caustica
{

EngineRenderer::EngineRenderer() = default;

EngineRenderer::~EngineRenderer()
{
    shutdown();
}

bool EngineRenderer::initialize(GpuDevice& gpuDevice,
    std::shared_ptr<SceneTypeFactory> sceneTypeFactory,
    EngineSceneCallbacks sceneCallbacks)
{
    createShaderFactory(gpuDevice);

    auto* device = gpuDevice.GetDevice();
    m_bindlessLayout = render::PathTracingWorldRenderer::CreateBindlessLayout(device);

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
        std::move(sceneTypeFactory));

    m_sceneManager->setLoadingCallbacks(
        std::move(sceneCallbacks.OnSceneLoaded),
        std::move(sceneCallbacks.OnSceneUnloading));

    m_renderCore->initializeRenderPipeline(m_shaderFactory);
    return true;
}

void EngineRenderer::createPathTracer(const PathTracerSessionParams& session)
{
    auto& lighting = m_scenePasses.lighting;
    auto& rayTracing = m_scenePasses.rayTracing;
    auto& gaussianSplats = m_scenePasses.gaussianSplats;

    m_pathTracingContext = std::make_unique<render::PathTracingContext>(render::PathTracingContext{
        .gpuDevice = session.gpuDevice,
        .sceneManager = *m_sceneManager,
        .renderCore = *m_renderCore,
        .settings = session.settings,
        .runtimeState = session.runtimeState,
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
        .sceneTime = session.sceneTime,
        .gaussianSplatEmissionProxies = gaussianSplats.emissionProxies(),
        .diagnostics = session.diagnostics,
        .frameExtensions = session.frameExtensions,
    });

    m_worldRenderer = std::make_unique<render::PathTracingWorldRenderer>(*m_pathTracingContext);
    m_worldRenderer->createBindingLayouts(m_bindlessLayout);

    attachScenePasses(session);
}

void EngineRenderer::attachScenePasses(const PathTracerSessionParams& session)
{
    assert(m_worldRenderer != nullptr);

    auto& lighting = m_scenePasses.lighting;
    auto& rayTracing = m_scenePasses.rayTracing;
    auto& gaussianSplats = m_scenePasses.gaussianSplats;

    rayTracing.attach(
        session.gpuDevice,
        *m_sceneManager,
        *m_renderCore,
        *m_worldRenderer,
        session.settings,
        session.runtimeState.Invalidation,
        lighting,
        *m_bindingCache);

    gaussianSplats.attach(
        session.gpuDevice,
        *m_sceneManager,
        *m_renderCore,
        *m_worldRenderer,
        session.settings,
        session.runtimeState.GaussianSplats,
        m_shaderFactory,
        m_commonPasses);

    gaussianSplats.setOnRequestFullRebuild(
        [&rayTracing]() { rayTracing.requestFullRebuild(); });

    rayTracing.setAdditionalAccelStructBuilder(
        [&gaussianSplats](nvrhi::ICommandList* commandList) {
            gaussianSplats.buildAccelStructs(commandList);
        });
}

void EngineRenderer::endFrame()
{
    if (m_bindlessTable)
        m_bindlessTable->FlushDeferredFrees();
}

void EngineRenderer::shutdown()
{
    m_worldRenderer.reset();
    m_pathTracingContext.reset();
    m_sceneManager.reset();
    m_renderCore.reset();

    m_textureCache.reset();
    m_descriptorTable.reset();
    m_bindlessTable.reset();
    m_bindingCache.reset();
    m_commonPasses.reset();
    m_shaderFactory.reset();
    m_bindlessLayout = nullptr;

    AssetSystem::Shutdown();
}

void EngineRenderer::createShaderFactory(GpuDevice& gpuDevice)
{
    const char* shaderTypeName = GetShaderTypeName(gpuDevice.GetGraphicsAPI());
    const std::filesystem::path appDirectory = GetDirectoryWithExecutable();
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
