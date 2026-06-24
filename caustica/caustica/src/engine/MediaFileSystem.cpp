#include <engine/MediaFileSystem.h>
#include <engine/SceneRender.h>
#include <scene/scene_utils.h>
#include <core/log.h>
#include <core/string_utils.h>
#include <core/vfs/TarFile.h>

#include <unordered_set>

using namespace caustica;
using namespace caustica;

MediaFileSystem::MediaFileSystem(
	std::shared_ptr<IFileSystem> parent, 
	const std::filesystem::path& mediaFolder)
{
	// always seach media folder vfs first
	auto mediafs = std::make_shared<RelativeFileSystem>(parent, mediaFolder);

	// open package files & add a vfs for each
	NativeFileSystem* nativeFS = dynamic_cast<NativeFileSystem*>(parent.get());
	if (nativeFS)
	{
		std::vector<std::string> packs;
		if (mediafs->enumerateFiles("", { ".tar" }, caustica::enumerate_to_vector(packs)) > 0)
		{
			// sort the packs in reverse because want to search
			// from 'highest revision' of a pack file down (ex: pack2.pkz is
			// searched before pack1.db)
			std::sort(packs.rbegin(), packs.rend());

			for (auto const& fileName : packs)
			{
				std::filesystem::path filePath = mediaFolder / fileName;

				bool mounted = false;
				if (caustica::string_utils::ends_with(fileName, ".tar"))
				{
					if (auto packfs = std::make_shared<TarFile>(filePath); packfs->isOpen())
					{
						m_FileSystems.push_back(packfs);
						mounted = true;
					}
				}
				else
				{
					caustica::warning("Cannot mount '%s': unsupported format. Skipping.", filePath.string().c_str());
					continue;
				}

				if (!mounted)
				{
					caustica::warning("Failed to mount '%s' (see above for errors). Skipping.", filePath.string().c_str());
				}
			}
		}
	}
}

std::vector<std::string> MediaFileSystem::GetAvailableScenes() const
{
	std::unordered_set<std::string> resultSet;
	for (auto fs : m_FileSystems)
	{
		if (auto scenes = FindScenes(*fs, "/"); !scenes.empty())
			resultSet.insert(scenes.begin(), scenes.end());
	}

	std::vector result(resultSet.begin(), resultSet.end());
	std::sort(result.begin(), result.end());
	return result;
}

// file system virtual overrides

bool MediaFileSystem::folderExists(const std::filesystem::path & path)
{
	for (const auto& fs : m_FileSystems)
		if (fs->folderExists(path))
			return true;
	return false;
}

bool MediaFileSystem::fileExists(const std::filesystem::path & path)
{
	for (const auto& fs : m_FileSystems)
		if (fs->fileExists(path))
			return true;
	return false;
}

std::shared_ptr<IBlob> MediaFileSystem::readFile(const std::filesystem::path & name)
{
	for (const auto& fs : m_FileSystems)
		if (std::shared_ptr<caustica::IBlob> blob = fs->readFile(name))
			return blob;
	return nullptr;
}

bool MediaFileSystem::writeFile(const std::filesystem::path & name, const void* data, size_t size)
{
	for (const auto& fs : m_FileSystems)
		if (fs->writeFile(name, data, size))
			return true;
	return false;
}

int MediaFileSystem::enumerateFiles(const std::filesystem::path& path, const std::vector<std::string>& extensions, enumerate_callback_t callback, bool allowDuplicates)
{
	int numRawResults = 0;
	std::unordered_set<std::string> resultSet;
	for (const auto& fs : m_FileSystems)
	{
		if (allowDuplicates)
		{
			int result = fs->enumerateFiles(path, extensions, callback, true);
			if (result >= 0)
				numRawResults += result;
		}
		else
		{
			fs->enumerateFiles(path, extensions,
				[&resultSet](std::string_view name)
				{
					resultSet.insert(std::string(name));
				}, true);
		}
	}

	if (!allowDuplicates)
	{
		// pass the deduplicated names to the caller
		std::for_each(resultSet.begin(), resultSet.end(), callback);
		return int(resultSet.size());
	}

	return numRawResults;
}

int MediaFileSystem::enumerateDirectories(const std::filesystem::path& path, enumerate_callback_t callback, bool allowDuplicates)
{
	int numRawResults = 0;
	std::unordered_set<std::string> resultSet;
	for (const auto& fs : m_FileSystems)
	{
		if (allowDuplicates)
		{
			int result = fs->enumerateDirectories(path, callback, true);
			if (result >= 0)
				numRawResults += result;
		}
		else
		{
			fs->enumerateDirectories(path,
				[&resultSet](std::string_view name)
				{
					resultSet.insert(std::string(name));
				}, true);
		}
	}

	if (!allowDuplicates)
	{
		// pass the deduplicated names to the caller
		std::for_each(resultSet.begin(), resultSet.end(), callback);
		return int(resultSet.size());
	}

	return numRawResults;
}
