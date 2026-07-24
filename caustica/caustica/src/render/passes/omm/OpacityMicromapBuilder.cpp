#include <render/passes/omm/OpacityMicromapBuilder.h>
#include <render/core/RenderDevice.h>
#include <render/SceneGpuResources.h>
#include <render/passes/lighting/MaterialGpuCache.h> // for StandardMaterial full definition

#include <assets/loader/ShaderFactory.h>
#include <render/core/FramebufferFactory.h>
#include <assets/loader/TextureLoader.h>

#include <core/scope.h>
#include <scene/ResourceTracker.h>

#include <rhi/utils.h>
#include <rhi/common/misc.h>

#include <imgui/imgui_renderer.h>
#include <imgui/ui_macros.h>

#include <core/file_utils.h>
#include <core/format.h>
#include <core/log.h>
#include <core/path_utils.h>
#include <core/progress.h>
#include <core/Timer.h>
#include <core/system_utils.h>
#include <core/command_line.h>
#include <core/scope.h>
#include <render/core/ScopedPerfMarker.h>
#include <render/core/TextureUtils.h>

#include <render/passes/omm/OmmBuildQueue.h>

using namespace caustica::math;
using namespace caustica;

#include <shaders/Misc/OmmGeometryDebugData.hlsli>

OpacityMicromapBuilder::OpacityMicromapBuilder(caustica::rhi::DeviceHandle device,
    std::shared_ptr<caustica::DescriptorTableManager> descriptorTableManager,
    std::shared_ptr<caustica::TextureLoader> textureCache,
    std::shared_ptr<caustica::ShaderFactory> shaderFactory)
    : m_device(device)
    , m_textureCache(std::move(textureCache))
    , m_bindingCache(device)
    , m_shaderFactory(std::move(shaderFactory))
    , m_descriptorTableManager(std::move(descriptorTableManager))
{
    // Setup OMM baker.
    m_ommBuildQueue = std::make_unique<OmmBuildQueue>(device, m_descriptorTableManager, m_shaderFactory);

    // allocate dummy buffer that works even if not enabled
    caustica::rhi::BufferDesc bufferDesc;
    bufferDesc.byteSize = sizeof(GeometryDebugData) * 1024;
    bufferDesc.debugName = "BindlessGeometryDebug";
    bufferDesc.structStride = sizeof(GeometryDebugData);
    bufferDesc.canHaveRawViews = true;
    bufferDesc.canHaveUAVs = true;
    bufferDesc.initialState = caustica::rhi::ResourceStates::Common;
    bufferDesc.keepInitialState = true;
    m_geometryDebugBuffer = m_device->createBuffer(bufferDesc);
}

OpacityMicromapBuilder::~OpacityMicromapBuilder()
{
}

void OpacityMicromapBuilder::ensureGeometryDebugCapacity(size_t geometryCount)
{
    const size_t allocationGranularity = 1024;
    if (geometryCount <= m_geometryDebugDataPtr.size())
        return;

    m_geometryDebugDataPtr.resize(caustica::rhi::align<size_t>(geometryCount, allocationGranularity));

    caustica::rhi::BufferDesc bufferDesc;
    bufferDesc.byteSize = sizeof(GeometryDebugData) * m_geometryDebugDataPtr.size();
    bufferDesc.debugName = "BindlessGeometryDebug";
    bufferDesc.structStride = sizeof(GeometryDebugData);
    bufferDesc.canHaveRawViews = true;
    bufferDesc.canHaveUAVs = true;
    bufferDesc.initialState = caustica::rhi::ResourceStates::Common;
    bufferDesc.keepInitialState = true;

    m_geometryDebugBuffer = m_device->createBuffer(bufferDesc);
}

void OpacityMicromapBuilder::sceneLoaded(size_t geometryCount)
{
    // Runtime imports grow geometry count without calling sceneLoaded again.
    ensureGeometryDebugCapacity(geometryCount);
}

void OpacityMicromapBuilder::sceneUnloading()
{
    m_ommBuildQueue->cancelPendingBuilds();
}

void OpacityMicromapBuilder::createRenderPasses(
    caustica::rhi::BindingLayoutHandle bindlessLayout,
    caustica::render::RenderDevice& renderDevice)
{
    m_bindlessLayout = std::move(bindlessLayout);
    m_sceneGpuResources = renderDevice.activeSceneGpuResources();
    m_ommBuildQueue->setSceneGpuResources(m_sceneGpuResources);
}

