#include <engine/EnqueueRenderCommand.h>

#include <engine/RenderSessionApi.h>

namespace caustica
{

void EnqueueRenderCommand(App& app, std::function<void()> command)
{
    enqueueGpuWorkOnRenderThread(app, std::move(command));
}

void EnqueueRenderCommandAndWait(App& app, std::function<void()> command)
{
    runGpuWorkOnRenderThread(app, std::move(command));
}

} // namespace caustica
