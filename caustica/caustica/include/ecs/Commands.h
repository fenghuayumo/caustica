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

    // Moves `other`'s commands to the back and leaves it empty. The parallel
    // schedule executor uses this to merge per-system buffers in a fixed order.
    void append(CommandQueue& other);

    [[nodiscard]] bool empty() const { return m_commands.empty(); }

private:
    std::vector<std::function<void(World&)>> m_commands;
};

} // namespace caustica::ecs
