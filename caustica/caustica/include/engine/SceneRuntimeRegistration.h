#pragma once

#include <engine/DefaultRuntimePlugin.h>
#include <engine/EngineBuilder.h>
#include <engine/Engine.h>

#include <memory>

namespace caustica
{

inline void registerSceneRuntime(Engine& engine, SceneRuntimeSubsystemConfig sceneConfig)
{
    EngineBuilder builder(engine);
    DefaultRuntimePlugin plugin(std::move(sceneConfig));
    plugin.build(builder);
}

template<typename Plugin, typename... Args>
inline void registerEnginePlugins(Engine& engine, Args&&... args)
{
    EngineBuilder builder(engine);
    builder.addPlugin<Plugin>(std::forward<Args>(args)...);
}

} // namespace caustica
