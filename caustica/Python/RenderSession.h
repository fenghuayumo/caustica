// RenderSession is the offline / extension-mode counterpart to EditorApplication.
// Where EditorApplication drives a blocking GLFW message loop, RenderSession
// initialises the same GpuDevice + EngineRenderer pipeline but lets
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
#include <platform/window.h>
#include <render/SceneGaussianSplatPasses.h>
#include <render/SceneLightingPasses.h>
#include <render/SceneRayTracingResources.h>
#include <render/WorldRenderer/PathTracingContext.h>

#include <core/command_line.h>
#include <EditorUI.h>

#if CAUSTICA_WITH_DX12 && defined(CAUSTICA_D3D_AGILITY_SDK_VERSION)
#include <wrl/client.h>
#endif

namespace caustica
{
class Application;
class EngineRenderer;
class GpuDevice;
}

namespace caustica::editor
{
class SceneEditor;
struct SampleUIData;
}

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

    caustica::editor::SceneEditor*       GetSceneEditor()       { return m_renderer.get(); }
    const caustica::editor::SceneEditor* GetSceneEditor() const { return m_renderer.get(); }

    caustica::editor::SampleUIData& GetSampleUIData() { return m_sampleUIData; }
    const caustica::editor::SampleUIData& GetSampleUIData() const { return m_sampleUIData; }

    caustica::GpuDevice* GetGpuDevice() { return m_deviceManager.get(); }
    const Config&              GetConfig() const  { return m_config; }

private:
    bool InitDevice();
    bool InitRenderer();
    caustica::render::PathTracingHooks buildPathTracingHooks();
    caustica::render::PathTracingContext buildPathTracingContext();
    void Shutdown();

    Config                                          m_config;
    CommandLineOptions                              m_cmdLine;
    caustica::editor::SampleUIData                    m_sampleUIData;
    caustica::editor::SceneLightingPasses           m_lightingPasses;
    caustica::editor::SceneRayTracingResources      m_rayTracingResources;
    caustica::editor::SceneGaussianSplatPasses      m_gaussianSplatPasses;
    std::unique_ptr<caustica::GpuDevice>      m_deviceManager;
    std::unique_ptr<caustica::Window>            m_Window;
    std::unique_ptr<caustica::Application>         m_AppLoop;
    std::unique_ptr<caustica::EngineRenderer>   m_engineRenderer;
    std::unique_ptr<caustica::editor::SceneEditor>             m_renderer;
#if CAUSTICA_WITH_DX12 && defined(CAUSTICA_D3D_AGILITY_SDK_VERSION)
    Microsoft::WRL::ComPtr<ID3D12DeviceFactory>     m_d3d12DeviceFactory;
#endif
    bool                                            m_initialized = false;
    double                                          m_lastTimeSeconds = 0.0;
    uint32_t                                        m_lastRenderedBackBufferIndex = UINT32_MAX;
};

#endif // CAUSTICA_WITH_PYTHON
