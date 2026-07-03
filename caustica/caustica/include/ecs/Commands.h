#pragma once

#include <ecs/Entity.h>

#include <functional>
#include <vector>

namespace caustica::ecs
{

class World;

// Deferred world mutations applied at a controlled point each frame (Bevy-style Commands).
class CommandQueue
{
public:
    void despawn(Entity entity);
    template<typename T, typename... Args>
    void emplace(Entity entity, Args&&... args);
    template<typename T>
    void remove(Entity entity);

    void push(std::function<void(World&)> command);
    void apply(World& world);
    void clear();

    [[nodiscard]] bool empty() const { return m_commands.empty(); }

private:
    std::vector<std::function<void(World&)>> m_commands;
};

} // namespace caustica::ecs
