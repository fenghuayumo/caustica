#pragma once

#include <filesystem>
#include <string>

namespace caustica
{
struct SceneImportResult;
class SceneTypeFactory;

#if CAUSTICA_WITH_OPENUSD
// Direct OpenUSD C++ load (.usd / .usda / .usdc) into a SceneImportResult.
// Does not write or require a .caususd sidecar.
bool LoadSceneFromOpenUsd(
    const std::filesystem::path& usdPath,
    SceneTypeFactory& factory,
    SceneImportResult& result,
    std::string* errorMessage = nullptr);
#endif

} // namespace caustica
