#pragma once

#include <functional>

namespace caustica
{

class App;

// Sole public Logic → Render enqueue API (ADR 0001).
// Forwards to App private impl → Affinity::Render (RenderThread pump or --syncRender).
// Non-blocking; does not idle-wait the RT. Callable must not touch Logic ECS / schedules.
void EnqueueRenderCommand(App& app, std::function<void()> command);

// Blocking sync point on Affinity::Render. Prefer EnqueueRenderCommand unless
// Logic must observe RT-side completion before continuing.
void EnqueueRenderCommandAndWait(App& app, std::function<void()> command);

} // namespace caustica
