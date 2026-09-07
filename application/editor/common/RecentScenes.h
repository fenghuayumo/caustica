#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace caustica::editor
{

// Most-recent-first paths, persisted across editor sessions.
const std::vector<std::string>& recentScenes();
void loadRecentScenes();
void addRecentScene(const std::filesystem::path& scenePath);
void clearRecentScenes();

// Pack-relative when possible; otherwise a stable absolute path.
[[nodiscard]] std::string recentSceneDisplayName(const std::filesystem::path& scenePath);

} // namespace caustica::editor
