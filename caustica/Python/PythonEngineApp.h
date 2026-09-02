#pragma once

#if CAUSTICA_WITH_PYTHON

#include "PythonDevice.h"
#include "RenderSession.h"

#include <engine/EngineApp.h>

#include <memory>
#include <stdexcept>
#include <string>

namespace caustica_py
{

// Python-facing EngineApp: owns a RenderSession (extension) or borrows the
// editor's EngineApp (embed). Methods match C++ EngineApp in snake_case.
class PyEngineApp
{
public:
    PyEngineApp(std::shared_ptr<PythonDevice> device, const RenderSession::Config& cfg);
    explicit PyEngineApp(caustica::EngineApp* borrowed);

    PyEngineApp(PyEngineApp&&) noexcept;
    PyEngineApp& operator=(PyEngineApp&&) noexcept;
    PyEngineApp(const PyEngineApp&) = delete;
    PyEngineApp& operator=(const PyEngineApp&) = delete;
    ~PyEngineApp();

    void shutdown();

    [[nodiscard]] bool isValid() const;
    [[nodiscard]] caustica::EngineApp& engine();
    [[nodiscard]] const caustica::EngineApp& engine() const;
    [[nodiscard]] caustica::EngineApp* tryEngine();
    [[nodiscard]] const caustica::EngineApp* tryEngine() const;
    [[nodiscard]] caustica::App* tryApp();
    [[nodiscard]] std::shared_ptr<PythonDevice> device() const;

    bool step(float dt = -1.f);
    bool stepN(int frames);
    int stepUntilAccumulated(int maxFrames = 0);
    int renderReferenceFrame(int spp = 64, bool oidn = true, int maxFrames = 0);
    bool renderRealtimeFrame(float dt = 1.f / 60.f);

private:
    void adoptCurrent();

    std::unique_ptr<RenderSession> m_session;
    caustica::EngineApp* m_borrowed = nullptr;
};

void setCurrentEngineApp(PyEngineApp* app);
[[nodiscard]] PyEngineApp* currentEngineApp();

} // namespace caustica_py

#endif // CAUSTICA_WITH_PYTHON
