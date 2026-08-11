#include <scene/SceneEcs.h>
#include <scene/SceneRenderSnapshot.h>

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

    {
        caustica::scene::SceneRenderSnapshot snapshot;
        snapshot.bufferForFrame(1).renderSettings.settings.ResetRealtimeCaches = true;
        snapshot.publish(1);
        snapshot.bufferForFrame(2).renderSettings.settings.ResetRealtimeCaches = false;
        snapshot.publish(2);

        passed &= expect(
            !snapshot.readBufferForFrameOrLatest(4).renderSettings.settings.ResetRealtimeCaches,
            "skipped frame replayed a stale ring-slot temporal reset");
    }

    return passed ? 0 : 1;
}