void OpacityMicromapBuilder::setMaterialGpuCache(MaterialGpuCache* materials)
{
    m_materialGpuCache = materials;
    m_ommBuildQueue->setMaterialGpuCache(materials);
}

void OpacityMicromapBuilder::createOpacityMicromaps(
    const caustica::scene::SceneRenderData& renderData)
{
    // Always grow the debug buffer for runtime imports — AS rebuild marks DebugDataDirty
    // and update() will write per-geometry slots even when OMM baking is disabled.
    ensureGeometryDebugCapacity(renderData.geometryCount);

    m_ommBuildQueue->cancelPendingBuilds();

    m_uiData.BuildsLeftInQueue = 0;
    m_uiData.BuildsQueued = 0;

    if (!m_uiData.Enable)
    {
        m_uiData.ActiveState.reset();
        return;
    }

    m_uiData.ActiveState = m_uiData.DesiredState;

    // Queue atomically only after every eligible alpha texture is GPU-ready. This
    // avoids duplicate partial queues while still allowing a cheap retry next frame.
    for (const auto& mesh : renderData.meshSnapshots)
    {
        if (mesh.isSkinPrototype || mesh.hasSkinPrototype)
            continue;
        for (const auto& geometry : mesh.geometries)
        {
            const std::shared_ptr<StandardMaterial> material = m_materialGpuCache
                ? m_materialGpuCache->findByResourceId(geometry.materialId)
                : nullptr;
            if (material && material->enableAlphaTesting
                && material->enableBaseTexture && material->baseTexture.loaded
                && !material->baseTexture.loaded->gpu.texture)
            {
                m_waitingForMaterialTextures = true;
                m_materialStateRevision = m_materialGpuCache->materialStateRevision();
                return;
            }
        }
    }

    m_waitingForMaterialTextures = false;
    for (const auto& mesh : renderData.meshSnapshots)
    {
        if (mesh.isSkinPrototype)
            continue; // skip the skinning prototypes
        if (mesh.hasSkinPrototype)
            continue;

        OmmBuildQueue::BuildInput input;
        input.mesh = mesh;

        for (size_t i = 0; i < mesh.geometries.size(); ++i)
        {
            const auto& geometry = mesh.geometries[i];
            const std::shared_ptr<StandardMaterial> material = m_materialGpuCache
                ? m_materialGpuCache->findByResourceId(geometry.materialId)
                : nullptr;
            if (!material)
                continue;
            if (!material->enableAlphaTesting)
                continue;
            if (!material->enableBaseTexture || !material->baseTexture.loaded)
                continue;

            std::shared_ptr<ImageAsset> alphaTexture = material->baseTexture.loaded.shared();
            if (!alphaTexture || alphaTexture->gpu.texture == nullptr)
                continue;

            OmmBuildQueue::BuildInput::Geometry geom;
            geom.geometryIndexInMesh = i;
            geom.alphaTexture = alphaTexture;
            geom.alphaCutoff = material->alphaCutoff;
            geom.maxSubdivisionLevel = m_uiData.ActiveState->MaxSubdivisionLevel;
            geom.dynamicSubdivisionScale = m_uiData.ActiveState->EnableDynamicSubdivision ? m_uiData.ActiveState->DynamicSubdivisionScale : 0.f;
            geom.format = m_uiData.ActiveState->Format;
            geom.flags = m_uiData.ActiveState->Flag;
            geom.alphaCutoffGT = (::omm::OpacityState)m_uiData.ActiveState->AlphaCutoffGT;
            geom.alphaCutoffLE = (::omm::OpacityState)m_uiData.ActiveState->AlphaCutoffLE;
            geom.maxOmmArrayDataSizeInMB = m_uiData.ActiveState->MaxOmmArrayDataSizeInMB;
            geom.computeOnly = m_uiData.ActiveState->ComputeOnly;
            geom.enableLevelLineIntersection = m_uiData.ActiveState->LevelLineIntersection;
            geom.enableTexCoordDeduplication = m_uiData.ActiveState->EnableTexCoordDeduplication;
            geom.force32BitIndices = m_uiData.ActiveState->Force32BitIndices;
            geom.enableSpecialIndices = m_uiData.ActiveState->EnableSpecialIndices;
            geom.enableNsightDebugMode = m_uiData.ActiveState->EnableNsightDebugMode;

            input.geometries.push_back(geom);
        }

        if (input.geometries.size() != 0ull)
        {
            m_uiData.BuildsQueued += (uint32_t)input.geometries.size();
            m_ommBuildQueue->queueBuild(input);
        }
    }
    if (m_materialGpuCache)
        m_materialStateRevision = m_materialGpuCache->materialStateRevision();
}

