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

bool startupEditor(caustica::App& app, EditorHost& host, int argc, const char* const* argv)
{
    korgi::init();
    installEditorLogFilter(host);

    GpuDeviceCreateDesc createDesc{};
    std::string preferredScene = "default.json";
    LocalConfig::PreferredSceneOverride(preferredScene);

    if (!ProcessEditorStartupCommandLine(argc, argv, host.cmdLine, createDesc, preferredScene))
        return false;

    createDesc.headless = host.cmdLine.noWindow;
    createDesc.windowTitle = g_windowTitle ? g_windowTitle : "caustica";

    if (!app.initializeGraphics(argc, argv, createDesc))
        return false;

    app.setUseDedicatedRenderThread(!host.cmdLine.syncRender);

    const bool automatedRun = host.cmdLine.nonInteractive
        || host.cmdLine.captureSimple
        || host.cmdLine.captureSequence;

    if (automatedRun)
    {
        caustica::App::FrameCallback previousAfterPresent = app.afterPresent;
        app.afterPresent = [previousAfterPresent](GpuDevice& device, uint32_t frameIndex) {
            if (previousAfterPresent)
                previousAfterPresent(device, frameIndex);

            const bool waitOk = device.getDevice()->waitForIdle();
            if (!waitOk)
                error("Automated run frame sync detected device loss after frame %u", frameIndex);
        };
    }

    const SceneAppConfig sceneConfig{
        .viewState = host.sceneEditor.viewState(),
        .diagnostics = host.diagnostics,
        .preferredScene = preferredScene,
        .renderState = &host.editorUiData.render,
        .cmdLine = &host.cmdLine,
        .applyCmdLineToRenderState = host.cmdLine.noWindow || automatedRun,
    };

    if (!host.cmdLine.noWindow)
    {
        EditorUISubsystemConfig uiConfig{
            .app = app,
            .sceneEditor = host.sceneEditor,
            .editorUiData = host.editorUiData,
            .cmdLine = host.cmdLine,
        };
        app.addPlugin<EditorPlugin>(sceneConfig, host.sceneEditor, &uiConfig);
    }
    else
    {
        app.addPlugin<EditorPlugin>(sceneConfig, host.sceneEditor, static_cast<const EditorUISubsystemConfig*>(nullptr));
    }

    app.setEventHandler([&host, &app](Event& event) {
        host.sceneEditor.onEvent(event);

        EventDispatcher dispatcher(event);
        dispatcher.dispatch<WindowCloseEvent>([&app](WindowCloseEvent&) {
            app.requestExit();
            return true;
        });
    });

    app.setDisplayScaleHandler([&app](float scaleX, float scaleY) {
        if (auto* uiSubsystem = app.tryResource<EditorUISubsystem>())
            uiSubsystem->onDisplayScaleChanged(scaleX, scaleY);
    });

    app.setBackBufferResizeHandler([&app](bool resizing, uint32_t width, uint32_t height, uint32_t sampleCount) {
        auto* uiSubsystem = app.tryResource<EditorUISubsystem>();
        if (!uiSubsystem)
            return;

        if (resizing)
            uiSubsystem->onBackBufferResizing();
        else
            uiSubsystem->onBackBufferResized(width, height, sampleCount);
    });

    if (!app.finishStartup())
        return false;

    return true;
}

} // namespace caustica::editor
