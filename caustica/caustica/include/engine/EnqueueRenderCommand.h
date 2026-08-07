#pragma once

#include <functional>

namespace caustica
{

class App;

// Sole Logic → Render enqueue API (ADR 0001). Non-blocking; does not idle-wait
// the RT. The callable must not touch Logic-thread ECS or App schedules.
void EnqueueRenderCommand(App& app, std::function<void()> command);

// Blocking sync point. Prefer EnqueueRenderCommand unless Logic must observe
// RT-side completion before continuing.
void EnqueueRenderCommandAndWait(App& app, std::function<void()> command);

} // namespace caustica
