#include "EditorLaunch.h"

#include "EditorPlugin.h"
#include "EditorStartup.h"
#include "common/LocalConfig.h"

#include <engine/App.h>
#include <engine/SceneLifecycle.h>
#include <core/log.h>
#include <events/application_event.h>
#include <events/event.h>
#include <platform/window.h>
#include <render/passes/debug/Korgi.h>

#include <memory>

extern const char* g_windowTitle;

namespace caustica::editor
{

std::unique_ptr<caustica::EngineApp> createEditorEngine(
    EditorHost& host,
    int argc,
    const char* const* argv,
    caustica::AppHook preGpuDeviceInit)
{
    korgi::init();
    installEditorLogFilter(host);

    GpuDeviceCreateDesc createDesc{};
    std::string preferredScene = "default.json";
    LocalConfig::PreferredSceneOverride(preferredScene);

    if (!ProcessEditorStartupCommandLine(argc, argv, host.cmdLine, createDesc, preferredScene))
        return nullptr;

    const bool automatedRun = host.cmdLine.nonInteractive
        || host.cmdLine.captureSimple
        || host.cmdLine.captureSequence;

    // Owned by SceneRuntimePlugin via EngineAppDesc — single source for Scene.Startup.
    EngineSceneCallbacks sceneCallbacks{
        .OnSceneLoaded = [&host]() {
            host.sceneEditor.onSceneLoadedFromLoader();
        },
        .OnSceneUnloading = [&host]() {
            if (App* editorApp = host.sceneEditor.app())
                caustica::onSceneUnloading(*editorApp);
            host.sceneEditor.onSceneUnloading();
        },
    };

    caustica::EngineAppDesc desc{};
    desc.width = createDesc.backBufferWidth;
    desc.height = createDesc.backBufferHeight;
    desc.headless = host.cmdLine.noWindow;
    desc.dedicatedRenderThread = !host.cmdLine.syncRender;
    desc.debugDevice = host.cmdLine.debug || createDesc.enableDebug;
    desc.adapterIndex = host.cmdLine.adapterIndex;
    desc.useVulkan = host.cmdLine.useVulkan;
    desc.fullscreen = host.cmdLine.fullscreen;
    desc.scene = preferredScene;
    desc.windowTitle = g_windowTitle ? g_windowTitle : "caustica";
    desc.finishStartup = false;
    desc.viewState = &host.sceneEditor.viewState();
    desc.diagnostics = &host.diagnostics;
    desc.renderState = &host.editorUiData.render;
    desc.cmdLine = &host.cmdLine;
    desc.applyCmdLineToRenderState = host.cmdLine.noWindow || automatedRun;
    desc.hasSceneCallbacks = true;
    desc.sceneCallbacks = std::move(sceneCallbacks);
    desc.preGpuDeviceInit = preGpuDeviceInit;

    auto engine = caustica::EngineApp::create(std::move(desc));
    if (!engine || !engine->isValid())
        return nullptr;

    caustica::App* app = &engine->app();

    if (automatedRun)
    {
        caustica::App::FrameCallback previousAfterPresent = app->afterPresent;
        app->afterPresent = [previousAfterPresent](GpuDevice& device, uint32_t frameIndex) {
            if (previousAfterPresent)
                previousAfterPresent(device, frameIndex);

            const bool waitOk = device.getDevice()->waitForIdle();
            if (!waitOk)
                error("Automated run frame sync detected device loss after frame %u", frameIndex);
        };
    }

    // EditorPlugin only needs renderState for PostAppInit; scene callbacks live on EngineAppDesc.
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
            .app = *app,
            .sceneEditor = host.sceneEditor,
            .editorUiData = host.editorUiData,
            .cmdLine = host.cmdLine,
        };
        app->addPlugin<EditorPlugin>(sceneConfig, host.sceneEditor, &uiConfig);
    }
    else
    {
        app->addPlugin<EditorPlugin>(sceneConfig, host.sceneEditor, static_cast<const EditorUISubsystemConfig*>(nullptr));
    }

    app->setEventHandler([&host, app](Event& event) {
        host.sceneEditor.onEvent(event);

        EventDispatcher dispatcher(event);
        dispatcher.dispatch<WindowCloseEvent>([app](WindowCloseEvent&) {
            app->requestExit();
            return true;
        });
    });

    app->setDisplayScaleHandler([app](float scaleX, float scaleY) {
        if (auto* uiSubsystem = app->tryResource<EditorUISubsystem>())
            uiSubsystem->onDisplayScaleChanged(scaleX, scaleY);
    });

    app->setBackBufferResizeHandler([app](bool resizing, uint32_t width, uint32_t height, uint32_t sampleCount) {
        auto* uiSubsystem = app->tryResource<EditorUISubsystem>();
        if (!uiSubsystem)
            return;

        if (resizing)
            uiSubsystem->onBackBufferResizing();
        else
            uiSubsystem->onBackBufferResized(width, height, sampleCount);
    });

    if (!engine->finishStartup())
        return nullptr;

    return engine;
}

} // namespace caustica::editor
