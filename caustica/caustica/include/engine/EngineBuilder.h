#pragma once

#include <engine/Engine.h>
#include <engine/IEnginePlugin.h>
#include <engine/ISubsystem.h>

#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace caustica::render
{
class FramePassRegistry;
}

namespace caustica
{

// Fluent builder used by IEnginePlugin::build(). Owns no Engine; callers pass
// the target Engine at construction time.
class EngineBuilder
{
public:
    explicit EngineBuilder(Engine& engine, render::FramePassRegistry* framePasses = nullptr);

    [[nodiscard]] Engine& engine() { return m_engine; }
    [[nodiscard]] const Engine& engine() const { return m_engine; }

    [[nodiscard]] render::FramePassRegistry* framePasses() { return m_framePasses; }
    [[nodiscard]] const render::FramePassRegistry* framePasses() const { return m_framePasses; }

    // Register a subsystem (same as Engine::addSubsystem, priority from ISubsystem).
    void addSubsystem(std::unique_ptr<ISubsystem> subsystem);

    template<typename T, typename... Args>
    T& emplaceSubsystem(Args&&... args)
    {
        static_assert(std::is_base_of_v<ISubsystem, T>, "T must derive from ISubsystem");
        auto subsystem = std::make_unique<T>(std::forward<Args>(args)...);
        T& ref = *subsystem;
        addSubsystem(std::move(subsystem));
        return ref;
    }

    // Nested plugin composition.
    template<typename T, typename... Args>
    requires std::is_base_of_v<IEnginePlugin, T>
    void addPlugin(Args&&... args)
    {
        T plugin(std::forward<Args>(args)...);
        plugin.build(*this);
    }

    void addPlugin(IEnginePlugin& plugin) { plugin.build(*this); }

    template<typename T>
    [[nodiscard]] T* getSubsystem() const
    {
        return m_engine.getSubsystem<T>();
    }

private:
    Engine& m_engine;
    render::FramePassRegistry* m_framePasses = nullptr;
};

inline EngineBuilder::EngineBuilder(Engine& engine, render::FramePassRegistry* framePasses)
    : m_engine(engine)
    , m_framePasses(framePasses)
{
}

inline void EngineBuilder::addSubsystem(std::unique_ptr<ISubsystem> subsystem)
{
    m_engine.addSubsystem(std::move(subsystem));
}

} // namespace caustica
