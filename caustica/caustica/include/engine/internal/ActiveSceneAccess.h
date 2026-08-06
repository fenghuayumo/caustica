#pragma once

// Engine / editor access to the live Scene pointer and ActiveScene commit.
// Applications / samples must not include this.
// Hosts: EngineApp::setScene + entityWorld() / EntityWorld / SceneQuery lifecycle.

#include <engine/ActiveScene.h>

#include <filesystem>
#include <memory>
#include <string>

namespace caustica
{

class App;
class Scene;

[[nodiscard]] std::shared_ptr<Scene> activeScene(const App& app);

void commitActiveScene(
    App& app,
    std::shared_ptr<Scene> scene,
    std::string name,
    std::filesystem::path path);
void commitActiveSceneFromManager(App& app);
void clearActiveScene(App& app);

} // namespace caustica