void OpacityMicromapBuilder::destroyOpacityMicromaps(
    caustica::rhi::CommandList& commandList,
    const caustica::scene::SceneRenderData& renderData)
{
    commandList.close();
    m_device->executeCommandList(&commandList);
    m_device->waitForIdle();
    commandList.open();

    for (const auto& mesh : renderData.meshSnapshots)
    {
        if (m_sceneGpuResources == nullptr)
            continue;
        const auto meshGpuIt = m_sceneGpuResources->meshRegistry.find(mesh.id);
        if (meshGpuIt == m_sceneGpuResources->meshRegistry.end())
            continue;
        caustica::render::MeshGpuRecord& meshGpu = meshGpuIt->second;
        meshGpu.accelStructOmm = nullptr;
        meshGpu.opacityMicromaps.clear();
        meshGpu.debugData = nullptr;
        meshGpu.geometryDebugData.clear();
        meshGpu.debugDataDirty = true;
    }
}

void OpacityMicromapBuilder::buildOpacityMicromaps(
    caustica::rhi::CommandList& commandList,
    const caustica::scene::SceneRenderData& renderData)
{
    commandList.beginMarker("OMM Updates");

    if (m_materialGpuCache
        && m_materialStateRevision != m_materialGpuCache->materialStateRevision())
    {
        m_uiData.TriggerRebuild = true;
    }

    if (!m_uiData.Enable)
    {
        m_ommBuildQueue->cancelPendingBuilds();
        m_uiData.BuildsLeftInQueue = 0;
        m_uiData.BuildsQueued = 0;
        commandList.endMarker();
        return;
    }

    if (m_uiData.TriggerRebuild)
    {
        destroyOpacityMicromaps(commandList, renderData);

        m_ommBuildQueue->cancelPendingBuilds();

        createOpacityMicromaps(renderData);

        m_uiData.TriggerRebuild = false;
    }
    else if (m_waitingForMaterialTextures)
    {
        createOpacityMicromaps(renderData);
    }

    m_ommBuildQueue->update(commandList);

    m_uiData.BuildsLeftInQueue = m_ommBuildQueue->numPendingBuilds();

    commandList.endMarker();
}

void OpacityMicromapBuilder::writeGeometryDebugBuffer(caustica::rhi::CommandList& commandList)
{
    commandList.writeBuffer(m_geometryDebugBuffer, m_geometryDebugDataPtr.data(), m_geometryDebugDataPtr.size() * sizeof(GeometryDebugData));
}

void OpacityMicromapBuilder::updateDebugGeometry(
    const caustica::scene::MeshRenderResourceSnapshot& mesh,
    const caustica::render::MeshGpuRecord& meshGpu)
{
    for (size_t geometryIndex = 0; geometryIndex < mesh.geometries.size(); ++geometryIndex)
    {
        const auto& geometry = mesh.geometries[geometryIndex];

        if (geometry.globalGeometryIndex < 0
            || static_cast<size_t>(geometry.globalGeometryIndex) >= m_geometryDebugDataPtr.size())
        {
            caustica::error(
                "OpacityMicromapBuilder: globalGeometryIndex %u out of range (debug slots=%zu); "
                "skipping debug write after runtime import.",
                geometry.globalGeometryIndex,
                m_geometryDebugDataPtr.size());
            continue;
        }

        if (const caustica::render::MeshGpuDebugData* debugData = meshGpu.debugData.get();
            debugData != nullptr && geometryIndex < meshGpu.geometryDebugData.size())
        {
            const caustica::render::MeshGeometryGpuDebugData& geometryDebug =
                meshGpu.geometryDebugData[geometryIndex];
            GeometryDebugData& dgdata = m_geometryDebugDataPtr[geometry.globalGeometryIndex];
            dgdata.ommArrayDataBufferIndex = debugData->ommArrayDataBufferDescriptor ? debugData->ommArrayDataBufferDescriptor->Get() : -1;
            dgdata.ommArrayDataBufferOffset = geometryDebug.ommArrayDataOffset;

            dgdata.ommDescArrayBufferIndex = debugData->ommDescBufferDescriptor ? debugData->ommDescBufferDescriptor->Get() : -1;
            dgdata.ommDescArrayBufferOffset = geometryDebug.ommDescBufferOffset;

            dgdata.ommIndexBufferIndex = debugData->ommIndexBufferDescriptor ? debugData->ommIndexBufferDescriptor->Get() : -1;
            dgdata.ommIndexBufferOffset = geometryDebug.ommIndexBufferOffset;
            dgdata.ommIndexBuffer16Bit = geometryDebug.ommIndexBufferFormat == caustica::rhi::Format::R16_UINT;
        }
        else
        {
            GeometryDebugData& dgdata = m_geometryDebugDataPtr[geometry.globalGeometryIndex];
            dgdata.ommArrayDataBufferIndex = -1;
            dgdata.ommArrayDataBufferOffset = 0xFFFFFFFF;
            dgdata.ommDescArrayBufferIndex = -1;
            dgdata.ommDescArrayBufferOffset = 0xFFFFFFFF;
            dgdata.ommIndexBufferIndex = -1;
            dgdata.ommIndexBufferOffset = 0xFFFFFFFF;
            dgdata.ommIndexBuffer16Bit = 0;
        }
    }
}

