#pragma once

#include <core/vfs/VFS.h>

#include <filesystem>
#include <memory>
#include <vector>

namespace caustica
{
	//
	// A dedicated virtual file system for media assets implementing file access
	// policies as follows:
	//
	//   * all media assets are located under a single 'path' under the 'parent' 
	//     filesystem (typically a physical caustica::NativeFileSystem)
	//
	//   * on creation, the MediaFileSystem scans the media directory for all
	//     package files at the media directory root (in parent file system), and,
	//     where possible, opens them with an appropriate virtual file system 
	//     (ex. caustica::TarFile)
	//
	//   * all file paths relative to the MediaFileSystem are resolved uniquely
	//     in the following order:
	//
	//        1. search the directory structure in the parent file system for
	//           an exact match
	//
	//        2. search package files in descending lexical order
	//           (ex. zap.db => pack2.db => pack1.db => abc.db)
	//
	// note: MediaFileSystem can be mounted under a RootFileSytem
	//
	class MediaFileSystem : public caustica::IFileSystem
	{
	public:

		MediaFileSystem(std::shared_ptr<IFileSystem> parent, const std::filesystem::path& path);

		// searches media directories & packages for scene files & returns a set of unique paths
		std::vector<std::string> GetAvailableScenes() const;
	
	public:

		// VFS overrides

		bool folderExists(const std::filesystem::path& name) override;
		bool fileExists(const std::filesystem::path& name) override;
		std::shared_ptr<caustica::IBlob> readFile(const std::filesystem::path& name) override;
		bool writeFile(const std::filesystem::path& name, const void* data, size_t size) override;
		int enumerateFiles(const std::filesystem::path& path, const std::vector<std::string>& extensions, caustica::enumerate_callback_t callback, bool allowDuplicates = false) override;
		int enumerateDirectories(const std::filesystem::path& path, caustica::enumerate_callback_t callback, bool allowDuplicates = false) override;

	private:
		std::vector<std::shared_ptr<caustica::IFileSystem>> m_FileSystems;
	};
} // end namespace caustica