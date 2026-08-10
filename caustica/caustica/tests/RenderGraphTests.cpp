#include <render/graph/GraphBuilder.h>

#include <algorithm>
#include <cstdio>
#include <memory>
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
    passed &= expect(graph.findPass("Child").index == child.index,
        "findPass did not resolve the registered pass");
    passed &= expect(!graph.findPass("Missing").isValid(),
        "findPass resolved an unknown pass");

    const std::vector<uint32_t>& order = graph.compiledPassOrder();
    const auto rootIt = std::find(order.begin(), order.end(), root.index);
    const auto childIt = std::find(order.begin(), order.end(), child.index);
    passed &= expect(rootIt != order.end() && childIt != order.end() && rootIt < childIt,
        "explicit PassHandle dependency was not preserved");

    const auto& waves = graph.compiledWaves();
    passed &= expect(waves.size() == 2, "independent passes were not grouped into two waves");
    passed &= expect(waves.size() >= 1 && waves[0].size() == 2,
        "independent root passes were not placed in the same wave");
    passed &= expect(waves.size() >= 2 && waves[1] == std::vector<uint32_t>{ child.index },
        "dependent pass was not isolated in the following wave");

    graph.reset();
    const caustica::rg::PassHandle cachedRoot = graph.addPass("Root", {}, {});
    graph.addPass("Independent", {}, {}, caustica::rg::PassOptions{ .sideEffect = true });
    const caustica::rg::PassHandle cachedChild = graph.addPass(
        "Child", {}, {}, caustica::rg::PassOptions{ .sideEffect = true, .after = cachedRoot });
    graph.compile();
    passed &= expect(graph.lastCompileCacheHit(), "stable graph did not reuse its compiled plan");
    passed &= expect(graph.compiledWaves().size() == 2,
        "cache-hit graph did not expose the borrowed wave topology");
    passed &= expect(graph.compiledPassOrder().back() == cachedChild.index,
        "cache-hit graph returned an invalid borrowed pass order");

    return passed ? 0 : 1;
}
