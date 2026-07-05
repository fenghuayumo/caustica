#include "EditorLaunch.h"

#include "EditorPlugin.h"
#include "EditorStartup.h"
#include "common/LocalConfig.h"

#include <engine/App.h>
#include <events/application_event.h>
#include <events/event.h>
#include <platform/window.h>
#include <render/passes/debug/Korgi.h>

extern const char* g_windowTitle;

namespace caustica::editor
{

bool startupEditor(caustica::App& app, EditorSession& session, int argc, const char* const* argv)
{
    korgi::Init();
    installEditorLogFilter(session);

    GpuDeviceCreateDesc createDesc{};
    std::string preferredScene = "default.json";
    LocalConfig::PreferredSceneOverride(preferredScene);

    if (!ProcessEditorStartupCommandLine(argc, argv, session.cmdLine, createDesc, preferredScene))
        return false;

    createDesc.headless = session.cmdLine.noWindow;
    createDesc.windowTitle = g_windowTitle ? g_windowTitle : "caustica";

    if (!app.initializeGraphics(argc, argv, createDesc))
        return false;

    app.setUseDedicatedRenderThread(!session.cmdLine.syncRender);

    const bool automatedRun = session.cmdLine.nonInteractive
        || session.cmdLine.captureSimple
        || session.cmdLine.captureSequence;

    if (automatedRun)
    {
        caustica::App::FrameCallback previousAfterPresent = app.afterPresent;
        app.afterPresent = [previousAfterPresent](GpuDevice& device, uint32_t frameIndex) {
            if (previousAfterPresent)
                previousAfterPresent(device, frameIndex);

            const bool waitOk = device.GetDevice()->waitForIdle();
            if (!waitOk)
                error("Automated run frame sync detected device loss after frame %u", frameIndex);
        };
    }

    const SceneRuntimeSubsystemConfig sceneConfig{
        .sceneRuntime = session.sceneEditor,
        .diagnostics = session.sessionDiagnostics,
        .preferredScene = preferredScene,
        .sessionState = &session.editorUiData.session,
        .cmdLine = &session.cmdLine,
        .applyCmdLineToSessionState = session.cmdLine.noWindow || automatedRun,
    };

    if (!session.cmdLine.noWindow)
    {
        EditorUISubsystemConfig uiConfig{
            .app = app,
            .sceneEditor = session.sceneEditor,
            .editorUiData = session.editorUiData,
            .cmdLine = session.cmdLine,
        };
        app.addPlugin<EditorPlugin>(sceneConfig, &uiConfig);
    }
    else
    {
        app.addPlugin<EditorPlugin>(sceneConfig, static_cast<const EditorUISubsystemConfig*>(nullptr));
    }

    app.setEventHandler([&session, &app](Event& event) {
        session.sceneEditor.onEvent(event);

        EventDispatcher dispatcher(event);
        dispatcher.Dispatch<WindowCloseEvent>([&app](WindowCloseEvent&) {
            if (Window* window = app.getWindow())
                window->setExit(true);
            return true;
        });
    });

    app.setDisplayScaleHandler([&app](float scaleX, float scaleY) {
        if (auto* uiSubsystem = app.engine().getSubsystem<EditorUISubsystem>())
            uiSubsystem->onDisplayScaleChanged(scaleX, scaleY);
    });

    if (!app.finishStartup())
        return false;

    return true;
}

} // namespace caustica::editor
