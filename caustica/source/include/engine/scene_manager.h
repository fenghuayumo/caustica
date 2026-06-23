/*
* Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved.
*/

#pragma once

#include <string>
#include <vector>
#include <memory>
#include <cstdint>

namespace caustica
{

class Scene;
class SceneGraph;

// Manages the collection of scenes, their loading/unloading, and switching.
// Uses PIMPL to avoid pulling in the existing engine headers.
class SceneManager
{
public:
    SceneManager();
    ~SceneManager();

    // --- Scene switching ---
    void switchScene(int index);
    void switchScene(const std::string& name);
    void applySceneSwitch();
    bool isSwitching() const;

    // --- Scene access ---
    Scene*    getCurrentScene() const;
    uint32_t  getCurrentSceneIndex() const;
    uint32_t  getSceneCount() const;

    // --- Scene management ---
    void enqueueScene(Scene* scene);
    void removeScene(int index);

    std::vector<std::string> getSceneNames() const;

    // --- File-based loading ---
    void enqueueSceneFromFile(const std::string& filePath);
    void addFileToLoadList(const std::string& filePath);
    void loadCurrentList();
    const std::vector<std::string>& getSceneFilePaths() const;

    // --- Scene graph ---
    SceneGraph* getCurrentSceneGraph() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_Pimpl;
};

} // namespace caustica
