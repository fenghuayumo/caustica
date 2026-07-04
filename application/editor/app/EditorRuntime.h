#pragma once

#include "EditorSceneSubsystem.h"
#include "EditorUISubsystem.h"

#include <engine/Engine.h>
#include <engine/EngineRenderer.h>

#include <memory>

namespace caustica::editor
{

inline void registerEditorRuntime(caustica::Engine& engine, const EditorSceneSubsystemConfig& sceneConfig)
{
    engine.addSubsystem(std::make_unique<caustica::EngineRenderer>());
    engine.addSubsystem(std::make_unique<EditorSceneSubsystem>(sceneConfig));
}

inline void registerEditorRuntime(caustica::Engine& engine,
    const EditorSceneSubsystemConfig& sceneConfig,
    const EditorUISubsystemConfig& uiConfig)
{
    registerEditorRuntime(engine, sceneConfig);
    engine.addSubsystem(std::make_unique<EditorUISubsystem>(uiConfig));
}

[[nodiscard]] inline caustica::EngineRenderer* getEngineRenderer(caustica::Engine& engine)
{
    return engine.getSubsystem<caustica::EngineRenderer>();
}

} // namespace caustica::editor
