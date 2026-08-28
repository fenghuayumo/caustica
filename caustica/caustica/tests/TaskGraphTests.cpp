#include <core/task/TaskGraph.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace
{
bool expect(bool condition, const char* message)
{
    if (condition)
        return true;
    std::fprintf(stderr, "TaskGraph test failed: %s\n", message);
    return false;
}

std::size_t indexOf(const std::vector<std::string>& log, const char* name)
{
    for (std::size_t i = 0; i < log.size(); ++i)
    {
        if (log[i] == name)
            return i;
    }
    return log.size();
}
} // namespace

int main()
{
    using namespace caustica::task;

    initialize(4);
    bool passed = true;

    // Diamond: A must precede B and C, both must precede D.
    {
        std::mutex mutex;
        std::vector<std::string> log;
        auto record = [&](const char* name) {
            std::lock_guard<std::mutex> lock(mutex);
            log.emplace_back(name);
        };

        TaskGraph graph("Diamond");
        const auto a = graph.addNode("A", [&] { record("A"); });
        const auto b = graph.addNode("B", [&] { record("B"); });
        const auto c = graph.addNode("C", [&] { record("C"); });
        const auto d = graph.addNode("D", [&] { record("D"); });
        graph.addEdge(a, { b, c });
        graph.addEdge(b, d);
        graph.addEdge(c, d);

        passed &= expect(graph.validate(nullptr), "diamond graph failed validation");
        passed &= expect(graph.dispatchAndWait(), "diamond graph failed to dispatch");
        passed &= expect(log.size() == 4, "diamond graph did not run every node");
        passed &= expect(indexOf(log, "A") < indexOf(log, "B"), "node A did not precede B");
        passed &= expect(indexOf(log, "A") < indexOf(log, "C"), "node A did not precede C");
        passed &= expect(indexOf(log, "B") < indexOf(log, "D"), "node B did not precede D");
        passed &= expect(indexOf(log, "C") < indexOf(log, "D"), "node C did not precede D");
    }

    // A cycle must be rejected instead of deadlocking the dispatcher.
    {
        TaskGraph graph("Cycle");
        const auto a = graph.addNode("A", [] {});
        const auto b = graph.addNode("B", [] {});
        graph.addEdge(a, b);
        graph.addEdge(b, a);

        std::string error;
        passed &= expect(!graph.validate(&error), "cyclic graph passed validation");
        passed &= expect(!error.empty(), "cyclic graph produced no diagnostic");
        passed &= expect(graph.topologicalOrder().empty(), "cyclic graph produced an order");
        passed &= expect(graph.dispatch() == nullptr, "cyclic graph was dispatched");
    }

    // Serial fallback runs the same nodes in topological order on this thread.
    {
        std::vector<std::string> log;
        TaskGraph graph("Serial");
        const auto first = graph.addNode("first", [&] { log.emplace_back("first"); });
        const auto second = graph.addNode("second", [&] { log.emplace_back("second"); });
        graph.addEdge(second, first);

        passed &= expect(graph.runSerial(), "serial run rejected a valid graph");
        passed &= expect(log.size() == 2 && log[0] == "second" && log[1] == "first",
            "serial run ignored the dependency edge");
    }

    // Continuations attached before the event fires.
    {
        std::atomic<int> ran{ 0 };
        TaskGraph graph("Continuation");
        graph.addNode("work", [] {});

        TaskEventRef event = graph.dispatch();
        passed &= expect(event != nullptr, "dispatch returned no event");
        if (event)
        {
            TaskEventRef done = TaskEvent::create("done");
            done->addDependency();
            event->then([&ran, done] {
                ran.fetch_add(1);
                done->signal();
            });
            done->signal();
            done->wait();
            passed &= expect(ran.load() == 1, "continuation did not run exactly once");
        }
    }

    // Continuations attached after the event already fired run immediately.
    {
        TaskGraph graph("AlreadyDone");
        graph.addNode("work", [] {});
        TaskEventRef event = graph.dispatch();
        if (event)
            event->wait();

        passed &= expect(event && event->isSignalled(), "event did not report completion");

        std::atomic<int> ran{ 0 };
        TaskEventRef done = TaskEvent::create("late");
        done->addDependency();
        event->then([&ran, done] {
            ran.fetch_add(1);
            done->signal();
        });
        done->signal();
        done->wait();
        passed &= expect(ran.load() == 1, "late continuation did not run");
    }

    // A held event must not fire until every dependency is signalled.
    {
        TaskEventRef event = TaskEvent::create("held");
        event->addDependency();
        event->signal();
        passed &= expect(!event->isSignalled(), "event fired while still held");
        event->signal();
        passed &= expect(event->isSignalled(), "event never fired after its last signal");
    }

    // An empty graph completes immediately.
    {
        TaskGraph graph("Empty");
        TaskEventRef event = graph.dispatch();
        passed &= expect(event && event->isSignalled(), "empty graph did not complete");
    }

    shutdown();
    return passed ? 0 : 1;
}
