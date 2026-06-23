
#pragma once

#include <engine/DeviceManager.h>
#include <core/vfs/VFS.h>
#include <rhi/nvrhi.h>
#include <filesystem>
#include <thread>
#include <vector>

namespace caustica
{
    class TextureCache;
    class CommonRenderPasses;
}

namespace caustica
{
    // Scene rendering pass (formerly Application — renamed to free up the
    // Application name for the engine-layer message loop).
    class SceneRender : public IRenderPass
    {
    private:
        bool m_SceneLoaded;
        bool m_AllTexturesFinalized;

    protected:
        typedef IRenderPass Super;

        std::shared_ptr<caustica::TextureCache> m_TextureCache;
        std::unique_ptr<std::thread> m_SceneLoadingThread;
        std::shared_ptr<caustica::CommonRenderPasses> m_CommonPasses;

        bool m_IsAsyncLoad;

    public:
        SceneRender(DeviceManager* deviceManager);

        virtual void SetLatewarpOptions() override { }
        virtual void Render(nvrhi::IFramebuffer* framebuffer) override;

        virtual void RenderScene(nvrhi::IFramebuffer* framebuffer);
        virtual void RenderSplashScreen(nvrhi::IFramebuffer* framebuffer);
        virtual void BeginLoadingScene(std::shared_ptr<caustica::IFileSystem> fs, const std::filesystem::path& sceneFileName);
        virtual bool LoadScene(std::shared_ptr<caustica::IFileSystem> fs, const std::filesystem::path& sceneFileName) = 0;
        virtual void SceneUnloading();
        virtual void SceneLoaded();

        void SetAsynchronousLoadingEnabled(bool enabled);
        bool IsSceneLoading() const;
        bool IsSceneLoaded() const;

        std::shared_ptr<caustica::CommonRenderPasses> GetCommonPasses() const;
    };

	// returns the path to the currently running application's binary
	std::filesystem::path GetDirectoryWithExecutable();

	// searches paths upward from 'startPath' for a directory 'dirname'
	std::filesystem::path FindDirectory(caustica::IFileSystem& fs, const std::filesystem::path& startPath, const std::filesystem::path& dirname, int maxDepth = 5);
    
	// searches paths upward from 'startPath' for a file with 'relativePath'
    std::filesystem::path FindDirectoryWithFile(caustica::IFileSystem& fs, const std::filesystem::path& startPath, const std::filesystem::path& relativeFilePath, int maxDepth = 5);
	
	// searches path for scene files (traverses direct subdirectories too if 'subdirs' is true)
	std::vector<std::string> FindScenes(caustica::IFileSystem& fs, std::filesystem::path const& path);

    // returns the name of the subdirectory with shaders, i.e. "dxil", "dxbc" or "spirv" - depending on the API and build settings.
    const char* GetShaderTypeName(nvrhi::GraphicsAPI api);

	// searches upward from 'startPath' for a directory containing the compiled shader 'baseFileName'
    std::filesystem::path FindDirectoryWithShaderBin(nvrhi::GraphicsAPI api, caustica::IFileSystem& fs, const std::filesystem::path& startPath, const std::filesystem::path& relativeFilePath, const std::string& baseFileName, int maxDepth = 5);

	// attempts to locate a media folder in the following sequence:
	//   1. check if the the environment variable (env_donut_media_path) is set and points to
	//      a valid location
	//   2. search updward from the directory containing the application binary for a
	//      directory with 'relativeFilePath'
	static constexpr char const* env_donut_media_path = "DONUT_MEDIA_PATH";
	std::filesystem::path FindMediaFolder(const std::filesystem::path& name = "media");
    
	// parse args for flags: -d3d11, -dx11, -d3d12, -dx12, -vulkan, -vk
    nvrhi::GraphicsAPI GetGraphicsAPIFromCommandLine(int argc, const char* const* argv);

    // searches for a given substring in the list of scenes, returns that name if found;
    // if not found, returns the first scene in the list.
    std::string FindPreferredScene(const std::vector<std::string>& available, const std::string& preferred);
}