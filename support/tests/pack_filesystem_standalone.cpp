// Standalone verification harness for PackFileSystem (not part of the engine
// build). Compile+run via support/tests/run_pack_filesystem_test.ps1.
#include <core/vfs/PackFileSystem.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

// Minimal log stub (engine links the real one).
namespace caustica
{
    void error(const char* fmt...)
    {
        (void)fmt;
    }

    // Minimal Blob implementation matching VFS.h (malloc/free contract).
    Blob::Blob(void* data, size_t size)
        : m_data(data)
        , m_size(size)
    {
    }
    Blob::~Blob()
    {
        std::free(m_data);
        m_data = nullptr;
        m_size = 0;
    }
    const void* Blob::data() const { return m_data; }
    size_t Blob::size() const { return m_size; }
} // namespace caustica

static std::vector<uint8_t> readNativeFile(const std::filesystem::path& path)
{
    std::FILE* f = nullptr;
#ifdef _WIN32
    _wfopen_s(&f, path.c_str(), L"rb");
#else
    f = std::fopen(path.string().c_str(), "rb");
#endif
    if (!f)
        return {};
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> data(static_cast<size_t>(size), 0u);
    if (size > 0)
        std::fread(data.data(), 1, data.size(), f);
    std::fclose(f);
    return data;
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::cerr << "usage: pack_filesystem_test <pack.caustica> <assetsRoot>\n";
        return 2;
    }
    const std::filesystem::path packPath = argv[1];
    const std::filesystem::path assetsRoot = argv[2];

    caustica::PackFileSystem pack(packPath, nullptr);
    if (!pack.isOpen())
    {
        std::cerr << "FAIL: pack did not open\n";
        return 1;
    }
    if (pack.getPrimarySceneEntry().empty())
    {
        std::cerr << "FAIL: no primary scene in manifest\n";
        return 1;
    }

    // Every non-manifest entry must read back byte-identical to disk.
    const char* entries[] = {
        "scenes/transparent-machines/transparent-machines.scene.json",
        "models/transparent-machines/transparent-machines-pt0.gltf",
        "models/transparent-machines/transparent-machines-pt0.bin",
        "models/transparent-machines/transparent-machines-pt1.gltf",
        "models/transparent-machines/transparent-machines-pt1.bin",
        "materials/transparent-machines-pt0.MaterialGlass1.material.json",
        "materials/transparent-machines-pt1.RandomColor_946.material.json",
        "env/simplebluesky.exr",
    };
    for (const char* entryPath : entries)
    {
        const std::filesystem::path native = assetsRoot / entryPath;
        const std::vector<uint8_t> expected = readNativeFile(native);
        auto blob = pack.readFile(entryPath);
        if (!blob || blob->size() != expected.size()
            || std::memcmp(blob->data(), expected.data(), expected.size()) != 0)
        {
            std::cerr << "FAIL: entry mismatch: " << entryPath << "\n";
            return 1;
        }
    }

    // Suffix match: absolute-style path resolves to the pack entry.
    {
        const std::filesystem::path absoluteStyle = assetsRoot / "models/transparent-machines/transparent-machines-pt1.bin";
        auto blob = pack.readFile(absoluteStyle);
        const std::vector<uint8_t> expected = readNativeFile(absoluteStyle);
        if (!blob || blob->size() != expected.size()
            || std::memcmp(blob->data(), expected.data(), expected.size()) != 0)
        {
            std::cerr << "FAIL: suffix match failed for absolute-style path\n";
            return 1;
        }
    }

    // fileExists probes.
    if (!pack.fileExists("scenes/transparent-machines/transparent-machines.scene.json")
        || pack.fileExists("scenes/does-not-exist.json"))
    {
        std::cerr << "FAIL: fileExists probes\n";
        return 1;
    }

    std::cout << "PackFileSystem standalone test passed (" << pack.getPrimarySceneEntry() << ")\n";
    return 0;
}
