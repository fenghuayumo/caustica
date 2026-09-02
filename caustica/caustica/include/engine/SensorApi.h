#pragma once

// Public sensor / AOV API for Caelis Sim and other hosts.
// RGB, linear depth, camera-space normals, stable instance/semantic IDs,
// and motion vectors. Multiple RenderProducts can be captured at the same
// physical time without advancing simulation.

#include <ecs/Entity.h>
#include <scene/SceneRenderData.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace caustica
{

class App;

enum class Aov : uint32_t
{
    None         = 0,
    Rgb          = 1u << 0,
    Depth        = 1u << 1,
    Normal       = 1u << 2,
    InstanceId   = 1u << 3,
    SemanticId   = 1u << 4,
    MotionVector = 1u << 5,
    Segmentation = InstanceId,
    All = Rgb | Depth | Normal | InstanceId | SemanticId | MotionVector
};

inline constexpr Aov operator|(Aov a, Aov b)
{
    return Aov(uint32_t(a) | uint32_t(b));
}
inline constexpr Aov operator&(Aov a, Aov b)
{
    return Aov(uint32_t(a) & uint32_t(b));
}
inline constexpr uint32_t aovMask(Aov aov)
{
    return uint32_t(aov);
}
inline constexpr bool hasAov(uint32_t mask, Aov aov)
{
    return (mask & uint32_t(aov)) != 0u;
}

[[nodiscard]] uint32_t parseAovMask(const std::vector<std::string>& names);
[[nodiscard]] std::string aovName(Aov aov);

// One named camera + AOV set. camera == NullEntity uses the current active camera.
struct RenderProductDesc
{
    std::string name;
    ecs::Entity camera = ecs::NullEntity;
    uint32_t aovs = uint32_t(Aov::All);
};

struct RenderProductRegistry
{
    std::vector<RenderProductDesc> products;

    // The renderer owns one live previous view, while a sensor rig may contain
    // many cameras. Preserve a previous pose per source entity and inject it
    // into the next frozen Extract for that camera.
    std::unordered_map<uint32_t, scene::ActiveCameraRenderProxy> previousCameras;
    std::optional<scene::ActiveCameraRenderProxy> pendingPreviousCamera;
};

// CPU-side sensor frame. Empty vectors mean the AOV was not requested or not available.
// Tightly packed, row-major, top-left origin, same as LdrFramebuffer.
struct SensorOutput
{
    std::string name;
    ecs::Entity camera = ecs::NullEntity;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t aovs = 0;

    std::vector<uint8_t> rgb;            // RGBA8, W*H*4
    std::vector<float> depth;            // linear |view Z| meters, W*H, 0 = miss
    std::vector<float> normal;           // camera-space XYZ, W*H*3
    std::vector<uint32_t> instanceId;    // W*H, 0 = miss
    std::vector<uint32_t> semanticId;    // W*H, 0 = unlabeled / miss
    std::vector<float> motionVector;     // pixel xy, W*H*2
};

bool addRenderProduct(App& app, RenderProductDesc product);
bool removeRenderProduct(App& app, std::string_view name);
void clearRenderProducts(App& app);
[[nodiscard]] std::vector<RenderProductDesc> renderProducts(const App& app);

bool setEntitySemanticLabel(
    App& app,
    ecs::Entity entity,
    uint32_t instanceId,
    uint32_t semanticId,
    std::string semanticLabel = {});
[[nodiscard]] uint32_t entityInstanceId(const App& app, ecs::Entity entity);
[[nodiscard]] uint32_t entitySemanticId(const App& app, ecs::Entity entity);
[[nodiscard]] std::string entitySemanticLabel(const App& app, ecs::Entity entity);

// Read AOVs for the camera that was just rendered (last stepFrame).
[[nodiscard]] std::optional<SensorOutput> readSensorOutput(App& app, uint32_t aovs = uint32_t(Aov::All));

// Capture every registered RenderProduct at the current physical time.
// Extra cameras re-extract + render without running simulation or advancing Time.
// If no products are registered, returns one output for the active camera.
[[nodiscard]] std::vector<SensorOutput> captureSensorOutputs(App& app);

} // namespace caustica
