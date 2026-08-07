#include <engine/EnqueueRenderCommand.h>

#include <engine/App.h>

namespace caustica
{

void EnqueueRenderCommand(App& app, std::function<void()> command)
{
    app.enqueueRenderCommandImpl(std::move(command));
}

void EnqueueRenderCommandAndWait(App& app, std::function<void()> command)
{
    app.enqueueRenderCommandAndWaitImpl(std::move(command));
}

} // namespace caustica
