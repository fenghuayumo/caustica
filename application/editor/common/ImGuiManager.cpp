#include "common/ImGuiManager.h"
#include "common/EditorTheme.h"
#include "common/IconsMaterialSymbols.h"
#include "common/RecentScenes.h"

#include <imgui/imgui_renderer.h>
#include <core/vfs/VFS.h>
#include <imgui.h>
#include <material_symbols/material_symbols_rounded_regular.h>

#include "EditorUI.h"
#include <core/file_utils.h>
#include <core/format.h>
#include <core/path_utils.h>
#include <core/progress.h>
#include <core/Timer.h>
#include <core/system_utils.h>
#include <core/command_line.h>
#include <core/scope.h>
#include <render/core/ScopedPerfMarker.h>
#include <render/core/TextureUtils.h>

#include <filesystem>
#include <cstdlib>

namespace caustica::editor
{

using namespace caustica;

namespace
{

// Must outlive ImGui IO — ImGui stores IniFilename as a raw pointer.
std::string& EditorIniPathStorage()
{
    static std::string path;
    return path;
}

std::filesystem::path ResolveEditorIniPath()
{
#if defined(_WIN32)
    if (const char* appData = std::getenv("APPDATA"))
        return std::filesystem::path(appData) / "Caustica" / "editor.ini";
#endif
    return std::filesystem::current_path() / "caustica_editor.ini";
}

std::shared_ptr<RegisteredFont> TryLoadFont(
    ImGui_Renderer& renderer,
    IFileSystem& fs,
    const std::filesystem::path& fontPath,
    float sizePx)
{
    if (fontPath.empty())
        return nullptr;

    std::error_code ec;
    if (!std::filesystem::exists(fontPath, ec))
        return nullptr;

    auto font = renderer.createFontFromFile(fs, fontPath, sizePx);
    if (!font || !font->hasFontData())
        return nullptr;
    return font;
}

} // namespace

ImGuiManager::ImGuiManager(EditorUIData&           uiData,
                           const CommandLineOptions& cmdLine,
                           bool                    nvapiSERSupported)
    : m_uiData(uiData)
    , m_cmdLine(cmdLine)
    , m_nvapiSERSupported(nvapiSERSupported)
{
    const std::filesystem::path iniPath = ResolveEditorIniPath();
    std::error_code ec;
    std::filesystem::create_directories(iniPath.parent_path(), ec);
    EditorIniPathStorage() = iniPath.string();

    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = EditorIniPathStorage().c_str();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigDockingWithShift = false;

    // First launch (no saved layout): build the default DockSpace split once.
    if (!std::filesystem::exists(iniPath))
        m_uiData.editor.Viewport.RequestResetDockLayout = true;

    LoadEditorThemePreference();
    loadRecentScenes();
    applyTheme(1.0f);
}

ImGuiManager::~ImGuiManager() = default;

void ImGuiManager::loadDefaultFont(caustica::ImGui_Renderer& renderer,
                                    const std::filesystem::path& assetsPath)
{
    auto nativeFS = std::make_shared<NativeFileSystem>();
    constexpr float kUiFontSize = 15.0f;

    std::shared_ptr<RegisteredFont> font;

#if defined(_WIN32)
    // Prefer the system UI face — much cleaner than Proggy / monospace.
    const char* windir = std::getenv("WINDIR");
    const std::filesystem::path fontsDir =
        windir ? (std::filesystem::path(windir) / "Fonts") : std::filesystem::path("C:/Windows/Fonts");
    font = TryLoadFont(renderer, *nativeFS, fontsDir / "segoeui.ttf", kUiFontSize);
#endif

    if (!font)
        font = TryLoadFont(renderer, *nativeFS, assetsPath / "Fonts/OpenSans/OpenSans-Regular.ttf", kUiFontSize);
    if (!font)
        font = TryLoadFont(renderer, *nativeFS, assetsPath / "fonts/OpenSans/OpenSans-Regular.ttf", kUiFontSize);
    if (!font)
        font = TryLoadFont(renderer, *nativeFS, assetsPath / "Fonts/DroidSans/DroidSans-Mono.ttf", kUiFontSize);

    if (font)
        renderer.setDefaultFont(font);

    // Material Symbols BEFORE SegoeIcons. ImGui 1.92 picks the first source that
    // contains a codepoint; SegoeIcons overlaps the Material Symbols PUA and was
    // stealing open_with / autorenew / attractions (showing Fluent "login" etc.).
    constexpr float kMsSize = kUiFontSize * (9.f / 7.f);
    auto materialSymbols = renderer.createFontFromMemoryCompressed(
        g_materialSymbolsRounded_compressed_data,
        g_materialSymbolsRounded_compressed_size,
        kMsSize);
    if (materialSymbols && materialSymbols->hasFontData())
    {
        materialSymbols->configureMerge(kMaterialSymbolsFullRange);
        // Keep Fluent chrome glyphs (search/lock/eye) on SegoeIcons.
        materialSymbols->configureGlyphExcludeRanges(kFluentEditorIconExcludeFromMs);
        materialSymbols->configureGlyphOffset(ImVec2(kMsSize * 0.01f, kMsSize * 0.20f));
    }

#if defined(_WIN32)
    // Fluent / MDL2 for editor chrome (eye, lock, refresh, …).
    static const ImWchar kEditorIconRanges[] = {
        0xE721, 0xE721, // Search
        0xE72C, 0xE72E, // Refresh … Lock
        0xE785, 0xE785, // Unlock
        0xE7B3, 0xE7B3, // RedEye
        0xED1A, 0xED1A, // Hide
        0,
    };
    const char* windirIcons = std::getenv("WINDIR");
    const std::filesystem::path iconsDir =
        windirIcons ? (std::filesystem::path(windirIcons) / "Fonts")
                    : std::filesystem::path("C:/Windows/Fonts");
    std::shared_ptr<RegisteredFont> icons =
        TryLoadFont(renderer, *nativeFS, iconsDir / "SegoeIcons.ttf", kUiFontSize);
    if (!icons)
        icons = TryLoadFont(renderer, *nativeFS, iconsDir / "segmdl2.ttf", kUiFontSize);
    if (icons)
        icons->configureMerge(kEditorIconRanges);
#endif
}

void ImGuiManager::applyTheme(float displayScale)
{
    ApplyEditorTheme(displayScale);
}

void ImGuiManager::configureExtensions(int graphicsAPI)
{
    if (m_extensionsConfigured)
        return;
    m_extensionsConfigured = true;

#if CAUSTICA_D3D_AGILITY_SDK_VERSION >= 619
    const bool isDX12 = (graphicsAPI == 2); // caustica::rhi::GraphicsAPI::D3D12
    m_uiData.render.settings.DXHitObjectExtension = isDX12;
    m_uiData.render.settings.NVAPIHitObjectExtension &= !isDX12 && m_nvapiSERSupported;
#else
    (void)graphicsAPI;
    m_uiData.render.settings.NVAPIHitObjectExtension &= m_nvapiSERSupported;
#endif
}

void ImGuiManager::applyCommandLineDefaults()
{
    if (m_cmdLineApplied)
        return;
    m_cmdLineApplied = true;

    InitializeEditorUIDataFromCommandLine(m_uiData, m_cmdLine);
}

} // namespace caustica::editor
