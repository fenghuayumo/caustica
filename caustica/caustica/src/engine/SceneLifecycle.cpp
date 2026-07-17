#include <engine/App.h>
#include <engine/GpuRenderSubsystem.h>
#include <engine/GpuSharedCaches.h>
#include <engine/AppResources.h>
#include <engine/SceneViewState.h>
#include <cassert>
#include <engine/SceneLifecycle.h>
#include <engine/SceneQuery.h>
#include <engine/CameraApi.h>
#include <engine/RenderSessionApi.h>
#include <engine/SceneApiInternal.h>
#include <engine/RenderThread.h>
#include <engine/SceneAccess.h>
#include <backend/GpuDevice.h>
#include <core/log.h>
#include <core/path_utils.h>
#include <core/vfs/VFS.h>
#include <scene/SceneManager.h>
#include <scene/scene_utils.h>
#include <scene/SceneCameraAccess.h>
#include <scene/SceneEcs.h>
#include <scene/SceneRenderExtract.h>
#include <render/core/PathTracerSettings.h>
#include <render/worldRenderer/WorldRenderer.h>
#include <render/core/CameraController.h>
#include <render/SceneLightingPasses.h>
#include <assets/loader/TextureLoader.h>
#include <algorithm>
#include <cctype>


namespace
{
    bool LooksLikeInlineSceneJson(const std::string& scene)
    {
        auto it = std::find_if_not(scene.begin(), scene.end(), [](unsigned char ch) {
            return std::isspace(ch);
        });
        return it != scene.end() && *it == '{';
    }

    void initViewState(SceneViewState& viewState)
    {
        if (!viewState.progressLoading.Active())
        {
            viewState.progressLoading.start("Initializing...");
            viewState.progressLoading.Set(50);
        }
    }

    void syncCameraFromScene(App& app)
    {
        auto scenePtr = caustica::activeScene(app);
        CameraController* cam = detail::sessionCamera(app);
        if (!scenePtr || !cam)
            return;

        const auto& cameraEntities = scenePtr->getCameraEntities();
        const auto* ew = scenePtr->getEntityWorld();
        bool syncedCamera = false;
        if (!cameraEntities.empty() && ew)
        {
            const uint32_t selectedIndex = cam->selectedCameraIndex();
            const uint32_t camIdx = (selectedIndex > 0) ? (selectedIndex - 1)
                : static_cast<uint32_t>(cameraEntities.size() - 1);
            if (camIdx < cameraEntities.size())
            {
                ecs::Entity camEntity = cameraEntities[camIdx];
                const auto* camComp = scene::tryGetCamera(ew->world(), camEntity);
                const auto* globalComp = ew->world().get<scene::GlobalTransformComponent>(camEntity);
                if (camComp && globalComp)
                {
                    const scene::CameraRenderProxy proxy =
                        scene::makeCameraRenderProxy(camEntity, *camComp, *globalComp);
                    PathTracerSettings* settingsPtr = caustica::settings(app);
                    scene::applyCameraRenderProxyToController(proxy, *cam, settingsPtr);
                    if (settingsPtr)
                        settingsPtr->ResetAccumulation = true;
                    if (auto* wr = caustica::worldRenderer(app))
                        wr->setGaussianSplatTemporalReset(true);
                    syncedCamera = proxy.projection == scene::CameraProjectionKind::Perspective;
                }
            }
        }
        if (!syncedCamera)
            cam->setupDefaultCamera();
    }
}

using namespace caustica::render;

