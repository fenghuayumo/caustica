#pragma once

#include <engine/Plugin.h>
#include <physics/Physics.h>

#include <memory>

namespace caustica::physics
{

class PhysicsPlugin final : public Plugin
{
public:
    // Passing a backend permits Jolt/MuJoCo/host adapters. With no argument the
    // plugin chooses PhysX when this build was configured with it.
    // `simulationEnabled` is true for tests/capture; the editor passes false so
    // a physics scene does not drop bodies before the user can inspect it.
    explicit PhysicsPlugin(
        std::unique_ptr<PhysicsBackend> backend = createPhysXBackend(),
        bool simulationEnabled = true)
        : m_backend(std::move(backend))
        , m_simulationEnabled(simulationEnabled)
    {
    }

    void build(App& app) override;
    void configureSchedules(App& app) override;

private:
    std::unique_ptr<PhysicsBackend> m_backend;
    bool m_simulationEnabled = true;
};

} // namespace caustica::physics
