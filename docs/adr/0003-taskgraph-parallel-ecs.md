# ADR 0003: TaskGraph authoring layer + conflict-aware parallel ECS

| Field | Value |
| --- | --- |
| Status | **Accepted**; TaskEvent / TaskGraph + ecs parallel executor + AppSchedules planner landed |
| Date | 2026-08-28 |
| Deciders | Caustica engine |
| Relates | [ADR 0001](0001-task-runtime-multithreading.md), [architecture-render-proxy.md](../architecture-render-proxy.md), [architecture-rhi-threading.md](../architecture-rhi-threading.md) |

## Context

[ADR 0001](0001-task-runtime-multithreading.md) made `caustica::task` the sole scheduler and
migrated frames, streaming and GraphBuilder waves onto it. Two gaps remained.

**1. There is no way to express a shaped dependency graph.** `TaskRuntime` gives
`launch` + `then` + `wait`, which is enough for fan-out (`parallelFor`) and for chains, but
callers that want a real DAG have to hand-roll handle bookkeeping, and there is nothing that
detects a cycle before it deadlocks. There is also no first-class completion signal: a producer
cannot hand a consumer "this will be done later, attach to it" without either blocking a thread
or inventing another atomic + condvar pair.

**2. ECS systems run strictly one at a time.** `AppSchedules` topologically sorted each phase and
called the systems in a loop. Logic-thread simulation therefore used exactly one core no matter
how many independent systems the host registered, while the worker pool sat idle. Bevy solves
this by deriving each system's data access from its parameter list and running any two systems
concurrently when their access does not conflict; caustica already had the ingredients (typed
`Query<...>` / `Res` / `ResMut` / `Commands` parameters) but threw the type information away at
registration.

### Constraints inherited from ADR 0001

- Workers never create / submit / present / GC. `AppSchedule::render` and the Extract publish
  path stay on their own domain.
- No second scheduler. Whatever we add is an authoring layer that lowers onto `TaskRuntime`.
- ADR 0001 listed "full UE TaskGraph / fiber runtime clone" as a non-goal. That still holds:
  what lands here is the *graph and event shape*, not fibers or named-thread stealing.

## Decision

Add two layers, neither of which owns a thread:

1. **`caustica::task::TaskEvent` / `TaskGraph`** (`include/core/task/TaskGraph.h`) — a UE
   `FGraphEvent`-shaped completion signal plus a declarative DAG builder that validates, then
   lowers to ordinary `TaskRuntime` tasks and `then()` edges.
2. **`caustica::ecs` access declarations + a conflict-aware executor**
   (`TypeId.h` / `SystemAccess.h` / `SystemExecutor.h`), driven by an `AppSchedules` planner that
   turns each phase into a system DAG once and re-dispatches it every frame.

### TaskEvent

A refcounted completion signal. Created *held* (pending count 1) so the owner can register
dependencies before releasing it, which removes the classic race where the event fires while the
producer is still wiring it up.

```cpp
TaskEventRef done = TaskEvent::create("Skinning");
done->addDependency();                 // one per outstanding producer
done->then([]{ publish(); });          // continuation; runs inline if already fired
done->signal();                        // matching signal per addDependency
done->wait();                          // drains Any-affinity work while blocked
```

- `then()` before the event fires queues a `TaskRuntime` task and releases it on fire;
  `then()` after it fires runs inline on the caller. Both are legal and race-free.
- `wait()` participates via `task::helpOnce()` rather than sleeping, so a waiting worker still
  makes progress. `helpOnce()` was changed from `void` to `bool` for this — the waiter needs to
  know whether it actually drained something before it backs off.

### TaskGraph

```cpp
task::TaskGraph graph("Frame");
const auto skin = graph.addNode("Skin", [&]{ skinMeshes(); });
const auto cull = graph.addNode("Cull", [&]{ cull(); });
graph.addEdge(skin, cull);
graph.dispatchAndWait();
```

Per node: `name` / `Priority` / `Affinity` / `Pipe*` / `generation` — i.e. the whole `TaskDesc`
surface, because a node *is* a task. `validate()` rejects dangling edges and cycles with a
human-readable reason and `dispatch()` submits nothing when validation fails, so a malformed
graph is a returned error rather than a hang. `runSerial()` runs everything inline in topological
order for debugging, and `dispatch()` silently takes that path when `task::isInitialized()` is
false so tools and tests without a runtime cannot deadlock.

### ECS access declaration

`TypeIdRegistry` interns `std::type_info` into dense `uint32_t` ids so access sets can be
`AccessMask`, a growable bitset with an `intersects` test. `SystemAccess` holds four masks
(component reads/writes, resource reads/writes) plus two flags:

