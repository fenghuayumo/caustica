#include "RenderSession.h"

#if CAUSTICA_WITH_PYTHON

#include <engine/EngineApp.h>
#include <backend/GpuSurface.h>
#include <platform/window.h>
#include <engine/EntryPoint.h>
#include <engine/GpuSharedCaches.h>
#include <engine/AppResources.h>
#include <engine/SceneViewState.h>
#include <engine/RenderFrameApi.h>
#include <engine/internal/SceneApiInternal.h>
#include <scene/SceneManager.h>
#include <assets/loader/TextureLoader.h>
#include <render/core/RenderDevice.h>
#include <core/file_utils.h>
#include <core/json.h>
#include <core/log.h>
#include <core/path_utils.h>
#include <core/progress.h>

#include <json/json.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <thread>
#include <vector>

namespace
{
    constexpr double c_HeadlessFrameTimeSeconds = 1.0 / 60.0;

    std::string TrimCopy(const std::string& value)
    {
        const auto begin = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) {
            return std::isspace(ch);
        });
        const auto end = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) {
            return std::isspace(ch);
        }).base();
        if (begin >= end)
            return {};
        return std::string(begin, end);
    }

    std::string ToLowerCopy(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
            return char(std::tolower(ch));
        });
        return value;
    }

    bool IsBuiltinModelReference(const std::string& modelName)
    {
        return ToLowerCopy(modelName).rfind("builtin:", 0) == 0;
    }

    std::string NormalizeBuiltinModelName(std::string modelName)
    {
        modelName = ToLowerCopy(TrimCopy(modelName));
        constexpr const char* prefix = "builtin:";
        if (modelName.rfind(prefix, 0) == 0)
            modelName.erase(0, std::strlen(prefix));

        for (char& ch : modelName)
        {
            if (ch == '-' || ch == ' ')
                ch = '_';
        }

        return modelName;
    }

    Json::Value MakeFloatArray(std::initializer_list<float> values)
    {
        Json::Value array(Json::arrayValue);
        for (float value : values)
            array.append(value);
        return array;
    }

    std::string BuildBuiltinDefaultSceneJson(const std::string& builtinModel)
    {
        Json::Value root(Json::objectValue);
        root["entities"] = Json::Value(Json::arrayValue);

        Json::Value modelNode(Json::objectValue);
        modelNode["id"] = "DefaultBuiltinModel";
        modelNode["name"] = "DefaultBuiltinModel";
        modelNode["components"]["PrefabInstance"]["source"] =
            std::string("builtin:") + NormalizeBuiltinModelName(builtinModel);
        root["entities"].append(modelNode);

        Json::Value lights(Json::objectValue);
        lights["id"] = "Lights";
        lights["name"] = "Lights";
        root["entities"].append(lights);

        Json::Value sun(Json::objectValue);
        sun["id"] = "Sun";
        sun["name"] = "Sun";
        sun["parent"] = "Lights";
        sun["components"]["Transform"]["rotation"] =
            MakeFloatArray({ -0.23053891f, -0.15879166f, -0.6890466f, 0.6684697f });
        sun["components"]["DirectionalLight"]["angularSize"] = 1.5f;
        sun["components"]["DirectionalLight"]["color"] = MakeFloatArray({ 1.0f, 0.96f, 0.9f });
        sun["components"]["DirectionalLight"]["irradiance"] = 4.0f;
        root["entities"].append(sun);

        Json::Value fill(Json::objectValue);
        fill["id"] = "Fill";
        fill["name"] = "Fill";
        fill["parent"] = "Lights";
        fill["components"]["Transform"]["translation"] = MakeFloatArray({ 0.0f, 2.5f, 3.0f });
        fill["components"]["PointLight"]["color"] = MakeFloatArray({ 1.0f, 0.95f, 0.85f });
        fill["components"]["PointLight"]["intensity"] = 30.0f;
        fill["components"]["PointLight"]["radius"] = 0.05f;
        fill["components"]["PointLight"]["range"] = 10.0f;
        root["entities"].append(fill);

        Json::Value cameras(Json::objectValue);
        cameras["id"] = "Cameras";
        cameras["name"] = "Cameras";
        root["entities"].append(cameras);

        Json::Value camera(Json::objectValue);
        camera["id"] = "Default";
        camera["name"] = "Default";
        camera["parent"] = "Cameras";
        camera["components"]["Transform"]["translation"] = MakeFloatArray({ 0.0f, 1.15f, 5.0f });
        camera["components"]["Transform"]["rotation"] = MakeFloatArray({ 0.0f, 0.0f, 0.0f, 1.0f });
        camera["components"]["PerspectiveCameraEx"]["verticalFov"] = 0.7f;
        camera["components"]["PerspectiveCameraEx"]["zNear"] = 0.001f;
        camera["components"]["PerspectiveCameraEx"]["exposureCompensation"] = 1.0f;
        camera["components"]["PerspectiveCameraEx"]["enableAutoExposure"] = false;
        root["entities"].append(camera);

        root["settings"]["realtimeMode"] = true;

        return caustica::json::toString(root);
    }

    std::string PrepareSceneArgument(const std::string& sceneArgument)
    {
        const std::string trimmed = TrimCopy(sceneArgument);
        if (trimmed.empty())
            return sceneArgument;

        if (IsBuiltinModelReference(trimmed))
            return BuildBuiltinDefaultSceneJson(trimmed);

        return sceneArgument;
    }

}

