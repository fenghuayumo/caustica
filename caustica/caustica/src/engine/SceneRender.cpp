#include <engine/SceneRender.h>
#include <engine/Scene.h>
#include <engine/TextureCache.h>
#include <engine/CommonRenderPasses.h>
#include <core/vfs/VFS.h>

#include <cstdlib>
#include <sstream>

#ifndef _WIN32
#include <unistd.h>
#include <cstdio>
#include <climits>
#else
#define PATH_MAX MAX_PATH
#endif // _WIN32

using namespace caustica;

SceneRender::SceneRender(DeviceManager* deviceManager)
    : Super(deviceManager)
    , m_SceneLoaded(false)
    , m_AllTexturesFinalized(false)
    , m_IsAsyncLoad(true)
{
}

void SceneRender::Render(nvrhi::IFramebuffer* framebuffer)
{
    if (m_TextureCache)
    {
        bool anyTexturesProcessed = m_TextureCache->ProcessRenderingThreadCommands(*m_CommonPasses, 20.f);

        if (m_SceneLoaded && !anyTexturesProcessed)
            m_AllTexturesFinalized = true;
    }
    else
        m_AllTexturesFinalized = true;

    if (!m_SceneLoaded || !m_AllTexturesFinalized)
    {
        RenderSplashScreen(framebuffer);
        return;
    }

    if (m_SceneLoaded && m_SceneLoadingThread)
    {
        m_SceneLoadingThread->join();
        m_SceneLoadingThread = nullptr;

        // SceneLoaded() would already get called from 
        // BeginLoadingScene() in case of synchronous loads
        SceneLoaded();
    }

    RenderScene(framebuffer);
}

void SceneRender::RenderScene(nvrhi::IFramebuffer* framebuffer)
{

}

void SceneRender::RenderSplashScreen(nvrhi::IFramebuffer* framebuffer)
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
    m_SceneLoaded = true;
}

void SceneRender::SetAsynchronousLoadingEnabled(bool enabled)
{
    m_IsAsyncLoad = enabled;
}

bool SceneRender::IsSceneLoading() const
{
    return m_SceneLoadingThread != nullptr;
}

bool SceneRender::IsSceneLoaded() const
{
    return m_SceneLoaded;
}

void SceneRender::BeginLoadingScene(std::shared_ptr<IFileSystem> fs, const std::filesystem::path& sceneFileName)
{
    if (m_SceneLoaded)
        SceneUnloading();

    m_SceneLoaded = false;
    m_AllTexturesFinalized = false;

    if (m_TextureCache)
    {
        m_TextureCache->Reset();
    }
    GetDevice()->waitForIdle();
    GetDevice()->runGarbageCollection();


    if (m_IsAsyncLoad)
    {
        m_SceneLoadingThread = std::make_unique<std::thread>([this, fs, sceneFileName]() {
			m_SceneLoaded = LoadScene(fs, sceneFileName); 
			});
    }
    else
    {
        m_SceneLoaded = LoadScene(fs, sceneFileName);
        SceneLoaded();
    }
}

std::shared_ptr<CommonRenderPasses> SceneRender::GetCommonPasses() const
{
    return m_CommonPasses;
}

const char* caustica::GetShaderTypeName(nvrhi::GraphicsAPI api)
{
    switch (api)
    {
    case nvrhi::GraphicsAPI::D3D11:
        return "dxbc";
    case nvrhi::GraphicsAPI::D3D12:
        return "dxil";
    case nvrhi::GraphicsAPI::VULKAN:
        return "spirv";
    default:
        assert(!"Unknown graphics API");
        return "";
    }
}

std::filesystem::path caustica::FindDirectoryWithShaderBin(nvrhi::GraphicsAPI api, IFileSystem& fs, const std::filesystem::path& startPath, const std::filesystem::path& relativeFilePath, const std::string& baseFileName, int maxDepth)
{
	std::string shaderFileSuffix = ".bin";
    std::filesystem::path shaderFileBasePath = GetShaderTypeName(api);
    std::filesystem::path findBytecodeFileName = relativeFilePath / shaderFileBasePath / (baseFileName + shaderFileSuffix);
	return FindDirectoryWithFile(fs, startPath, findBytecodeFileName, maxDepth);
}

