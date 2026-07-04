#pragma once

#include "EditorPlugin.h"
#include "EditorUISubsystem.h"

#include <engine/Engine.h>
#include <engine/GpuRenderSubsystem.h>
#include <engine/SceneRuntimeRegistration.h>
#include <engine/SceneRuntimeSubsystem.h>

namespace caustica::editor
{

// Convenience entry points mirroring registerSceneRuntime() for the editor stack.
inline void registerEditorRuntime(caustica::Engine& engine, const caustica::SceneRuntimeSubsystemConfig& sceneConfig)
{
    caustica::registerEnginePlugins<EditorPlugin>(engine, sceneConfig, static_cast<const EditorUISubsystemConfig*>(nullptr));
}

inline void registerEditorRuntime(caustica::Engine& engine,
    const caustica::SceneRuntimeSubsystemConfig& sceneConfig,
    const EditorUISubsystemConfig& uiConfig)
{
    caustica::registerEnginePlugins<EditorPlugin>(engine, sceneConfig, &uiConfig);
}

[[nodiscard]] inline caustica::GpuRenderSubsystem* getGpuRenderSubsystem(caustica::Engine& engine)
{
    return engine.getSubsystem<caustica::GpuRenderSubsystem>();
}

} // namespace caustica::editor