namespace caustica_py
{
    std::string BuiltinSceneJson(const std::string& builtinModel)
    {
        return BuildBuiltinDefaultSceneJson(builtinModel);
    }
}

RenderSession::RenderSession(std::shared_ptr<caustica_py::PythonDevice> device, const Config& cfg)
    : m_config(cfg)
    , m_device(std::move(device))
{
    if (!m_device)
    {
        caustica::error("RenderSession: Device is required");
        return;
    }

    m_config.width = m_config.width > 0 ? m_config.width : 1920;
    m_config.height = m_config.height > 0 ? m_config.height : 1080;
    m_config.scene = PrepareSceneArgument(cfg.scene);

    if (!m_device->ensureCreated(m_config.width, m_config.height, m_config.headless))
        return;
    if (!m_device->tryBind())
        return;

    m_config.width = m_device->width();
    m_config.height = m_device->height();
    m_config.headless = m_device->headless();
    m_config.useVulkan = m_device->useVulkan();
    m_config.adapter = m_device->config().adapter;
    m_config.debug = m_device->config().debug;

    caustica::EngineAppDesc desc{};
    desc.width = uint32_t(m_config.width);
    desc.height = uint32_t(m_config.height);
    desc.headless = m_config.headless;
    desc.debugDevice = m_config.debug;
    desc.adapter = m_device->adapterSelector();
    desc.useVulkan = m_config.useVulkan;
    desc.scene = m_config.scene;
    desc.windowTitle = "caustica_py";
    desc.dedicatedRenderThread = !m_config.headless;
    desc.runtimeDirectory = caustica_py::ResolveRuntimeDirectory();
    desc.device = m_device->gpu();
    desc.window = m_device->window();
    desc.surface = m_device->surface();
    desc.cli.nonInteractive = cfg.nonInteractive;
    desc.cli.OverrideToReferenceMode = !cfg.realtimeMode;
    desc.cli.OverrideToRealtimeMode = cfg.realtimeMode;
    desc.cli.ReferenceSamplesPerPixel = cfg.accumulationTarget;
#if CAUSTICA_WITH_DX12 && defined(CAUSTICA_D3D_AGILITY_SDK_VERSION)
    desc.d3d12DeviceFactory = m_device->d3d12Factory();
#endif

    m_engine = caustica::EngineApp::create(std::move(desc));
    if (!m_engine || !m_engine->isValid())
    {
        caustica::error("RenderSession: failed to initialize EngineApp");
        m_device->unbind();
        return;
    }

    auto& cmdLine = m_engine->commandLine();
    cmdLine.nonInteractive = cfg.nonInteractive;
    cmdLine.OverrideToReferenceMode = !cfg.realtimeMode;
    cmdLine.OverrideToRealtimeMode = cfg.realtimeMode;
    cmdLine.ReferenceSamplesPerPixel = cfg.accumulationTarget;
    m_engine->renderAppState().settings.AccumulationTarget = cfg.accumulationTarget;

    m_initialized = true;

    if (!m_config.scene.empty() && !WaitUntilReady())
    {
        caustica::error("RenderSession: scene did not become ready");
        shutdown();
    }
}

RenderSession::~RenderSession()
{
    shutdown();
}

void RenderSession::shutdown()
{
    if (m_engine)
        m_engine->shutdown();
    m_engine.reset();
    m_initialized = false;
    if (m_device)
        m_device->unbind();
}

bool RenderSession::LoadScene(const std::string& sceneName, bool waitUntilReady,
                              double timeoutSeconds, int warmupFrames)
{
    if (!m_initialized || !m_engine)
        return false;

    m_engine->setScene(PrepareSceneArgument(sceneName), /*forceReload=*/true);

    if (waitUntilReady)
        return WaitUntilReady(timeoutSeconds, warmupFrames);
    return true;
}

