#pragma once

#include <functional>
#include <optional>

namespace caustica {

class DeviceManager;
class Window;

// =============================================================================
// Application — Engine layer: owns the message loop, frame pacing, and
// coordinates subsystems (Window, DeviceManager, Input, RenderPassManager).
//
// This is the REAL Application class. The old Application (scene loader)
// has been renamed to SceneApp in Application.h.
// =============================================================================
class Application
{
public:
    Application(DeviceManager* dm, Window* window = nullptr);
    ~Application() = default;

    // --- Message loop ---
    void run();

    // --- Single frame (for hosts that drive the loop, e.g. Python) ---
    bool stepFrame();
    bool stepFrame(double fixedElapsedTimeSeconds);

    // --- Frame callbacks ---
    using FrameCallback = std::function<void(DeviceManager&, uint32_t frameIndex)>;
    FrameCallback beforeFrame;

private:
    void syncWindowState();

    DeviceManager* m_DM;
    Window*        m_Window;
};

} // namespace caustica
