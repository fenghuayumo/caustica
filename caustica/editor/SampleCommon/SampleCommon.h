#pragma once

// =============================================================================
// SampleCommon.h — Compatibility header.
//
// Previously contained many utility functions that have been moved to the
// causticaBase layer (base/core/). This header now includes the new locations
// and only defines editor/UI-specific helpers that remain in this layer.
// =============================================================================

// --- Moved to causticaBase (base/core/) ---
#include <core/file_utils.h>
#include <core/format.h>
#include <core/path_utils.h>
#include <core/progress.h>
#include <core/Timer.h>
#include <core/system_utils.h>
#include <core/command_line.h>

// Bring base/ functions into global namespace for backward compatibility
using caustica::StringFormat;
using caustica::HexString;
using caustica::StripNonAsciiAlnum;
using caustica::ParseFloat3Consume;
using caustica::FindSubStringIgnoreCase;
using caustica::EqualsIgnoreCase;
using caustica::EnsureDirectoryExists;
using caustica::EnumerateFilesWithWildcard;
using caustica::StringLoadFromFile;
using caustica::GetLatestModifiedTimeDirectoryRecursive;
using caustica::GetFileModifiedTime;
using caustica::GetLocalPath;
using caustica::SetLocalPathBaseOverride;
using caustica::GetRuntimeDirectory;
using caustica::SetRuntimeDirectoryOverride;
using caustica::ResolveMediaRelativePath;
using caustica::ResolveSceneMediaPath;
using caustica::IsProceduralSky;
using caustica::SystemShell;
using caustica::HelpersRegisterActiveWindow;
using caustica::HelpersSetNonInteractive;
using caustica::HelpersIsNonInteractive;
using caustica::HelpersGetActiveWindow;
using caustica::c_AssetsFolder;
using caustica::c_EnvMapProcSky;
using caustica::c_EnvMapSceneDefault;
using caustica::ProgressBar;
using caustica::FPSLimiter;

// --- Editor/UI macros (remain in this layer) ---
#include <utility>
#include <imgui.h>

#define TOKEN_COMBINE1(X,Y) X##Y
#define TOKEN_COMBINE(X,Y) TOKEN_COMBINE1(X,Y)

template<typename AcquireType, typename FinalizeType>
class GenericScope
{
    FinalizeType m_finalize;
public:
    GenericScope(AcquireType&& acquire, FinalizeType&& finalize) : m_finalize(std::move(finalize)) { acquire(); }
    ~GenericScope() { m_finalize(); }
};

#define RAII_SCOPE(enter, leave) GenericScope TOKEN_COMBINE(_generic_raii_scopevar_, __COUNTER__)([&](){ enter }, [&](){ leave });
#define UI_SCOPED_INDENT(indent) RAII_SCOPE(ImGui::Indent(indent); , ImGui::Unindent(indent); )
#define UI_SCOPED_DISABLE(cond)  RAII_SCOPE(ImGui::BeginDisabled(cond); , ImGui::EndDisabled(); )

// --- Misc utilities (JSON, NVRHI-dependent) ---
#include <rhi/nvrhi.h>

class ScopedPerfMarker
{
public:
    ScopedPerfMarker(const char* label, nvrhi::CommandListHandle cmdList)
        : m_cmdList(cmdList)
    {
        cmdList->beginMarker(label);
    }
    ~ScopedPerfMarker()
    {
        m_cmdList->endMarker();
    }
private:
    nvrhi::CommandListHandle m_cmdList;
};

#include <shaders/PathTracer/Config.h>

constexpr static const int c_SwapchainCount = 3;

// JSON utilities
namespace Json { class Value; }
std::vector<std::string> JsonLoadStringVector(const Json::Value& arr);
bool SaveJsonToFile(const std::filesystem::path& filePath, const Json::Value& rootNode);
bool LoadJsonFromFile(const std::filesystem::path& filePath, Json::Value& outRootNode);
std::string SaveJsonToString(const Json::Value& rootNode);
bool LoadJsonFromString(const std::string& jsonData, Json::Value& outRootNode);

// NVRHI utility
uint64_t GetEstimatedTextureSize(const nvrhi::TextureDesc& desc);

// --- Texture compression (depends on engine types, stays in this layer) ---
#include <map>
namespace caustica { struct LoadedTexture; }

enum class TextureCompressionType
{
    Normalmap,
    GenericSRGB,
    GenericLinear,
};

bool CompressTextures(std::map<std::shared_ptr<caustica::LoadedTexture>, TextureCompressionType>& uncompressedTextures);