bool RenderSession::IsSceneReady() const
{
    if (!m_initialized || !m_engine || !m_engine->isSceneLoaded())
        return false;

    const auto* viewState = m_engine->app().tryResource<caustica::SceneViewState>();
    if (!viewState)
        return !m_engine->isSceneLoading();

    // Secondary opacity/OMM streaming may continue after the scene is already
    // published and renderable. Prewarm only needs the primary load transaction
    // to finish and rendering suspension to be released.
    return !viewState->loadSession.isActive()
        && !viewState->sceneGpuSuspended.load(std::memory_order_acquire);
}

bool RenderSession::WaitUntilReady(double timeoutSeconds, int warmupFrames)
{
    if (!m_initialized || !m_engine)
        return false;

    const auto start = std::chrono::steady_clock::now();
    const auto timeout = std::chrono::duration<double>(std::max(0.0, timeoutSeconds));
    while (timeoutSeconds <= 0.0 || std::chrono::steady_clock::now() - start < timeout)
    {
        if (!Step(0.0f))
            return false;
        if (IsSceneReady())
        {
            if (warmupFrames > 0 && !StepN(warmupFrames))
                return false;
            m_engine->renderAppState().settings.ResetAccumulation = true;
            return true;
        }
        // Scene import and GPU streaming run on worker/render domains. A headless
        // loop can otherwise consume the frame budget in a fraction of a second
        // before those domains get enough wall-clock time to finish.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto* viewState = m_engine->app().tryResource<caustica::SceneViewState>();
    const auto* manager = caustica::detail::sessionManager(m_engine->app());
    caustica::warning(
        "RenderSession: scene did not become ready within %.1f seconds "
        "(phase=%s managerLoaded=%d managerLoading=%d gpuSuspended=%d secondaryStreaming=%d)",
        timeoutSeconds,
        viewState ? caustica::loadSessionPhaseName(viewState->loadSession.phase) : "Unavailable",
        manager && manager->isSceneLoaded() ? 1 : 0,
        manager && manager->isSceneLoading() ? 1 : 0,
        viewState && viewState->sceneGpuSuspended.load(std::memory_order_acquire) ? 1 : 0,
        viewState && viewState->loadSession.secondaryStreaming.load(std::memory_order_acquire) ? 1 : 0);
    return false;
}

bool RenderSession::Step(float dt)
{
    if (!m_initialized || !m_engine)
        return false;

    auto* device = m_engine->device();
    if (!device)
        return false;

    const bool frameOk = dt >= 0.0f
        ? m_engine->stepFrame(dt)
        : m_engine->stepFrame(m_config.headless ? float(c_HeadlessFrameTimeSeconds) : -1.f);

    if (!frameOk)
        return false;

    caustica::Window* window = m_engine->app().getWindow();
    return !window || !window->getExit();
}

bool RenderSession::StepN(int frames)
{
    for (int i = 0; i < frames; ++i)
    {
        if (!Step())
            return false;
    }
    return true;
}

int RenderSession::StepUntilAccumulated(int maxFrames)
{
    if (!m_initialized || !m_engine)
        return 0;

    // Force reference / accumulation mode so we know "done" actually means
    // the SPP target has been reached.
    auto& settings = m_engine->renderAppState().settings;
    settings.ResetAccumulation = true;

    int target = (maxFrames > 0)
        ? maxFrames
        : std::max(1, settings.AccumulationTarget + 128);

    int frames = 0;
    while (frames < target)
    {
        // All samples of one reference output frame must observe the same
        // simulation/animation time. Sequence callers advance the pose once,
        // then this loop freezes time while accumulating samples.
        if (!Step(0.0f)) break;
        ++frames;
        if (m_engine->accumulationCompleted())
            break;
    }
    return frames;
}

bool RenderSession::PrepareAnimationFrame(
    double sceneTime,
    bool importedAnimations,
    bool keyframes)
{
    if (!m_initialized || !m_engine || !std::isfinite(sceneTime))
        return false;

    caustica::App& app = m_engine->app();
    auto& settings = m_engine->renderAppState().settings;
    const bool previousRealtime = settings.RealtimeMode;
    const bool previousAnimations = settings.EnableAnimations;
    const bool previousKeyframes = settings.EnableKeyframes;

    // Animation evaluation currently belongs to the realtime update path.
    // Evaluate exactly once at the requested clock, then restore the caller's
    // mode so reference accumulation can sample a frozen pose.
    caustica::setSceneTime(app, sceneTime);
    if (auto* viewState = app.tryResource<caustica::SceneViewState>())
        viewState->keyframeTime = sceneTime;
    settings.RealtimeMode = true;
    settings.EnableAnimations = importedAnimations;
    settings.EnableKeyframes = keyframes;
    settings.ResetRealtimeCaches = true;
    const bool ok = Step(0.0f);

    settings.RealtimeMode = previousRealtime;
    settings.EnableAnimations = previousAnimations;
    settings.EnableKeyframes = previousKeyframes;
    settings.ResetAccumulation = true;
    return ok;
}

int RenderSession::RenderReferenceFrame(int spp, bool oidn, int maxFrames)
{
    if (!m_initialized || !m_engine || spp <= 0)
        return 0;

    auto& settings = m_engine->renderAppState().settings;
    settings.RealtimeMode = false;
    settings.AccumulationTarget = spp;
    settings.AccumulationPreWarmRealtimeCaches = false;
    settings.ReferenceOIDNDenoiser = oidn;
    settings.ReferenceOIDNDenoiserChanged = true;
    return StepUntilAccumulated(maxFrames);
}

bool RenderSession::RenderRealtimeFrame(float dt)
{
    if (!m_initialized || !m_engine || !std::isfinite(dt) || dt < 0.0f)
        return false;

    auto& settings = m_engine->renderAppState().settings;
    if (!settings.RealtimeMode)
    {
        settings.RealtimeMode = true;
        settings.ResetAccumulation = true;
        settings.ResetRealtimeCaches = true;
    }
    settings.AccumulationTarget = 1;
    return Step(dt);
}

bool RenderSession::SaveScreenshot(const std::string& outputPath)
{
    if (!m_initialized || !m_engine)
        return false;

    auto* device = m_engine->device();
    if (!device)
        return false;

    caustica::rhi::Texture* tex = m_engine->ldrColorTexture();
    caustica::rhi::ResourceStates state = caustica::rhi::ResourceStates::ShaderResource;

    if (!tex)
    {
        uint32_t backBufferIndex = m_engine->surface()
            ? m_engine->surface()->getLastPresentedBackBufferIndex()
            : 0;

        tex = device->getBackBuffer(backBufferIndex);
        state = m_config.headless
            ? caustica::rhi::ResourceStates::RenderTarget
            : caustica::rhi::ResourceStates::Present;
    }

    if (!tex)
    {
        caustica::error("RenderSession: no current output texture");
        return false;
    }

    auto* infra = caustica::gpuSharedCaches(m_engine->app());
    auto* renderDevice = (infra && infra->renderDevice) ? infra->renderDevice.get() : nullptr;
    if (!renderDevice)
    {
        caustica::error("RenderSession: render device not initialized yet");
        return false;
    }

    // saveTextureToFile creates its own command list. wait for the last rendered
    // frame to finish so LdrColor is not still in use by an in-flight submit.
    if (!device->getDevice()->waitForIdle())
    {
        caustica::error("RenderSession: GPU device lost or removed before screenshot");
        return false;
    }

    std::filesystem::path p(outputPath);
    if (p.has_parent_path())
        caustica::ensureDirectoryExists(p.parent_path());

    return caustica::saveTextureToFile(
        device->getDevice(),
        *renderDevice,
        tex,
        state,
        outputPath.c_str());
}

std::optional<RenderSession::FramebufferLdr> RenderSession::GetFramebufferLdr()
{
    if (!m_initialized || !m_engine)
        return std::nullopt;

    auto* gpuDevice = m_engine->device();
    if (!gpuDevice)
        return std::nullopt;

    caustica::rhi::Device* device = gpuDevice->getDevice();
    if (!device)
        return std::nullopt;

    caustica::rhi::Texture* texture = m_engine->ldrColorTexture();
    caustica::rhi::ResourceStates textureState = caustica::rhi::ResourceStates::ShaderResource;

    if (!texture)
    {
        uint32_t backBufferIndex = m_engine->surface()
            ? m_engine->surface()->getLastPresentedBackBufferIndex()
            : 0;

        texture = gpuDevice->getBackBuffer(backBufferIndex);
        textureState = m_config.headless
            ? caustica::rhi::ResourceStates::RenderTarget
            : caustica::rhi::ResourceStates::Present;
    }

    if (!texture)
    {
        caustica::error("RenderSession: no current output texture for framebuffer readback");
        return std::nullopt;
    }

    auto* infra = caustica::gpuSharedCaches(m_engine->app());
    auto* renderDevice = (infra && infra->renderDevice) ? infra->renderDevice.get() : nullptr;
    if (!renderDevice)
    {
        caustica::error("RenderSession: render device not initialized yet");
        return std::nullopt;
    }

    if (!device->waitForIdle())
    {
        caustica::error("RenderSession: GPU device lost or removed before framebuffer readback");
        return std::nullopt;
    }

    // Mirror saveTextureToFile: blit non-RGBA8 targets to SRGBA8, then staging copy.
    caustica::rhi::TextureDesc desc = texture->getDesc();
    caustica::rhi::TextureHandle tempTexture;
    caustica::rhi::FramebufferHandle tempFramebuffer;

    caustica::rhi::CommandListHandle commandList = device->createCommandList();
    if (!commandList || !commandList->open())
        return std::nullopt;

    if (textureState != caustica::rhi::ResourceStates::Unknown)
        commandList->beginTrackingTextureState(texture, caustica::rhi::TextureSubresourceSet(0, 1, 0, 1), textureState);

    switch (desc.format)
    {
    case caustica::rhi::Format::RGBA8_UNORM:
    case caustica::rhi::Format::SRGBA8_UNORM:
        tempTexture = texture;
        break;
    default:
        desc.format = caustica::rhi::Format::SRGBA8_UNORM;
        desc.isRenderTarget = true;
        desc.initialState = caustica::rhi::ResourceStates::RenderTarget;
        desc.keepInitialState = true;
        tempTexture = device->createTexture(desc);
        tempFramebuffer = device->createFramebuffer(caustica::rhi::FramebufferDesc().addColorAttachment(tempTexture));
        renderDevice->blit().blitTexture(commandList, tempFramebuffer, texture);
        break;
    }

    caustica::rhi::TextureDesc stagingDesc = desc;
    stagingDesc.isRenderTarget = false;
    stagingDesc.isUAV = false;
    stagingDesc.isTypeless = false;
    stagingDesc.initialState = caustica::rhi::ResourceStates::CopyDest;
    stagingDesc.keepInitialState = true;
    stagingDesc.debugName = "GetFramebufferLdr Staging";

    caustica::rhi::StagingTextureHandle stagingTexture = device->createStagingTexture(stagingDesc, caustica::rhi::CpuAccessMode::Read);
    if (!stagingTexture)
    {
        commandList->close();
        return std::nullopt;
    }

    commandList->copyTexture(stagingTexture, caustica::rhi::TextureSlice(), tempTexture, caustica::rhi::TextureSlice());

    if (textureState != caustica::rhi::ResourceStates::Unknown)
    {
        commandList->setTextureState(texture, caustica::rhi::TextureSubresourceSet(0, 1, 0, 1), textureState);
        commandList->commitBarriers();
    }

    commandList->close();
    device->executeCommandList(commandList);

    if (!device->waitForIdle())
        return std::nullopt;

    size_t rowPitch = 0;
    const uint8_t* mapped = static_cast<const uint8_t*>(device->mapStagingTexture(
        stagingTexture, caustica::rhi::TextureSlice(), caustica::rhi::CpuAccessMode::Read, &rowPitch));
    if (!mapped)
        return std::nullopt;

    FramebufferLdr result;
    result.width = desc.width;
    result.height = desc.height;
    result.channels = 4;
    result.pixels.resize(size_t(desc.width) * size_t(desc.height) * 4u);

    const size_t dstStride = size_t(desc.width) * 4u;
    for (uint32_t row = 0; row < desc.height; ++row)
    {
        std::memcpy(
            result.pixels.data() + size_t(row) * dstStride,
            mapped + size_t(row) * rowPitch,
            dstStride);
    }

    device->unmapStagingTexture(stagingTexture);
    return result;
}

bool RenderSession::SetCamera(const caustica::math::float3& pos,
                              const caustica::math::float3& dir,
                              const caustica::math::float3& up)
{
    if (!m_engine) return false;

    auto v3 = [](const caustica::math::float3& v) {
        return std::to_string(v.x) + "," + std::to_string(v.y) + "," + std::to_string(v.z);
    };
    std::string s = v3(pos) + "," + v3(dir) + "," + v3(up);
    return m_engine->setCameraPosDirUp(s);
}

void RenderSession::SetCameraFOV(float verticalFovDegrees)
{
    if (m_engine)
        m_engine->setCameraVerticalFOV(caustica::math::radians(verticalFovDegrees));
}

void RenderSession::setCameraIntrinsics(float fx, float fy, float cx, float cy, float width, float height)
{
    if (m_engine)
        m_engine->setCameraIntrinsics(fx, fy, cx, cy, width, height);
}

#endif // CAUSTICA_WITH_PYTHON
