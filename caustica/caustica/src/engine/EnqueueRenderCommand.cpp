#include <engine/EnqueueRenderCommand.h>

#include <engine/App.h>

namespace caustica
{

void EnqueueRenderCommand(App& app, std::function<void()> command)
{
    app.enqueueGpuWorkOnRenderThread(std::move(command));
}

void EnqueueRenderCommandAndWait(App& app, std::function<void()> command)
{
    app.runGpuWorkOnRenderThread(std::move(command));
}

} // namespace caustica
