#pragma once

#include <ecs/Entity.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace caustica
{

class Scene;

// Path marker for scenes loaded from inline JSON (no on-disk parent folder).
inline constexpr const char* inlineSceneSentinel() { return "__CAUSTICA_INLINE_SCENE_JSON__"; }

[[nodiscard]] inline bool isInlineScenePath(const std::filesystem::path& scenePath)
{
    return scenePath == std::filesystem::path(inlineSceneSentinel());
}

bool isDirectMeshSceneFile(const std::filesystem::path& sceneFileName);

std::string findPreferredScene(const std::vector<std::string>& available,
    const std::string& preferred);

ecs::Entity findEnvironmentLightEntity(const Scene& scene);

void refreshEnvironmentMapMediaList(
    const std::filesystem::path& assetsPath,
    const std::filesystem::path& envMapSubFolder,
    const std::filesystem::path& currentScenePath,
    std::vector<std::filesystem::path>& outMediaList,
    std::filesystem::path& outMediaFolder);

} // namespace caustica
