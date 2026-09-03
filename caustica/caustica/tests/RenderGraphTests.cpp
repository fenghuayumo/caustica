#include <render/graph/GraphBuilder.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace
{
bool expect(bool condition, const char* message)
{
    if (condition)
        return true;
    std::fprintf(stderr, "RenderGraph test failed: %s\n", message);
    return false;
}

caustica::rhi::Texture* fakeTexture(uintptr_t id)
{
    return reinterpret_cast<caustica::rhi::Texture*>(id);
}

bool passBefore(
    const std::vector<uint32_t>& order,
    caustica::rg::PassHandle earlier,
    caustica::rg::PassHandle later)
{
    const auto a = std::find(order.begin(), order.end(), earlier.index);
    const auto b = std::find(order.begin(), order.end(), later.index);
    return a != order.end() && b != order.end() && a < b;
}
}

int main()
{
    bool passed = true;
    {
        caustica::rg::GraphBuilder setupLifetimeGraph;
        auto setupCapture = std::make_shared<int>(42);
        const std::weak_ptr<int> setupCaptureWeak = setupCapture;
        setupLifetimeGraph.addPass(
            "SetupCaptureLifetime",
            [setupCapture](caustica::rg::PassBuilder&) {},
            {});
        setupCapture.reset();
        passed &= expect(setupCaptureWeak.expired(),
            "registration-only setup capture leaked into graph lifetime");
    }

    caustica::rg::GraphBuilder graph;

    const caustica::rg::PassHandle root = graph.addPass("Root", {}, {});
    const caustica::rg::PassHandle independent = graph.addPass(
        "Independent", {}, {}, caustica::rg::PassOptions{ .sideEffect = true });
    const caustica::rg::PassHandle child = graph.addPass(
        "Child", {}, {}, caustica::rg::PassOptions{ .sideEffect = true, .after = root });

    graph.compile();

    passed &= expect(root.index == 0 && independent.index == 1 && child.index == 2,
        "addPass returned unstable handles");
    passed &= expect(!graph.lastCompileHadCycle(), "acyclic graph reported a cycle");

    const std::vector<uint32_t>& order = graph.compiledPassOrder();
    passed &= expect(passBefore(order, root, child),
        "explicit PassHandle dependency was not preserved");

    const auto& waves = graph.compiledWaves();
    passed &= expect(waves.size() == 2, "independent passes were not grouped into two waves");
    passed &= expect(waves.size() >= 1 && waves[0].size() == 2,
        "independent root passes were not placed in the same wave");

    graph.reset();
    const caustica::rg::PassHandle cachedRoot = graph.addPass("Root", {}, {});
    graph.addPass("Independent", {}, {}, caustica::rg::PassOptions{ .sideEffect = true });
    graph.addPass("Child", {}, {}, caustica::rg::PassOptions{ .sideEffect = true, .after = cachedRoot });
    graph.compile();
    passed &= expect(graph.lastCompileCacheHit(), "stable graph did not reuse its compiled plan");

    {
        caustica::rg::GraphBuilder ownershipGraph;
        caustica::rg::TextureDesc desc{};
        desc.width = 8;
        desc.height = 8;
        desc.isUAV = true;
        const auto created = ownershipGraph.createTexture(desc);
        const auto imported = ownershipGraph.importTexture(
            fakeTexture(0x10), caustica::rg::TextureAccess::ShaderResource);
        passed &= expect(
            ownershipGraph.textureOwnership(created) == caustica::rg::ResourceOwnership::Graph,
            "createTexture should be graph-owned");
        passed &= expect(
            ownershipGraph.textureOwnership(imported) == caustica::rg::ResourceOwnership::External,
            "importTexture should be external");
    }

    {
        caustica::rg::GraphBuilder cullGraph;
        cullGraph.addPass("DeadNoAccess", {}, {});
        cullGraph.addPass("Live", {}, {}, caustica::rg::PassOptions{ .sideEffect = true });
        cullGraph.compile();
        passed &= expect(!cullGraph.isPassActive("DeadNoAccess"),
            "pass with no access and no sideEffect was not culled");
        passed &= expect(cullGraph.isPassActive("Live"), "sideEffect pass was culled");
    }

    {
        caustica::rg::GraphBuilder depGraph;
        const auto tex = depGraph.importTexture(
            fakeTexture(0x2000), caustica::rg::TextureAccess::UnorderedAccess);
        const auto writer = depGraph.addPass(
            "RawWriter",
            [tex](caustica::rg::PassBuilder& setup) {
                setup.write(tex, caustica::rg::TextureAccess::UnorderedAccess);
            },
            {});
        const auto reader = depGraph.addPass(
            "RawReader",
            [tex](caustica::rg::PassBuilder& setup) {
                setup.read(tex, caustica::rg::TextureAccess::ShaderResource);
            },
            {});
        const auto warWriter = depGraph.addPass(
            "WarWriter",
            [tex](caustica::rg::PassBuilder& setup) {
                setup.write(tex, caustica::rg::TextureAccess::UnorderedAccess);
            },
            {});
        depGraph.extractTexture(tex, caustica::rg::TextureAccess::UnorderedAccess);
        depGraph.compile();
        passed &= expect(
            passBefore(depGraph.compiledPassOrder(), writer, reader)
                && passBefore(depGraph.compiledPassOrder(), reader, warWriter),
            "RAW/WAR/WAW order was not preserved");
        passed &= expect(depGraph.compiledWaves().size() >= 3,
            "dependent RAW/WAR passes were not isolated into separate waves");
    }

    {
        caustica::rg::GraphBuilder cycleGraph;
        cycleGraph.addPass(
            "CycleA",
            {},
            {},
            caustica::rg::PassOptions{ .sideEffect = true, .after = caustica::rg::PassHandle{ 1 } });
        cycleGraph.addPass(
            "CycleB",
            {},
            {},
            caustica::rg::PassOptions{ .sideEffect = true, .after = caustica::rg::PassHandle{ 0 } });
        cycleGraph.compile();
        passed &= expect(cycleGraph.lastCompileHadCycle(),
            "A<->B after cycle was flattened instead of rejected");
        passed &= expect(!cycleGraph.isCompiled(), "cyclic graph was marked compiled");
    }

    {
        caustica::rg::GraphBuilder queueGraph;
        const auto tex = queueGraph.importTexture(
            fakeTexture(0x3000), caustica::rg::TextureAccess::UnorderedAccess);
        const auto compute = queueGraph.addPass(
            "ComputeWrite",
            [tex](caustica::rg::PassBuilder& setup) {
                setup.write(tex, caustica::rg::TextureAccess::UnorderedAccess);
            },
            {},
            caustica::rg::PassOptions{
                .sideEffect = true,
                .queue = caustica::rhi::CommandQueue::Compute,
            });
        const auto graphics = queueGraph.addPass(
            "GraphicsRead",
            [tex](caustica::rg::PassBuilder& setup) {
                setup.read(tex, caustica::rg::TextureAccess::ShaderResource);
            },
            {},
            caustica::rg::PassOptions{ .sideEffect = true });
        queueGraph.compile();

        passed &= expect(passBefore(queueGraph.compiledPassOrder(), compute, graphics),
            "compute producer was not scheduled before the graphics consumer");
        passed &= expect(queueGraph.compiledWaves().size() >= 2,
            "compute and graphics passes shared a wave");
        passed &= expect(
            !queueGraph.compiledWaveQueues().empty()
                && queueGraph.compiledWaveQueues().front() == caustica::rhi::CommandQueue::Compute,
            "compute wave was not emitted before graphics");
        passed &= expect(
            queueGraph.compiledWaveWaits().size() >= 2
                && !queueGraph.compiledWaveWaits()[1].empty(),
            "graphics consumer did not wait on the compute producer wave");
    }

    {
        caustica::rg::GraphBuilder cacheGraph;
        for (int i = 0; i < 17; ++i)
        {
            cacheGraph.reset();
            cacheGraph.addPass(
                std::string("CachePass") + std::to_string(i),
                {},
                {},
                caustica::rg::PassOptions{ .sideEffect = true });
            cacheGraph.compile();
        }

        cacheGraph.reset();
        cacheGraph.addPass("CachePass0", {}, {}, caustica::rg::PassOptions{ .sideEffect = true });
        cacheGraph.compile();
        passed &= expect(!cacheGraph.lastCompileCacheHit(),
            "oldest compile plan was not evicted after the cache filled");

        cacheGraph.reset();
        cacheGraph.addPass("CachePass16", {}, {}, caustica::rg::PassOptions{ .sideEffect = true });
        cacheGraph.compile();
        passed &= expect(cacheGraph.lastCompileCacheHit(),
            "newest compile plan was evicted instead of the oldest");
    }

    {
        caustica::rg::GraphBuilder handleGraph;
        caustica::rg::TextureDesc desc{};
        desc.name = "namedScratch";
        desc.width = 4;
        desc.height = 4;
        desc.isUAV = true;
        const auto first = handleGraph.createTexture(desc);
        const auto reused = handleGraph.createTexture(desc);
        passed &= expect(first.index == reused.index && first.generation == reused.generation,
            "createTexture with the same name should reuse the handle");
        passed &= expect(handleGraph.findTexture("namedScratch").index == first.index,
            "findTexture did not return the named createTexture handle");

        handleGraph.reset();
        passed &= expect(!handleGraph.isHandleCurrent(first),
            "texture handle stayed current after reset()");
    }

    {
        caustica::rg::GraphBuilder asGraph;
        auto* fakeAS = reinterpret_cast<caustica::rhi::rt::AccelStruct*>(0x5000);
        const auto as = asGraph.importAccelStruct(fakeAS, caustica::rg::AccelStructAccess::Build);
        const auto writer = asGraph.addPass(
            "AsBuild",
            [as](caustica::rg::PassBuilder& setup) {
                setup.write(as, caustica::rg::AccelStructAccess::Build);
            },
            {});
        const auto reader = asGraph.addPass(
            "AsTrace",
            [as](caustica::rg::PassBuilder& setup) {
                setup.read(as, caustica::rg::AccelStructAccess::ShaderResource);
            },
            {},
            caustica::rg::PassOptions{ .sideEffect = true });
        asGraph.compile();
        passed &= expect(
            passBefore(asGraph.compiledPassOrder(), writer, reader),
            "accel-struct RAW order was not preserved");
        passed &= expect(asGraph.compiledWaves().size() >= 2,
            "accel-struct producer and consumer shared a wave");
    }

    return passed ? 0 : 1;
}
