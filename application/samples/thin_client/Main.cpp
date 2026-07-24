// Official thin client: EngineApp + Simulation update + spawn/transform only.
// No editor UI, ImGui, or WorldRenderer digs.

#include <engine/EngineApp.h>
#include <engine/EntryPoint.h>
#include <engine/SceneSpawn.h>
#include <engine/SceneTransform.h>
#include <engine/SceneQuery.h>
#include <engine/EnqueueRenderCommand.h>
#include <engine/SystemSets.h>

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
    caustica::ecs::Entity spawned = caustica::ecs::NullEntity;
    bool spawnRequested = false;
    bool loggedRenderEnqueue = false;
    float angleRadians = 0.f;
};

void registerThinClientSystems(caustica::EngineApp& engine)
{
    auto& app = engine.app();
    app.emplaceResource<ThinClientState>();

    app.addSystem<ThinClientSpinLabel>(
        caustica::AppSchedule::update,
        [](caustica::SystemContext& ctx) {
            auto* state = ctx.tryRes<ThinClientState>();
            if (!state)
                return;

            // Once: spawn an extra mesh into the already-loaded scene.
            if (!state->spawnRequested && caustica::isSceneLoaded(ctx.app))
            {
                state->spawnRequested = true;
                state->spawned = caustica::spawnFromFile(
                    ctx.app, "Models/GlassSphere/GlassSphere.gltf");
                if (caustica::ecs::isValid(state->spawned))
                {
                    caustica::setEntityLocalTransform(
                        ctx.app,
                        state->spawned,
                        dm::double3{ 2.0, 1.0, 0.0 },
                        std::nullopt,
                        dm::double3{ 0.5, 0.5, 0.5 });
                    caustica::info("thin_client: spawned GlassSphere entity");
                }
                else
                {
                    caustica::warning("thin_client: spawnFromFile failed");
                }
            }

            if (!caustica::ecs::isValid(state->spawned))
                return;

            state->angleRadians += ctx.deltaTimeSeconds * 0.8f;
            const double half = 0.5 * static_cast<double>(state->angleRadians);
            const dm::dquat rotation = dm::dquat::fromWXYZ(
                std::cos(half),
                dm::double3{ 0.0, std::sin(half), 0.0 });
            caustica::setEntityLocalTransform(
                ctx.app,
                state->spawned,
                dm::double3{ 2.0, 1.0, 0.0 },
                rotation,
                dm::double3{ 0.5, 0.5, 0.5 });

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
}

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
        .finishStartup = false,
    });

    if (!engine || !engine->isValid())
    {
        caustica::error("thin_client: EngineApp::create failed");
        caustica::shutdownAppPlatform();
        return 1;
    }

    registerThinClientSystems(*engine);

    if (!engine->finishStartup())
    {
        caustica::error("thin_client: finishStartup failed");
        engine->shutdown();
        caustica::shutdownAppPlatform();
        return 1;
    }

    return caustica::runEngineApp(std::move(engine));
}
