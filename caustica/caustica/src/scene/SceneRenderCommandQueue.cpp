#include <scene/SceneRenderCommandQueue.h>
#include <scene/Scene.h>

namespace caustica::scene
{

void SceneRenderCommandQueue::enqueue(std::function<void(Scene&)> command)
{
    if (!command)
        return;

    std::lock_guard lock(m_mutex);
    m_pending.push_back(std::move(command));
}

void SceneRenderCommandQueue::drain(Scene& scene)
{
    std::vector<std::function<void(Scene&)>> commands;
    {
        std::lock_guard lock(m_mutex);
        commands.swap(m_pending);
    }

    for (std::function<void(Scene&)>& command : commands)
    {
        if (command)
            command(scene);
    }
}

void SceneRenderCommandQueue::clear()
{
    std::lock_guard lock(m_mutex);
    m_pending.clear();
}

} // namespace caustica::scene