std::filesystem::path caustica::FindDirectory(IFileSystem& fs, const std::filesystem::path& startPath, const std::filesystem::path& dirname, int maxDepth)
{
	std::filesystem::path searchPath = "";

	for (int depth = 0; depth < maxDepth; depth++)
	{
		std::filesystem::path currentPath = startPath / searchPath / dirname;

		if (fs.folderExists(currentPath))
		{
			return currentPath.lexically_normal();
		}

		searchPath = ".." / searchPath;
	}
	return {};
}

std::filesystem::path caustica::FindDirectoryWithFile(IFileSystem& fs, const std::filesystem::path& startPath, const std::filesystem::path& relativeFilePath, int maxDepth)
{
    std::filesystem::path searchPath = "";

    for (int depth = 0; depth < maxDepth; depth++)
    {
        std::filesystem::path currentPath = startPath / searchPath / relativeFilePath;

        if (fs.fileExists(currentPath))
        {
            return currentPath.parent_path().lexically_normal();
        }

        searchPath = ".." / searchPath;
    }
	return {};
}

std::filesystem::path caustica::FindMediaFolder(const std::filesystem::path& name)
    {
		caustica::NativeFileSystem fs;

	// first check if the environment variable is set
	const char* value = getenv(env_donut_media_path);
	if (value && fs.folderExists(value))
		return value;

	return FindDirectory(fs, GetDirectoryWithExecutable(), name);
}

// XXXX mk: as of C++20, there is no portable solution (yet ?)
std::filesystem::path caustica::GetDirectoryWithExecutable()
{
	
    char path[PATH_MAX] = {0};
#ifdef _WIN32
    if (GetModuleFileNameA(nullptr, path, dim(path)) == 0)
        return "";
#else // _WIN32
	// /proc/self/exe is mostly linux-only, but can't hurt to try it elsewhere
	if (readlink("/proc/self/exe", path, std::size(path)) <= 0)
	{
		// portable but assumes executable dir == cwd
		if (!getcwd(path, std::size(path)))
			return ""; // failure
	}
#endif // _WIN32

    std::filesystem::path result = path;
    result = result.parent_path();

    return result;
}

nvrhi::GraphicsAPI caustica::GetGraphicsAPIFromCommandLine(int argc, const char* const* argv)
{
    for (int n = 1; n < argc; n++)
    {
        const char* arg = argv[n];

        if (!strcmp(arg, "-d3d11") || !strcmp(arg, "-dx11") || !strcmp(arg, "--d3d11") || !strcmp(arg, "--dx11"))
            return nvrhi::GraphicsAPI::D3D11;
        else if (!strcmp(arg, "-d3d12") || !strcmp(arg, "-dx12") || !strcmp(arg, "--d3d12") || !strcmp(arg, "--dx12"))
            return nvrhi::GraphicsAPI::D3D12;
        else if(!strcmp(arg, "-vk") || !strcmp(arg, "-vulkan") || !strcmp(arg, "--vk") || !strcmp(arg, "--vulkan"))
            return nvrhi::GraphicsAPI::VULKAN;
    }

#if DONUT_WITH_DX12
    return nvrhi::GraphicsAPI::D3D12;
#elif DONUT_WITH_VULKAN
    return nvrhi::GraphicsAPI::VULKAN;
#elif DONUT_WITH_DX11
    return nvrhi::GraphicsAPI::D3D11;
#else
    #error "No Graphics API defined"
#endif
}

std::vector<std::string> caustica::FindScenes(caustica::IFileSystem& fs, std::filesystem::path const& path)
{
    std::vector<std::string> scenes;
    std::vector<std::string> sceneExtensions = { ".scene.json", ".gltf", ".glb" };

    std::deque<std::filesystem::path> searchList;
    searchList.push_back(path);

    while(!searchList.empty())
    {
        std::filesystem::path currentPath = searchList.front();
        searchList.pop_front();

        // search current directory
        fs.enumerateFiles(currentPath, sceneExtensions, [&scenes, &currentPath](std::string_view name)
        {
            scenes.push_back((currentPath / name).generic_string());
        });

        // search subdirectories
        fs.enumerateDirectories(currentPath, [&searchList, &currentPath](std::string_view name)
        {
            if (name != "glTF-Draco")
                searchList.push_back(currentPath / name);
        });
    }

    return scenes;
}

std::string caustica::FindPreferredScene(const std::vector<std::string>& available, const std::string& preferred)
{
    if (available.empty())
        return "";

    for (auto s : available)
        if (s.find(preferred) != std::string::npos)
            return s;

    return available.front();
}
