#pragma once

#include <scene/SceneEcs.h>

#include <memory>

namespace caustica
{
class SceneGraph;
}

namespace caustica::scene
{
void RebuildWorldFromLegacyScene(SceneEntityWorld& entityWorld, const std::shared_ptr<SceneGraph>& legacyScene);
void SyncWorldFromLegacyScene(SceneEntityWorld& entityWorld, const std::shared_ptr<SceneGraph>& legacyScene);
}
