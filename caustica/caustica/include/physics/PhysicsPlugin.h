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
    explicit PhysicsPlugin(std::unique_ptr<PhysicsBackend> backend = createPhysXBackend())
        : m_backend(std::move(backend)) {}

    void build(App& app) override;
    void configureSchedules(App& app) override;

private:
    std::unique_ptr<PhysicsBackend> m_backend;
};

} // namespace caustica::physics
