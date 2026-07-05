#include "EditorApplication.h"

#include "EditorUISubsystem.h"

#include <engine/EntryPoint.h>
#include <backend/GpuDevice.h>

#include <events/event.h>
#include <events/application_event.h>

#include <render/passes/debug/Korgi.h>
#include "common/LocalConfig.h"
#include <core/log.h>
#include "EditorRuntime.h"
#include "EditorStartup.h"
#include <engine/SceneRuntimeSubsystem.h>
#include <optional>
#include <string>

#include <platform/window.h>

extern const char* g_windowTitle;

namespace caustica::editor
{

EditorApplication::EditorApplication()
    : m_sceneEditor(CmdLine, m_editorUIData, m_sessionDiagnostics)
{
    setEngine(&m_engine);
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
        return StartupResult::FailProcessingCommandLine;

    createDesc.headless = CmdLine.noWindow;
    createDesc.windowTitle = g_windowTitle ? g_windowTitle : "caustica";

    if (!initializeGraphics(argc, argv, createDesc))
        return StartupResult::FailToCreateDevice;

    setUseDedicatedRenderThread(!CmdLine.syncRender);

    const bool automatedRun = CmdLine.nonInteractive || CmdLine.captureSimple || CmdLine.captureSequence;
    if (automatedRun)
    {
        FrameCallback previousAfterPresent = afterPresent;
        afterPresent = [previousAfterPresent](caustica::GpuDevice& device, uint32_t frameIndex)
        {
            if (previousAfterPresent)
                previousAfterPresent(device, frameIndex);

            const bool waitOk = device.GetDevice()->waitForIdle();
            if (!waitOk)
                caustica::error("Automated run frame sync detected device loss after frame %u", frameIndex);
        };
    }

    const caustica::SceneRuntimeSubsystemConfig sceneConfig{
        .sceneRuntime = m_sceneEditor,
        .diagnostics = m_sessionDiagnostics,
        .preferredScene = preferredScene,
        .sessionState = &m_editorUIData.session,
        .cmdLine = &CmdLine,
        .applyCmdLineToSessionState = CmdLine.noWindow || automatedRun,
    };

    if (!CmdLine.noWindow)
    {
        registerEditorRuntime(m_engine, sceneConfig, EditorUISubsystemConfig{
            .editorApplication = *this,
            .sceneEditor = m_sceneEditor,
            .editorUiData = m_editorUIData,
            .cmdLine = CmdLine,
        });
    }
    else
    {
        registerEditorRuntime(m_engine, sceneConfig);
    }

    if (!m_engine.initialize(caustica::EngineInitContext{
            .gpuDevice = getGpuDevice(),
            .window = getWindow(),
            .application = this,
        }))
    {
        return StartupResult::FailToCreateDevice;
    }

    return StartupResult::Success;
}

void EditorApplication::shutdown()
{
    m_engine.shutdown();
    EngineFrameApplication::shutdown();
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

    if (m_DefaultLogCallback)
        m_DefaultLogCallback(severity, message);
}

void EditorApplication::onDisplayScaleChanged(float scaleX, float scaleY)
{
    if (auto* uiSubsystem = m_engine.getSubsystem<EditorUISubsystem>())
        uiSubsystem->onDisplayScaleChanged(scaleX, scaleY);
}

void EditorApplication::onEvent(caustica::Event& event)
{
    m_sceneEditor.onEvent(event);

    caustica::EventDispatcher dispatcher(event);

    dispatcher.Dispatch<caustica::WindowCloseEvent>([this](caustica::WindowCloseEvent&) {
        if (caustica::Window* window = getWindow())
            window->setExit(true);
        return true;
    });
}

} // namespace caustica::editor
