#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace caustica
{

class App;
class Scene;

namespace scene
{
class SceneEntityWorld;
}

// App-owned committed scene identity (name / path / generation).
// The live Scene pointer is engine-internal — use entityWorld() / SceneLifecycle,
// not ActiveScene::scene digs. Prefer EngineApp setScene + EntityWorld for hosts.
struct ActiveScene
{
    std::string name;
    std::filesystem::path path;
    uint64_t generation = 0;

    [[nodiscard]] bool isValid() const { return m_scene != nullptr; }

private:
    friend std::shared_ptr<Scene> activeScene(const App& app);
    friend void commitActiveScene(
        App& app,
        std::shared_ptr<Scene> scene,
        std::string name,
        std::filesystem::path path);
    friend void clearActiveScene(App& app);
    friend scene::SceneEntityWorld* entityWorld(const App& app);

    std::shared_ptr<Scene> m_scene;
};

} // namespace caustica
