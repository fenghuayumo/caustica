#include <engine/AppSchedules.h>
#include <engine/internal/AppTest.h>
#include <physics/PhysicsPlugin.h>
#include <scene/SceneComponentBuilders.h>
#include <scene/SceneEcs.h>
#include <core/json.h>

#include <cmath>
#include <cstdio>
#include <string>

namespace
{
bool expect(bool condition, const char* message)
{
    if (condition)
        return true;
    std::fprintf(stderr, "Physics test failed: %s\n", message);
    return false;
}

caustica::scene::LocalTransformComponent makePose(const caustica::math::double3& translation)
{
    return caustica::scene::LocalTransformComponent::fromTRS(
        translation, caustica::math::dquat::identity(), caustica::math::double3(1.0));
}

void stepPhysics(caustica::App& app, float dt, uint32_t frameIndex)
{
    caustica::SystemContext context{
        app,
        app.world(),
        nullptr,
        dt,
        frameIndex,
        true,
        true,
    };
    context.runUpdate = true;
    app.runSchedule(caustica::AppSchedule::update, context);
    app.runSchedule(caustica::AppSchedule::PostUpdate, context);
}

const caustica::math::double3* translationOf(
    caustica::scene::SceneEntityWorld& world, caustica::ecs::Entity entity)
{
    const auto* local = world.world().tryGet<caustica::scene::LocalTransformComponent>(entity);
    return local ? &local->translation : nullptr;
}
}

int main()
{
    bool passed = true;

    {
        Json::Value bodyJson;
        caustica::json::fromString(R"({"motion":"dynamic","mass":2.0,"enabled":false})", bodyJson);
        auto body = caustica::scene::makeRigidBodyComponentFromJson(bodyJson);
        passed &= expect(body && body->type == caustica::physics::RigidBodyType::Dynamic,
            "JSON RigidBody.motion=dynamic was not parsed");
        passed &= expect(body && std::abs(body->mass - 2.f) < 1e-5f,
            "JSON RigidBody.mass was not parsed");
        passed &= expect(body && body->enabled == false,
            "JSON RigidBody.enabled was not parsed");

        Json::Value colliderJson;
        caustica::json::fromString(R"({"shape":"box","dimensions":[1,2,3],"restitution":0.25})", colliderJson);
        auto collider = caustica::scene::makeColliderComponentFromJson(colliderJson);
        passed &= expect(collider && collider->shape == caustica::physics::ColliderShape::Box,
            "JSON Collider.shape=box was not parsed");
        passed &= expect(collider && std::abs(collider->dimensions.y - 2.f) < 1e-5f,
            "JSON Collider.dimensions was not parsed");
    }

    if (!caustica::physics::isPhysXAvailable())
    {
        std::fprintf(stderr,
            "causPhysicsTests: PhysX backend is not compiled (CAUSTICA_WITH_PHYSX=OFF). "
            "JSON authoring checks passed; skip live simulation.\n");
        return passed ? 0 : 1;
    }

    auto app = caustica::detail::createBareAppForTest();
    passed &= expect(app != nullptr, "createBareAppForTest failed");
    if (!app)
        return 1;

    caustica::physics::PhysicsPlugin plugin(
        caustica::physics::createPhysXBackend(),
        /*simulationEnabled=*/false);
    plugin.build(*app);
    plugin.configureSchedules(*app);
    passed &= expect(app->tryResource<caustica::physics::PhysicsRuntime>() != nullptr,
        "PhysicsPlugin did not install PhysicsRuntime");
    auto* runtime = app->tryResource<caustica::physics::PhysicsRuntime>();
    passed &= expect(runtime && runtime->backend && std::string(runtime->backend->name()) == "PhysX",
        "PhysX backend was not selected");
    passed &= expect(runtime && runtime->simulationEnabled == false,
        "paused PhysicsPlugin started with simulation on");
    if (!runtime)
        return 1;

    caustica::scene::SceneEntityWorld world(app->world());
    const caustica::ecs::Entity root = world.createEntity("Root");
    const caustica::ecs::Entity ground = world.createEntity("Ground", root);
    const caustica::ecs::Entity box = world.createEntity("FallingBox", root);

    world.world().emplace<caustica::scene::LocalTransformComponent>(
        ground, makePose(caustica::math::double3(0.0, -0.1, 0.0)));
    world.world().emplace<caustica::physics::RigidBodyComponent>(
        ground, caustica::physics::RigidBodyComponent{
            .type = caustica::physics::RigidBodyType::Static,
            .mass = 0.f,
            .gravity = false,
        });
    world.world().emplace<caustica::physics::ColliderComponent>(
        ground, caustica::physics::ColliderComponent{
            .shape = caustica::physics::ColliderShape::Box,
            .dimensions = caustica::math::float3(20.f, 0.2f, 20.f),
            .staticFriction = 0.6f,
            .dynamicFriction = 0.6f,
            .restitution = 0.f,
        });

    constexpr double kStartY = 3.0;
    world.world().emplace<caustica::scene::LocalTransformComponent>(
        box, makePose(caustica::math::double3(0.0, kStartY, 0.0)));
    world.world().emplace<caustica::physics::RigidBodyComponent>(
        box, caustica::physics::RigidBodyComponent{
            .type = caustica::physics::RigidBodyType::Dynamic,
            .mass = 1.f,
            .gravity = true,
        });
    world.world().emplace<caustica::physics::ColliderComponent>(
        box, caustica::physics::ColliderComponent{
            .shape = caustica::physics::ColliderShape::Box,
            .dimensions = caustica::math::float3(1.f, 1.f, 1.f),
            .staticFriction = 0.5f,
            .dynamicFriction = 0.5f,
            .restitution = 0.05f,
        });

    world.refreshHierarchy();

    constexpr float kDt = 1.f / 60.f;
    for (uint32_t frame = 0; frame < 12; ++frame)
        stepPhysics(*app, kDt, frame);
    if (const auto* paused = translationOf(world, box))
        passed &= expect(std::abs(paused->y - kStartY) < 0.01,
            "paused simulation dropped the box before Simulate Physics was enabled");
    else
        passed &= expect(false, "falling box lost its transform while paused");

    runtime->simulationEnabled = true;

    double midY = kStartY;
    for (uint32_t frame = 12; frame < 24; ++frame)
        stepPhysics(*app, kDt, frame);
    if (const auto* mid = translationOf(world, box))
        midY = mid->y;
    passed &= expect(midY < kStartY - 0.05,
        "dynamic box did not fall under gravity");

    for (uint32_t frame = 24; frame < 192; ++frame)
        stepPhysics(*app, kDt, frame);

    const auto* boxPose = translationOf(world, box);
    const auto* groundPose = translationOf(world, ground);
    passed &= expect(boxPose != nullptr, "falling box lost its transform");
    passed &= expect(groundPose != nullptr, "ground lost its transform");
    if (boxPose && groundPose)
    {
        // Ground top is at y=0; 1m cube rests with center near y=0.5.
        passed &= expect(boxPose->y > 0.35 && boxPose->y < 0.75,
            "dynamic box did not come to rest on the static ground");
        passed &= expect(std::abs(boxPose->x) < 0.35 && std::abs(boxPose->z) < 0.35,
            "dynamic box slid off the ground");
        passed &= expect(std::abs(groundPose->y + 0.1) < 0.02,
            "static ground moved");
        std::printf("PhysX drop: box y=%.3f (start=%.3f, mid=%.3f) ground y=%.3f\n",
            boxPose->y, kStartY, midY, groundPose->y);
    }

    return passed ? 0 : 1;
}
