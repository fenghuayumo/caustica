#pragma once

#include <engine/DefaultRuntimePlugin.h>
#include <engine/IEnginePlugin.h>

#include "EditorSceneSubsystem.h"
#include "EditorUISubsystem.h"
#include "SceneEditor.h"

namespace caustica::editor
{

// Editor stack: DefaultRuntimePlugin + EditorSceneSubsystem (+ optional UI).
//
// Requires SceneEditor : SceneRuntime (Phase 1). sceneConfig.sceneRuntime must
// reference the same SceneEditor instance passed to EditorSceneSubsystem.
struct EditorPlugin : IEnginePlugin
{
    EditorPlugin(SceneRuntimeSubsystemConfig sceneConfig,
        EditorUISubsystemConfig* uiConfig = nullptr)
        : sceneConfig(std::move(sceneConfig))
        , uiConfig(uiConfig)
    {
    }

    void build(EngineBuilder& builder) override
    {
        builder.addPlugin<DefaultRuntimePlugin>(sceneConfig);

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
    EditorUISubsystemConfig* uiConfig = nullptr;
};

} // namespace caustica::editor
