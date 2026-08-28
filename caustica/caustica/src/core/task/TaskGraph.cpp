#include <core/task/TaskGraph.h>

#include <core/log.h>

#include <algorithm>
#include <utility>

namespace caustica::task
{
namespace
{

bool isStaleGeneration(uint64_t generation, TaskDesc::GenerationDomain domain)
{
    if (generation == 0)
        return false;
    const uint64_t current = (domain == TaskDesc::GenerationDomain::Load)
        ? loadGeneration()
        : frameGeneration();
    return generation != current;
}

} // namespace

// --- TaskEvent --------------------------------------------------------------

TaskEventRef TaskEvent::create(const char* name)
{
    return TaskEventRef(new TaskEvent(name));
}

void TaskEvent::addDependency()
{
    m_pending.fetch_add(1, std::memory_order_acq_rel);
}

void TaskEvent::signal()
{
    if (m_pending.fetch_sub(1, std::memory_order_acq_rel) == 1)
        fire();
}

void TaskEvent::fire()
{
    std::vector<TaskHandle> continuations;
    {
        std::lock_guard<std::mutex> lock(m_continuationMutex);
        m_signalled.store(true, std::memory_order_release);
        continuations.swap(m_continuations);
    }

    m_signalled.notify_all();

    for (TaskHandle& handle : continuations)
        submit(std::move(handle));
}

void TaskEvent::then(TaskDesc desc)
{
    {
        std::lock_guard<std::mutex> lock(m_continuationMutex);
        if (!m_signalled.load(std::memory_order_acquire))
        {
            m_continuations.push_back(caustica::task::create(std::move(desc)));
            return;
        }
    }

    (void)caustica::task::launch(std::move(desc));
}

void TaskEvent::then(std::function<void()> body, Priority priority, Affinity affinity)
{
    TaskDesc desc;
    desc.name = name();
    desc.priority = priority;
    desc.affinity = affinity;
    desc.body = std::move(body);
    then(std::move(desc));
}

void TaskEvent::wait()
{
    while (!m_signalled.load(std::memory_order_acquire))
    {
        if (!helpOnce())
            m_signalled.wait(false, std::memory_order_acquire);
    }
}

// --- TaskGraph --------------------------------------------------------------

TaskGraph::TaskGraph(const char* name)
    : m_name(name ? name : "TaskGraph")
{
}

TaskGraph::NodeId TaskGraph::addNode(NodeDesc desc)
{
    const NodeId id = static_cast<NodeId>(m_nodes.size());
    Node node;
    node.desc = std::move(desc);
    m_nodes.push_back(std::move(node));
    return id;
}

TaskGraph::NodeId TaskGraph::addNode(
    const char* name,
    std::function<void()> body,
    Priority priority,
    Affinity affinity)
{
    NodeDesc desc;
    desc.name = name;
    desc.priority = priority;
    desc.affinity = affinity;
    desc.body = std::move(body);
    return addNode(std::move(desc));
}

bool TaskGraph::addEdge(NodeId before, NodeId after)
{
    if (!isValidNode(before) || !isValidNode(after) || before == after)
        return false;

    std::vector<NodeId>& successors = m_nodes[before].successors;
    if (std::find(successors.begin(), successors.end(), after) != successors.end())
        return true;

    successors.push_back(after);
    ++m_nodes[after].prerequisites;
    return true;
}

bool TaskGraph::addEdge(NodeId before, std::initializer_list<NodeId> after)
{
    bool ok = true;
    for (NodeId target : after)
        ok = addEdge(before, target) && ok;
    return ok;
}

const char* TaskGraph::nodeName(NodeId node) const
{
    if (!isValidNode(node))
        return "<invalid>";
    const char* name = m_nodes[node].desc.name;
    return name ? name : "<unnamed>";
}

std::vector<TaskGraph::NodeId> TaskGraph::topologicalOrder() const
{
    const std::size_t count = m_nodes.size();
    std::vector<uint32_t> remaining(count);
    for (std::size_t i = 0; i < count; ++i)
        remaining[i] = m_nodes[i].prerequisites;

    std::vector<NodeId> order;
    order.reserve(count);
    std::vector<bool> emitted(count, false);

    while (order.size() < count)
    {
        // Lowest ready index first: dispatch order stays stable across runs.
        NodeId next = kInvalidNode;
        for (std::size_t i = 0; i < count; ++i)
        {
            if (!emitted[i] && remaining[i] == 0)
            {
                next = static_cast<NodeId>(i);
                break;
            }
        }

        if (next == kInvalidNode)
            return {};

        emitted[next] = true;
        order.push_back(next);
        for (NodeId successor : m_nodes[next].successors)
            --remaining[successor];
    }

    return order;
}

bool TaskGraph::validate(std::string* error) const
{
    for (std::size_t i = 0; i < m_nodes.size(); ++i)
    {
        for (NodeId successor : m_nodes[i].successors)
        {
            if (!isValidNode(successor))
            {
                if (error)
                    *error = std::string(m_name) + ": node '" + nodeName(static_cast<NodeId>(i))
                        + "' has an edge to an unknown node";
                return false;
            }
        }
    }

    if (!m_nodes.empty() && topologicalOrder().empty())
    {
        if (error)
            *error = std::string(m_name) + ": dependency cycle";
        return false;
    }

    return true;
}

TaskEventRef TaskGraph::dispatch()
{
    std::string error;
    if (!validate(&error))
    {
        caustica::error("TaskGraph::dispatch rejected graph: %s", error.c_str());
        return nullptr;
    }

    TaskEventRef event = TaskEvent::create(m_name.c_str());
    if (m_nodes.empty() || !isInitialized())
    {
        // No runtime means no workers to drain the queue; run inline instead of
        // handing back an event that can never fire.
        for (NodeId node : topologicalOrder())
        {
            const NodeDesc& desc = m_nodes[node].desc;
            if (desc.body && !isStaleGeneration(desc.generation, desc.generationDomain))
                desc.body();
        }
        event->signal();
        return event;
    }

    const std::size_t count = m_nodes.size();
    std::vector<TaskHandle> handles;
    handles.reserve(count);

    for (const Node& node : m_nodes)
    {
        event->addDependency();

        TaskDesc desc;
        desc.name = node.desc.name ? node.desc.name : "TaskGraph.Node";
        desc.priority = node.desc.priority;
        desc.affinity = node.desc.affinity;
        desc.pipe = node.desc.pipe;
        // Generation is checked inside the body: letting TaskRuntime skip the
        // wrapper would leave the event permanently unsignalled.
        desc.body = [body = node.desc.body,
                     event,
                     generation = node.desc.generation,
                     domain = node.desc.generationDomain]() {
            if (body && !isStaleGeneration(generation, domain))
                body();
            event->signal();
        };

        handles.push_back(caustica::task::create(std::move(desc)));
    }

    // All edges must be wired before anything is submitted: TaskRuntime only
    // re-queues a task whose prerequisites were registered ahead of submit().
    for (std::size_t i = 0; i < count; ++i)
    {
        for (NodeId successor : m_nodes[i].successors)
            caustica::task::then(handles[i], handles[successor]);
    }

    for (TaskHandle& handle : handles)
        caustica::task::submit(std::move(handle));

    event->signal();
    return event;
}

bool TaskGraph::dispatchAndWait()
{
    TaskEventRef event = dispatch();
    if (!event)
        return false;

    event->wait();
    return true;
}

bool TaskGraph::runSerial()
{
    std::string error;
    if (!validate(&error))
    {
        caustica::error("TaskGraph::runSerial rejected graph: %s", error.c_str());
        return false;
    }

    for (NodeId node : topologicalOrder())
    {
        const NodeDesc& desc = m_nodes[node].desc;
        if (desc.body && !isStaleGeneration(desc.generation, desc.generationDomain))
            desc.body();
    }

    return true;
}

void TaskGraph::clear()
{
    m_nodes.clear();
}

} // namespace caustica::task
