// Verifies that ShaderDependencyIndex agrees with the cook that wrote the manifest.
//
// This is the linchpin of the whole scheme: the C++ fingerprint rule and the one in
// support/python/shader_cook_cache.py must produce identical strings. If they drift,
// nothing crashes — every cooked shader simply looks stale forever, silently
// restoring the full-recompile behaviour this replaced. So assert it directly
// against a real manifest.

#include <assets/loader/ShaderDependencyIndex.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

namespace
{
    int g_failures = 0;

    void check(bool condition, const std::string& what)
    {
        if (!condition)
        {
            std::printf("  FAIL: %s\n", what.c_str());
            g_failures++;
        }
    }

    std::filesystem::path findRepoRoot()
    {
        // Walk up from the working directory looking for the shader tree.
        std::filesystem::path candidate = std::filesystem::current_path();
        for (int i = 0; i < 8; i++)
        {
            if (std::filesystem::exists(candidate / "caustica" / "caustica" / "shaders"))
                return candidate;
            if (!candidate.has_parent_path() || candidate.parent_path() == candidate)
                break;
            candidate = candidate.parent_path();
        }
        return {};
    }

    struct ManifestBins
    {
        // cache hash -> (source, fingerprint)
        std::map<std::string, std::pair<std::string, std::string>> entries;
    };

    ManifestBins readBinRecords(const std::filesystem::path& manifest)
    {
        ManifestBins bins;
        std::ifstream stream(manifest, std::ios::binary);
        std::string line;
        while (std::getline(stream, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.size() < 3 || line[0] != 'B')
                continue;
            std::istringstream fields(line.substr(2));
            std::string cacheHash, source, fingerprint;
            if (fields >> cacheHash >> source >> fingerprint)
                bins.entries[cacheHash] = { source, fingerprint };
        }
        return bins;
    }
}

int main()
{
    const std::filesystem::path repoRoot = findRepoRoot();
    if (repoRoot.empty())
    {
        std::printf("SKIP: shader tree not found from '%s'\n",
            std::filesystem::current_path().string().c_str());
        return 0;
    }

    const std::filesystem::path shaderRoot = repoRoot / "caustica" / "caustica" / "shaders";
    const std::filesystem::path manifest =
        repoRoot / "bin" / "ShaderBin" / "dxil" / "deps.manifest";

    if (!std::filesystem::exists(manifest))
    {
        std::printf("SKIP: no cooked manifest at '%s' (run the PT shader cook first)\n",
            manifest.string().c_str());
        return 0;
    }

    caustica::ShaderDependencyIndex index;
    index.initialize(manifest, shaderRoot);
    check(index.load(), "manifest loads");
    check(index.isLoaded(), "index reports loaded");

    const ManifestBins bins = readBinRecords(manifest);
    check(!bins.entries.empty(), "manifest contains bin records");

    // Every recorded fingerprint must be reproduced from the sources on disk. A
    // single mismatch means the two implementations disagree.
    size_t checkedSources = 0;
    std::map<std::string, std::string> expectedBySource;
    for (const auto& [cacheHash, record] : bins.entries)
    {
        const auto& [source, fingerprint] = record;
        auto known = expectedBySource.find(source);
        if (known == expectedBySource.end())
        {
            expectedBySource.emplace(source, fingerprint);
            const std::string computed = index.currentFingerprint(source);
            check(!computed.empty(), "fingerprint computed for '" + source + "'");
            if (computed != fingerprint)
            {
                std::printf("  FAIL: fingerprint mismatch for '%s'\n    cook: %s\n    c++ : %s\n",
                    source.c_str(), fingerprint.c_str(), computed.c_str());
                g_failures++;
            }
            checkedSources++;
        }

        check(index.isUpToDate(cacheHash), "bin reported up to date: " + cacheHash);
    }

    // An unknown hash must read as stale rather than silently pass.
    check(!index.isUpToDate("0000000000000000000000000000000000000000000000000000000000000000"),
        "unknown cache hash reports stale");

    std::printf("checked %zu root sources and %zu bin records\n",
        checkedSources, bins.entries.size());

    if (g_failures != 0)
    {
        std::printf("FAILED: %d check(s)\n", g_failures);
        return 1;
    }
    std::printf("PASS: C++ fingerprints match the cook's manifest\n");
    return 0;
}
