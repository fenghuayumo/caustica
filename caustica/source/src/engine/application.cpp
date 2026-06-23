/*
* Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved.
*/

#include "engine/application.h"
#include "engine/input.h"
#include "engine/events/event.h"
#include "platform/window.h"
#include "platform/timer.h"
#include "platform/file_system.h"
#include "render/renderer.h"

#include <rhi/nvrhi.h>

#include <cstdio>

namespace caustica
{

Application* Application::s_Instance = nullptr;

Application::Application()
    : m_FrameCount(0)
    , m_SceneViewWidth(1280)
    , m_SceneViewHeight(720)
{
    if (s_Instance)
    {
        fprintf(stderr, "[Application] Warning: multiple Application instances created\n");
    }
    s_Instance = this;
}

Application::~Application()
{
    // Release subsystems in reverse creation order
    m_Renderer.reset();
    m_SceneManager.reset();
    m_Window.reset();
}

void Application::init()
{
    fprintf(stdout, "[Application] Initializing...\n");

    // 1. Create engine singleton
    Engine::get();
    fprintf(stdout, "[Application] Engine initialized\n");

    // 2. Create scene manager
    m_SceneManager = std::make_unique<SceneManager>();
    fprintf(stdout, "[Application] SceneManager created\n");

    // 3. Create timer
    m_Timer = std::make_unique<Timer>();

    // 4. Create window
    WindowDesc desc;
    desc.Width      = m_Settings.Width;
    desc.Height     = m_Settings.Height;
    desc.RenderAPI  = m_Settings.RenderAPI;
    desc.Fullscreen = m_Settings.Fullscreen;
    desc.Borderless = m_Settings.Borderless;
    desc.ShowConsole = m_Settings.ShowConsole;
    desc.Title      = m_Settings.Title;
    desc.VSync      = m_Settings.VSync;

    m_Window.reset(Window::create(desc));
    if (!m_Window || !m_Window->hasInitialised())
    {
        fprintf(stderr, "[Application] Failed to create window\n");
        m_State = AppState::Closing;
        return;
    }

    // Wire window events to Application::handleEvent
    m_Window->setEventCallback([this](Event& e) {
        this->handleEvent(e);
    });

    fprintf(stdout, "[Application] Window created: %ux%u\n",
        m_Window->getWidth(), m_Window->getHeight());

    // 5. Initialize ImGui (placeholder — wired in Phase C)
    // m_ImGuiManager = std::make_unique<ImGuiManager>();
    // m_ImGuiManager->init(m_Window.get());

    // 6. Initialize input system
    Input::get();

    m_State = AppState::Running;
    fprintf(stdout, "[Application] Initialization complete\n");
}

void Application::initRenderer(nvrhi::IDevice* device, nvrhi::ISwapchain* swapchain)
{
    if (!device || !swapchain)
    {
        fprintf(stderr, "[Application] initRenderer: null device or swapchain\n");
        return;
    }

    m_Renderer = std::make_unique<Renderer>(device, swapchain);
    auto fbSize = m_Window->getFramebufferSize();
    m_Renderer->init(fbSize);
    m_Renderer->startRenderThread();

    fprintf(stdout, "[Application] Renderer initialized\n");
}

void Application::run()
{
    while (frame())
    {
        // Main loop driven by frame()
    }
    quit();
}

bool Application::frame()
{
    // Process pending scene switch
    if (m_SceneManager && m_SceneManager->isSwitching())
    {
        m_SceneManager->applySceneSwitch();
    }

    // Update timing
    float deltaTime = static_cast<float>(m_Timer->getElapsedSeconds());
    m_Timer->reset();

    Engine::timeStep().update(deltaTime);

    // Process event queue
    processEventQueue();

    // Reset input edge flags
    Input::get().resetPressed();

    // Process window events (GLFW poll)
    if (m_Window)
        m_Window->processInput();

    // Skip rendering if minimized
    if (m_Minimized)
        return m_State != AppState::Closing;

    // Application update
    {
        update(deltaTime);
        m_UpdateCount++;
    }

    // ImGui new frame
    // if (m_ImGuiManager)
    //     m_ImGuiManager->newFrame();

    // Debug draw
    debugDraw();

    // ImGui render
    // if (m_ImGuiManager)
    //     m_ImGuiManager->render([this]() { imguiRender(); });

    // Scene render
    render();

    // Window update
    if (m_Window)
        m_Window->onUpdate();

    m_FrameCount++;

    // FPS calculation (once per second)
    double now = m_Timer->getElapsedSeconds() + m_SecondTimer;
    if (now - m_SecondTimer > 1.0)
    {
        auto& stats = Engine::get().statistics();
        stats.FramesPerSecond  = m_FrameCount;
        stats.UpdatesPerSecond = m_UpdateCount;
        m_FrameCount  = 0;
        m_UpdateCount = 0;
        m_SecondTimer = now;
    }

    return m_State != AppState::Closing;
}

void Application::quit()
{
    fprintf(stdout, "[Application] Shutting down...\n");
    m_State = AppState::Closing;

    // Release in reverse order
    // m_Renderer.reset();     // Phase C
    // m_ImGuiManager.reset(); // Phase C
    m_SceneManager.reset();
    m_Window.reset();

    // Shut down renderer before other subsystems
    if (m_Renderer)
    {
        m_Renderer->stopRenderThread();
        m_Renderer.reset();
    }

    m_SceneManager.reset();
    m_Window.reset();

    Input::release();
    Engine::release();

    delete this;  // Self-destruct after cleanup
}

void Application::update(float /*deltaTime*/)
{
    // Override in subclass
}

void Application::render()
{
    if (!m_Renderer)
        return;

    float deltaTime = Engine::timeStep().getSecondsF();
    auto extent    = getWindowSize();
    auto packet    = m_Renderer->buildFramePacket(deltaTime, extent);
    m_Renderer->enqueueFramePacket(std::move(packet));
}

void Application::imguiRender()
{
    // Override in subclass
}

void Application::debugDraw()
{
    // Override in subclass
}

void Application::handleEvent(Event& e)
{
    EventDispatcher dispatcher(e);

    dispatcher.dispatch<WindowCloseEvent>([this](WindowCloseEvent& ev) {
        onWindowClose(ev);
    });

    dispatcher.dispatch<WindowResizeEvent>([this](WindowResizeEvent& ev) {
        onWindowResize(ev);
    });

    // Forward to ImGui first (it consumes events when hovering UI)
    // if (m_ImGuiManager)
    //     m_ImGuiManager->handleEvent(e);

    if (e.handled())
        return;

    // Forward to input system
    Input::get().onEvent(e);
}

bool Application::onWindowClose(WindowCloseEvent& /*e*/)
{
    m_State = AppState::Closing;
    return true;
}

bool Application::onWindowResize(WindowResizeEvent& e)
{
    int w = e.getWidth();
    int h = e.getHeight();

    if (w == 0 || h == 0)
    {
        m_Minimized = true;
        return false;
    }

    m_Minimized = false;
    fprintf(stdout, "[Application] Window resized to %dx%d\n", w, h);

    // if (m_Renderer)
    //     m_Renderer->handleResize(w, h);  // Phase C

    return false;
}

std::array<uint32_t, 2> Application::getWindowSize() const
{
    if (!m_Window)
        return {0, 0};
    return {m_Window->getWidth(), m_Window->getHeight()};
}

float Application::getWindowDPIScale() const
{
    if (!m_Window)
        return 1.0f;
    return m_Window->getDPIScale();
}

void Application::release()
{
    if (s_Instance)
    {
        // Application manages its own lifetime via quit()
        s_Instance = nullptr;
    }
}

void Application::processEventQueue()
{
    std::scoped_lock<std::mutex> lock(m_EventMutex);
    while (!m_EventQueue.empty())
    {
        auto& func = m_EventQueue.front();
        func();
        m_EventQueue.pop();
    }
}

} // namespace caustica
