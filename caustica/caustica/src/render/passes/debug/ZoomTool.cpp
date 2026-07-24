//
// Author(s):  Filip Strugar (filip.strugar@intel.com)
//
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#include <rhi/utils.h>
#include <assets/loader/ShaderFactory.h>
#include <render/core/RenderPassConstants.h>
#include <render/passes/debug/ZoomTool.h>
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
#include <imgui/imgui_renderer.h>

#define GLFW_INCLUDE_NONE // Do not include any OpenGL headers
#include <GLFW/glfw3.h>

using namespace caustica::math;

#include <shaders/render/misc/ZoomTool.hlsl>


ZoomTool::ZoomTool( caustica::rhi::IDevice* device, std::shared_ptr<caustica::ShaderFactory> shaderFactory )
    : m_device(device)
    , m_bindingCache(device)
{ 

    caustica::rhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = caustica::rhi::ShaderType::Compute;
    layoutDesc.bindings = { 
        caustica::rhi::BindingLayoutItem::VolatileConstantBuffer(0),
        caustica::rhi::BindingLayoutItem::Texture_UAV(0) };
    m_bindingLayout = m_device->createBindingLayout(layoutDesc);

    caustica::rhi::ComputePipelineDesc pipelineDesc;

    // These need to know about the scene
    pipelineDesc.bindingLayouts = { m_bindingLayout };
    m_CSZoomTool.init(m_device, *shaderFactory, "caustica/shaders/render/misc/ZoomTool.hlsl", "main", std::vector<caustica::ShaderMacro>(), pipelineDesc.bindingLayouts);

    m_constantBuffer = m_device->createBuffer(caustica::rhi::utils::CreateVolatileConstantBufferDesc(sizeof(ZoomToolShaderConstants), "ZoomToolShaderConstants", caustica::c_MaxRenderPassConstantBufferVersions));
}

ZoomTool::~ZoomTool( )
{
}

bool ZoomTool::keyboardUpdate(int key, int scancode, int action, int mods)
{
    if (key == GLFW_KEY_Z && action == GLFW_PRESS && mods == GLFW_MOD_CONTROL)
        m_settings.enabled = !m_settings.enabled;

    if (key == GLFW_KEY_Z && mods == GLFW_MOD_CONTROL)
        return true;

    return false;
}

void ZoomTool::mousePosUpdate(double xpos, double ypos)
{
    m_lastMousePos = float2(xpos, ypos);
}

bool ZoomTool::mouseButtonUpdate(int button, int action, int mods)
{
    if (m_settings.enabled)
    {
        if (button == GLFW_MOUSE_BUTTON_1)
        {
            if (action == GLFW_PRESS)
                m_settings.BoxPos = int2(m_lastMousePos - float2(m_settings.BoxSize.x / 2, m_settings.BoxSize.y / 2));
            return true;
        }
    }
    return false;
}

void ZoomTool::render( caustica::rhi::ICommandList * commandList, caustica::rhi::TextureHandle colorInOut )
{
    if( !m_settings.enabled )
        return;

    ZoomToolShaderConstants consts{};

    consts.SourceRectangle = float4((float)m_settings.BoxPos.x, (float)m_settings.BoxPos.y, (float)m_settings.BoxPos.x + m_settings.BoxSize.x, (float)m_settings.BoxPos.y + m_settings.BoxSize.y);
    consts.ZoomFactor = m_settings.ZoomFactor;

    {
        RAII_SCOPE(commandList->beginMarker("ZoomTool");, commandList->endMarker(); );

        commandList->writeBuffer(m_constantBuffer, &consts, sizeof(consts));
        caustica::rhi::BindingSetDesc bindingSetDesc; bindingSetDesc.bindings = {
            caustica::rhi::BindingSetItem::ConstantBuffer(0, m_constantBuffer),
            caustica::rhi::BindingSetItem::Texture_UAV(0, colorInOut, caustica::rhi::Format::RGBA8_UNORM) };

        caustica::rhi::BindingSetHandle bindingSet = m_bindingCache.getOrCreateBindingSet(bindingSetDesc, m_bindingLayout);

        int threadGroupCountX = (colorInOut->getDesc().width + 16 - 1) / 16;
        int threadGroupCountY = (colorInOut->getDesc().height + 16 - 1) / 16;

        m_CSZoomTool.execute(commandList, threadGroupCountX, threadGroupCountY, 1, bindingSet);
    }
}

bool ZoomTool::debugGUI(float indent)
{
    //ImGui::PushItemWidth(120.0f);

    ImGui::Checkbox("enabled", &m_settings.enabled);
    ImGui::InputInt("ZoomFactor", &m_settings.ZoomFactor, 1);
    m_settings.ZoomFactor = caustica::math::clamp(m_settings.ZoomFactor, 2, 32);

    ImGui::InputInt2("BoxPos", &m_settings.BoxPos.x);
    ImGui::InputInt2("BoxSize", &m_settings.BoxSize.x);

    //ImGui::PopItemWidth();
    return false;
}