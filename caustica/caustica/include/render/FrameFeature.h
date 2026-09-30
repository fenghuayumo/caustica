#pragma once

#include <cstdint>

namespace caustica::render
{

struct FrameGraphContext;
struct FrameSlots;

// Registration order is a GPU ordering constraint. A feature's passes are not
// contiguous in the frame graph, so one registerPasses() call cannot emit all
// of them. Phases match the slots inside registerDefaultFrameGraphPasses.
enum class FrameGraphPhase : uint8_t
{
    BeforePathTrace,
    BeforeAntiAlias,
    Composite,
};

// Render-thread, before graph compilation. Sync points stay here.
struct FramePrepareContext
{
};

// GPU half of a scene feature. Logic-side data still arrives through an Extract
// system. This is not an ECS system and must not touch EntityWorld.
class FrameFeature
{
public:
    virtual ~FrameFeature() = default;

    virtual void prepare(const FramePrepareContext&) {}
    virtual void registerPasses(FrameGraphPhase, FrameGraphContext&, FrameSlots&) {}
};

} // namespace caustica::render
