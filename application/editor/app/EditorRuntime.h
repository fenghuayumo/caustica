#pragma once

#include "EditorSceneSubsystem.h"

#include <engine/Engine.h>
#include <engine/RenderingSubsystem.h>

#include <memory>

namespace caustica::editor
{

inline void registerEditorRuntime(caustica::Engine& engine, const EditorSceneSubsystemConfig& sceneConfig)
{
    engine.addSubsystem(std::make_unique<caustica::RenderingSubsystem>());
    engine.addSubsystem(std::make_unique<EditorSceneSubsystem>(sceneConfig));
}

[[nodiscard]] inline caustica::RenderingSubsystem* getRenderingSubsystem(caustica::Engine& engine)
{
    return engine.getSubsystem<caustica::RenderingSubsystem>();
}

} // namespace caustica::editor
