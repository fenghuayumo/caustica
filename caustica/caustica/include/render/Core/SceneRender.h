#pragma once

#include <backend/GpuDevice.h>
#include <core/vfs/VFS.h>
#include <assets/loader/SceneLoader.h>
#include <rhi/nvrhi.h>

#include <filesystem>
#include <memory>

namespace caustica
{

class TextureCache;
class CommonRenderPasses;

} // namespace caustica

namespace caustica
{

// =============================================================================
// SceneRender — IRenderPass that manages scene loading and rendering.
//
// Loading is delegated to SceneLoader.  Subclasses implement LoadScene() to
// perform the actual work (parsing glTF/JSON, creating GPU resources, etc.).
//
// Scene discovery utilities live in scene/scene_utils.h.
// Path utilities live in core/path_utils.h.
// =============================================================================
class SceneRender : public IRenderPass
{
public:
    explicit SceneRender(GpuDevice* deviceManager);

    // --- IRenderPass overrides ---
    void SetLatewarpOptions() override { }
    void Render(nvrhi::IFramebuffer* framebuffer) override;

    // --- Scene loading API ---
    void BeginLoadingScene(std::shared_ptr<IFileSystem> fs,
        const std::filesystem::path& sceneFileName);
    virtual bool LoadScene(std::shared_ptr<IFileSystem> fs,
        const std::filesystem::path& sceneFileName) = 0;
    virtual void SceneUnloading();
    virtual void SceneLoaded();

    void SetAsynchronousLoadingEnabled(bool enabled);
    bool IsSceneLoading() const;
    bool IsSceneLoaded() const;

    // --- Rendering API ---
    virtual void RenderScene(nvrhi::IFramebuffer* framebuffer);
    virtual void RenderSplashScreen(nvrhi::IFramebuffer* framebuffer);

    std::shared_ptr<CommonRenderPasses> GetCommonPasses() const;

    std::shared_ptr<TextureCache> GetTextureCache() const { return m_TextureCache; }

protected:
    std::shared_ptr<TextureCache>        m_TextureCache;
    std::shared_ptr<CommonRenderPasses>  m_CommonPasses;
    SceneLoader                           m_Loader;

private:
    bool m_AllTexturesFinalized = false;
};

} // namespace caustica
