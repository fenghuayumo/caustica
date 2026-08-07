// Official thin client / public API reference (P0 freeze).
// Depends only on <caustica.h> (+ math). No editor UI, ImGui, or WorldRenderer digs.
// Coverage checklist: docs/public-api.md

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

struct ThinClientSpinLabel
{
    static constexpr const char* name = "ThinClient.SpinSpawned";
};

struct ThinClientState
{
    caustica::ecs::Entity spawnedMesh = caustica::ecs::NullEntity;
    caustica::ecs::Entity spawnedLight = caustica::ecs::NullEntity;
    bool spawnRequested = false;
    bool loggedScene = false;
    bool loggedQuery = false;
    bool loggedRenderEnqueue = false;
    bool toggledVisibility = false;
    bool despawnedLight = false;
    float angleRadians = 0.f;
    float elapsedSeconds = 0.f;
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
    engine->setCameraVerticalFOV(dm::radians(55.f));
    (void)engine->settings();

    engine->emplaceResource<ThinClientState>();
    engine->addSystem<ThinClientSpinLabel>(
        caustica::AppSchedule::update,
        [](caustica::ResMut<ThinClientState> state,
           caustica::EntityWorld scene,
           caustica::Query<
               caustica::scene::LocalTransformComponent,
               caustica::scene::MeshInstanceComponent> meshes,
           caustica::SystemContext& ctx) {
            if (!scene)
                return;

            state->elapsedSeconds += ctx.deltaTimeSeconds;

            if (!state->loggedScene && caustica::isSceneLoaded(ctx.app))
            {
                state->loggedScene = true;
                caustica::info(
                    "thin_client: scene ready name=%s",
                    caustica::currentSceneName(ctx.app).c_str());
            }

            // Prefab spawn + ECS bundle spawn (point light).
            if (!state->spawnRequested && caustica::isSceneLoaded(ctx.app))
            {
                state->spawnRequested = true;
                state->spawnedMesh = caustica::spawnFromFile(
                    ctx.app, "Models/GlassSphere/GlassSphere.gltf");
                if (caustica::ecs::isValid(state->spawnedMesh))
                {
                    scene.setLocalTransform(
                        state->spawnedMesh,
                        dm::double3{ 2.0, 1.0, 0.0 },
                        std::nullopt,
                        dm::double3{ 0.5, 0.5, 0.5 });
                    caustica::info("thin_client: spawned GlassSphere entity");
                }
                else
                {
                    caustica::warning("thin_client: spawnFromFile failed");
                }

                state->spawnedLight = scene.spawnNamed(
                    "ThinClient.PointLight",
                    caustica::scene::LocalTransformComponent::fromTRS(
                        dm::double3{ 2.0, 2.5, 0.0 },
                        dm::dquat::identity(),
                        dm::double3{ 1.0, 1.0, 1.0 }),
                    caustica::scene::PointLightComponent{ .intensity = 8.f });
                if (caustica::ecs::isValid(state->spawnedLight))
                    caustica::info("thin_client: spawned point light bundle");

                // Path lookup (EntityWorld + free function).
                const caustica::ecs::Entity byPath = scene.findEntity("ThinClient.PointLight");
                if (byPath != state->spawnedLight)
                    caustica::warning("thin_client: findEntity path mismatch");
                (void)caustica::findEntity(ctx.app, "ThinClient.PointLight");
            }

            // One-shot Query over mesh instances.
            if (!state->loggedQuery && caustica::isSceneLoaded(ctx.app))
            {
                state->loggedQuery = true;
                std::size_t count = 0;
                meshes.each([&](caustica::ecs::Entity, auto&, auto&) { ++count; });
                caustica::info("thin_client: Query mesh instances=%zu", count);
            }

            if (!caustica::ecs::isValid(state->spawnedMesh))
                return;

            state->angleRadians += ctx.deltaTimeSeconds * 0.8f;
            const double half = 0.5 * static_cast<double>(state->angleRadians);
            const dm::dquat rotation = dm::dquat::fromWXYZ(
                std::cos(half),
                dm::double3{ 0.0, std::sin(half), 0.0 });
            scene.setLocalTransform(
                state->spawnedMesh,
                dm::double3{ 2.0, 1.0, 0.0 },
                rotation,
                dm::double3{ 0.5, 0.5, 0.5 });

            // Visibility toggle.
            if (!state->toggledVisibility && state->elapsedSeconds > 2.f)
            {
                state->toggledVisibility = true;
                scene.setVisible(state->spawnedMesh, true);
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

            // Demonstrate the thin RT enqueue once (non-blocking; no Logic ECS).
            if (!state->loggedRenderEnqueue)
            {
                state->loggedRenderEnqueue = true;
                caustica::EnqueueRenderCommand(ctx.app, []() {
                    caustica::info("thin_client: EnqueueRenderCommand ran on render thread");
                });
            }
        },
        caustica::AppSystemOrdering{}.inSet<caustica::system_set::Simulation>());

    return caustica::runEngineApp(std::move(engine));
}
