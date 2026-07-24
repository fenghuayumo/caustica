#pragma once

#include <functional>

namespace caustica
{

class App;

// Thin UE-style render-thread enqueue. Non-blocking; does not idle-wait the RT.
// Prefer this over digging RenderThread / App::enqueueGpuWorkOnRenderThread.
// The callable must not touch Logic-thread ECS or App schedules.
void EnqueueRenderCommand(App& app, std::function<void()> command);

// Blocking variant (flush). Prefer EnqueueRenderCommand unless the caller must
// observe RT-side completion before continuing on Logic.
void EnqueueRenderCommandAndWait(App& app, std::function<void()> command);

} // namespace caustica