| Field | Meaning |
| --- | --- |
| `exclusive` | **Default true.** System can reach anything; scheduler runs it alone. |
| `deferred` | Mutates only through a `CommandQueue`; safe to run beside other systems. |

Two systems conflict when either is exclusive, or when one writes what the other reads or writes.
Anything the planner cannot prove safe stays exclusive, so the failure mode of a missing
declaration is lost parallelism, never a data race.

Access is **derived, not declared by hand**. `App::addSystem` inspects the system's parameter
pack: `Query<const T, U>` yields a read of `T` and a write of `U`, query filters
(`With` / `Without` / `Changed` / `Added`) yield reads, `Res<T>` / `ResMut<T>` yield resource
reads / writes, and `Commands` sets `deferred`. A system taking `SystemContext&` or `EntityWorld`
falls back to exclusive, because both can reach the whole world.

### Narrow parameters for the two things every frame needs

Exclusive-by-default is safe, but it is only useful if hosts have a non-exclusive way to write
ordinary per-frame systems. Two parameters existed only inside `SystemContext` / `EntityWorld`
and were dragging otherwise-parallel systems into exclusivity:

| Added | Replaces | Declares |
| --- | --- | --- |
| `Time` resource (`engine/Time.h`) | `SystemContext::deltaTimeSeconds` | read of one resource |
| `SceneTransforms` (`engine/SceneTransforms.h`) | `EntityWorld::setLocalTransform` | write of `LocalTransformComponent` |

`Time` is refreshed once per frame in `App::runFrame` before any schedule runs. Reading the clock
was the single most common reason a gameplay system took `SystemContext&`, so this alone converts
most host systems.

`SceneTransforms` is sound under the conflict rule rather than by locking: only one system can
write `LocalTransformComponent` at a time, so the per-type change-tick map has a single writer,
and rule 4 already guarantees the storage exists before dispatch. It deliberately does **not**
create a missing transform — that would be a structural edit racing systems that iterate the
registry — and it skips the `TransformChangedEvent` send that `SceneEntityWorld` performs, since
nothing reads that event (it is cleared each frame in `endChangeDetectionFrame`).

The resulting host pattern is a split rather than an avoidance: structural work keeps
`EntityWorld` and is usually one-shot, tagging entities with a marker component via
`EntityWorld::emplace`; per-frame work selects those entities with a `Query` and writes through
narrow parameters. `examples/cpp/thin_client/Main.cpp` is the reference.

There is deliberately **no public API that accepts a hand-written `SystemAccess`**. The backing
`App::addSystemWithAccess` is private and only the typed `addSystem` calls it. A hand-written
declaration is a silent correctness hazard — it can claim less than the system touches, and
nothing would catch it — so the only supported way to widen access is to widen the signature.

Writes also register a **change-tick warmup**. `ChangeDetection` stores per-component tick maps in
the registry context and used to create them lazily on first write — which under a parallel
executor means mutating the context while other systems read it. `SystemAccess::writeComponent<T>`
records a `world.ensureChangeTicks<T>()` thunk that the executor runs, single-threaded, before any
worker starts.

### SystemExecutor

`runSystemsParallel` walks a prepared `SystemNode` list (access + dependents + prerequisite count)
and maintains a running set. A node starts when its prerequisites are done and its access does not
conflict with anything currently running. Exclusive nodes wait for the running set to drain and
then execute on the scheduling thread. The scheduling thread also runs the first ready node of each
round itself rather than dispatching all of them and blocking, which keeps small phases off the
worker pool entirely. It falls back to a plain serial loop when `TaskRuntime` has no workers.

### AppSchedules planning

Each phase builds its plan once (`planDirty`) and reuses it:

1. Topologically order systems from `runBefore` / `runAfter` / set rules.
2. Record those as **explicit** edges.
3. Walk the order and add an **implicit** edge between any earlier/later pair whose access
   conflicts and that is not already ordered transitively — maintaining `reaches` / `reachedBy`
   closures so the edge count stays minimal.
4. Publish `SchedulePlanInfo` (system / exclusive / explicit-edge / implicit-edge counts and max
   parallel width) for diagnostics and tests.

Implicit edges make the result deterministic: conflicting systems always run in plan order, so
enabling parallelism cannot change observable behaviour, only timing. `describePlan()` renders the
whole thing as text.

Deferred systems get a **per-system `CommandQueue`** from a pooled `commandBuffers` vector, so two
`Commands`-taking systems never touch the same queue. The buffers are merged into the world's queue
in plan order after the phase, via `CommandQueue::append`.

### Which phases are parallel

