#include "EditorUISubsystem.h"

#include "EditorApplication.h"
#include "SceneEditor.h"
#include <EditorUI.h>
#include <imgui/imgui_renderer.h>

#include <engine/Engine.h>
#include <engine/GpuRenderSubsystem.h>
#include <platform/window.h>
#include <render/Passes/Debug/ZoomTool.h>

namespace caustica::editor
{

EditorUISubsystem::EditorUISubsystem(EditorUISubsystemConfig config)
    : m_config(std::move(config))
{
}

void EditorUISubsystem::initialize(caustica::EngineInitContext& context)
{
    if (!context.gpuDevice || !context.window || !context.subsystems || !context.application)
        return;

    auto* gpuRenderSubsystem = context.subsystems->get<caustica::GpuRenderSubsystem>();
    if (!gpuRenderSubsystem)
        return;

    const bool serSupported = context.gpuDevice->SupportsShaderExecutionReordering()
        && !m_config.cmdLine.disableSER;

    m_ui = std::make_unique<EditorUI>(
        context.gpuDevice,
        m_config.editorApplication,
        m_config.editorUiData,
        serSupported,
        m_config.cmdLine);
    m_ui->Init(gpuRenderSubsystem->shaderFactory());

    if (caustica::Window* platformWindow = context.gpuDevice->GetPlatformWindow())
    {
        platformWindow->setFileDropCallback(
            [this](int count, const char** paths)
            {
                for (int i = 0; i < count; ++i)
                    m_config.editorUiData.editor.PendingDroppedFiles.emplace_back(paths[i]);
            });
    }

    caustica::Engine::syncSwapChain(*context.gpuDevice, *context.application);
}

void EditorUISubsystem::shutdown()
{
    m_ui.reset();
}

void EditorUISubsystem::onUpdate(float elapsedTimeSeconds, bool windowFocused)
{
    if (!m_ui)
        return;

    auto& ui = static_cast<caustica::ImGui_Renderer&>(*m_ui);
    if (windowFocused || ui.ShouldAnimateUnfocused())
        ui.Animate(elapsedTimeSeconds);
}

void EditorUISubsystem::onRenderScene(caustica::GpuDevice& gpuDevice)
{
    if (!m_ui)
        return;

    nvrhi::IFramebuffer* framebuffer = gpuDevice.GetCurrentFramebuffer(m_ui->SupportsDepthBuffer());
    if (ZoomTool* zoom = m_config.sceneEditor.GetOrCreateZoomTool())
    {
        if (zoom->Enabled())
        {
            nvrhi::ITexture* color = framebuffer->getDesc().colorAttachments[0].texture;
            nvrhi::CommandListHandle commandList = gpuDevice.GetDevice()->createCommandList();
            commandList->open();
            zoom->Render(commandList, color);
            commandList->close();
            gpuDevice.GetDevice()->executeCommandList(commandList);
        }
    }

    m_ui->Render(framebuffer);
}

void EditorUISubsystem::onBackBufferResizing()
{
    if (m_ui)
        m_ui->BackBufferResizing();
}

void EditorUISubsystem::onBackBufferResized(uint32_t width, uint32_t height, uint32_t sampleCount)
{
    if (m_ui)
        m_ui->BackBufferResized(width, height, sampleCount);
}

void EditorUISubsystem::onDisplayScaleChanged(float scaleX, float scaleY)
{
    if (m_ui)
        static_cast<caustica::ImGui_Renderer&>(*m_ui).DisplayScaleChanged(scaleX, scaleY);
}

} // namespace caustica::editor
