// RenderSession is the offline / extension-mode counterpart to the desktop editor.
// Where the editor drives a blocking GLFW message loop via App::run(), RenderSession
// initialises the same GpuDevice + DefaultPlugins pipeline but lets Python step
// frames manually and dump the framebuffer to disk.

#pragma once

#if CAUSTICA_WITH_PYTHON

#include <engine/SceneViewState.h>

#include <memory>
#include <string>
#include <vector>
#include <cstdint>

#include <math/math.h>
#include <platform/window.h>
#include <render/RenderSessionState.h>
#include <render/SessionDiagnostics.h>

#include <core/command_line.h>

#if CAUSTICA_WITH_DX12 && defined(CAUSTICA_D3D_AGILITY_SDK_VERSION)
#include <d3d12.h>
#include <wrl/client.h>
#endif

namespace caustica
{
class App;
class GpuDevice;
class PathTracerSceneHost;
}

namespace caustica_py
{
    std::string BuiltinSceneJson(const std::string& builtinModel = "plane_cube");
}

class RenderSession
{
public:
    struct Config
    {
        int         width             = 1920;
        int         height            = 1080;
        bool        headless          = true;
#if CAUSTICA_WITH_DX12
        bool        useVulkan         = false;
#else
        bool        useVulkan         = true;
#endif
        int         adapterIndex      = -1;
        bool        debug             = false;
        bool        nonInteractive    = true;
        std::string scene;
        bool        realtimeMode      = false;
        int         accumulationTarget = 64;
    };

    explicit RenderSession(const Config& cfg);
    ~RenderSession();

    RenderSession(const RenderSession&) = delete;
    RenderSession& operator=(const RenderSession&) = delete;

    bool LoadScene(const std::string& sceneName, bool waitUntilReady = true);
    bool WaitUntilReady(int maxFrames = 256);
    bool Step(float dt = -1.0f);
    bool StepN(int frames);
    int  StepUntilAccumulated(int maxFrames = 0);
    bool SaveScreenshot(const std::string& outputPath);

    bool SetCamera(const caustica::math::float3& pos,
                   const caustica::math::float3& dir,
                   const caustica::math::float3& up);
    void SetCameraFOV(float verticalFovDegrees);
    void setCameraIntrinsics(float fx, float fy, float cx, float cy, float width, float height);

    caustica::App* GetApp() { return m_app.get(); }
    const caustica::App* GetApp() const { return m_app.get(); }

    caustica::PathTracerSceneHost*       GetSceneHost();
    const caustica::PathTracerSceneHost* GetSceneHost() const;

    caustica::render::RenderSessionState& renderSessionState() { return m_sessionState; }
    const caustica::render::RenderSessionState& renderSessionState() const { return m_sessionState; }

    caustica::GpuDevice* GetGpuDevice() { return m_deviceManager.get(); }
    const Config&              GetConfig() const  { return m_config; }

private:
    bool InitDevice();
    bool InitRenderer();
    void Shutdown();

    Config                                          m_config;
    CommandLineOptions                              m_cmdLine;
    caustica::render::RenderSessionState            m_sessionState;
    caustica::render::SessionDiagnostics              m_sessionDiagnostics;
    caustica::SceneViewState                        m_viewState;
    std::unique_ptr<caustica::GpuDevice>      m_deviceManager;
    std::unique_ptr<caustica::Window>            m_Window;
    std::unique_ptr<caustica::App>                 m_app;
    std::unique_ptr<caustica::PathTracerSceneHost> m_sceneHost;
#if CAUSTICA_WITH_DX12 && defined(CAUSTICA_D3D_AGILITY_SDK_VERSION)
    Microsoft::WRL::ComPtr<ID3D12DeviceFactory>     m_d3d12DeviceFactory;
#endif
    bool                                            m_initialized = false;
    double                                          m_lastTimeSeconds = 0.0;
    uint32_t                                        m_lastRenderedBackBufferIndex = UINT32_MAX;
};

#endif // CAUSTICA_WITH_PYTHON
