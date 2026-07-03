#pragma once

#include <functional>
#include <mutex>
#include <vector>

namespace caustica
{
    class Scene;
}

namespace caustica::scene
{
    // Deferred commands posted from the logic thread and executed on the render thread.
    class SceneRenderCommandQueue
    {
    public:
        void enqueue(std::function<void(Scene&)> command);
        void drain(Scene& scene);

        void clear();

    private:
        std::mutex m_mutex;
        std::vector<std::function<void(Scene&)>> m_pending;
    };

} // namespace caustica::scene
