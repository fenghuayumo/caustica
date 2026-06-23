#include <engine/SceneRender.h>
#include <engine/Scene.h>
#include <assets/cache/TextureCache.h>
#include <engine/CommonRenderPasses.h>
#include <core/vfs/VFS.h>

using namespace caustica;

SceneRender::SceneRender(GpuDevice* deviceManager)
    : IRenderPass(deviceManager)
{
    // Wire SceneLoader → SceneRender callbacks.
    // The load function itself is set by the subclass (Sample) after
    // construction because LoadScene() is pure virtual.
    m_Loader.onLoaded   = [this] { SceneLoaded(); };
    m_Loader.onUnloading = [this] { SceneUnloading(); };
}

void SceneRender::Render(nvrhi::IFramebuffer* framebuffer)
{
    if (m_TextureCache)
    {
        bool anyTexturesProcessed =
            m_TextureCache->ProcessRenderingThreadCommands(*m_CommonPasses, 20.f);

        if (m_Loader.isLoaded() && !anyTexturesProcessed)
            m_AllTexturesFinalized = true;
    }
    else
    {
        m_AllTexturesFinalized = true;
    }

    if (!m_Loader.isLoaded() || !m_AllTexturesFinalized)
    {
        RenderSplashScreen(framebuffer);
        return;
    }

    // If an async load just completed, join the thread and fire SceneLoaded()
    m_Loader.update();

    RenderScene(framebuffer);
}

void SceneRender::RenderScene(nvrhi::IFramebuffer* /*framebuffer*/)
{
}

void SceneRender::RenderSplashScreen(nvrhi::IFramebuffer* /*framebuffer*/)
{
}

void SceneRender::SceneUnloading()
{
}

void SceneRender::SceneLoaded()
{
    if (m_TextureCache)
    {
        m_TextureCache->ProcessRenderingThreadCommands(*m_CommonPasses, 0.f);
        m_TextureCache->LoadingFinished();
    }
}

void SceneRender::SetAsynchronousLoadingEnabled(bool enabled)
{
    m_Loader.setAsyncEnabled(enabled);
}

bool SceneRender::IsSceneLoading() const
{
    return m_Loader.isLoading();
}

bool SceneRender::IsSceneLoaded() const
{
    return m_Loader.isLoaded();
}

void SceneRender::BeginLoadingScene(std::shared_ptr<IFileSystem> fs,
    const std::filesystem::path& sceneFileName)
{
    m_AllTexturesFinalized = false;

    if (m_TextureCache)
        m_TextureCache->Reset();

    GetDevice()->waitForIdle();
    GetDevice()->runGarbageCollection();

    // Ensure the load function is wired (subclass should have done this in
    // its constructor, but guard against misuse).
    m_Loader.beginLoading(std::move(fs), sceneFileName);

    // Sync load: the work already completed; fire the loaded callback now.
    if (!m_Loader.isLoading())
        SceneLoaded();
}

std::shared_ptr<CommonRenderPasses> SceneRender::GetCommonPasses() const
{
    return m_CommonPasses;
}
