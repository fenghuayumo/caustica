#include "RenderSession.h"

#if CAUSTICA_WITH_PYTHON

#include <engine/EngineApp.h>
#include <core/log.h>
#include <platform/window.h>

#include <algorithm>
#include <cmath>

namespace caustica_py
{
    std::string BuiltinSceneJson(const std::string& builtinModel)
    {
        return caustica::builtinSceneJson(builtinModel);
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
    m_config.scene = caustica::prepareSceneSource(cfg.scene);

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
    m_engine->settings().AccumulationTarget = cfg.accumulationTarget;

    m_initialized = true;

    if (!m_config.scene.empty() && !m_engine->waitUntilReady())
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

    m_engine->setScene(sceneName, /*forceReload=*/true);

    if (waitUntilReady)
        return m_engine->waitUntilReady(timeoutSeconds, warmupFrames);
    return true;
}

bool RenderSession::IsSceneReady() const
{
    return m_engine && m_engine->isSceneReady();
}

bool RenderSession::WaitUntilReady(double timeoutSeconds, int warmupFrames)
{
    return m_engine && m_engine->waitUntilReady(timeoutSeconds, warmupFrames);
}

bool RenderSession::Step(float dt)
{
    if (!m_initialized || !m_engine)
        return false;

    const bool headless = m_engine->window() == nullptr;
    const bool frameOk = dt >= 0.0f
        ? m_engine->stepFrame(dt)
        : m_engine->stepFrame(headless ? (1.f / 60.f) : -1.f);
    if (!frameOk)
        return false;

    caustica::Window* window = m_engine->window();
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

    auto& settings = m_engine->settings();
    settings.ResetAccumulation = true;
    const int target = (maxFrames > 0)
        ? maxFrames
        : std::max(1, settings.AccumulationTarget + 128);

    int frames = 0;
    while (frames < target)
    {
        if (!Step(0.0f))
            break;
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
    return m_engine && m_engine->prepareAnimationFrame(sceneTime, importedAnimations, keyframes);
}

int RenderSession::RenderReferenceFrame(int spp, bool oidn, int maxFrames)
{
    if (!m_initialized || !m_engine || spp <= 0)
        return 0;
    m_engine->setReferenceMode(spp, oidn);
    m_engine->settings().AccumulationPreWarmRealtimeCaches = false;
    return StepUntilAccumulated(maxFrames);
}

bool RenderSession::RenderRealtimeFrame(float dt)
{
    if (!m_initialized || !m_engine || !std::isfinite(dt) || dt < 0.0f)
        return false;
    auto& settings = m_engine->settings();
    if (!settings.RealtimeMode)
        m_engine->setRealtimeMode(settings.StandaloneDenoiser, settings.RealtimeAA);
    settings.AccumulationTarget = 1;
    return Step(dt);
}

bool RenderSession::SaveScreenshot(const std::string& outputPath)
{
    return m_engine && m_engine->saveScreenshot(outputPath);
}

std::optional<RenderSession::FramebufferLdr> RenderSession::GetFramebufferLdr()
{
    if (!m_engine)
        return std::nullopt;
    auto fb = m_engine->readLdrFramebuffer();
    if (!fb)
        return std::nullopt;
    FramebufferLdr result;
    result.width = fb->width;
    result.height = fb->height;
    result.channels = fb->channels;
    result.pixels = std::move(fb->pixels);
    return result;
}

bool RenderSession::SetCamera(const caustica::math::float3& pos,
                              const caustica::math::float3& dir,
                              const caustica::math::float3& up)
{
    return m_engine && m_engine->setCameraPosDirUp(pos, dir, up);
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
