#include "EditorLaunch.h"

#include "EditorPlugin.h"
#include "EditorStartup.h"
#include "common/LocalConfig.h"
#include "ui/RenderSettingsConsole.h"

#include <engine/App.h>
#include <engine/SceneLifecycle.h>
#include <backend/GpuSurface.h>
#include <core/console/ConsoleObjects.h>
#include <core/log.h>
#include <platform/window.h>
#include <render/passes/debug/Korgi.h>

#include <memory>
#include <string>

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

    std::string preferredScene = "default.scene.json";
    LocalConfig::PreferredSceneOverride(preferredScene);

    caustica::EngineAppDesc desc{};
    desc.scene = preferredScene;
    desc.windowTitle = "caustica";

    if (!ProcessEditorStartupCommandLine(argc, argv, desc, editor.editorCmdLine(), preferredScene))
        return nullptr;

    desc.scene = preferredScene;
    desc.preGpuDeviceInit = preGpuDeviceInit;
    desc.hasSceneCallbacks = true;
    desc.sceneCallbacks = EngineSceneCallbacks{
        .OnSceneLoaded = [&editor]() {
            editor.onSceneLoadedFromLoader();
        },
        .OnSceneUnloading = [&editor]() {
            if (App* editorApp = editor.app())
                caustica::onSceneUnloading(*editorApp);
            editor.onSceneUnloading();
        },
    };

    auto engine = caustica::EngineApp::create(std::move(desc));
    if (!engine || !engine->isValid())
        return nullptr;

    editor.bindEngine(*engine);

    InitializeEditorUIDataFromCommandLine(editor.uiData(), editor.cmdLine());
    editor.setConsole(std::make_unique<RenderSettingsConsoleBinding>(
        editor.uiData(),
        [&editor]() -> caustica::App* { return editor.app(); }));
    ApplyConsoleSets(editor, true);

    const bool automatedRun = editor.cmdLine().nonInteractive
        || editor.cmdLine().captureSimple
        || editor.cmdLine().captureSequence;

    if (automatedRun && engine->surface())
        engine->surface()->setWaitForIdleAfterPresent(true);

    caustica::App* app = &engine->app();
    if (!editor.cmdLine().noWindow)
    {
        EditorUISubsystemConfig uiConfig{
            .app = *app,
            .sceneEditor = editor,
            .editorUiData = editor.uiData(),
            .cmdLine = editor.cmdLine(),
            .console = *editor.console(),
        };
        app->addPlugin<EditorPlugin>(editor, &uiConfig);
    }
    else
    {
        app->addPlugin<EditorPlugin>(editor, static_cast<const EditorUISubsystemConfig*>(nullptr));
    }

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
