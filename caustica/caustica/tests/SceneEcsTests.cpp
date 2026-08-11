#include <scene/SceneEcs.h>

#include <cstdio>

namespace
{
bool expect(bool condition, const char* message)
{
    if (condition)
        return true;
    std::fprintf(stderr, "SceneEcs test failed: %s\n", message);
    return false;
}
}

int main()
{
    bool passed = true;

    caustica::scene::SceneEntityWorld entityWorld;
    const caustica::ecs::Entity root = entityWorld.createEntity("Root");
    const caustica::ecs::Entity light = entityWorld.createEntity("Sun", root);
    entityWorld.setDirectionalLight(light, caustica::scene::DirectionalLightComponent{});

    // Establish a fully consumed baseline frame.
    entityWorld.refresh(0);
    entityWorld.endChangeDetectionFrame();
    passed &= expect(!entityWorld.hasPendingLightChanges(),
        "baseline light dirty state did not clear after Extract/publish");

    entityWorld.destroyEntity(light);
    passed &= expect(entityWorld.hasPendingLightChanges(),
        "deleting a light did not mark the light list dirty");

    // PostUpdate refresh used to clear the only deletion signal here. It must
    // remain pending until the subsequent Extract/publish ends the ECS frame.
    entityWorld.refresh(1);
    passed &= expect(entityWorld.hasPendingLightChanges(),
        "light deletion dirty state was lost between refresh and Extract");

    entityWorld.endChangeDetectionFrame();
    passed &= expect(!entityWorld.hasPendingLightChanges(),
        "published light deletion remained dirty in the following frame");

    return passed ? 0 : 1;
}
