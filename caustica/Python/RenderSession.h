// RenderSession is the offline / extension-mode counterpart to the desktop editor.
// It wraps EngineApp and lets Python step frames / dump the framebuffer.

#pragma once

#if CAUSTICA_WITH_PYTHON

#include <engine/EngineApp.h>

#include <memory>
#include <string>
#include <cstdint>

#include <math/math.h>

#if CAUSTICA_WITH_DX12 && defined(CAUSTICA_D3D_AGILITY_SDK_VERSION)
#include <d3d12.h>
#include <wrl/client.h>
#endif

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

    caustica::App* GetApp() { return m_engine ? &m_engine->app() : nullptr; }
    const caustica::App* GetApp() const { return m_engine ? &m_engine->app() : nullptr; }

    caustica::EngineApp* GetEngine() { return m_engine.get(); }
    const caustica::EngineApp* GetEngine() const { return m_engine.get(); }

    caustica::render::RenderAppState& renderAppState() { return m_engine->renderAppState(); }
    const caustica::render::RenderAppState& renderAppState() const { return m_engine->renderAppState(); }

    caustica::GpuDevice* getGpuDevice() { return m_engine ? m_engine->device() : nullptr; }
    const Config&              GetConfig() const  { return m_config; }

private:
    void shutdown();

    Config                                          m_config;
    std::unique_ptr<caustica::EngineApp>            m_engine;
#if CAUSTICA_WITH_DX12 && defined(CAUSTICA_D3D_AGILITY_SDK_VERSION)
    Microsoft::WRL::ComPtr<ID3D12DeviceFactory>     m_d3d12DeviceFactory;
#endif
    bool                                            m_initialized = false;
    double                                          m_lastTimeSeconds = 0.0;
    uint32_t                                        m_lastRenderedBackBufferIndex = UINT32_MAX;
};

#endif // CAUSTICA_WITH_PYTHON
