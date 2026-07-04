#pragma once

#include <engine/EngineBuilder.h>
#include <engine/GpuRenderSubsystem.h>
#include <engine/IEnginePlugin.h>

#include "EditorSceneSubsystem.h"
#include "EditorUISubsystem.h"
#include "SceneEditor.h"

namespace caustica::editor
{

// Editor stack: GpuRenderSubsystem + EditorSceneSubsystem (+ optional UI).
//
// sceneConfig.sceneRuntime must reference the same SceneEditor instance wired
// into EditorSceneSubsystem.
struct EditorPlugin : IEnginePlugin
{
    EditorPlugin(SceneRuntimeSubsystemConfig sceneConfig,
        const EditorUISubsystemConfig* uiConfig = nullptr)
        : sceneConfig(std::move(sceneConfig))
        , uiConfig(uiConfig)
    {
    }

    void build(EngineBuilder& builder) override
    {
        builder.emplaceSubsystem<GpuRenderSubsystem>();

        auto& sceneEditor = static_cast<SceneEditor&>(sceneConfig.sceneRuntime);
        builder.emplaceSubsystem<EditorSceneSubsystem>(EditorSceneSubsystemConfig{
            .sceneEditor = sceneEditor,
            .diagnostics = sceneConfig.diagnostics,
            .preferredScene = sceneConfig.preferredScene,
            .sessionState = sceneConfig.sessionState,
            .cmdLine = sceneConfig.cmdLine,
            .refreshEnvMapMediaList = sceneConfig.refreshEnvMapMediaList,
            .applyCmdLineToSessionState = sceneConfig.applyCmdLineToSessionState,
        });

        if (uiConfig)
            builder.emplaceSubsystem<EditorUISubsystem>(*uiConfig);
    }

    SceneRuntimeSubsystemConfig sceneConfig;
    const EditorUISubsystemConfig* uiConfig = nullptr;
};

} // namespace caustica::editor