| Phase | Mode | Why |
| --- | --- | --- |
| `First` / `preUpdate` / `update` / `PostUpdate` / `Last` | Parallel | Logic-domain simulation |
| `Startup` / `PostStartup` | Serial | One-shot; registration order is load-bearing |
| `Extract` | Serial | Publishes into the render snapshot |
| `render` | Serial | Render domain; ADR 0001 forbids handing this to Any workers |

Overridable per phase with `AppSchedules::setExecutionMode`. Global kill switch:
`AppSchedules::setParallelExecutionEnabled`, wired to `--serialSystems` through
`EngineAppDesc::parallelSystems` for debugging.

## Non-goals

- Fibers, work-stealing named threads, or a UE `FTaskGraphInterface` clone
- Parallelising Extract or the render phase (ADR 0001 R3 territory)
- Automatic access for systems taking `SystemContext&` / `World&` — those stay exclusive by design
- Multi-world / sub-app scheduling
- Replacing `parallelFor`; `TaskGraph` is for shaped graphs, not flat fan-out

## Consequences

### Positive

- Logic simulation scales past one core without hosts writing any threading code
- A missing or wrong access declaration costs parallelism, not correctness
- Cycles and dangling edges are reported at validate time instead of hanging
- `TaskEvent` gives producers a way to publish "done later" without a blocked thread
- Plans are inspectable (`describePlan`, `SchedulePlanInfo`, `lastRunStats`), so "why did this not
  go parallel" is answerable

### Negative / cost

- `App::addSystem` is now template-heavy; compile time on system registration TUs goes up
- Exclusive-by-default means a host that uses untyped `SystemContext&` systems sees no speedup
- Small phases can be slower in parallel mode than serial (dispatch overhead exceeds the work),
  which is why per-phase mode is overridable

### Risks

| Risk | Mitigation |
| --- | --- |
| Derived access misses something a system really touches | Exclusive default; typed parameters are the only way to opt out of it |
| Concurrent lazy `entt` storage creation | Change-tick warmups run single-threaded before dispatch |
| Two `Commands` systems racing one queue | Per-system buffer, merged in plan order |
| Nondeterminism between serial and parallel runs | Implicit conflict edges force plan order on any conflicting pair |
| Hidden global state in host systems | `--serialSystems` reproduces the old behaviour for bisecting |

## Implementation status

- [x] `TaskEvent` + `TaskGraph` (`core/task/TaskGraph.h` / `.cpp`); `helpOnce()` returns `bool`
- [x] `ecs::TypeId` / `TypeIdRegistry` / `AccessMask`
- [x] `ecs::SystemAccess` with conflict detection + change-tick warmups
- [x] `ecs::runSystemsParallel` + `SystemExecutorStats`, serial fallback with no workers
- [x] `AppSchedules` planner (explicit + implicit edges, `SchedulePlanInfo`, `describePlan`)
- [x] Access derived from typed system parameters in `App::addSystem` (no hand-written overload)
- [x] Per-system deferred `CommandQueue` + `CommandQueue::append` merge
- [x] `--serialSystems` → `EngineAppDesc::parallelSystems` → `setParallelExecutionEnabled`
- [x] `Time` resource + `SceneTransforms` / `EntityWorld::emplace` so per-frame systems stay narrow
- [x] `examples/cpp/thin_client` split into exclusive setup + parallel per-frame systems
- [x] Tests: `causTaskGraphTests`, `causSystemSchedulerTests`, `causSchedulePlanTests`
      (including that the documented system shapes derive non-conflicting access)

## Frozen rules

1. **Do not** add a scheduler. `TaskGraph` nodes lower to `TaskRuntime` tasks; keep it that way.
2. **Do not** expose a public `addSystem` that takes a `SystemAccess`. Access is derived from the
   signature or the system stays exclusive.
3. **Do not** make `AppSchedule::render` or `Extract` parallel without a new ADR — the RHI
   threading contract owns those domains.
4. **Do not** create `entt` storage lazily from a system body. Register a warmup.
5. **Do not** share a `CommandQueue` between systems that can run concurrently.

## Success metrics

| Metric | Signal |
| --- | --- |
| Parallelism | `SchedulePlanInfo::maxParallelWidth` > 1 on `update` in editor + thin client |
| Correctness | `--serialSystems` and parallel runs produce identical frames |
| Safety | Systems without typed parameters remain exclusive (no silent racy default) |
| Diagnosability | `describePlan()` explains every implicit edge |

## References

- Scheduler foundation: [ADR 0001](0001-task-runtime-multithreading.md)
- RHI domain rules: [architecture-rhi-threading.md](../architecture-rhi-threading.md)
- SystemSet / Extract composition: [architecture-render-proxy.md](../architecture-render-proxy.md)
- Informational: Bevy `MultiThreadedExecutor` + `SystemParam` access derivation; UE `FGraphEvent`
  completion-signal shape (not its fiber / named-thread machinery)
