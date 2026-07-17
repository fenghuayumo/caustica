#pragma once

#include <math/math.h>
#include <rhi/nvrhi.h>

// Shared shader structs use unqualified math aliases (float3, uint, float3x4).
using namespace caustica::math;

#include <shaders/bindless.h>
#include <shaders/material_cb.h>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace caustica::render
{

// Per-skinned-mesh GPU state owned by the render side (not ECS).
struct SkinnedMeshGpuState
{
    nvrhi::BufferHandle jointBuffer;
    nvrhi::BindingSetHandle skinningBindingSet;
    bool skinningInitialized = false;
};

// Per-frame GPU buffer views for the render thread.
// Filled at beginGpuReadFrame; prefer these over Scene::getGpuResources() digs.
struct SceneGpuFrameHandles
{
    nvrhi::BufferHandle instanceBuffer;
    nvrhi::BufferHandle geometryBuffer;

    [[nodiscard]] bool valid() const
    {
        return instanceBuffer != nullptr && geometryBuffer != nullptr;
    }
};

// GPU-side storage derived from a CPU scene.
struct SceneGpuResources
{
    std::vector<MaterialConstants> materialData;
    std::vector<GeometryData> geometryData;
    std::vector<InstanceData> instanceData;

    nvrhi::BufferHandle materialBuffer;
    nvrhi::BufferHandle geometryBuffer;
    nvrhi::BufferHandle instanceBuffer;

    nvrhi::DeviceHandle device;
    nvrhi::ShaderHandle skinningShader;
    nvrhi::ComputePipelineHandle skinningPipeline;
    nvrhi::BindingLayoutHandle skinningBindingLayout;

    // Keyed by ecs::Entity raw id. render thread only.
    std::unordered_map<uint32_t, SkinnedMeshGpuState> skinnedGpuByEntity;

    bool enableBindlessResources = false;
    bool useResourceDescriptorHeapBindless = false;
    bool rayTracingSupported = false;

    [[nodiscard]] SceneGpuFrameHandles frameHandles() const
    {
        return SceneGpuFrameHandles{ instanceBuffer, geometryBuffer };
    }
};

} // namespace caustica::render
