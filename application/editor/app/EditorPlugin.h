#pragma once

#include <engine/App.h>
#include <engine/EngineScheduleRegistration.h>
#include <engine/SceneSessionResources.h>
#include <engine/GpuRenderSubsystem.h>
#include <engine/Plugin.h>
#include <engine/SceneRuntimeSubsystem.h>

#include "EditorSceneSubsystem.h"
#include "EditorUISubsystem.h"

namespace caustica::editor
{

struct EditorPlugin : Plugin
{
    EditorPlugin(SceneRuntimeSubsystemConfig sceneConfig,
        const EditorUISubsystemConfig* uiConfig = nullptr)
        : sceneConfig(std::move(sceneConfig))
        , uiConfig(uiConfig)
    {
    }

    void build(App& app) override
    {
        registerSceneSessionResources(app, sceneConfig);
        app.emplaceSubsystem<GpuRenderSubsystem>();
        app.emplaceSubsystem<EditorSceneSubsystem>(EditorSceneSubsystemConfig{
            .runtime = sceneConfig,
        });

        if (uiConfig)
            app.emplaceSubsystem<EditorUISubsystem>(*uiConfig);
    }

    void configureSchedules(App& app) override
    {
        registerEngineScheduleBridge(app);
    }

    SceneRuntimeSubsystemConfig sceneConfig;
    const EditorUISubsystemConfig* uiConfig = nullptr;
};

} // namespace caustica::editor
