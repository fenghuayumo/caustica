#pragma once

#include <ecs/Entity.h>

#include <memory>

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
    };
}