bool OpacityMicromapBuilder::update(
    caustica::rhi::CommandList& commandList,
    const caustica::scene::SceneRenderData& renderData)
{
    RAII_SCOPE( commandList.beginMarker("OpacityMicromapBuilder");, commandList.endMarker(); );

    // Runtime drag-drop grows geometry count without sceneLoaded(); resize before any writes.
    ensureGeometryDebugCapacity(renderData.geometryCount);

    bool anyDirty = false;
    for (const auto& mesh : renderData.meshSnapshots)
    {
        if (m_sceneGpuResources == nullptr)
            continue;
        const auto meshGpuIt = m_sceneGpuResources->meshRegistry.find(mesh.id);
        if (meshGpuIt == m_sceneGpuResources->meshRegistry.end())
            continue;
        caustica::render::MeshGpuRecord& meshGpu = meshGpuIt->second;
        if (meshGpu.debugDataDirty)
        {
            meshGpu.debugDataDirty = false;
            anyDirty = true;
            updateDebugGeometry(mesh, meshGpu);
        }
    }
    if (anyDirty)
        writeGeometryDebugBuffer(commandList);
    return anyDirty;
}

void OpacityMicromapBuilder::setGlobalShaderMacros(std::vector<caustica::ShaderMacro>& macros)
{
    if (m_uiData.DebugView == OpacityMicroMapDebugView::InWorld)
        macros.push_back( { "OMM_DEBUG_VIEW_IN_WORLD", "1" } );
    if (m_uiData.DebugView == OpacityMicroMapDebugView::Overlay)
        macros.push_back( { "OMM_DEBUG_VIEW_OVERLAY", "1" } );
}

