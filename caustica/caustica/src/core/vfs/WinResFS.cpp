#include <core/vfs/WinResFS.h>
#include <core/log.h>
#include <core/string_utils.h>
#include <Windows.h>

using namespace caustica;
namespace fs = std::filesystem;

class NonOwningBlob : public IBlob
{
private:
    void* m_data;
    size_t m_size;

public:
    NonOwningBlob(void* data, size_t size) : m_data(data), m_size(size) { }
    [[nodiscard]] const void* data() const override { return m_data; }
    [[nodiscard]] size_t size() const override { return m_size; }
};

static BOOL CALLBACK EnumResourcesCallback(HMODULE hModule, LPCSTR lpType, LPSTR lpName, LONG_PTR lParam)
{
    if (!IS_INTRESOURCE(lpName))
    {
        auto pNames = (std::vector<std::string>*)lParam;
        pNames->push_back(lpName);
    }

    return true;
}

caustica::WinResFileSystem::WinResFileSystem(const void* hModule, const char* type)
    : m_hModule(hModule)
    , m_Type(type)
{
    EnumResourceNamesA((HMODULE)m_hModule, m_Type.c_str(), EnumResourcesCallback, (LONG_PTR)&m_ResourceNames);
}

bool WinResFileSystem::folderExists(const fs::path& name)
{
    return false;
}

bool WinResFileSystem::fileExists(const fs::path& name)
{
    std::string nameString = name.lexically_normal().generic_string();
    caustica::string_utils::ltrim(nameString, '/');

    HRSRC hResource = FindResourceA((HMODULE)m_hModule, nameString.c_str(), m_Type.c_str());

    return (hResource != nullptr);
}

std::shared_ptr<IBlob> WinResFileSystem::readFile(const fs::path& name)
{
    std::string nameString = name.lexically_normal().generic_string();
    caustica::string_utils::ltrim(nameString, '/');

    HRSRC hResource = FindResourceA((HMODULE)m_hModule, nameString.c_str(), m_Type.c_str());

    if (hResource == nullptr)
        return nullptr;

    DWORD size = SizeofResource((HMODULE)m_hModule, hResource);
    if (size == 0)
    {
        // empty resource (can that really happen?)
        return std::make_shared<NonOwningBlob>(nullptr, 0);
    }

    HGLOBAL hGlobal = LoadResource((HMODULE)m_hModule, hResource);

    if (hGlobal == nullptr)
        return nullptr;

    void* pData = LockResource(hGlobal);
    return std::make_shared<NonOwningBlob>(pData, size);
}

bool WinResFileSystem::writeFile(const fs::path&, const void*, size_t)
{
    return false; // unsupported
}

int WinResFileSystem::enumerateFiles(const std::filesystem::path& path, const std::vector<std::string>& extensions, enumerate_callback_t callback, bool allowDuplicates)
{
    (void)allowDuplicates;

    std::basic_regex<char> regex(getFileSearchRegex(path.relative_path(), extensions), std::regex::icase);

    int numEntries = 0;
    for (const std::string& name : m_ResourceNames)
    {
        if (std::regex_match(name, regex))
        {
            callback(name);
            ++numEntries;
        }
    }

    return numEntries;
}

int WinResFileSystem::enumerateDirectories(const std::filesystem::path& path, enumerate_callback_t callback, bool allowDuplicates)
{
    (void)path;
    (void)callback;
    (void)allowDuplicates;
    return status::NotImplemented;
}
