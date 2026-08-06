#pragma once

// Engine-internal accessor. Applications / samples / Python must not include this.
// Use RenderSessionApi / CameraApi / SceneLifecycle / SceneQuery instead.

namespace caustica
{

class App;

namespace render
{
class WorldRenderer;
}

[[nodiscard]] render::WorldRenderer* worldRenderer(const App& app);

} // namespace caustica
