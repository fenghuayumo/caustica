#pragma once

// Public debug API — named render-target texture visibility for editor tooling
// (`vis <name>` console command and the texture viewer window).

#include <cstdint>
#include <string>
#include <string_view>

namespace caustica::rhi
{
class Texture;
}

namespace caustica
{

class App;

// Number of debug-viewable frame textures for the current feature set.
[[nodiscard]] uint32_t debugViewTextureCount(const App& app);

// Copies the name into caller-owned storage. GPU texture is borrowed.
// Name + GPU texture at enumeration index. Returns false past the end or when
// the texture is not currently created (disabled feature).
[[nodiscard]] bool debugViewTextureInfo(
    const App& app,
    uint32_t index,
    std::string* outName,
    rhi::Texture** outTexture);

// Case-insensitive lookup by name. Returns nullptr when unknown or unavailable.
[[nodiscard]] rhi::Texture* findDebugViewTexture(const App& app, std::string_view name);

// Queue a one-frame capture of a named RenderGraph texture. The renderer adds
// a final copy pass, so the source cannot be aliased before its contents are
// copied. On success the result is listed as `frozen/<name>`.
void requestDebugViewTextureCapture(App& app, std::string_view name);

// Drop the persistent texture held by the debug viewer.
void clearDebugViewTextureCapture(App& app);

} // namespace caustica
