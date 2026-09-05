#include <scene/SceneEcs.h>
#include <scene/SceneLightAccess.h>
#include <scene/SceneRenderSnapshot.h>
#include <scene/SceneSemanticIds.h>

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

    {
        caustica::scene::SceneEntityWorld entityWorld;
        const caustica::ecs::Entity root = entityWorld.createEntity("Root");
        const caustica::ecs::Entity light = entityWorld.createEntity("Panel", root);
        caustica::scene::RectLightComponent rect;
        rect.intensity = 12.f;
        rect.width = 2.f;
        rect.height = 3.f;
        entityWorld.setRectLight(light, rect);

        const auto* stored = caustica::scene::tryGetRectLight(entityWorld.world(), light);
        passed &= expect(stored && stored->width == 2.f && stored->height == 3.f,
            "setRectLight did not preserve rectangle dimensions");
        passed &= expect(caustica::scene::getLightType(*stored) == LightType_Rect,
            "RectLight component did not map to LightType_Rect");

        entityWorld.setPointLight(light, caustica::scene::PointLightComponent{});
        passed &= expect(!caustica::scene::tryGetRectLight(entityWorld.world(), light),
            "light type exclusivity did not remove RectLight");
    }

    {
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
    }

    {
        caustica::scene::SceneEntityWorld entityWorld;
        const caustica::ecs::Entity root = entityWorld.createEntity("Root");
        const caustica::ecs::Entity light = entityWorld.createEntity("Sky", root);
        const caustica::ecs::Entity animation = entityWorld.createEntity("Animation", root);
        entityWorld.setEnvironmentLight(light, caustica::scene::EnvironmentLightComponent{});
        entityWorld.setAnimation(animation, caustica::scene::AnimationComponent{});

        entityWorld.refresh(0);
        entityWorld.endChangeDetectionFrame();

        // Animation and other non-light component updates may happen every frame.
        // They must not invalidate temporal lighting history when no light changed.
        entityWorld.world().notifyComponentChanged<caustica::scene::AnimationComponent>(animation);
        passed &= expect(!entityWorld.hasPendingLightChanges(),
            "non-light animation update incorrectly dirtied the light list");
    }

    {
        caustica::scene::SceneEntityWorld entityWorld;
        const caustica::ecs::Entity root = entityWorld.createEntity("Root");
        const caustica::ecs::Entity light = entityWorld.createEntity("Sun", root);
        entityWorld.setDirectionalLight(light, caustica::scene::DirectionalLightComponent{});
        entityWorld.refresh(0);
        entityWorld.endChangeDetectionFrame();

        passed &= expect(
            caustica::scene::setLightProperty(
                entityWorld.world(), light, "irradiance", math::float4(3.f, 0.f, 0.f, 0.f)),
            "setLightProperty rejected a valid directional-light property");
        passed &= expect(entityWorld.hasPendingLightChanges(),
            "setLightProperty did not notify light change detection");
    }

    {
        caustica::scene::SceneRenderSnapshot snapshot;
        snapshot.pendingState().lightsChanged = true;
        snapshot.publish(1);

        passed &= expect(snapshot.publishedStateForFrame(1).lightsChanged,
            "published light change was not visible to its render frame");
        passed &= expect(!snapshot.publishedStateForFrame(4).lightsChanged,
            "stale ring-buffer light change leaked into a skipped render frame");

        snapshot.bufferForFrame(1).renderSettings.settings.ResetRealtimeCaches = true;
        snapshot.bufferForFrame(2).renderSettings.settings.ResetRealtimeCaches = false;
        snapshot.publish(2);
        passed &= expect(
            !snapshot.readBufferForFrameOrLatest(4).renderSettings.settings.ResetRealtimeCaches,
            "skipped frame replayed a stale ring-slot temporal reset");
    }

    {
        struct Clock
        {
            int ticks = 7;
        };

        caustica::ecs::World live;
        live.insertResource<Clock>();

        caustica::scene::SceneEntityWorld scratch;
        const caustica::ecs::Entity root = scratch.createEntity("Root");
        const caustica::ecs::Entity light = scratch.createEntity("Sun", root);
        scratch.setDirectionalLight(light, caustica::scene::DirectionalLightComponent{});

        scratch.adoptInto(live);

        passed &= expect(&scratch.world() == &live,
            "adoptInto did not rebind the scene graph onto the live registry");
        passed &= expect(!scratch.ownsRegistry(),
            "adoptInto kept a scratch registry");
        passed &= expect(live.resource<Clock>().ticks == 7,
            "adoptInto cleared live resources");
        passed &= expect(live.isAlive(scratch.root()),
            "adoptInto lost the scene root");
        const caustica::ecs::Entity sun = scratch.findEntity("Sun", scratch.root());
        passed &= expect(
            live.has<caustica::scene::DirectionalLightComponent>(sun),
            "adoptInto dropped scene components");
        const caustica::ecs::Entity extra = scratch.createEntity("Extra", scratch.root());
        passed &= expect(live.isAlive(extra) && &scratch.world() == &live,
            "borrowed SceneEntityWorld did not spawn into the live registry");
    }

    {
        using caustica::scene::hashStableLabel;
        using caustica::scene::resolveInstanceId;
        using caustica::scene::resolveSemanticId;
        using caustica::scene::setSemanticLabel;

        passed &= expect(hashStableLabel("cube") != 0u, "stable label hash reserved 0");
        passed &= expect(hashStableLabel("cube") == hashStableLabel("cube"), "stable label hash is not stable");
        passed &= expect(hashStableLabel("cube") != hashStableLabel("sphere"), "distinct labels collided");

        caustica::scene::SceneEntityWorld entityWorld;
        const caustica::ecs::Entity root = entityWorld.createEntity("Root");
        const caustica::ecs::Entity cube = entityWorld.createEntity("Cube", root);
        entityWorld.world().emplace<caustica::scene::SceneAuthoringIdComponent>(
            cube, caustica::scene::SceneAuthoringIdComponent{ "link/wrist" });

        const uint32_t autoId = resolveInstanceId(entityWorld.world(), cube);
        passed &= expect(autoId == hashStableLabel("link/wrist"),
            "instance id did not hash SceneAuthoringId");
        passed &= expect(resolveSemanticId(entityWorld.world(), cube) == 0u,
            "unlabeled entity did not report semantic id 0");

        passed &= expect(
            setSemanticLabel(entityWorld.world(), cube, 42u, 7u, "manipulator"),
            "setSemanticLabel failed");
        passed &= expect(resolveInstanceId(entityWorld.world(), cube) == 42u,
            "explicit instance_id was not used");
        passed &= expect(resolveSemanticId(entityWorld.world(), cube) == 7u,
            "explicit semantic_id was not used");

        passed &= expect(
            setSemanticLabel(entityWorld.world(), cube, 0u, 0u, "cube"),
            "setSemanticLabel failed to restore auto ids");
        passed &= expect(resolveInstanceId(entityWorld.world(), cube) == hashStableLabel("link/wrist"),
            "instance id 0 did not fall back to authoring id");
        passed &= expect(resolveSemanticId(entityWorld.world(), cube) == hashStableLabel("cube"),
            "semantic id 0 did not hash semantic_label");
    }

    return passed ? 0 : 1;
}
