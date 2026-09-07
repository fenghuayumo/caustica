// Official C++ thin client / public API reference (P0 freeze).
// Depends only on <caustica.h> (+ math). No editor UI, ImGui, or WorldRenderer digs.
// Coverage checklist: docs/public-api.md
//
// Systems are split by what they need to touch, which is also what decides
// whether they can run in parallel (ADR 0003):
//
//   Setup  - structural (spawn / despawn / tag). Takes EntityWorld, so it is
//            exclusive. Kept one-shot so it costs nothing after the first frame.
//   Spin   - per-frame animation. Takes Res<Time> + SceneTransforms + a Query,
//            all of which declare narrow access, so it runs in parallel.
//   Report - per-frame read-only. Res<Time> + a const Query. Also parallel.
//
// The rule of thumb: keep the whole world (EntityWorld / SystemContext&) out of
// systems that run every frame, and the scheduler will overlap them for you.

#include <caustica.h>

#include <core/log.h>
#include <math/math.h>
#include <math/quat.h>

#include <cmath>
#include <memory>
#include <optional>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace
{

struct ThinClientSetupLabel
{
    static constexpr const char* name = "ThinClient.Setup";
};

struct ThinClientSpinLabel
{
    static constexpr const char* name = "ThinClient.SpinSpawned";
};

struct ThinClientReportLabel
{
    static constexpr const char* name = "ThinClient.Report";
};

// Marker component. Tagging the entity is what lets the per-frame system find
// it with a Query instead of asking the world for it by handle.
struct ThinClientSpun
{
    float radiansPerSecond = 0.8f;
};

// App resource shared by the systems below. Res / ResMut declare access to it,
// so the scheduler knows Setup and Report cannot overlap while Spin can.
struct ThinClientState
{
    caustica::ecs::Entity spawnedMesh = caustica::ecs::NullEntity;
    caustica::ecs::Entity spawnedLight = caustica::ecs::NullEntity;
    bool spawnRequested = false;
    bool loggedQuery = false;
    bool loggedRenderEnqueue = false;
    bool despawnedLight = false;
    float elapsedSeconds = 0.f;
    float angleRadians = 0.f;
};

} // namespace

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
#else
int main(int, char**)
#endif
{
    caustica::initializeAppPlatform();
#ifdef _WIN32
    caustica::initNativeConsole(/*visibleByDefault=*/true);
#endif

    auto engine = caustica::EngineApp::create(caustica::EngineAppDesc{
        .width = 1280,
        .height = 720,
        .scene = "convergence-test.scene.json",
        .windowTitle = "caustica thin client",
    });

    if (!engine || !engine->isValid())
    {
        caustica::error("thin_client: EngineApp::create failed");
        caustica::shutdownAppPlatform();
        return 1;
    }

    // Settings / camera via EngineApp (PathTracerSettings + CameraApi).
    engine->setCameraVerticalFOV(math::radians(55.f));
    (void)engine->settings();

    engine->emplaceResource<ThinClientState>();

    // ---------------------------------------------------------------------
    // Setup: structural work. Exclusive because EntityWorld can reach anything,
    // which is fine here — every branch is one-shot.
    // ---------------------------------------------------------------------
    engine->addSystem<ThinClientSetupLabel>(
        caustica::AppSchedule::update,
        [](caustica::ResMut<ThinClientState> state,
           caustica::EntityWorld scene,
           caustica::SystemContext& ctx) {
            if (!scene || !caustica::isSceneLoaded(ctx.app))
                return;

            if (!state->spawnRequested)
            {
                state->spawnRequested = true;
                caustica::info(
                    "thin_client: scene ready name=%s",
                    caustica::currentSceneName(ctx.app).c_str());

                // Prefab spawn, then tag it so the per-frame system can select
                // it with a Query rather than needing the world.
                state->spawnedMesh = caustica::spawnFromFile(
                    ctx.app, "models/GlassSphere/GlassSphere.gltf");
                if (caustica::ecs::isValid(state->spawnedMesh))
                {
                    scene.setLocalTransform(
                        state->spawnedMesh,
                        math::double3{ 2.0, 1.0, 0.0 },
                        std::nullopt,
                        math::double3{ 0.5, 0.5, 0.5 });
                    scene.setVisible(state->spawnedMesh, true);
                    scene.emplace<ThinClientSpun>(state->spawnedMesh);
                    caustica::info("thin_client: spawned GlassSphere entity");
                }
                else
                {
                    caustica::warning("thin_client: spawnFromFile failed");
                }

                // ECS bundle spawn (point light).
                state->spawnedLight = scene.spawnNamed(
                    "ThinClient.PointLight",
                    caustica::scene::LocalTransformComponent::fromTRS(
                        math::double3{ 2.0, 2.5, 0.0 },
                        math::dquat::identity(),
                        math::double3{ 1.0, 1.0, 1.0 }),
                    caustica::scene::PointLightComponent{ .intensity = 8.f });
                if (caustica::ecs::isValid(state->spawnedLight))
                    caustica::info("thin_client: spawned point light bundle");

                // Path lookup (EntityWorld + free function).
                const caustica::ecs::Entity byPath = scene.findEntity("ThinClient.PointLight");
                if (byPath != state->spawnedLight)
                    caustica::warning("thin_client: findEntity path mismatch");
                (void)caustica::findEntity(ctx.app, "ThinClient.PointLight");

                // Thin RT enqueue (non-blocking; no Logic ECS on the render thread).
                if (!state->loggedRenderEnqueue)
                {
                    state->loggedRenderEnqueue = true;
                    caustica::EnqueueRenderCommand(ctx.app, []() {
                        caustica::info("thin_client: EnqueueRenderCommand ran on render thread");
                    });
                }
            }

            // Despawn the temporary light (structure edit via SceneSpawn::despawn).
            if (!state->despawnedLight
                && caustica::ecs::isValid(state->spawnedLight)
                && state->elapsedSeconds > 4.f)
            {
                if (caustica::despawn(ctx.app, state->spawnedLight))
                {
                    state->despawnedLight = true;
                    state->spawnedLight = caustica::ecs::NullEntity;
                    caustica::info("thin_client: despawned point light");
                }
            }
        },
        caustica::AppSystemOrdering{}.inSet<caustica::system_set::Simulation>());

    // ---------------------------------------------------------------------
    // Spin: runs every frame and runs in parallel. Res<Time> replaces the usual
    // reason for taking SystemContext&, and SceneTransforms declares only
    // "writes LocalTransformComponent" instead of handing over the world.
    // ---------------------------------------------------------------------
    engine->addSystem<ThinClientSpinLabel>(
        caustica::AppSchedule::update,
        [](caustica::Res<caustica::Time> time,
           caustica::SceneTransforms transforms,
           caustica::Query<const ThinClientSpun> spun) {
            spun.each([&](caustica::ecs::Entity entity, const ThinClientSpun& spin) {
                const double angle = time->elapsedSeconds * double(spin.radiansPerSecond);
                const double half = 0.5 * angle;
                transforms.setRotation(
                    entity,
                    math::dquat::fromWXYZ(
                        std::cos(half), math::double3{ 0.0, std::sin(half), 0.0 }));
            });
        },
        caustica::AppSystemOrdering{}
            .inSet<caustica::system_set::Simulation>()
            .runAfter<ThinClientSetupLabel>());

    // ---------------------------------------------------------------------
    // Report: read-only per-frame work, also parallel. It writes only its own
    // resource and reads mesh components, so it overlaps with Spin.
    // ---------------------------------------------------------------------
    engine->addSystem<ThinClientReportLabel>(
        caustica::AppSchedule::update,
        [](caustica::Res<caustica::Time> time,
           caustica::ResMut<ThinClientState> state,
           caustica::Query<const caustica::scene::MeshInstanceComponent> meshes) {
            state->elapsedSeconds += time->deltaSeconds;

            if (!state->loggedQuery && state->spawnRequested)
            {
                state->loggedQuery = true;
                std::size_t count = 0;
                meshes.each([&](caustica::ecs::Entity, const auto&) { ++count; });
                caustica::info("thin_client: Query mesh instances=%zu", count);
            }
        },
        caustica::AppSystemOrdering{}
            .inSet<caustica::system_set::Simulation>()
            .runAfter<ThinClientSetupLabel>());

    return caustica::runEngineApp(std::move(engine));
}
