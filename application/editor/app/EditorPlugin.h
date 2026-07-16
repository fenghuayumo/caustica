#pragma once

#include <engine/DefaultPlugins.h>
#include <engine/Plugin.h>
#include <engine/SceneStartup.h>

#include "EditorSceneStartup.h"
#include "EditorUISubsystem.h"
#include "SceneEditor.h"

#include <optional>

namespace caustica::editor
{

// Editor delta on top of DefaultPlugins (GPU + scene bootstrap + SceneAccess).
struct EditorPlugin : Plugin
{
    EditorPlugin(SceneAppConfig appConfig,
        SceneEditor& sceneEditor,
        const EditorUISubsystemConfig* uiConfig = nullptr)
        : defaults(std::move(appConfig))
        , m_sceneEditor(sceneEditor)
        , uiConfig(uiConfig
            ? std::optional<EditorUISubsystemConfig>(*uiConfig)
            : std::nullopt)
    {
    }

    void build(App& app) override;
    void configureSchedules(App& app) override;
    void configureLateSchedules(App& app) override;

    DefaultPlugins defaults;
    SceneEditor& m_sceneEditor;
    std::optional<EditorUISubsystemConfig> uiConfig;
};

} // namespace caustica::editor
