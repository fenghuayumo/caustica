#include "EditorUISubsystem.h"

#include "SceneEditor.h"
#include <EditorUI.h>
#include <imgui/imgui_renderer.h>

#include <engine/App.h>
#include <engine/RenderInfra.h>
#include <platform/window.h>
#include <render/passes/debug/ZoomTool.h>

namespace caustica::editor
{

EditorUISubsystem::EditorUISubsystem(EditorUISubsystemConfig config)
    : m_config(std::move(config))
{
}

EditorUISubsystem::~EditorUISubsystem() = default;

void EditorUISubsystem::startup(caustica::GpuDevice& gpuDevice, caustica::Window& window, caustica::App& app)
{
    auto* renderInfra = app.tryResource<caustica::RenderInfra>();
    if (!renderInfra || !renderInfra->shaderFactory)
        return;

    const bool serSupported = gpuDevice.supportsShaderExecutionReordering()
        && !m_config.cmdLine.disableSER;

    m_ui = std::make_unique<EditorUI>(
        &gpuDevice,
        m_config.sceneEditor,
        m_config.editorUiData,
        serSupported,
        m_config.cmdLine);
    m_ui->init(renderInfra->shaderFactory);

    if (caustica::Window* platformWindow = gpuDevice.getPlatformWindow())
    {
        platformWindow->setFileDropCallback(
            [this](int count, const char** paths)
            {
                for (int i = 0; i < count; ++i)
                    m_config.editorUiData.editor.PendingDroppedFiles.emplace_back(paths[i]);
            });
    }

    (void)window;
}

void EditorUISubsystem::shutdown()
{
    m_ui.reset();
}

void EditorUISubsystem::animateScheduled(float elapsedTimeSeconds, bool windowFocused)
{
    if (!m_ui)
        return;

    auto& ui = static_cast<caustica::ImGui_Renderer&>(*m_ui);
    if (windowFocused || ui.shouldAnimateUnfocused())
        ui.animate(elapsedTimeSeconds);
}

void EditorUISubsystem::renderSceneScheduled(caustica::GpuDevice& gpuDevice)
{
    if (!m_ui)
        return;

    nvrhi::IFramebuffer* framebuffer = gpuDevice.getCurrentFramebuffer(m_ui->supportsDepthBuffer());
    if (ZoomTool* zoom = m_config.sceneEditor.getOrCreateZoomTool())
    {
        if (zoom->enabled())
        {
            nvrhi::ITexture* color = framebuffer->getDesc().colorAttachments[0].texture;
            nvrhi::CommandListHandle commandList = gpuDevice.getDevice()->createCommandList();
            commandList->open();
            zoom->render(commandList, color);
            commandList->close();
            gpuDevice.getDevice()->executeCommandList(commandList);
        }
    }

    m_ui->render(framebuffer);
}

void EditorUISubsystem::onBackBufferResizing()
{
    if (m_ui)
        m_ui->backBufferResizing();
}

void EditorUISubsystem::onBackBufferResized(uint32_t width, uint32_t height, uint32_t sampleCount)
{
    if (m_ui)
        m_ui->backBufferResized(width, height, sampleCount);
}

void EditorUISubsystem::onDisplayScaleChanged(float scaleX, float scaleY)
{
    if (m_ui)
        static_cast<caustica::ImGui_Renderer&>(*m_ui).displayScaleChanged(scaleX, scaleY);
}

} // namespace caustica::editor