bool OpacityMicromapBuilder::debugGUI(
    float indent,
    const caustica::scene::SceneRenderData& renderData)
{
    RAII_SCOPE(ImGui::PushID("OpacityMicromapBuilderDebugGUI"); , ImGui::PopID(); );
    
    bool resetAccumulation = false;
    #define RESET_ON_CHANGE(code) do{if (code) resetAccumulation = true;} while(false)

    if (ImGui::Checkbox("Enable", &m_uiData.Enable))
        resetAccumulation = true;

    {
        {
            UI_SCOPED_DISABLE(m_uiData.ActiveState.has_value() && m_uiData.ActiveState->Format != caustica::rhi::rt::OpacityMicromapFormat::OC1_4_State);
            if (ImGui::Checkbox("Force 2 State", &m_uiData.Force2State))
                resetAccumulation = true;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Will force 2-State via TLAS instance mask.");
        }

        {
            if (ImGui::Checkbox("render ONLY OMMs", &m_uiData.OnlyOMMs))
                resetAccumulation = true;
        }

        ImGui::Separator();
        ImGui::Text("Bake settings (Require Rebuild to take effect)");

        if (m_uiData.BuildsLeftInQueue != 0)
        {
            const float progress = (1.f - (float)m_uiData.BuildsLeftInQueue / m_uiData.BuildsQueued);
            std::stringstream ss;
            ss << "Build progress: " << (uint32_t)(100.f * progress) << "%";
            std::string str = ss.str();
            ImGui::ProgressBar(progress, ImVec2(-FLT_MIN, 0), str.c_str());
        }

        {
            UI_SCOPED_DISABLE(m_uiData.ActiveState.has_value() && m_uiData.ActiveState == m_uiData.DesiredState);
            if (ImGui::Button("Trigger Rebuild"))
            {
                m_uiData.TriggerRebuild = true;
            }
        }

        {
            ImGui::Checkbox("Dynamic subdivision level", &m_uiData.DesiredState.EnableDynamicSubdivision);
        }

        {
            UI_SCOPED_DISABLE(!m_uiData.DesiredState.EnableDynamicSubdivision);
            ImGui::SliderFloat("Dynamic subdivision scale", &m_uiData.DesiredState.DynamicSubdivisionScale, 0.01f, 20.f, "%.1f", ImGuiSliderFlags_Logarithmic);
        }

        {
            const int MaxSubdivisionLevel = m_uiData.DesiredState.ComputeOnly ? 12 : 10;
            m_uiData.DesiredState.MaxSubdivisionLevel = std::clamp(m_uiData.DesiredState.MaxSubdivisionLevel, 1, MaxSubdivisionLevel);
            ImGui::SliderInt("Max subdivision level", &m_uiData.DesiredState.MaxSubdivisionLevel, 1, MaxSubdivisionLevel, "%d", ImGuiSliderFlags_AlwaysClamp);
        }

        {
            std::array<const char*, 3> formatNames =
            {
                "None",
                "Fast Trace",
                "Fast Build"
            };

            std::array<caustica::rhi::rt::OpacityMicromapBuildFlags, 3> formats =
            {
                caustica::rhi::rt::OpacityMicromapBuildFlags::None,
                caustica::rhi::rt::OpacityMicromapBuildFlags::FastTrace,
                caustica::rhi::rt::OpacityMicromapBuildFlags::FastBuild
            };

            if (ImGui::BeginCombo("Flag", formatNames[(uint32_t)m_uiData.DesiredState.Flag]))
            {
                for (uint i = 0; i < formats.size(); i++)
                {
                    bool is_selected = formats[i] == m_uiData.DesiredState.Flag;
                    if (ImGui::Selectable(formatNames[i], is_selected))
                        m_uiData.DesiredState.Flag = formats[i];
                    if (is_selected)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        }

        {
            auto FormatToString = [ ](caustica::rhi::rt::OpacityMicromapFormat format) {
                assert(format == caustica::rhi::rt::OpacityMicromapFormat::OC1_2_State || format == caustica::rhi::rt::OpacityMicromapFormat::OC1_4_State);
                return format == caustica::rhi::rt::OpacityMicromapFormat::OC1_2_State ? "2-State" : "4-State";
            };
            std::array<caustica::rhi::rt::OpacityMicromapFormat, 2> formats = { caustica::rhi::rt::OpacityMicromapFormat::OC1_2_State, caustica::rhi::rt::OpacityMicromapFormat::OC1_4_State };
            if (ImGui::BeginCombo("Format", FormatToString(m_uiData.DesiredState.Format)))
            {
                for (uint i = 0; i < formats.size(); i++)
                {
                    bool is_selected = formats[i] == m_uiData.DesiredState.Format;
                    if (ImGui::Selectable(FormatToString(formats[i]), is_selected))
                        m_uiData.DesiredState.Format = formats[i];
                    if (is_selected)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        }

        {
            auto StateToString = [ ](::omm::OpacityState state) {
                const char* strings[] = { "Transparent", "Opaque", "UnknownTransparent", "UnknownOpaque" };
                assert((int)state >= 0 && (int)state < IM_ARRAYSIZE(strings));
                return strings[(int)state];
            };
            const std::array<::omm::OpacityState, 4> states = { ::omm::OpacityState::Transparent, ::omm::OpacityState::Opaque, ::omm::OpacityState::UnknownTransparent, ::omm::OpacityState::UnknownOpaque };

            if (ImGui::BeginCombo("AlphaCutoffGT", StateToString((::omm::OpacityState)m_uiData.DesiredState.AlphaCutoffGT)))
            {
                for (uint i = 0; i < states.size(); i++)
                {
                    bool is_selected = states[i] == (::omm::OpacityState)m_uiData.DesiredState.AlphaCutoffGT;
                    if (ImGui::Selectable(StateToString(states[i]), is_selected))
                        m_uiData.DesiredState.AlphaCutoffGT = (int)states[i];
                    if (is_selected)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            if (ImGui::BeginCombo("AlphaCutoffLE", StateToString((::omm::OpacityState)m_uiData.DesiredState.AlphaCutoffLE)))
            {
                for (uint i = 0; i < states.size(); i++)
                {
                    bool is_selected = states[i] == (::omm::OpacityState)m_uiData.DesiredState.AlphaCutoffLE;
                    if (ImGui::Selectable(StateToString(states[i]), is_selected))
                        m_uiData.DesiredState.AlphaCutoffLE = (int)states[i];
                    if (is_selected)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        }

        if (ImGui::CollapsingHeader("Debug settings"))
        {
            UI_SCOPED_INDENT(indent);

#if ENABLE_DEBUG_VIZUALISATIONS
            RESET_ON_CHANGE( ImGui::Combo("Debug View", (int*)&m_uiData.DebugView, "Disabled\0InWorld\0Overlay\0\0") );
#else
            ImGui::Text("Please enable ENABLE_DEBUG_VIZUALISATIONS for debug viz");
            m_uiData.DebugView = OpacityMicroMapDebugView::Disabled;
#endif

            ImGui::Checkbox("Compute Only", &m_uiData.DesiredState.ComputeOnly);

            ImGui::Checkbox("Enable \"Level Line Intersection\"", &m_uiData.DesiredState.LevelLineIntersection);

            ImGui::Checkbox("Enable TexCoord deduplication", &m_uiData.DesiredState.EnableTexCoordDeduplication);

            ImGui::Checkbox("Force 32-bit indices", &m_uiData.DesiredState.Force32BitIndices);

            ImGui::Checkbox("Enable Special Indices", &m_uiData.DesiredState.EnableSpecialIndices);

            ImGui::SliderInt("Max memory per OMM", &m_uiData.DesiredState.MaxOmmArrayDataSizeInMB, 1, 1000, "%dMB", ImGuiSliderFlags_Logarithmic);

            ImGui::Checkbox("Enable NSight debug mode", &m_uiData.DesiredState.EnableNsightDebugMode);
        }

        ImGui::Separator();
        ImGui::Text("Stats");

        {
            std::stringstream ss;
            ss << m_uiData.BuildsQueued << " active OMMs";
            std::string str = ss.str();
            ImGui::Text(str.c_str());

            if (ImGui::CollapsingHeader("Bake Stats"))
            {
                UI_SCOPED_INDENT(indent);

                for (const auto& mesh : renderData.meshSnapshots)
                {
                    if (m_sceneGpuResources == nullptr)
                        continue;
                    const auto meshGpuIt =
                        m_sceneGpuResources->meshRegistry.find(mesh.id);
                    if (meshGpuIt == m_sceneGpuResources->meshRegistry.end())
                        continue;
                    const auto& geometryDebugData = meshGpuIt->second.geometryDebugData;
                    bool meshHasOmms = false;
                    for (const caustica::render::MeshGeometryGpuDebugData& debugData : geometryDebugData)
                    {
                        if (debugData.ommIndexBufferOffset != 0xFFFFFFFF)
                        {
                            meshHasOmms = true;
                            break;
                        }
                    }

                    if (!meshHasOmms)
                        continue;

                    ImGui::Text(mesh.debugName.c_str());

                    {
                        UI_SCOPED_INDENT(indent);

                        for (const caustica::render::MeshGeometryGpuDebugData& debugData : geometryDebugData)
                        {
                            if (debugData.ommIndexBufferOffset == 0xFFFFFFFF)
                                continue;

                            const uint64_t known = debugData.ommStatsTotalKnown;
                            const uint64_t unknown = debugData.ommStatsTotalUnknown;
                            const uint64_t total = known + unknown;
                            const float ratio = total == 0 ? -1.f : 100.f * float(known) / float(total);

                            std::stringstream ss;
                            ss << ratio << "%% (" << known << " known, " << unknown << " unknown" << ")";

                            std::string str = ss.str();
                            ImGui::Text(str.c_str());
                        }
                    }
                }
            }
        }
    }

    return resetAccumulation;
}

