#pragma once

#include <scene/SceneTypes.h>
#include <render/SceneGpuResources.h>
#include <assets/loader/TextureLoader.h>
#include <render/core/AccelerationStructureUtil.h>

#include <rhi/nvrhi.h>
#include <omm-gpu-nvrhi.h>
#include <unordered_map>
#include <list>

namespace caustica
{
		class ShaderFactory;
		class DescriptorTableManager;
}

class OmmBuildQueue
{
public:
	struct BuildInput
	{
		struct Geometry
		{
			int geometryIndexInMesh = -1;
			std::shared_ptr < caustica::ImageAsset > alphaTexture;
			float alphaCutoff = 0.5f;

			// settings
			uint32_t maxSubdivisionLevel = 5;
			float dynamicSubdivisionScale = 2.f;
			nvrhi::rt::OpacityMicromapFormat format = nvrhi::rt::OpacityMicromapFormat::OC1_4_State;
			nvrhi::rt::OpacityMicromapBuildFlags flags = nvrhi::rt::OpacityMicromapBuildFlags::FastTrace;
			uint32_t maxOmmArrayDataSizeInMB; // Limit OMM memory footprint to this value.
			omm::OpacityState alphaCutoffGT = omm::OpacityState::Opaque;
			omm::OpacityState alphaCutoffLE = omm::OpacityState::Transparent;

			// Debug settings
			bool computeOnly = false;
			bool enableLevelLineIntersection = true;
			bool enableTexCoordDeduplication = true;
			bool force32BitIndices = false;
			bool enableSpecialIndices = true;
			bool enableNsightDebugMode = false;
		};

		caustica::scene::MeshRenderResourceSnapshot mesh;
		std::vector < Geometry > geometries;
		bvh::Config bvhCfg;
	};

	OmmBuildQueue(
		nvrhi::DeviceHandle& device, 
		std::shared_ptr<caustica::DescriptorTableManager>,
		std::shared_ptr<caustica::ShaderFactory> shaderFactory
	);
	~OmmBuildQueue();

	void update(nvrhi::ICommandList& commandList);
	void setSceneGpuResources(caustica::render::SceneGpuResources* resources)
	{
		m_sceneGpuResources = resources;
	}
	void setMaterialGpuCache(MaterialGpuCache* materials)
	{
		m_materialGpuCache = materials;
	}
	void cancelPendingBuilds();
	void queueBuild(const BuildInput& inputs);
	uint32_t numPendingBuilds() const;

private:

	enum BuildState
	{
		None,
		Setup,
		BakeAndBuild,
	};

	struct BufferInfo
	{
		nvrhi::Format	ommIndexFormat;
		uint32_t		ommIndexCount;
		size_t			ommIndexOffset;
		size_t			ommDescArrayOffset;
		size_t			ommDescArrayHistogramOffset;
		size_t			ommDescArrayHistogramSize;
		size_t			ommDescArrayHistogramReadbackOffset;
		size_t			ommIndexHistogramOffset;
		size_t			ommIndexHistogramSize;
		size_t			ommIndexHistogramReadbackOffset;
		size_t			ommPostDispatchInfoOffset;
		size_t			ommPostDispatchInfoReadbackOffset;

		// below will be populated after Setup pass has finished.
		uint32_t		ommArrayDataOffset;
		std::vector<nvrhi::rt::OpacityMicromapUsageCount> ommIndexHistogram;
		std::vector<nvrhi::rt::OpacityMicromapUsageCount> ommArrayHistogram;
	};           

	struct Buffers
	{
		nvrhi::BufferHandle ommArrayDataBuffer;
		nvrhi::BufferHandle ommIndexBuffer;
		nvrhi::BufferHandle ommDescBuffer;
		nvrhi::BufferHandle ommDescArrayHistogramBuffer;
		nvrhi::BufferHandle ommIndexArrayHistogramBuffer;
		nvrhi::BufferHandle ommPostDispatchInfoBuffer;
		nvrhi::BufferHandle ommReadbackBuffer;
	};

	struct BuildTask
	{
		BuildInput input;
		BuildState state = BuildState::None;

		Buffers buffers;
		std::vector<BufferInfo> bufferInfos;

		BuildTask(const BuildInput& input) : input(input) {}
		void reset();
	};

	void consumeOneTask(nvrhi::ICommandList& commandList, BuildState taskState);
	bool executeTask(nvrhi::ICommandList& commandList, BuildTask& taskState); // Returns whether the task is finished and can be removed from the queue

	void runSetup(nvrhi::ICommandList& commandList, BuildTask& task);
	void runBakeAndBuild(nvrhi::ICommandList& commandList, BuildTask& task);
	void finalize(nvrhi::ICommandList& commandList, BuildTask& task);
	
	void allocateOMMArrayDataBuffer(BuildTask& task);
	void bakeOmmArrayData(nvrhi::ICommandList& commandList, BuildTask& task);
	std::vector<bvh::OmmAttachment> buildOMMAttachments(nvrhi::ICommandList& commandList, BuildTask& task);
	void buildBLASWithOMM(nvrhi::ICommandList& commandList, BuildTask& task, const std::vector<bvh::OmmAttachment>& ommAttachment);
	caustica::render::MeshGpuRecord* findMeshGpu(
		const caustica::scene::MeshRenderResourceSnapshot& mesh) const;

	bool readyToRecordWork();
	void submitAndSubscribeQuery(nvrhi::ICommandList& commandList);

	std::vector<BuildTask> m_pending;
	nvrhi::EventQueryHandle m_InFlightQuery;

	nvrhi::DeviceHandle m_device;
	caustica::render::SceneGpuResources* m_sceneGpuResources = nullptr;
	MaterialGpuCache* m_materialGpuCache = nullptr;
	std::shared_ptr<caustica::DescriptorTableManager> m_descriptorTable;
	std::shared_ptr<caustica::ShaderFactory> m_shaderFactory;
	std::unique_ptr<omm::GpuBakeNvrhi> m_baker;
};
