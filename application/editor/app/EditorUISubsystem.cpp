#include "EditorUISubsystem.h"

#include "SceneEditor.h"
#include <EditorUI.h>
#include "common/EditorViewport.h"
#include <imgui/imgui_renderer.h>

#include <engine/App.h>
#include <engine/Input.h>
#include <engine/RenderFramebufferOverride.h>
#include <engine/RenderSessionApi.h>
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
    auto factory = caustica::shaderFactory(app);
    if (!factory)
        return;

    const bool serSupported = gpuDevice.supportsShaderExecutionReordering()
        && !m_config.cmdLine.disableSER;

    m_ui = std::make_unique<EditorUI>(
        &gpuDevice,
        m_config.sceneEditor,
        m_config.editorUiData,
        serSupported,
        m_config.cmdLine,
        m_config.console);
    m_ui->init(std::move(factory));
    m_viewport = std::make_unique<EditorViewport>();
    m_compositeCommandList = gpuDevice.getDevice()->createCommandList();

    app.emplaceResource<caustica::RenderFramebufferOverride>();

    window.setFileDropCallback(
        [this](int count, const char** paths)
        {
            for (int i = 0; i < count; ++i)
                m_config.editorUiData.editor.PendingDroppedFiles.emplace_back(paths[i]);
        });
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
    m_compositeCommandList = nullptr;
    m_ui.reset();
}

void EditorUISubsystem::animateScheduled(float elapsedTimeSeconds, bool windowFocused)
{
    if (!m_ui)
        return;

    auto& ui = static_cast<caustica::ImGui_Renderer&>(*m_ui);
    if (windowFocused || ui.shouldAnimateUnfocused())
    {
        if (const auto* input = m_config.app.tryResource<caustica::InputState>())
            caustica::imGuiApplyFrameInput(*input);
        ui.animate(elapsedTimeSeconds);
    }
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
    // path-tracer targets every frame and flash the progress card.
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

    caustica::rhi::Device* rhiDevice = gpuDevice.getDevice();
    if (!rhiDevice || !rhiDevice->isDeviceHealthy())
        return;

    caustica::rhi::Framebuffer* swapchainFb = gpuDevice.getCurrentFramebuffer(m_ui->supportsDepthBuffer());
    // Swapchain FB vectors are cleared during backBufferResizing(); skip UI GPU submit.
    if (!swapchainFb)
    {
        if (m_viewport)
            m_viewport->flushRetired(gpuDevice);
        return;
    }

    ZoomTool* zoom = m_config.sceneEditor.getOrCreateZoomTool();
    const bool clearChrome = m_viewport && m_viewport->isValid();
    const bool renderZoom = zoom && zoom->enabled();
    if ((clearChrome || renderZoom) && m_compositeCommandList)
    {
        if (!m_compositeCommandList->open())
        {
            caustica::error("Editor compositor command list failed to open; stopping GPU submissions");
            gpuDevice.setShuttingDown(true);
            return;
        }

        if (clearChrome)
        {
            caustica::rhi::utils::ClearColorAttachment(
                m_compositeCommandList,
                swapchainFb,
                0,
                caustica::rhi::Color(0.08f, 0.09f, 0.11f, 1.f));
        }

        if (renderZoom)
        {
            caustica::rhi::Texture* color = (m_viewport && m_viewport->isValid())
                ? m_viewport->colorTexture()
                : swapchainFb->getDesc().colorAttachments[0].texture;
            if (color)
                zoom->render(m_compositeCommandList, color);
        }

        m_compositeCommandList->close();
        const uint64_t submission = rhiDevice->executeCommandList(m_compositeCommandList);
        if ((rhiDevice->getGraphicsAPI() == caustica::rhi::GraphicsAPI::D3D12 && submission == 0)
            || !rhiDevice->isDeviceHealthy())
        {
            gpuDevice.setShuttingDown(true);
            return;
        }
    }

    if (!rhiDevice->isDeviceHealthy())
        return;

    m_ui->render(swapchainFb);
    if (!rhiDevice->isDeviceHealthy())
        gpuDevice.setShuttingDown(true);

    // ImGui draw cmds from this frame may still reference the pre-resize color texture.
    if (m_viewport)
        m_viewport->flushRetired(gpuDevice);
}

void EditorUISubsystem::onSurfaceResizing()
{
    if (m_ui)
        m_ui->backBufferResizing();
}

void EditorUISubsystem::onSurfaceResized(uint32_t width, uint32_t height, uint32_t sampleCount)
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
