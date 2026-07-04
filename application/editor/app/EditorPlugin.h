#pragma once

#include <engine/EngineBuilder.h>
#include <engine/GpuRenderSubsystem.h>
#include <engine/IEnginePlugin.h>

#include "EditorSceneSubsystem.h"
#include "EditorUISubsystem.h"

namespace caustica::editor
{

// Editor stack: GpuRenderSubsystem + EditorSceneSubsystem (+ optional UI).
//
// sceneConfig.sceneRuntime must be a SceneEditor (or derived) instance.
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

        builder.emplaceSubsystem<EditorSceneSubsystem>(EditorSceneSubsystemConfig{
            .runtime = sceneConfig,
        });

        if (uiConfig)
            builder.emplaceSubsystem<EditorUISubsystem>(*uiConfig);
    }

    SceneRuntimeSubsystemConfig sceneConfig;
    const EditorUISubsystemConfig* uiConfig = nullptr;
};

} // namespace caustica::editor
