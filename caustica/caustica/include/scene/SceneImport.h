#pragma once

#include <ecs/Entity.h>

#include <memory>

namespace caustica
{
class SceneGraphNode;
}

namespace caustica::scene
{
class SceneEntityWorld;
}

namespace caustica
{
    struct SceneImportResult
    {
        std::shared_ptr<scene::SceneEntityWorld> entityWorld;
        ecs::Entity rootEntity = ecs::NullEntity;

        // Compatibility output while glTF/OBJ importers and editor/rendering paths are migrated.
        std::shared_ptr<SceneGraphNode> rootNode;
    };
}
