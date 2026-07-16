#pragma once

#include <engine/App.h>
#include <engine/EngineScheduleRegistration.h>
#include <engine/SceneAppResources.h>
#include <engine/GpuRenderSubsystem.h>
#include <engine/Plugin.h>
#include <engine/SceneStartup.h>
#include <render/worldRenderer/WorldRenderer.h>

#include "EditorSceneStartup.h"
#include "EditorUISubsystem.h"
#include "common/CaptureScriptManager.h"
#include "SceneEditor.h"

#include <imgui/imgui_renderer.h>
#include <optional>

namespace caustica::editor
{

struct EditorPlugin : Plugin
{
    EditorPlugin(SceneAppConfig appConfig,
        SceneEditor& sceneEditor,
        const EditorUISubsystemConfig* uiConfig = nullptr)
        : appConfig(std::move(appConfig))
        , m_sceneEditor(sceneEditor)
        , uiConfig(uiConfig
            ? std::optional<EditorUISubsystemConfig>(*uiConfig)
            : std::nullopt)
    {
    }

    void build(App& app) override;
    void configureSchedules(App& app) override;
    void configureLateSchedules(App& app) override;

    SceneAppConfig appConfig;
    SceneEditor& m_sceneEditor;
    std::optional<EditorUISubsystemConfig> uiConfig;
};

} // namespace caustica::editor
