#include <physics/Physics.h>

#if CAUSTICA_WITH_PHYSX
#include <PxPhysicsAPI.h>

#include <unordered_map>

namespace caustica::physics
{
namespace
{
using namespace physx;

PxVec3 px(const math::double3& value) { return PxVec3(float(value.x), float(value.y), float(value.z)); }
PxQuat px(const math::dquat& value) { return PxQuat(float(value.x), float(value.y), float(value.z), float(value.w)); }

class PhysXBackend final : public PhysicsBackend
{
public:
    PhysXBackend()
    {
        m_foundation = PxCreateFoundation(PX_PHYSICS_VERSION, m_allocator, m_errorCallback);
        if (!m_foundation)
            return;
        m_physics = PxCreatePhysics(PX_PHYSICS_VERSION, *m_foundation, PxTolerancesScale());
        if (!m_physics)
            return;
        PxSceneDesc desc(m_physics->getTolerancesScale());
        desc.gravity = PxVec3(0.f, -9.81f, 0.f);
        m_dispatcher = PxDefaultCpuDispatcherCreate(2);
        desc.cpuDispatcher = m_dispatcher;
        desc.filterShader = PxDefaultSimulationFilterShader;
        m_scene = m_physics->createScene(desc);
    }

    ~PhysXBackend() override
    {
        for (auto& [_, actor] : m_actors)
            actor->release();
        if (m_scene) m_scene->release();
        if (m_dispatcher) m_dispatcher->release();
        if (m_physics) m_physics->release();
        if (m_foundation) m_foundation->release();
    }

    [[nodiscard]] const char* name() const override { return "PhysX"; }

    bool createBody(ecs::Entity entity, const RigidBodyComponent& body, const ColliderComponent& collider, const PhysicsPose& pose) override
    {
        if (!m_scene || !m_physics || m_actors.contains(entity)) return false;
        const PxTransform transform(px(pose.translation), px(pose.rotation));
        PxRigidActor* actor = nullptr;
        if (body.type == RigidBodyType::Dynamic || body.type == RigidBodyType::Kinematic)
        {
            auto* dynamic = m_physics->createRigidDynamic(transform);
            dynamic->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, body.type == RigidBodyType::Kinematic);
            dynamic->setActorFlag(PxActorFlag::eDISABLE_GRAVITY, !body.gravity);
            dynamic->setLinearVelocity(PxVec3(body.linearVelocity.x, body.linearVelocity.y, body.linearVelocity.z));
            dynamic->setAngularVelocity(PxVec3(body.angularVelocity.x, body.angularVelocity.y, body.angularVelocity.z));
            actor = dynamic;
        }
        else actor = m_physics->createRigidStatic(transform);
        if (!actor) return false;

        PxMaterial* material = m_physics->createMaterial(collider.staticFriction, collider.dynamicFriction, collider.restitution);
        PxShape* shape = nullptr;
        switch (collider.shape)
        {
        case ColliderShape::Sphere: shape = m_physics->createShape(PxSphereGeometry(collider.dimensions.x), *material); break;
        case ColliderShape::Capsule: shape = m_physics->createShape(PxCapsuleGeometry(collider.dimensions.x, collider.dimensions.y * 0.5f), *material); break;
        default: shape = m_physics->createShape(PxBoxGeometry(collider.dimensions.x * 0.5f, collider.dimensions.y * 0.5f, collider.dimensions.z * 0.5f), *material); break;
        }
        material->release();
        if (!shape) { actor->release(); return false; }
        shape->setFlag(PxShapeFlag::eSIMULATION_SHAPE, !collider.isTrigger);
        shape->setFlag(PxShapeFlag::eTRIGGER_SHAPE, collider.isTrigger);
        actor->attachShape(*shape);
        shape->release();
        if (auto* dynamic = actor->is<PxRigidDynamic>()) PxRigidBodyExt::updateMassAndInertia(*dynamic, body.mass);
        m_scene->addActor(*actor);
        m_actors.emplace(entity, actor);
        return true;
    }

    void destroyBody(ecs::Entity entity) override
    {
        if (auto it = m_actors.find(entity); it != m_actors.end()) { it->second->release(); m_actors.erase(it); }
    }
    void step(float dt) override
    {
        if (!m_scene)
            return;
        m_scene->simulate(dt);
        m_scene->fetchResults(true);
    }
    bool pose(ecs::Entity entity, PhysicsPose& out) const override
    {
        const auto it = m_actors.find(entity); if (it == m_actors.end()) return false;
        const PxTransform t = it->second->getGlobalPose();
        out.translation = math::double3(t.p.x, t.p.y, t.p.z);
        out.rotation = math::dquat(t.q.w, t.q.x, t.q.y, t.q.z);
        return true;
    }
private:
    PxDefaultAllocator m_allocator;
    PxDefaultErrorCallback m_errorCallback;
    PxFoundation* m_foundation = nullptr;
    PxPhysics* m_physics = nullptr;
    PxDefaultCpuDispatcher* m_dispatcher = nullptr;
    PxScene* m_scene = nullptr;
    std::unordered_map<ecs::Entity, PxRigidActor*> m_actors;
};
} // namespace

std::unique_ptr<PhysicsBackend> createPhysXBackend()
{
    auto backend = std::make_unique<PhysXBackend>();
    return backend;
}
bool isPhysXAvailable() { return true; }
} // namespace caustica::physics
#else
namespace caustica::physics
{
std::unique_ptr<PhysicsBackend> createPhysXBackend() { return nullptr; }
bool isPhysXAvailable() { return false; }
} // namespace caustica::physics
#endif
