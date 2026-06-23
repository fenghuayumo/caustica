#pragma once

#include "engine/engine.h"
#include "engine/input.h"
#include "engine/scene_manager.h"
#include "engine/events/application_event.h"
#include "platform/window.h"
#include "platform/timer.h"

#include <memory>
#include <functional>
#include <queue>
#include <mutex>
#include <string>
#include <array>
#include <cstdint>

// Forward-declare nvrhi types outside caustica namespace
namespace nvrhi {
    class IDevice;
    class ISwapchain;
}

namespace caustica
{

// Application state
enum class AppState
{
    Loading,
    Running,
    Closing
};

// Forward declarations
class Renderer;

// Main application controller — owns all subsystems and drives the main loop.
// The client (editor/) subclasses this and implements virtual callbacks.
//
// Created by createApplication() in entry_point.h
class Application
{
public:
    Application();
    virtual ~Application();

    // --- Lifecycle (called from entry_point.h) ---
    void run();
    bool frame();
    void quit();

    // --- Virtual callbacks (override in client subclass) ---
    virtual void init();
    virtual void update(float deltaTime);
    virtual void render();
    virtual void imguiRender();
    virtual void debugDraw();
    virtual void handleEvent(Event& e);

    // Window event handlers
    virtual bool onWindowClose(WindowCloseEvent& e);
    virtual bool onWindowResize(WindowResizeEvent& e);

    // --- Project settings ---
    struct ProjectSettings
    {
        std::string ProjectRoot;
        std::string ProjectName;
        std::string EngineAssetPath;
        uint32_t Width       = 1280;
        uint32_t Height      = 720;
        bool Fullscreen      = false;
        bool VSync           = true;
        bool Borderless      = false;
        bool ShowConsole     = true;
        std::string Title    = "caustica";
        int  RenderAPI       = 1;       // nvrhi::GraphicsAPI
        int  ProjectVersion  = 1;
        int  DesiredGPUIndex = -1;
        std::string IconPath;
        bool DefaultIcon     = true;
    };

    ProjectSettings& settings() { return m_Settings; }

    // --- Subsystem access ---
    Window*       getWindow()       const { return m_Window.get(); }
    SceneManager* getSceneManager() const { return m_SceneManager.get(); }
    Renderer*     getRenderer()     const { return m_Renderer.get(); }

    // Initialize the renderer (called after GPU device is created by subclass)
    virtual void initRenderer(nvrhi::IDevice* device, nvrhi::ISwapchain* swapchain);

    // --- Window helpers ---
    std::array<uint32_t, 2> getWindowSize() const;
    float getWindowDPIScale() const;

    // --- App state ---
    AppState getState() const { return m_State; }
    void     setState(AppState s) { m_State = s; }
    bool     isMinimized() const { return m_Minimized; }

    // --- Event queue (thread-safe event dispatch) ---
    template <typename F>
    void queueEvent(F&& func)
    {
        std::scoped_lock<std::mutex> lock(m_EventMutex);
        m_EventQueue.push(std::forward<F>(func));
    }

    template <typename TEvent, typename... TArgs>
    void dispatchEvent(TArgs&&... args)
    {
        auto event = std::make_unique<TEvent>(std::forward<TArgs>(args)...);
        handleEvent(*event);
    }

    // --- Static access ---
    static Application& get() { return *s_Instance; }
    static void release();

    // --- Frame counting ---
    uint32_t getFrameCount() const { return m_FrameCount; }

    // --- Scene view dimensions (for render target sizing) ---
    std::tuple<uint32_t, uint32_t> getSceneViewDimensions() const
    {
        return { m_SceneViewWidth, m_SceneViewHeight };
    }
    void setSceneViewDimensions(uint32_t width, uint32_t height)
    {
        m_SceneViewWidth  = width;
        m_SceneViewHeight = height;
    }

protected:
    // --- Owned subsystems ---
    std::unique_ptr<Window>       m_Window;
    std::unique_ptr<Timer>        m_Timer;
    std::unique_ptr<SceneManager> m_SceneManager;
    std::unique_ptr<Renderer>     m_Renderer;

    ProjectSettings m_Settings;

    // Frame state
    uint32_t m_FrameCount      = 0;
    uint32_t m_UpdateCount     = 0;
    double   m_SecondTimer     = 0.0;
    bool     m_Minimized       = false;
    AppState m_State           = AppState::Loading;

    // Scene view dimensions
    uint32_t m_SceneViewWidth  = 0;
    uint32_t m_SceneViewHeight = 0;

    // Thread-safe event queue
    std::mutex m_EventMutex;
    std::queue<std::function<void()>> m_EventQueue;

    static Application* s_Instance;

private:
    void processEventQueue();
};

// Defined by the client application (editor/)
Application* createApplication();

} // namespace caustica
