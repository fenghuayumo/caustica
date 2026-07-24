#include "EditorUISubsystem.h"

#include "SceneEditor.h"
#include <EditorUI.h>
#include "common/EditorViewport.h"
#include <imgui/imgui_renderer.h>

#include <engine/App.h>
#include <engine/GpuSharedCaches.h>
#include <engine/RenderFramebufferOverride.h>
#include <platform/window.h>
#include <render/passes/debug/ZoomTool.h>
#include <backend/GpuDevice.h>
#include <backend/rhi/utils.h>

#include <algorithm>

namespace caustica::editor
{

EditorUISubsystem::EditorUISubsystem(EditorUISubsystemConfig config)
    : m_config(std::move(config))
{
}

EditorUISubsystem::~EditorUISubsystem() = default;

void EditorUISubsystem::startup(caustica::GpuDevice& gpuDevice, caustica::Window& window, caustica::App& app)
{
    auto* gpuSharedCaches = app.tryResource<caustica::GpuSharedCaches>();
    if (!gpuSharedCaches || !gpuSharedCaches->shaderFactory)
        return;

    const bool serSupported = gpuDevice.supportsShaderExecutionReordering()
        && !m_config.cmdLine.disableSER;

    m_ui = std::make_unique<EditorUI>(
        &gpuDevice,
        m_config.sceneEditor,
        m_config.editorUiData,
        serSupported,
        m_config.cmdLine);
    m_ui->init(gpuSharedCaches->shaderFactory);
    m_viewport = std::make_unique<EditorViewport>();

    app.emplaceResource<caustica::RenderFramebufferOverride>();

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
    if (auto* overrideFb = m_config.app.tryResource<caustica::RenderFramebufferOverride>())
        overrideFb->framebuffer = nullptr;
    if (m_viewport)
    {
        // Ensure GPU is done with viewport / retired textures before dropping handles.
        if (caustica::GpuDevice* gpu = m_config.app.getGpuDevice())
            m_viewport->flushRetired(*gpu);
        m_viewport->release();
    }
    m_viewport.reset();
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

void EditorUISubsystem::prepareViewportForRender(caustica::GpuDevice& gpuDevice)
{
    auto* overrideFb = m_config.app.tryResource<caustica::RenderFramebufferOverride>();
    if (!overrideFb || !m_viewport || !m_ui)
        return;

    const auto& vp = m_config.editorUiData.editor.Viewport;
    if (!vp.ShowViewport || !vp.RectValid || vp.DesiredWidth < 16 || vp.DesiredHeight < 16)
    {
        overrideFb->framebuffer = nullptr;
        m_ui->setViewportColorTexture(nullptr);
        return;
    }

    // Quantize + debounce: continuous dock/window drags would otherwise recreate
    // path-tracer targets every frame and flash "Initializing renderer...".
    constexpr uint32_t kQuantize = 4u;
    constexpr auto kSettle = std::chrono::milliseconds(150);
    const uint32_t desiredW = std::max(16u, (vp.DesiredWidth + kQuantize / 2u) / kQuantize * kQuantize);
    const uint32_t desiredH = std::max(16u, (vp.DesiredHeight + kQuantize / 2u) / kQuantize * kQuantize);
    const auto now = std::chrono::steady_clock::now();

    if (desiredW != m_pendingViewportWidth || desiredH != m_pendingViewportHeight)
    {
        m_pendingViewportWidth = desiredW;
        m_pendingViewportHeight = desiredH;
        m_viewportSizeChangedAt = now;
    }

    const bool hasValidFb = m_viewport->isValid();
    const bool sizeMatches =
        hasValidFb && m_viewport->width() == desiredW && m_viewport->height() == desiredH;
    const bool settled = (now - m_viewportSizeChangedAt) >= kSettle;

    if (!hasValidFb || (settled && !sizeMatches))
        m_viewport->ensureSize(gpuDevice, desiredW, desiredH);

    if (!m_viewport->isValid())
    {
        overrideFb->framebuffer = nullptr;
        m_ui->setViewportColorTexture(nullptr);
        return;
    }

    overrideFb->framebuffer = m_viewport->framebuffer();
    m_ui->setViewportColorTexture(m_viewport->colorTexture());
}

void EditorUISubsystem::renderSceneScheduled(caustica::GpuDevice& gpuDevice)
{
    if (!m_ui)
        return;

    caustica::rhi::IFramebuffer* swapchainFb = gpuDevice.getCurrentFramebuffer(m_ui->supportsDepthBuffer());
    // Swapchain FB vectors are cleared during backBufferResizing(); skip UI GPU submit.
    if (!swapchainFb)
    {
        if (m_viewport)
            m_viewport->flushRetired(gpuDevice);
        return;
    }

    // Clear the window back buffer to editor chrome background; scene lives in the viewport texture.
    if (m_viewport && m_viewport->isValid())
    {
        caustica::rhi::CommandListHandle cmd = gpuDevice.getDevice()->createCommandList();
        cmd->open();
        caustica::rhi::utils::ClearColorAttachment(cmd, swapchainFb, 0, caustica::rhi::Color(0.08f, 0.09f, 0.11f, 1.f));
        cmd->close();
        gpuDevice.getDevice()->executeCommandList(cmd);
    }

    if (ZoomTool* zoom = m_config.sceneEditor.getOrCreateZoomTool())
    {
        if (zoom->enabled())
        {
            caustica::rhi::ITexture* color = (m_viewport && m_viewport->isValid())
                ? m_viewport->colorTexture()
                : swapchainFb->getDesc().colorAttachments[0].texture;
            if (color)
            {
                caustica::rhi::CommandListHandle commandList = gpuDevice.getDevice()->createCommandList();
                commandList->open();
                zoom->render(commandList, color);
                commandList->close();
                gpuDevice.getDevice()->executeCommandList(commandList);
            }
        }
    }

    m_ui->render(swapchainFb);

    // ImGui draw cmds from this frame may still reference the pre-resize color texture.
    if (m_viewport)
        m_viewport->flushRetired(gpuDevice);
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
