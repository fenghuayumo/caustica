#pragma once

#include <json/json.h>
#include <cstddef>

namespace caustica::scene
{
class SceneEntityWorld;

// Logic-thread observation of the expanded live graph. Runtime ids are only
// valid within the host's scene generation; authored ids survive save/reload
// within their scene/prefab authoring scope.
// Does not advance time, refresh history, or change the world. Bounds reflect
// the last hierarchy propagation. Pages are ordered by runtime entity handle.
[[nodiscard]] Json::Value inspectSceneEntities(
    const SceneEntityWorld& world, size_t offset = 0, size_t limit = 100);
}
