// RenderSession is the offline / extension-mode counterpart to EditorApplication.
// Where EditorApplication drives a blocking GLFW message loop, RenderSession
// initialises the same GpuDevice + AdvancedPathTracer pipeline but lets
// Python step frames manually and dump the framebuffer to disk.
//
// Usage from C++:
//    RenderSession::Config cfg;
//    cfg.width = 1280; cfg.height = 720; cfg.headless = true;
//    auto session = std::make_unique<RenderSession>(cfg);
//    session->LoadScene("bistro-programmer-art.scene.json");
//    session->StepN(64);
//    session->SaveScreenshot("out.png");
//
// Headless = no OS window or swap chain is created. The renderer uses
// offscreen back buffers owned by GpuDevice.

#pragma once

#if CAUSTICA_WITH_PYTHON

#include <memory>
#include <string>
#include <vector>
#include <cstdint>

#include <math/math.h>
#include <engine/SceneManager.h>
#include <render/Core/RenderCore.h>
#include <platform/window.h>
#include <render/Core/CommonRenderPasses.h>
#include <render/Core/BindingCache.h>
#include <render/Core/DescriptorTableManager.h>
#include <render/WorldRenderer/WorldRendererServices.h>
#include <assets/cache/TextureCache.h>

#include <core/command_line.h>
#include <SampleUI.h>

#if CAUSTICA_WITH_DX12 && defined(CAUSTICA_D3D_AGILITY_SDK_VERSION)
#include <wrl/client.h>
#endif

class PathTracerApp;
namespace caustica { class ShaderFactory; }
namespace caustica::render { class PathTracingWorldRenderer; }

namespace caustica_py
{
    // Returns a minimal scene JSON string that references an RTXPT builtin
    // primitive model ("plane", "cube", "sphere", or "plane_cube").
    std::string BuiltinSceneJson(const std::string& builtinModel = "plane_cube");
}

class RenderSession
{
public:
    struct Config
    {
        int         width             = 1920;
        int         height            = 1080;
        bool        headless          = true;     // render into offscreen back buffers
#if CAUSTICA_WITH_DX12
        bool        useVulkan         = false;    // false => DX12
#else
        bool        useVulkan         = true;     // Vulkan is the only backend on Linux/WSL
#endif
        int         adapterIndex      = -1;       // -1 => default
        bool        debug             = false;
        bool        nonInteractive    = true;     // do not popup blocking dialogs
        std::string scene;                        // optional initial scene
        bool        realtimeMode      = false;    // start in reference (accumulation) mode
        int         accumulationTarget = 64;      // SPP target in reference mode
    };

    explicit RenderSession(const Config& cfg);
    ~RenderSession();

    RenderSession(const RenderSession&) = delete;
    RenderSession& operator=(const RenderSession&) = delete;

    // Loads a scene by relative path (e.g. "bistro-programmer-art.scene.json").
    // Returns true on success.  If a scene is already loaded, it gets replaced.
    bool LoadScene(const std::string& sceneName, bool waitUntilReady = true);

    // Wait until the requested scene has fully finished loading and the engine
    // has caught up (acceleration structures, materials, lights all updated).
    // If `maxFrames` > 0, gives up after that many internal frames.
    bool WaitUntilReady(int maxFrames = 256);

    // Single frame.  `dt` is the simulation delta time in seconds.  Pass a
    // negative value to use the wall clock in windowed mode; headless sessions
    // use a fixed 1/60 s step for deterministic offline rendering.
    bool Step(float dt = -1.0f);

    // Convenience: call Step() N times.
    bool StepN(int frames);

    // Convenience: keep stepping until rendering is "settled" (accumulation
    // target reached in reference mode, or `maxFrames` frames produced in
    // realtime mode).  Returns the number of frames actually executed.
    int  StepUntilAccumulated(int maxFrames = 0);

    // Dumps the current backbuffer to disk.  Supported formats: BMP/PNG/JPG/TGA.
    bool SaveScreenshot(const std::string& outputPath);

    // Convenience helpers for camera control (mirror the embed-mode API).
    bool SetCamera(const caustica::math::float3& pos,
                   const caustica::math::float3& dir,
                   const caustica::math::float3& up);
    void SetCameraFOV(float verticalFovDegrees);
    void SetCameraIntrinsics(float fx, float fy, float cx, float cy, float width, float height);

    PathTracerApp*       GetPathTracerApp()       { return m_renderer.get(); }
    const PathTracerApp* GetPathTracerApp() const { return m_renderer.get(); }

    SampleUIData& GetSampleUIData() { return m_sampleUIData; }
    const SampleUIData& GetSampleUIData() const { return m_sampleUIData; }

    caustica::GpuDevice* GetGpuDevice() { return m_deviceManager.get(); }
    const Config&              GetConfig() const  { return m_config; }

private:
    bool InitDevice();
    bool InitRenderer();
    void initRenderInfrastructurePhase1();
    void initRenderInfrastructurePhase2(nvrhi::IBindingLayout* bindlessLayout);
    caustica::render::WorldRendererServices buildWorldRendererServices();
    void initWorldRenderer(nvrhi::IBindingLayout* bindlessLayout);
    void initSceneServices();
    void Shutdown();

    Config                                          m_config;
    CommandLineOptions                              m_cmdLine;
    SampleUIData                                    m_sampleUIData;
    std::unique_ptr<caustica::GpuDevice>      m_deviceManager;
    std::unique_ptr<caustica::Window>            m_Window;
    std::unique_ptr<caustica::Application>         m_AppLoop;
    std::shared_ptr<caustica::ShaderFactory>   m_shaderFactory;
    std::shared_ptr<caustica::CommonRenderPasses> m_commonPasses;
    std::unique_ptr<caustica::BindingCache> m_bindingCache;
    std::shared_ptr<caustica::DescriptorTableManager> m_descriptorTable;
    std::shared_ptr<caustica::TextureCache> m_textureCache;
    std::unique_ptr<caustica::render::WorldRendererServices> m_worldRendererServices;
    std::unique_ptr<caustica::RenderCore>      m_renderCore;
    std::unique_ptr<SceneManager>            m_sceneManager;
    std::unique_ptr<PathTracerApp>             m_renderer;
    std::unique_ptr<caustica::render::PathTracingWorldRenderer> m_worldRenderer;
#if CAUSTICA_WITH_DX12 && defined(CAUSTICA_D3D_AGILITY_SDK_VERSION)
    Microsoft::WRL::ComPtr<ID3D12DeviceFactory>     m_d3d12DeviceFactory;
#endif
    bool                                            m_initialized = false;
    double                                          m_lastTimeSeconds = 0.0;
    uint32_t                                        m_lastRenderedBackBufferIndex = UINT32_MAX;
};

#endif // CAUSTICA_WITH_PYTHON
