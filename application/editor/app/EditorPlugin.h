#pragma once

#include <engine/Plugin.h>

#include "EditorSceneStartup.h"
#include "EditorUISubsystem.h"
#include "SceneEditor.h"

#include <optional>

namespace caustica::editor
{

// Editor delta on top of DefaultPlugins (GPU + scene bootstrap + ActiveScene).
struct EditorPlugin : Plugin
{
    EditorPlugin(SceneEditor& sceneEditor,
        const EditorUISubsystemConfig* uiConfig = nullptr)
        : m_sceneEditor(sceneEditor)
        , uiConfig(uiConfig
            ? std::optional<EditorUISubsystemConfig>(*uiConfig)
            : std::nullopt)
    {
    }

    void build(App& app) override;
    void configureSchedules(App& app) override;
    void configureLateSchedules(App& app) override;

    SceneEditor& m_sceneEditor;
    std::optional<EditorUISubsystemConfig> uiConfig;
};

} // namespace caustica::editor
