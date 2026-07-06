#pragma once

#include <engine/App.h>
#include <engine/EngineScheduleRegistration.h>
#include <engine/SceneSessionResources.h>
#include <engine/GpuRenderSubsystem.h>
#include <engine/Plugin.h>
#include <engine/SceneSessionStartup.h>
#include <render/worldRenderer/WorldRenderer.h>

#include "EditorSceneStartup.h"
#include "EditorUISubsystem.h"
#include "common/CaptureScriptManager.h"
#include "SceneEditor.h"

#include <imgui/imgui_renderer.h>

namespace caustica::editor
{

struct EditorPlugin : Plugin
{
    EditorPlugin(SceneSessionConfig sessionConfig,
        SceneEditor& sceneEditor,
        const EditorUISubsystemConfig* uiConfig = nullptr)
        : sessionConfig(std::move(sessionConfig))
        , m_sceneEditor(sceneEditor)
        , uiConfig(uiConfig)
    {
    }

    void build(App& app) override;
    void configureSchedules(App& app) override;
    void configureLateSchedules(App& app) override;

    SceneSessionConfig sessionConfig;
    SceneEditor& m_sceneEditor;
    const EditorUISubsystemConfig* uiConfig = nullptr;
};

} // namespace caustica::editor
