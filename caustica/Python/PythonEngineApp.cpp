#include "PythonEngineApp.h"

#if CAUSTICA_WITH_PYTHON

#include <platform/window.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace caustica_py
{
namespace
{
    PyEngineApp* g_current = nullptr;
}

void setCurrentEngineApp(PyEngineApp* app)
{
    g_current = app;
}

PyEngineApp* currentEngineApp()
{
    return g_current;
}

PyEngineApp::PyEngineApp(std::shared_ptr<PythonDevice> device, const RenderSession::Config& cfg)
{
    m_session = std::make_unique<RenderSession>(std::move(device), cfg);
    if (!m_session->GetEngine())
        throw std::runtime_error("caustica.EngineApp: failed to initialize (see log for details)");
    m_context->engine = m_session->GetEngine();
    adoptCurrent();
}

PyEngineApp::PyEngineApp(caustica::EngineApp* borrowed)
    : m_borrowed(borrowed)
{
    if (!m_borrowed)
        throw std::runtime_error("caustica.EngineApp: borrowed engine is null");
    m_context->engine = m_borrowed;
}

PyEngineApp::PyEngineApp(PyEngineApp&& other) noexcept
    : m_session(std::move(other.m_session))
    , m_borrowed(other.m_borrowed)
    , m_context(std::move(other.m_context))
{
    other.m_borrowed = nullptr;
    other.m_context = std::make_shared<PyEngineAppContext>();
    if (g_current == &other)
        g_current = this;
}

PyEngineApp& PyEngineApp::operator=(PyEngineApp&& other) noexcept
{
    if (this == &other)
        return *this;
    shutdown();
    m_session = std::move(other.m_session);
    m_borrowed = other.m_borrowed;
    m_context = std::move(other.m_context);
    other.m_borrowed = nullptr;
    other.m_context = std::make_shared<PyEngineAppContext>();
    if (g_current == &other)
        g_current = this;
    return *this;
}

PyEngineApp::~PyEngineApp()
{
    shutdown();
}

void PyEngineApp::adoptCurrent()
{
    g_current = this;
}

void PyEngineApp::shutdown()
{
    if (g_current == this)
        g_current = nullptr;
    if (m_context)
        m_context->engine = nullptr;
    if (m_session)
        m_session.reset();
    m_borrowed = nullptr;
}

bool PyEngineApp::isValid() const
{
    return tryEngine() != nullptr;
}

caustica::EngineApp& PyEngineApp::engine()
{
    caustica::EngineApp* engine = tryEngine();
    if (!engine)
        throw std::runtime_error("caustica.EngineApp: engine is closed");
    return *engine;
}

const caustica::EngineApp& PyEngineApp::engine() const
{
    const caustica::EngineApp* engine = tryEngine();
    if (!engine)
        throw std::runtime_error("caustica.EngineApp: engine is closed");
    return *engine;
}

caustica::EngineApp* PyEngineApp::tryEngine()
{
    if (m_session)
        return m_session->GetEngine();
    return m_borrowed;
}

const caustica::EngineApp* PyEngineApp::tryEngine() const
{
    if (m_session)
        return m_session->GetEngine();
    return m_borrowed;
}

caustica::App* PyEngineApp::tryApp()
{
    caustica::EngineApp* engine = tryEngine();
    return engine ? &engine->app() : nullptr;
}

std::shared_ptr<PythonDevice> PyEngineApp::device() const
{
    return m_session ? m_session->GetDevice() : nullptr;
}

bool PyEngineApp::step(float dt)
{
    caustica::EngineApp* engine = tryEngine();
    if (!engine)
        return false;

    const bool headless = engine->window() == nullptr;
    const bool frameOk = dt >= 0.f
        ? engine->stepFrame(dt)
        : engine->stepFrame(headless ? (1.f / 60.f) : -1.f);
    if (!frameOk)
        return false;

    caustica::Window* window = engine->window();
    return !window || !window->getExit();
}

bool PyEngineApp::stepN(int frames)
{
    for (int i = 0; i < frames; ++i)
    {
        if (!step())
            return false;
    }
    return true;
}

int PyEngineApp::stepUntilAccumulated(int maxFrames)
{
    caustica::EngineApp* engine = tryEngine();
    if (!engine)
        return 0;

    auto& settings = engine->settings();
    settings.ResetAccumulation = true;
    const int target = (maxFrames > 0)
        ? maxFrames
        : std::max(1, settings.AccumulationTarget + 128);

    int frames = 0;
    while (frames < target)
    {
        if (!step(0.f))
            break;
        ++frames;
        if (engine->accumulationCompleted())
            break;
    }
    return frames;
}

int PyEngineApp::renderReferenceFrame(int spp, bool oidn, int maxFrames)
{
    caustica::EngineApp* engine = tryEngine();
    if (!engine || spp <= 0)
        return 0;
    engine->setReferenceMode(spp, oidn);
    engine->settings().AccumulationPreWarmRealtimeCaches = false;
    return stepUntilAccumulated(maxFrames);
}

bool PyEngineApp::renderRealtimeFrame(float dt)
{
    caustica::EngineApp* engine = tryEngine();
    if (!engine || !std::isfinite(dt) || dt < 0.f)
        return false;
    auto& settings = engine->settings();
    if (!settings.RealtimeMode)
        engine->setRealtimeMode(settings.StandaloneDenoiser, settings.RealtimeAA);
    settings.AccumulationTarget = 1;
    return step(dt);
}

} // namespace caustica_py

#endif // CAUSTICA_WITH_PYTHON
