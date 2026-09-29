#include <assets/AssetRegistry.h>
#include <assets/HotReload.h>
#include <core/PathUtils.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{

bool expect(bool condition, const char* message)
{
    if (condition)
        return true;
    std::fprintf(stderr, "AssetPath test failed: %s\n", message);
    return false;
}

std::filesystem::path tempSceneFile()
{
    const auto dir = std::filesystem::temp_directory_path() / "caustica_asset_path_tests";
    std::filesystem::create_directories(dir);
    return dir / "watch.scene.json";
}

void writeTempFile(const std::filesystem::path& path, const char* text)
{
    std::ofstream out(path, std::ios::trunc);
    out << text;
}

} // namespace

int main()
{
    bool passed = true;

    // --- canonicalAssetPath / canonicalAssetKey ---
    {
        // Dot-segments and separators must not change identity.
        const std::string a = caustica::canonicalAssetKey("dir/../asset.txt");
        const std::string b = caustica::canonicalAssetKey("asset.txt");
        passed &= expect(a == b, "canonicalAssetKey must collapse dot-segments");

#ifdef _WIN32
        // Windows filesystems are case-insensitive: fold in the key...
        const std::string upper = caustica::canonicalAssetKey("Asset.TXT");
        const std::string lower = caustica::canonicalAssetKey("asset.txt");
        passed &= expect(upper == lower, "canonicalAssetKey must case-fold on Windows");
#endif
        // ...but keep the path form untouched for display / non-key use.
        const std::filesystem::path pathForm = caustica::canonicalAssetPath("Asset.TXT");
        passed &= expect(
            pathForm.filename().generic_string().find("Asset.TXT") == 0,
            "canonicalAssetPath must preserve original case");
        passed &= expect(
            caustica::canonicalAssetPath(std::filesystem::path()).empty(),
            "canonicalAssetPath of an empty path must stay empty");
    }

    // --- AssetRegistry: one file, one id, regardless of spelling ---
    {
        caustica::AssetRegistry registry;
        const caustica::AssetId first =
            registry.registerAsset("scenes/MyScene.scene.json", caustica::AssetType::Scene);
        const caustica::AssetId second =
            registry.registerAsset("scenes\\../scenes/MyScene.scene.json", caustica::AssetType::Scene);
        passed &= expect(first == second, "same file with different spelling must reuse one AssetId");
        passed &= expect(
            registry.findByPath("scenes/./MyScene.scene.json") == first,
            "findByPath must canonicalize like registerAsset");

        registry.unregisterAsset(first);
        passed &= expect(
            !registry.findByPath("scenes/MyScene.scene.json").isValid(),
            "unregisterAsset must remove the canonical path entry");
    }

    // --- HotReloadTracker: owned writes across path spellings ---
    {
        const std::filesystem::path file = tempSceneFile();
        writeTempFile(file, "{}");

        caustica::HotReloadTracker tracker;
        tracker.watch(caustica::AssetId::generate(), file);

        // Prime the timestamp by polling once.
        (void)tracker.pollChangedFiles();

        // Begin with a different spelling than watch() used.
        const std::filesystem::path alternate = file.parent_path() / "." / file.filename();
        tracker.beginOwnedWrite(alternate);
        writeTempFile(file, "{\"edited\":true}");
        passed &= expect(
            tracker.pollChangedFiles().empty(),
            "owned write must not be reported as a hot-reload change");
        tracker.endOwnedWrite(alternate);
        passed &= expect(
            tracker.pollChangedFiles().empty(),
            "endOwnedWrite must advance the tracked timestamp");

        // An external edit (different mtime) is still detected.
        const auto later = std::filesystem::last_write_time(file) + std::chrono::seconds(2);
        std::filesystem::last_write_time(file, later);
        passed &= expect(
            tracker.pollChangedFiles().size() == 1,
            "external edits must still be reported");
    }

    if (passed)
        std::printf("AssetPath tests passed\n");
    return passed ? 0 : 1;
}
