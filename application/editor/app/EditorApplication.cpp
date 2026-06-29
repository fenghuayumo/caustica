#include "EditorApplication.h"

#include <engine/EntryPoint.h>
#include <engine/EngineRenderer.h>
#include <backend/GpuDevice.h>

#include <events/event.h>
#include <events/key_event.h>
#include <events/mouse_event.h>
#include <events/application_event.h>

#include <imgui/imgui_renderer.h>

#include <string>
#include <render/Passes/Debug/Korgi.h>
#include <EditorUI.h>
#include "common/LocalConfig.h"
#include <core/path_utils.h>
#include <core/log.h>
#include "SceneEditor.h"
#include "PathTracerSessionBootstrap.h"
#include "EditorStartup.h"
#include <render/WorldRenderer/PathTracingWorldRenderer.h>
#include <platform/window.h>

#include <utility>

extern const char* g_windowTitle;

namespace caustica::editor
{

EditorApplication::EditorApplication()
    : Application()
    , m_sceneEditor(CmdLine, m_editorUIData, m_sessionDiagnostics)
{
    RegisterLogCallback();
    korgi::Init();
}

EditorApplication::~EditorApplication()
{
    korgi::Shutdown();
}

bool EditorApplication::init(int argc, const char* const* argv)
{
    return startup(argc, argv) == StartupResult::Success;
}

EditorApplication::StartupResult EditorApplication::startup(int argc, const char* const* argv)
{
    caustica::GpuDeviceCreateDesc createDesc{};

    std::string preferredScene = "default.json";
    LocalConfig::PreferredSceneOverride(preferredScene);

    if (!ProcessEditorStartupCommandLine(argc, argv, CmdLine, createDesc, preferredScene))
    {
        return StartupResult::FailProcessingCommandLine;
    }

    createDesc.headless = CmdLine.noWindow;
    createDesc.windowTitle = g_windowTitle ? g_windowTitle : "caustica";

    if (!initializeGraphics(argc, argv, createDesc))
    {
        return StartupResult::FailToCreateDevice;
    }

    m_sceneEditor.setGpuDevice(*m_GpuDevice);
    m_sceneEditor.initStreamlineAndWindow();

    m_engineRenderer = bootstrapPathTracerSession(PathTracerSessionBootstrapParams{
        .gpuDevice = *m_GpuDevice,
        .sceneEditor = m_sceneEditor,
        .diagnostics = m_sessionDiagnostics,
        .frameExtensions = m_frameExtensions,
        .preferredScene = preferredScene,
    });
    m_engineRenderer->lightingPasses().refreshEnvironmentMapMediaList(
        GetLocalPath(c_AssetsFolder), std::filesystem::path());
    syncPassesToBackBuffer();

    if (!CmdLine.noWindow)
    {
        m_uiPass = std::make_unique<EditorUI>(m_GpuDevice.get(), *this, m_editorUIData, IsSERSupported(), CmdLine);
        m_uiPass->Init(m_engineRenderer->shaderFactory());
        syncPassesToBackBuffer();
    }
    else
    {
        InitializeEditorUIDataFromCommandLine(m_editorUIData, CmdLine);
    }

    if (!CmdLine.noWindow && m_GpuDevice->GetPlatformWindow())
    {
        m_GpuDevice->GetPlatformWindow()->setFileDropCallback(
            [this](int count, const char** paths)
            {
                for (int i = 0; i < count; ++i)
                    m_editorUIData.PendingDroppedFiles.emplace_back(paths[i]);
            });
    }

    LocalConfig::PostAppInit(m_editorUIData);

    return StartupResult::Success;
}

void EditorApplication::shutdown()
{
    if (m_shutdownCalled)
        return;

    unbindFrameDriver(m_GpuDevice.get());
    m_uiPass.reset();
    m_engineRenderer.reset();

    if (m_GpuDevice)
        m_GpuDevice->ReleaseWindowOwnership();

    m_Window.reset();

    if (m_GpuDevice)
    {
        m_GpuDevice->Shutdown();
        m_GpuDevice.reset();
    }

    Application::shutdown();
}

void EditorApplication::RegisterLogCallback()
{
    m_DefaultLogCallback = caustica::GetCallback();

    caustica::SetCallback([this](caustica::Severity severity, const char* message)
        {
            this->SampleLogCallback(severity, message);
        });
}

void EditorApplication::SampleLogCallback(caustica::Severity severity, const char* message)
{
    if (severity == caustica::Severity::Error)
    {
        std::string msg(message);
        if (msg.find("Don't know the size") != std::string::npos)
            severity = caustica::Severity::Warning;
        if (msg.find("dlss_gEntry.cpp") != std::string::npos)
        {
            if (msg.find("Unable to find DRS context") != std::string::npos
                || msg.find("NGX indicates DLSS-G is not available") != std::string::npos)
                severity = caustica::Severity::Warning;
        }
        if (msg.find("Missing NGX context") != std::string::npos
            || msg.find("Unable to find NGX ") != std::string::npos
            || msg.find("NvAPI_D3D_Sleep") != std::string::npos)
            severity = caustica::Severity::Warning;
    }

    m_DefaultLogCallback(severity, message);
}

bool EditorApplication::IsSERSupported() const
{
    return m_GpuDevice && m_GpuDevice->SupportsShaderExecutionReordering() && !CmdLine.disableSER;
}

void EditorApplication::syncPassesToBackBuffer()
{
    if (!m_GpuDevice)
        return;

    const caustica::BackBufferInfo backBuffer = m_GpuDevice->GetBackBufferInfo();
    notifyBackBufferResizing();
    notifyBackBufferResized(backBuffer.width, backBuffer.height, backBuffer.sampleCount);
}

void EditorApplication::onUpdate(float elapsedTimeSeconds, bool windowFocused)
{
    if (windowFocused)
        m_sceneEditor.Animate(elapsedTimeSeconds);

    if (m_uiPass && (windowFocused || m_uiPass->ShouldAnimateUnfocused()))
    {
        caustica::ImGui_Renderer& ui = *m_uiPass;
        ui.Animate(elapsedTimeSeconds);
    }
}

void EditorApplication::onRender()
{
    caustica::GpuDevice* dm = getGpuDevice();
    if (!dm)
        return;

    m_sceneEditor.Render(dm->GetCurrentFramebuffer(true));

    if (m_uiPass)
        m_uiPass->Render(dm->GetCurrentFramebuffer(m_uiPass->SupportsDepthBuffer()));

    if (m_engineRenderer)
        m_engineRenderer->endFrame();
}

void EditorApplication::onBackBufferResizing()
{
    m_sceneEditor.BackBufferResizing();
    if (m_uiPass)
        m_uiPass->BackBufferResizing();
}

void EditorApplication::onBackBufferResized(uint32_t width, uint32_t height, uint32_t sampleCount)
{
    if (m_uiPass)
        m_uiPass->BackBufferResized(width, height, sampleCount);
}

void EditorApplication::onDisplayScaleChanged(float scaleX, float scaleY)
{
    if (m_uiPass)
    {
        caustica::ImGui_Renderer& ui = *m_uiPass;
        ui.DisplayScaleChanged(scaleX, scaleY);
    }
}

bool EditorApplication::shouldRenderWhenUnfocused() const
{
    return m_sceneEditor.ShouldRenderUnfocused();
}

void EditorApplication::onEvent(caustica::Event& event)
{
    m_sceneEditor.onEvent(event);

    caustica::EventDispatcher dispatcher(event);

    dispatcher.Dispatch<caustica::WindowCloseEvent>([this](caustica::WindowCloseEvent&) {
        if (m_Window)
            m_Window->setExit(true);
        return true;
    });
}

SceneManager* EditorApplication::GetSceneManager()
{
    return m_engineRenderer ? m_engineRenderer->sceneManager() : nullptr;
}

const SceneManager* EditorApplication::GetSceneManager() const
{
    return m_engineRenderer ? m_engineRenderer->sceneManager() : nullptr;
}

caustica::RenderCore* EditorApplication::GetRenderCore()
{
    return m_engineRenderer ? m_engineRenderer->renderCore() : nullptr;
}

const caustica::RenderCore* EditorApplication::GetRenderCore() const
{
    return m_engineRenderer ? m_engineRenderer->renderCore() : nullptr;
}

caustica::render::PathTracingWorldRenderer* EditorApplication::GetWorldRenderer()
{
    return m_engineRenderer ? m_engineRenderer->worldRenderer() : nullptr;
}

const caustica::render::PathTracingWorldRenderer* EditorApplication::GetWorldRenderer() const
{
    return m_engineRenderer ? m_engineRenderer->worldRenderer() : nullptr;
}

} // namespace caustica::editor