namespace caustica
{

void bindSessionCameraSideEffects(App& app)
{
    PathTracerSettings* cfg = settings(app);
    CameraController* cam = detail::sessionCamera(app);
    assert(cfg);
    assert(cam);
    cam->bindSideEffects(*cfg, worldRenderer(app));
}

void initStreamlineAndWindow(App& app)
{
#if CAUSTICA_WITH_STREAMLINE
    PathTracerSettings* cfg = settings(app);
    GpuDevice* device = gpuDevice(app);
    if (!cfg || !device || device->isHeadless())
        return;

    cfg->IsDLSSSuported = device->getStreamline().isDLSSAvailable();
    cfg->IsDLSSFGSupported = device->getStreamline().isDLSSGAvailable();
    cfg->IsReflexSupported = device->getStreamline().isReflexAvailable();
    cfg->IsDLSSRRSupported = device->getStreamline().isDLSSRRAvailable();
#endif
}

void initializeScene(App& app, const std::string& preferredScene)
{
    if (SceneViewState* vs = viewState(app))
        initViewState(*vs);

    auto* wr = worldRenderer(app);
    if (!wr)
    {
        caustica::fatal("caustica::initializeScene requires a path tracer world renderer");
        return;
    }

    GpuSharedCaches* caches = gpuSharedCaches(app);
    render::WorldRenderer* wrResource = worldRenderer(app);
    if (!caches || !caches->shaderFactory || !caches->descriptorTable || !caches->textureLoader
        || !detail::sessionCamera(app) || !wrResource)
    {
        caustica::fatal("caustica::initializeScene requires GpuSharedCaches / SessionCamera / WorldRenderer wiring");
        return;
    }
    const auto shaderFactory = caches->shaderFactory;
    const auto descriptorTable = caches->descriptorTable;
    const auto textureLoader = caches->textureLoader;

    if (!wr->getRenderTargets())
        wr->createDeviceResources();

    ::SceneManager* manager = detail::sessionManager(app);
    if (!manager)
    {
        caustica::fatal("caustica::initializeScene requires scene manager");
        return;
    }

    PathTracerSettings* cfg = settings(app);
    assert(cfg);

    cfg->EnableGaussianSplats = true;

    GpuDevice* device = gpuDevice(app);
    if (device && device->getDevice()->queryFeatureSupport(nvrhi::Feature::RayTracingOpacityMicromap))
    {
        wrResource->lightingPasses().createOpacityMapsIfSupported(
            device->getDevice(), descriptorTable, textureLoader, shaderFactory);
    }

    manager->discoverAvailableScenes(getLocalPath(c_AssetsFolder));

    std::string sceneArg;
    if (LooksLikeInlineSceneJson(preferredScene))
    {
        sceneArg = preferredScene;
    }
    else
    {
        std::filesystem::path preferredScenePath(preferredScene);
        sceneArg = (!preferredScene.empty() && (preferredScenePath.is_absolute() || std::filesystem::exists(preferredScenePath)))
            ? preferredScene
            : findPreferredScene(manager->getAvailableScenes(), preferredScene);
    }

    setCurrentScene(app, sceneArg);
}

void setCurrentScene(App& app, const std::string& sceneName, bool forceReload)
{
    if (caustica::isRenderThread())
    {
        SceneViewState* vs = viewState(app);
        assert(vs);
        std::lock_guard lock(vs->pendingSceneSwitchMutex);
        vs->pendingSceneSwitch = SceneViewState::PendingSceneSwitch{ sceneName, forceReload };
        return;
    }

    detail::applySceneSwitch(app, sceneName, forceReload);
}

void onSceneUnloading(App& app)
{
    PathTracerSettings* cfg = settings(app);
    SceneViewState* vs = viewState(app);
    assert(cfg && vs);

    cfg->EnvironmentMapParams = EnvironmentMapRuntimeParameters();
    vs->uncompressedTextures.clear();

    runGpuWorkOnRenderThread(app, [&app]() {
        if (GpuRenderSubsystem* gr = app.tryResource<GpuRenderSubsystem>())
            gr->onSceneUnloading();
    });

    if (auto* access = app.tryResource<SceneAccess>())
        access->active.reset();
}

void onSceneLoaded(App& app)
{
    GpuRenderSubsystem* gr = app.tryResource<GpuRenderSubsystem>();
    if (!gr)
        return;

    ::SceneManager* manager = detail::sessionManager(app);
    SceneViewState* vs = viewState(app);
    const CommandLineOptions* cmd = cmdLine(app);
    if (!manager || !vs || !cmd)
        return;

    syncSceneAccess(app);

    const std::filesystem::path assetsRoot = getLocalPath(c_AssetsFolder);
    if (render::WorldRenderer* wrResource = worldRenderer(app))
        wrResource->lightingPasses().refreshEnvironmentMapMediaList(assetsRoot, manager->getCurrentScenePath());

    vs->progressLoading.Set(50);
    vs->progressLoading.Set(55);

    gr->onSceneLoadedBegin();

    runGpuWorkOnRenderThread(app, [&app]() {
        if (GpuRenderSubsystem* gpu = app.tryResource<GpuRenderSubsystem>())
            gpu->onSceneLoadedGpuPrep();
    });

    syncCameraFromScene(app);

    vs->progressLoading.Set(60);

    gr->onSceneLoadedGpuFinish();

    collectUncompressedTextures(app);

    vs->progressLoading.Set(70);
    vs->progressLoading.Set(90);

    gr->applyCmdLinePostLoadOverrides();
    if (!cmd->cameraPosDirUp.empty())
        setCurrentCameraPosDirUp(app, cmd->cameraPosDirUp);

    if (CameraController* cam = detail::sessionCamera(app))
        cam->syncPreviousViewFromCurrent();

    vs->progressLoading.Set(100);

    if (GpuDevice* device = gpuDevice(app))
    {
        device->setPreparedRenderFrameIndex(device->getFrameIndex());
        if (const std::shared_ptr<Scene> scenePtr = activeScene(app))
            scenePtr->extractAndPublishRenderSnapshot(device->getPreparedRenderFrameIndex());
    }
}

void collectUncompressedTextures(App& app)
{
    SceneViewState* vs = viewState(app);
    assert(vs);
    vs->uncompressedTextures.clear();

    auto listUncompressedTextureIfNeeded = [&](Handle<ImageAsset> texture, bool normalMap)
    {
        if (texture == nullptr || texture->gpu.texture == nullptr)
            return;
        nvrhi::TextureDesc desc = texture->gpu.texture->getDesc();
        if (nvrhi::getFormatInfo(desc.format).blockSize != 1)
            return;
        TextureCompressionType compressionType = normalMap ? (TextureCompressionType::Normalmap) : (
            (nvrhi::getFormatInfo(desc.format).isSRGB) ? (TextureCompressionType::GenericSRGB) : (TextureCompressionType::GenericLinear));

        auto it = vs->uncompressedTextures.insert(std::make_pair(texture, compressionType));
        if (!it.second)
        {
            assert(it.first->second == compressionType);
            return;
        }
    };
    if (render::WorldRenderer* wrResource = worldRenderer(app))
    {
        wrResource->lightingPasses().forEachUsedMaterialTexture([&](Handle<ImageAsset> texture, bool normalMap)
        {
            listUncompressedTextureIfNeeded(texture, normalMap);
        });
    }
}

bool hasAsyncLoadingInProgress(const App& app)
{
    AppDiagnostics* diag = diagnostics(app);
    RenderRuntimeState* runtime = runtimeState(app);
    if (!diag || !runtime)
        return false;
    return diag->asyncLoadingInProgress
        || runtime->Invalidation.ShaderAndACRefreshDelayedRequest > 0;
}

} // namespace caustica
