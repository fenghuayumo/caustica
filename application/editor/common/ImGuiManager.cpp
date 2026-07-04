#include "common/ImGuiManager.h"

#include <imgui/imgui_renderer.h>
#include <core/vfs/VFS.h>
#include <imgui.h>

#include "EditorUI.h"
#include <core/file_utils.h>
#include <core/format.h>
#include <core/path_utils.h>
#include <core/progress.h>
#include <core/Timer.h>
#include <core/system_utils.h>
#include <core/command_line.h>
#include <core/scope.h>
#include <render/Core/ScopedPerfMarker.h>
#include <render/Core/TextureUtils.h>

namespace caustica::editor
{

using namespace caustica;

ImGuiManager::ImGuiManager(EditorUIData&           uiData,
                           const CommandLineOptions& cmdLine,
                           bool                    nvapiSERSupported)
    : m_uiData(uiData)
    , m_cmdLine(cmdLine)
    , m_nvapiSERSupported(nvapiSERSupported)
{
    ImGui::GetIO().IniFilename = nullptr;
}

ImGuiManager::~ImGuiManager() = default;

void ImGuiManager::loadDefaultFont(caustica::ImGui_Renderer& renderer,
                                    const std::filesystem::path& assetsPath)
{
    auto nativeFS = std::make_shared<NativeFileSystem>();
    auto fontPath = assetsPath / "fonts/DroidSans/DroidSans-Mono.ttf";
    renderer.CreateFontFromFile(*nativeFS, fontPath, 16.0f);
}

void ImGuiManager::configureExtensions(int graphicsAPI)
{
    if (m_extensionsConfigured)
        return;
    m_extensionsConfigured = true;

#if CAUSTICA_D3D_AGILITY_SDK_VERSION >= 619
    const bool isDX12 = (graphicsAPI == 2); // nvrhi::GraphicsAPI::D3D12
    m_uiData.session.settings.DXHitObjectExtension = isDX12;
    m_uiData.session.settings.NVAPIHitObjectExtension &= !isDX12 && m_nvapiSERSupported;
#else
    (void)graphicsAPI;
    m_uiData.session.settings.NVAPIHitObjectExtension &= m_nvapiSERSupported;
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
