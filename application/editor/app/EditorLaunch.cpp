#include "EditorLaunch.h"

#include "EditorPlugin.h"
#include "EditorStartup.h"
#include "common/LocalConfig.h"
#include "ui/RenderSettingsConsole.h"

#include <engine/App.h>
#include <engine/SceneLifecycle.h>
#include <core/console/ConsoleObjects.h>
#include <core/log.h>
#include <events/application_event.h>
#include <events/event.h>
#include <platform/window.h>
#include <render/passes/debug/Korgi.h>

#include <memory>

extern const char* g_windowTitle;

namespace caustica::editor
{

namespace
{

bool ApplyConsoleSets(SceneEditor& editor, bool registeredOnly)
{
    auto* console = editor.console();
    if (!console)
        return true;

    bool success = true;
    for (const std::string& assignment : editor.cmdLine().consoleSets)
    {
        std::string command = assignment;
        if (const size_t equals = command.find('='); equals != std::string::npos)
            command[equals] = ' ';

        if (registeredOnly)
        {
            const size_t separator = command.find(' ');
            const std::string_view name(
                command.data(),
                separator == std::string::npos ? command.size() : separator);
            if (!caustica::console::findVariable(name))
                continue;
        }

        std::string output;
        if (!console->execute(
                command,
                &output,
                caustica::console::VariableState::COMMAND_LINE))
        {
            caustica::error("Failed command-line CVar assignment: %s", assignment.c_str());
            success = false;
        }
    }
    return success;
}

void ApplyConsoleExec(SceneEditor& editor)
{
    auto* console = editor.console();
    if (!console)
        return;

    for (const std::string& command : editor.cmdLine().consoleExec)
    {
        std::string output;
        if (!console->execute(
                command,
                &output,
                caustica::console::VariableState::COMMAND_LINE))
        {
            caustica::error("Failed command-line console command: %s", command.c_str());
        }
        else if (!output.empty())
        {
            caustica::info("%s", output.c_str());
        }
    }
}

} // namespace

void installEditorLogFilter()
{
    Callback defaultCallback = getCallback();
    setCallback([defaultCallback](Severity severity, const char* message) {
        if (severity == Severity::Error)
        {
            std::string msg(message);
            if (msg.find("Don't know the size") != std::string::npos)
                severity = Severity::Warning;
            if (msg.find("dlss_gEntry.cpp") != std::string::npos)
            {
                if (msg.find("Unable to find DRS context") != std::string::npos
                    || msg.find("NGX indicates DLSS-G is not available") != std::string::npos)
                    severity = Severity::Warning;
            }
            if (msg.find("Missing NGX context") != std::string::npos
                || msg.find("Unable to find NGX ") != std::string::npos
                || msg.find("NvAPI_D3D_Sleep") != std::string::npos)
                severity = Severity::Warning;
        }

        if (defaultCallback)
            defaultCallback(severity, message);
    });
}

std::unique_ptr<caustica::EngineApp> createEditorEngine(
    SceneEditor& editor,
    int argc,
    const char* const* argv,
    caustica::AppHook preGpuDeviceInit)
{
    korgi::init();
    installEditorLogFilter();

    GpuDeviceCreateDesc createDesc{};
    std::string preferredScene = "default.json";
    LocalConfig::PreferredSceneOverride(preferredScene);

    if (!ProcessEditorStartupCommandLine(argc, argv, editor.cmdLine(), createDesc, preferredScene))
        return nullptr;

    InitializeEditorUIDataFromCommandLine(editor.uiData(), editor.cmdLine());
    editor.setConsole(std::make_unique<RenderSettingsConsoleBinding>(editor.uiData()));
    ApplyConsoleSets(editor, true);

    const bool automatedRun = editor.cmdLine().nonInteractive
        || editor.cmdLine().captureSimple
        || editor.cmdLine().captureSequence;

    // Owned by SceneRuntimePlugin via EngineAppDesc — single source for Scene.Startup.
    EngineSceneCallbacks sceneCallbacks{
        .OnSceneLoaded = [&editor]() {
            editor.onSceneLoadedFromLoader();
        },
        .OnSceneUnloading = [&editor]() {
            if (App* editorApp = editor.app())
                caustica::onSceneUnloading(*editorApp);
            editor.onSceneUnloading();
        },
    };

    caustica::EngineAppDesc desc{};
    desc.width = createDesc.backBufferWidth;
    desc.height = createDesc.backBufferHeight;
    desc.headless = editor.cmdLine().noWindow;
    desc.dedicatedRenderThread = !editor.cmdLine().syncRender;
    desc.debugDevice = editor.cmdLine().debug || createDesc.enableDebug;
    desc.adapterIndex = editor.cmdLine().adapterIndex;
    desc.useVulkan = editor.cmdLine().useVulkan;
    desc.fullscreen = editor.cmdLine().fullscreen;
    desc.scene = preferredScene;
    desc.windowTitle = g_windowTitle ? g_windowTitle : "caustica";
    desc.viewState = &editor.viewState();
    desc.diagnostics = &editor.diagnostics();
    desc.renderState = &editor.uiData().render;
    desc.cmdLine = &editor.cmdLine();
    desc.applyCmdLineToRenderState = editor.cmdLine().noWindow || automatedRun;
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
        .viewState = editor.viewState(),
        .diagnostics = editor.diagnostics(),
        .preferredScene = preferredScene,
        .renderState = &editor.uiData().render,
        .cmdLine = &editor.cmdLine(),
        .applyCmdLineToRenderState = editor.cmdLine().noWindow || automatedRun,
    };

    if (!editor.cmdLine().noWindow)
    {
        EditorUISubsystemConfig uiConfig{
            .app = *app,
            .sceneEditor = editor,
            .editorUiData = editor.uiData(),
            .cmdLine = editor.cmdLine(),
            .console = *editor.console(),
        };
        app->addPlugin<EditorPlugin>(sceneConfig, editor, &uiConfig);
    }
    else
    {
        app->addPlugin<EditorPlugin>(sceneConfig, editor, static_cast<const EditorUISubsystemConfig*>(nullptr));
    }

    app->setEventHandler([&editor, app](Event& event) {
        editor.onEvent(event);

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

    // Re-apply assignments after scene/subsystem startup so late registrations
    // and scene-provided render settings still receive command-line priority.
    ApplyConsoleSets(editor, false);
    ApplyConsoleExec(editor);
    caustica::console::lockStartupVariables();

    return engine;
}

} // namespace caustica::editor
