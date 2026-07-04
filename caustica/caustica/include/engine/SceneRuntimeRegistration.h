#pragma once

#include <engine/DefaultRuntimePlugin.h>
#include <engine/EngineBuilder.h>
#include <engine/Engine.h>
#include <render/FramePassRegistry.h>

#include <memory>

namespace caustica
{

// Optional shared frame-pass registry for the process lifetime. WorldRenderer applies
// this when building PathTracingFramePipeline and when composing post-process graphs.
inline render::FramePassRegistry& getGlobalFramePassRegistry()
{
    static render::FramePassRegistry registry;
    return registry;
}

inline void registerSceneRuntime(Engine& engine, SceneRuntimeSubsystemConfig sceneConfig)
{
    EngineBuilder builder(engine, &getGlobalFramePassRegistry());
    DefaultRuntimePlugin plugin(std::move(sceneConfig));
    plugin.build(builder);
}

template<typename Plugin, typename... Args>
inline void registerEnginePlugins(Engine& engine, Args&&... args)
{
    EngineBuilder builder(engine, &getGlobalFramePassRegistry());
    builder.addPlugin<Plugin>(std::forward<Args>(args)...);
}

} // namespace caustica
