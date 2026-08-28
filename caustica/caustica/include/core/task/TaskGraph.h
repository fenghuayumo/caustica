#pragma once

#include <core/task/TaskRuntime.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// =============================================================================
// caustica::task — declarative task graph (ADR 0003)
//
// A thin authoring layer over TaskRuntime, not a second scheduler: every node
// becomes an ordinary TaskRuntime task with the usual priority / affinity /
// pipe / generation knobs, and edges become TaskRuntime `then()` dependencies.
//
// TaskEvent is the UE FGraphEvent analogue: a shared completion signal that can
// gain continuations before or after it fires, so producers and consumers can
// be wired without either side blocking a thread.
// =============================================================================

namespace caustica::task
{

class TaskEvent;
using TaskEventRef = std::shared_ptr<TaskEvent>;

// Completion signal with a pending count. Created held (count 1) so the owner
// can register dependencies before releasing it; the event fires once every
// addDependency() has a matching signal() and the initial hold is released.
class TaskEvent
{
public:
    [[nodiscard]] static TaskEventRef create(const char* name = nullptr);

    TaskEvent(const TaskEvent&) = delete;
    TaskEvent& operator=(const TaskEvent&) = delete;

    [[nodiscard]] const char* name() const { return m_name.c_str(); }
    [[nodiscard]] bool isSignalled() const { return m_signalled.load(std::memory_order_acquire); }

    void addDependency();
    void signal();

    // Runs once the event fires; runs inline on the caller when already fired.
    void then(TaskDesc desc);
    void then(
        std::function<void()> body,
        Priority priority = Priority::Normal,
        Affinity affinity = Affinity::Any);

    // Blocks the caller, draining Any-affinity work while it waits.
    void wait();

private:
    explicit TaskEvent(const char* name)
        : m_name(name ? name : "TaskEvent")
    {
    }

    void fire();

    std::string m_name;
    std::atomic<uint32_t> m_pending{ 1 };
    std::atomic<bool> m_signalled{ false };
    std::mutex m_continuationMutex;
    std::vector<TaskHandle> m_continuations;
};

// Directed acyclic graph of tasks dispatched as a unit.
//
//   TaskGraph graph("Frame");
//   const auto skin = graph.addNode("Skin", [&] { skinMeshes(); });
//   const auto cull = graph.addNode("Cull", [&] { cull(); });
//   graph.addEdge(skin, cull);
//   graph.dispatchAndWait();
class TaskGraph
{
public:
    using NodeId = uint32_t;
    static constexpr NodeId kInvalidNode = ~NodeId{ 0 };

    struct NodeDesc
    {
        const char* name = nullptr;
        Priority priority = Priority::Normal;
        Affinity affinity = Affinity::Any;
        Pipe* pipe = nullptr;
        // 0 = immortal. Otherwise the body is skipped when the matching
        // generation counter has moved on, exactly like TaskDesc.
        uint64_t generation = 0;
        TaskDesc::GenerationDomain generationDomain = TaskDesc::GenerationDomain::Frame;
        std::function<void()> body;
    };

    explicit TaskGraph(const char* name = "TaskGraph");

    NodeId addNode(NodeDesc desc);
    NodeId addNode(
        const char* name,
        std::function<void()> body,
        Priority priority = Priority::Normal,
        Affinity affinity = Affinity::Any);

    // `before` must finish before `after` starts. Returns false for unknown or
    // self-referential ids.
    bool addEdge(NodeId before, NodeId after);
    bool addEdge(NodeId before, std::initializer_list<NodeId> after);

    [[nodiscard]] const char* name() const { return m_name.c_str(); }
    [[nodiscard]] std::size_t nodeCount() const { return m_nodes.size(); }
    [[nodiscard]] const char* nodeName(NodeId node) const;

    // Rejects dangling edges and cycles. `error` receives a human-readable
    // reason when validation fails.
    [[nodiscard]] bool validate(std::string* error = nullptr) const;

    // Deterministic topological order; empty when the graph contains a cycle.
    [[nodiscard]] std::vector<NodeId> topologicalOrder() const;

    // Submits every node. The returned event fires once the whole graph is done;
    // null when validation fails (nothing is submitted in that case).
    [[nodiscard]] TaskEventRef dispatch();

    // dispatch() + wait(). False when validation fails.
    bool dispatchAndWait();

    // Debug path: runs every node on the calling thread in topological order,
    // ignoring affinity. False when validation fails.
    bool runSerial();

    void clear();

private:
    struct Node
    {
        NodeDesc desc;
        std::vector<NodeId> successors;
        uint32_t prerequisites = 0;
    };

    [[nodiscard]] bool isValidNode(NodeId node) const { return node < m_nodes.size(); }

    std::string m_name;
    std::vector<Node> m_nodes;
};

} // namespace caustica::task
