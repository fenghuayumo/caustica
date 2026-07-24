/*
Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.

NVIDIA CORPORATION and its licensors retain all intellectual property
and proprietary rights in and to this software, related documentation
and any modifications thereto. Any use, reproduction, disclosure or
distribution of this software and related documentation without an express
license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

#include <render/omm/GpuBakeRhi.h>

#include <rhi/common/misc.h>
#include <rhi/utils.h>

#include <omm.hpp>

#include <vector>
#include <stdint.h>
#include <utility>
#include <unordered_map>
#include <list>
#include <shared_mutex>

constexpr uint32_t g_DebugRTVDimension =  6 * 1024;

namespace
{
	caustica::rhi::SamplerAddressMode GetRhiAddressMode(const ::omm::TextureAddressMode addressingMode)
	{
		switch (addressingMode)
		{
		case ::omm::TextureAddressMode::Wrap: return caustica::rhi::SamplerAddressMode::Wrap;
		case ::omm::TextureAddressMode::Mirror: return caustica::rhi::SamplerAddressMode::Mirror;
		case ::omm::TextureAddressMode::Clamp: return caustica::rhi::SamplerAddressMode::Clamp;
		case ::omm::TextureAddressMode::Border: return caustica::rhi::SamplerAddressMode::Border;
		case ::omm::TextureAddressMode::MirrorOnce: return caustica::rhi::SamplerAddressMode::MirrorOnce;
		default:
		{
			assert(false);
			return caustica::rhi::SamplerAddressMode::Clamp;
		}
		}
	}

	::omm::TextureAddressMode GetTextureAddressMode(const caustica::rhi::SamplerAddressMode addressingMode)
	{
		switch (addressingMode)
		{
		case caustica::rhi::SamplerAddressMode::Wrap: return ::omm::TextureAddressMode::Wrap;
		case caustica::rhi::SamplerAddressMode::Mirror: return ::omm::TextureAddressMode::Mirror;
		case caustica::rhi::SamplerAddressMode::Clamp: return ::omm::TextureAddressMode::Clamp;
		case caustica::rhi::SamplerAddressMode::Border: return ::omm::TextureAddressMode::Border;
		case caustica::rhi::SamplerAddressMode::MirrorOnce: return ::omm::TextureAddressMode::MirrorOnce;
		default:
		{
			assert(false);
			return ::omm::TextureAddressMode::Clamp;
		}
		}
	}

	static ::omm::TexCoordFormat GetTexCoordFormat(caustica::rhi::Format format)
	{
		switch (format)
		{
		case caustica::rhi::Format::R16_UNORM:
		{
			return ::omm::TexCoordFormat::UV16_UNORM;
		}
		case caustica::rhi::Format::R16_FLOAT:
		{
			return ::omm::TexCoordFormat::UV16_FLOAT;
		}
		case caustica::rhi::Format::R32_FLOAT:
		{
			return ::omm::TexCoordFormat::UV32_FLOAT;
		}
		default:
			assert(false);
			return ::omm::TexCoordFormat::UV32_FLOAT;
		}
	}

	static ::omm::IndexFormat GetIndexFormat(caustica::rhi::Format format)
	{
		switch (format)
		{
		case caustica::rhi::Format::R16_UINT:
		{
			return ::omm::IndexFormat::UINT_16;
		}
		case caustica::rhi::Format::R32_UINT:
		{
			return ::omm::IndexFormat::UINT_32;
		}
		default:
			assert(false);
			return ::omm::IndexFormat::MAX_NUM;
		}
	}

	/// -- BINDING CACHE FROM DONUT -- 

	/*
	BindingCache maintains a dictionary that maps binding set descriptors
	into actual binding set objects. The binding sets are created on demand when
	GetOrCreateBindingSet(...) is called and the requested binding set does not exist.
	Created binding sets are stored for the lifetime of BindingCache, or until
	Clear() is called.

	All BindingCache methods are thread-safe.
	*/
	class BindingCache
	{
	private:
		caustica::rhi::DeviceHandle m_Device;
		std::unordered_map<size_t, caustica::rhi::BindingSetHandle> m_BindingSets;
		std::shared_mutex m_Mutex;

	public:
		BindingCache(caustica::rhi::Device* device)
			: m_Device(device)
		{ }

		caustica::rhi::BindingSetHandle GetCachedBindingSet(const caustica::rhi::BindingSetDesc& desc, caustica::rhi::BindingLayout* layout)
		{
			size_t hash = 0;
			caustica::rhi::hash_combine(hash, desc);
			caustica::rhi::hash_combine(hash, layout);

			m_Mutex.lock_shared();

			caustica::rhi::BindingSetHandle result = nullptr;
			auto it = m_BindingSets.find(hash);
			if (it != m_BindingSets.end())
				result = it->second;

			m_Mutex.unlock_shared();

			if (result)
			{
				assert(result->getDesc());
				assert(*result->getDesc() == desc);
			}

			return result;
		}
		caustica::rhi::BindingSetHandle GetOrCreateBindingSet(const caustica::rhi::BindingSetDesc& desc, caustica::rhi::BindingLayout* layout)
		{
			size_t hash = 0;
			caustica::rhi::hash_combine(hash, desc);
			caustica::rhi::hash_combine(hash, layout);

			m_Mutex.lock_shared();

			caustica::rhi::BindingSetHandle result;
			auto it = m_BindingSets.find(hash);
			if (it != m_BindingSets.end())
				result = it->second;

			m_Mutex.unlock_shared();

			if (!result)
			{
				m_Mutex.lock();

				caustica::rhi::BindingSetHandle& entry = m_BindingSets[hash];
				if (!entry)
				{
					result = m_Device->createBindingSet(desc, layout);
					entry = result;
				}
				else
					result = entry;

				m_Mutex.unlock();
			}

			if (result)
			{
				assert(result->getDesc());
				assert(*result->getDesc() == desc);
			}

			return result;
		}

		void Clear()
		{
			m_Mutex.lock();
			m_BindingSets.clear();
			m_Mutex.unlock();
		}
	};
}

namespace caustica::omm
{

class GpuBakeRhiImpl
{
public:

	GpuBakeRhiImpl(caustica::rhi::DeviceHandle device, caustica::rhi::CommandListHandle commandList, bool enableDebug, GpuBakeRhi::ShaderProvider* shaderProvider, std::optional<GpuBakeRhi::MessageCallback> messageCallback);
	~GpuBakeRhiImpl();

	// CPU side pre-build info.
	void GetPreDispatchInfo(const GpuBakeRhi::Input& params, GpuBakeRhi::PreDispatchInfo& info);

	void Dispatch(
		caustica::rhi::CommandListHandle commandList,
		const GpuBakeRhi::Input& params,
		const GpuBakeRhi::Buffers& buffers);

	void Clear();

	static void ReadPostDispatchInfo(void* pData, size_t byteSize, GpuBakeRhi::PostDispatchInfo& outPostDispatchInfo);
	static void ReadUsageDescBuffer(void* pData, size_t byteSize, std::vector<caustica::rhi::rt::OpacityMicromapUsageCount>& outVmUsages);

	// Debug dumping
	void DumpDebug(
		const char* folderName,
		const char* debugName,
		const GpuBakeRhi::Input& params,
		const std::vector<uint8_t>& ommArrayBuffer,
		const std::vector<uint8_t>& ommDescBuffer,
		const std::vector<uint8_t>& ommIndexBuffer,
		caustica::rhi::Format ommIndexBufferFormat,
		const std::vector<uint8_t>& ommDescArrayHistogramBuffer,
		const std::vector<uint8_t>& ommIndexHistogramBuffer,
		const void* indexBuffer,
		const uint32_t indexCount,
		caustica::rhi::Format ommTexCoordBufferFormat,
		const void* texCoords,
		const float* imageData,
		const uint32_t width,
		const uint32_t height
	);

	GpuBakeRhi::Stats GetStats(const ::omm::Cpu::BakeResultDesc& desc);

private:

	void InitStaticBuffers(caustica::rhi::CommandListHandle commandList);
	void InitBaker(GpuBakeRhi::ShaderProvider* shaderProvider);
	void DestroyBaker();

	void SetupPipelines(
		const ::omm::Gpu::PipelineInfoDesc* desc,
		GpuBakeRhi::ShaderProvider* shaderProvider);

	::omm::Gpu::DispatchConfigDesc GetConfig(const GpuBakeRhi::Input& params);

	void ReserveGlobalCBuffer(size_t size, uint32_t slot);
	void ReserveScratchBuffers(const ::omm::Gpu::PreDispatchInfo& info);
	caustica::rhi::TextureHandle GetTextureResource(const GpuBakeRhi::Input& params, const GpuBakeRhi::Buffers& output, const ::omm::Gpu::Resource& resource);
	caustica::rhi::BufferHandle GetBufferResource(const GpuBakeRhi::Input& params, const GpuBakeRhi::Buffers& output, const ::omm::Gpu::Resource& resource, uint32_t& offsetInBytes);

	void ExecuteDispatchChain(
		caustica::rhi::CommandListHandle commandList,
		const GpuBakeRhi::Input& params,
		const GpuBakeRhi::Buffers& output,
		const ::omm::Gpu::DispatchChain* outDispatchDesc);

	caustica::rhi::DeviceHandle m_device;
	caustica::rhi::BufferHandle m_staticIndexBuffer;
	caustica::rhi::BufferHandle m_staticVertexBuffer;
	caustica::rhi::BufferHandle m_globalCBuffer;
	uint32_t m_globalCBufferSlot;
	uint32_t m_localCBufferSlot;
	uint32_t m_localCBufferSize;
	caustica::rhi::FramebufferHandle m_nullFbo;
	caustica::rhi::FramebufferHandle m_debugFbo;
	std::vector<caustica::rhi::BufferHandle> m_transientPool;
	std::vector<caustica::rhi::ResourceHandle> m_pipelines;
	std::vector<std::pair<caustica::rhi::SamplerHandle, uint32_t>> m_samplers;
	BindingCache* m_bindingCache;

	::omm::Baker m_baker;
	::omm::Baker m_cpuBaker;
	::omm::Gpu::Pipeline m_pipeline;
	bool m_enableDebug = false;
	std::optional<GpuBakeRhi::MessageCallback> m_messageCallback;
};

GpuBakeRhiImpl::GpuBakeRhiImpl(caustica::rhi::DeviceHandle device, caustica::rhi::CommandListHandle commandList, bool enableDebug, GpuBakeRhi::ShaderProvider* shaderProvider, std::optional<GpuBakeRhi::MessageCallback> messageCallback)
	: m_device(device)
	, m_bindingCache(new BindingCache(device))
	, m_enableDebug(enableDebug)
	, m_messageCallback(messageCallback)
{
	InitStaticBuffers(commandList);
	InitBaker(shaderProvider);
}

GpuBakeRhiImpl::~GpuBakeRhiImpl()
{
	delete m_bindingCache;
	m_bindingCache = nullptr;
	DestroyBaker();
}

void GpuBakeRhiImpl::InitStaticBuffers(caustica::rhi::CommandListHandle commandList)
{
	{
		size_t size = 0;
		::omm::Result res = ::omm::Gpu::GetStaticResourceData(::omm::Gpu::ResourceType::STATIC_VERTEX_BUFFER, nullptr, &size);
		assert(res == ::omm::Result::SUCCESS);

		std::vector<uint8_t> vertexData(size);
		res = ::omm::Gpu::GetStaticResourceData(::omm::Gpu::ResourceType::STATIC_VERTEX_BUFFER, vertexData.data(), &size);
		assert(res == ::omm::Result::SUCCESS);

		caustica::rhi::BufferDesc bufferDesc;
		bufferDesc.isVertexBuffer = true;
		bufferDesc.byteSize = vertexData.size();
		bufferDesc.debugName = "::omm::Gpu::ResourceType::STATIC_VERTEX_BUFFER";
		bufferDesc.format = caustica::rhi::Format::R32_UINT;
		m_staticVertexBuffer = m_device->createBuffer(bufferDesc);

		commandList->beginTrackingBufferState(m_staticVertexBuffer, caustica::rhi::ResourceStates::Common);
		commandList->writeBuffer(m_staticVertexBuffer, vertexData.data(), vertexData.size());
		commandList->setPermanentBufferState(m_staticVertexBuffer, caustica::rhi::ResourceStates::VertexBuffer);
	}

	{
		size_t size = 0;
		::omm::Result res = ::omm::Gpu::GetStaticResourceData(::omm::Gpu::ResourceType::STATIC_INDEX_BUFFER, nullptr, &size);
		assert(res == ::omm::Result::SUCCESS);

		std::vector<uint8_t> indexData(size);
		res = ::omm::Gpu::GetStaticResourceData(::omm::Gpu::ResourceType::STATIC_INDEX_BUFFER, indexData.data(), &size);
		assert(res == ::omm::Result::SUCCESS);

		caustica::rhi::BufferDesc bufferDesc;
		bufferDesc.isIndexBuffer = true;
		bufferDesc.byteSize = indexData.size();
		bufferDesc.debugName = "::omm::Gpu::ResourceType::STATIC_INDEX_BUFFER";
		bufferDesc.format = caustica::rhi::Format::R32_UINT;
		m_staticIndexBuffer = m_device->createBuffer(bufferDesc);

		commandList->beginTrackingBufferState(m_staticIndexBuffer, caustica::rhi::ResourceStates::Common);
		commandList->writeBuffer(m_staticIndexBuffer, indexData.data(), indexData.size());
		commandList->setPermanentBufferState(m_staticIndexBuffer, caustica::rhi::ResourceStates::IndexBuffer);
	}

	{ // RHI has trouble binding zero RTV's
		caustica::rhi::TextureHandle virtualTexture;
		{
			caustica::rhi::TextureDesc desc;
			desc.debugName = "NULL_VMRT";
			desc.width = m_enableDebug ? g_DebugRTVDimension : 1;
			desc.height = m_enableDebug ? g_DebugRTVDimension : 1;
			desc.format = caustica::rhi::Format::RGBA16_FLOAT;
			desc.dimension = caustica::rhi::TextureDimension::Texture2D;
			desc.clearValue = caustica::rhi::Color();
			desc.useClearValue = true;
			desc.isRenderTarget = true;
			desc.isVirtual = false;
			virtualTexture = m_device->createTexture(desc);
		}

		{
			caustica::rhi::FramebufferDesc desc;
			caustica::rhi::FramebufferAttachment tex;
			tex.format = caustica::rhi::Format::RGBA16_FLOAT;
			tex.setTexture(virtualTexture);
			desc.addColorAttachment(tex);
			m_nullFbo = m_device->createFramebuffer(desc);
		}
	}
}

void GpuBakeRhiImpl::ReserveGlobalCBuffer(size_t byteSize, uint32_t slot)
{
	if (!m_globalCBuffer.Get() || m_globalCBuffer->getDesc().byteSize < byteSize)
	{
		m_globalCBuffer = m_device->createBuffer(caustica::rhi::utils::CreateStaticConstantBufferDesc((uint32_t)byteSize, "::omm::Gpu::GlobalConstantBuffer"));
	}

	m_globalCBufferSlot = slot;
}

void GpuBakeRhiImpl::InitBaker(GpuBakeRhi::ShaderProvider* shaderProvider)
{
	assert(m_device->getGraphicsAPI() == caustica::rhi::GraphicsAPI::D3D12 || m_device->getGraphicsAPI() == caustica::rhi::GraphicsAPI::VULKAN);

	{
		::omm::BakerCreationDesc desc;
		desc.type = ::omm::BakerType::GPU;
		if (m_messageCallback.has_value())
		{
			desc.messageInterface.userArg = this;
			desc.messageInterface.messageCallback = [](::omm::MessageSeverity severity, const char* message, void* userArg) {
				GpuBakeRhiImpl* _this = (GpuBakeRhiImpl*)userArg;
				_this->m_messageCallback.value()(severity, message);
			};
		}

		::omm::Result res = ::omm::CreateBaker(desc, &m_baker);
		assert(res == ::omm::Result::SUCCESS);
	}

	{
		::omm::BakerCreationDesc desc;
		desc.type = ::omm::BakerType::CPU;

		::omm::Result res = ::omm::CreateBaker(desc, &m_cpuBaker);
		assert(res == ::omm::Result::SUCCESS);
	}

	{
		::omm::Gpu::PipelineConfigDesc config;
		config.renderAPI = m_device->getGraphicsAPI() == caustica::rhi::GraphicsAPI::D3D12 ? ::omm::Gpu::RenderAPI::DX12 : ::omm::Gpu::RenderAPI::Vulkan;

		::omm::Result res = ::omm::Gpu::CreatePipeline(m_baker, config, &m_pipeline);
		assert(res == ::omm::Result::SUCCESS);

		const ::omm::Gpu::PipelineInfoDesc* desc;
		res = ::omm::Gpu::GetPipelineDesc(m_pipeline, &desc);
		assert(res == ::omm::Result::SUCCESS);

		SetupPipelines(desc, shaderProvider);

		ReserveGlobalCBuffer(desc->globalConstantBufferDesc.maxDataSize, desc->globalConstantBufferDesc.registerIndex);
		m_localCBufferSlot = desc->localConstantBufferDesc.registerIndex;
		m_localCBufferSize = desc->localConstantBufferDesc.maxDataSize;
	}
}

void GpuBakeRhiImpl::DestroyBaker()
{
	::omm::Result res = ::omm::Gpu::DestroyPipeline(m_baker, m_pipeline);
	assert(res == ::omm::Result::SUCCESS);

	res = ::omm::DestroyBaker(m_baker);
	assert(res == ::omm::Result::SUCCESS);

	res = ::omm::DestroyBaker(m_cpuBaker);
	assert(res == ::omm::Result::SUCCESS);
}

void GpuBakeRhiImpl::SetupPipelines(
	const ::omm::Gpu::PipelineInfoDesc* desc,
	GpuBakeRhi::ShaderProvider* shaderProvider)
{
	auto CreateBindingLayout = [this, shaderProvider, desc](
		caustica::rhi::ShaderType visibility, 
		const ::omm::Gpu::DescriptorRangeDesc* ranges, uint32_t numRanges)->caustica::rhi::BindingLayoutHandle {

		caustica::rhi::BindingLayoutDesc layoutDesc;
		layoutDesc.visibility = visibility;
        layoutDesc.bindingOffsets.shaderResource    = (shaderProvider != nullptr && shaderProvider->bindingOffsets.shaderResource  != UINT_MAX) ? shaderProvider->bindingOffsets.shaderResource  : desc->spirvBindingOffsets.textureOffset;
        layoutDesc.bindingOffsets.sampler           = (shaderProvider != nullptr && shaderProvider->bindingOffsets.sampler         != UINT_MAX) ? shaderProvider->bindingOffsets.sampler         : desc->spirvBindingOffsets.samplerOffset;
        layoutDesc.bindingOffsets.constantBuffer    = (shaderProvider != nullptr && shaderProvider->bindingOffsets.constantBuffer  != UINT_MAX) ? shaderProvider->bindingOffsets.constantBuffer  : desc->spirvBindingOffsets.constantBufferOffset;
        layoutDesc.bindingOffsets.unorderedAccess   = (shaderProvider != nullptr && shaderProvider->bindingOffsets.unorderedAccess != UINT_MAX) ? shaderProvider->bindingOffsets.unorderedAccess : desc->spirvBindingOffsets.storageTextureAndBufferOffset;

		caustica::rhi::BindingLayoutItem constantBufferItem = caustica::rhi::BindingLayoutItem::ConstantBuffer(desc->globalConstantBufferDesc.registerIndex);
		layoutDesc.bindings.push_back(constantBufferItem);

		caustica::rhi::BindingLayoutItem pushConstantBufferItem = caustica::rhi::BindingLayoutItem::PushConstants(
			desc->localConstantBufferDesc.registerIndex, desc->localConstantBufferDesc.maxDataSize);
		layoutDesc.bindings.push_back(pushConstantBufferItem);

		for (uint32_t samplerIt = 0; samplerIt < desc->staticSamplersNum; ++samplerIt)
		{
			const ::omm::Gpu::StaticSamplerDesc& sampler = desc->staticSamplers[samplerIt];
			layoutDesc.bindings.push_back(caustica::rhi::BindingLayoutItem::Sampler(sampler.registerIndex));
		}

		for (uint32_t descriptorRangeIndex = 0; descriptorRangeIndex < numRanges; descriptorRangeIndex++)
		{
			const ::omm::Gpu::DescriptorRangeDesc& descriptorRange = ranges[descriptorRangeIndex];

			caustica::rhi::BindingLayoutItem resourceItem = {};
			switch (descriptorRange.descriptorType)
			{
			case ::omm::Gpu::DescriptorType::TextureRead:
				resourceItem.type = caustica::rhi::ResourceType::Texture_SRV;
				resourceItem.size = 1;
				break;
			case ::omm::Gpu::DescriptorType::RawBufferRead:
				resourceItem.type = caustica::rhi::ResourceType::RawBuffer_SRV;
				resourceItem.size = 1;
				break;
			case ::omm::Gpu::DescriptorType::RawBufferWrite:
				resourceItem.type = caustica::rhi::ResourceType::RawBuffer_UAV;
				resourceItem.size = 1;
				break;
			case ::omm::Gpu::DescriptorType::BufferRead:
				resourceItem.type = caustica::rhi::ResourceType::TypedBuffer_SRV;
				resourceItem.size = 1;
				break;
			default:
				assert(!"Unknown NRD descriptor type");
				break;
			}

			resourceItem.size = 1;
			for (uint32_t descriptorOffset = 0; descriptorOffset < descriptorRange.descriptorNum; descriptorOffset++)
			{
				resourceItem.slot = descriptorRange.baseRegisterIndex + descriptorOffset;
				layoutDesc.bindings.push_back(resourceItem);
			}
		}

		return m_device->createBindingLayout(layoutDesc);
	};

	for (uint32_t samplerIt = 0; samplerIt < desc->staticSamplersNum; ++samplerIt)
	{
		const ::omm::SamplerDesc& samplerDesc = desc->staticSamplers[samplerIt].desc;
		caustica::rhi::SamplerDesc sDesc;
		sDesc.setAllFilters(samplerDesc.filter == ::omm::TextureFilterMode::Linear);
		sDesc.setAllAddressModes(GetRhiAddressMode(samplerDesc.addressingMode));
		m_samplers.push_back(std::make_pair(m_device->createSampler(sDesc), desc->staticSamplers[samplerIt].registerIndex));
	}

	for (uint32_t pipelineIt = 0; pipelineIt < desc->pipelineNum; ++pipelineIt)
	{
		const ::omm::Gpu::PipelineDesc& pipeline = desc->pipelines[pipelineIt];

		switch (pipeline.type)
		{
		case ::omm::Gpu::PipelineType::Compute:
		{
			const ::omm::Gpu::ComputePipelineDesc& compute = pipeline.compute;

			caustica::rhi::ShaderHandle shader;
			if (shaderProvider)
			{
				shader = shaderProvider->shaders(caustica::rhi::ShaderType::Compute, compute.shaderFileName, compute.shaderEntryPointName);
			}
			else
			{
				caustica::rhi::ShaderDesc shaderDesc(caustica::rhi::ShaderType::Compute);
				shaderDesc.debugName = compute.shaderFileName;
				shaderDesc.entryName = compute.shaderEntryPointName;
				shader = m_device->createShader(shaderDesc, compute.computeShader.data, compute.computeShader.size);
			}

			caustica::rhi::BindingLayoutHandle layout = CreateBindingLayout(caustica::rhi::ShaderType::Compute, 
				compute.descriptorRanges, compute.descriptorRangeNum);
			
			caustica::rhi::ComputePipelineHandle pipeline;
			{
				caustica::rhi::ComputePipelineDesc csDesc;
				csDesc.CS = shader;
				csDesc.bindingLayouts = { layout };
				pipeline = m_device->createComputePipeline(csDesc);
			}
			m_pipelines.push_back(pipeline);
			break;
		}
		case ::omm::Gpu::PipelineType::Graphics:
		{
			const ::omm::Gpu::GraphicsPipelineDesc& gfx = pipeline.graphics;
			static_assert(OMM_GRAPHICS_PIPELINE_DESC_VERSION == 3, "New GFX pipeline version detected, update integration code.");

			caustica::rhi::ShaderHandle vertex;
			if (shaderProvider)
			{
				vertex = shaderProvider->shaders(caustica::rhi::ShaderType::Vertex, gfx.vertexShaderFileName, gfx.vertexShaderEntryPointName);
			}
			else
			{
				caustica::rhi::ShaderDesc shaderDesc(caustica::rhi::ShaderType::Vertex);
				shaderDesc.debugName = gfx.vertexShaderFileName;
				shaderDesc.entryName = gfx.vertexShaderEntryPointName;
				vertex = m_device->createShader(shaderDesc, gfx.vertexShader.data, gfx.vertexShader.size);
			}

			caustica::rhi::ShaderHandle geometry;
			if (gfx.geometryShaderFileName)
			{
				if (shaderProvider)
				{
					geometry = shaderProvider->shaders(caustica::rhi::ShaderType::Geometry, gfx.geometryShaderFileName, gfx.geometryShaderEntryPointName);
				}
				else
				{
					caustica::rhi::ShaderDesc shaderDesc(caustica::rhi::ShaderType::Geometry);
					shaderDesc.debugName = gfx.geometryShaderFileName;
					shaderDesc.entryName = gfx.geometryShaderEntryPointName;
					geometry = m_device->createShader(shaderDesc, gfx.geometryShader.data, gfx.geometryShader.size);
				}
			}

			caustica::rhi::ShaderHandle pixel;
			{
				if (shaderProvider)
				{
					pixel = shaderProvider->shaders(caustica::rhi::ShaderType::Pixel, gfx.pixelShaderFileName, gfx.pixelShaderEntryPointName);
				}
				else
				{
					caustica::rhi::ShaderDesc shaderDesc(caustica::rhi::ShaderType::Pixel);
					shaderDesc.debugName = gfx.pixelShaderFileName;
					shaderDesc.entryName = gfx.pixelShaderEntryPointName;
					pixel = m_device->createShader(shaderDesc, gfx.pixelShader.data, gfx.pixelShader.size);
				}
			}

			caustica::rhi::BindingLayoutHandle layout = CreateBindingLayout(caustica::rhi::ShaderType::AllGraphics, gfx.descriptorRanges, gfx.descriptorRangeNum);

			caustica::rhi::InputLayoutHandle inputLayout;
			{
				caustica::rhi::VertexAttributeDesc desc;
				desc.name = ::omm::Gpu::GraphicsPipelineInputElementDesc::semanticName;
				desc.format = caustica::rhi::Format::R32_UINT;
				desc.elementStride = sizeof(uint32_t);
				static_assert(::omm::Gpu::GraphicsPipelineInputElementDesc::format == ::omm::Gpu::BufferFormat::R32_UINT);
				desc.arraySize = 1;
				desc.bufferIndex = 0;
				static_assert(::omm::Gpu::GraphicsPipelineInputElementDesc::inputSlot == 0);
				desc.offset = 0;
				static_assert(::omm::Gpu::GraphicsPipelineInputElementDesc::semanticIndex == 0);
				inputLayout = m_device->createInputLayout(&desc, 1 /*attributeCount*/, vertex);
			}

			caustica::rhi::GraphicsPipelineHandle pipeline;
			{
				caustica::rhi::GraphicsPipelineDesc gfxDesc;
				gfxDesc.primType = caustica::rhi::PrimitiveType::TriangleList;
				gfxDesc.renderState.depthStencilState.disableDepthTest();
				gfxDesc.renderState.depthStencilState.disableDepthWrite();
				gfxDesc.renderState.depthStencilState.disableStencil();
				gfxDesc.VS = vertex;
				gfxDesc.GS = geometry;
				gfxDesc.PS = pixel;
				gfxDesc.bindingLayouts = { layout };
				gfxDesc.inputLayout = inputLayout;
				gfxDesc.renderState.rasterState.conservativeRasterEnable = gfx.conservativeRasterization;
				gfxDesc.renderState.rasterState.cullMode = caustica::rhi::RasterCullMode::None;
				gfxDesc.renderState.rasterState.frontCounterClockwise = true;
				gfxDesc.renderState.rasterState.enableScissor(); // <- This is to prevent the framebuffer from implicitly setting the scissor rect...
				pipeline = m_device->createGraphicsPipeline(gfxDesc, m_nullFbo->getFramebufferInfo());
			}
			m_pipelines.push_back(pipeline);
			break;
		}
		default:
		{
			assert(false);
			break;
		}
		}
	}
}

::omm::Gpu::DispatchConfigDesc GpuBakeRhiImpl::GetConfig(const GpuBakeRhi::Input& params)
{
	using namespace ::omm;

	assert(params.operation != GpuBakeRhi::Operation::Invalid);

	Gpu::DispatchConfigDesc config;
	config.runtimeSamplerDesc.addressingMode	= GetTextureAddressMode(params.sampleMode);
	config.runtimeSamplerDesc.filter			= params.bilinearFilter ? TextureFilterMode::Linear : TextureFilterMode::Nearest;
	
	config.bakeFlags = Gpu::BakeFlags::Invalid;

	if (((uint32_t)params.operation & (uint32_t)GpuBakeRhi::Operation::Setup) == (uint32_t)GpuBakeRhi::Operation::Setup)
		config.bakeFlags = (::omm::Gpu::BakeFlags)((uint32_t)config.bakeFlags | (uint32_t)::omm::Gpu::BakeFlags::PerformSetup);

	if (((uint32_t)params.operation & (uint32_t)GpuBakeRhi::Operation::Bake) == (uint32_t)GpuBakeRhi::Operation::Bake)
		config.bakeFlags = (::omm::Gpu::BakeFlags)((uint32_t)config.bakeFlags | (uint32_t)::omm::Gpu::BakeFlags::PerformBake);

	if (params.enableStats)
		config.bakeFlags = (::omm::Gpu::BakeFlags)((uint32_t)config.bakeFlags | (uint32_t)::omm::Gpu::BakeFlags::EnablePostDispatchInfoStats);

	if (m_enableDebug)
		config.bakeFlags = (::omm::Gpu::BakeFlags)((uint32_t)config.bakeFlags | (uint32_t)::omm::Gpu::BakeFlags::EnableNsightDebugMode);
	
	if (!params.enableSpecialIndices)
		config.bakeFlags = (::omm::Gpu::BakeFlags)((uint32_t)config.bakeFlags | (uint32_t)::omm::Gpu::BakeFlags::DisableSpecialIndices);

	if (params.force32BitIndices)
		config.bakeFlags = (::omm::Gpu::BakeFlags)((uint32_t)config.bakeFlags | (uint32_t)::omm::Gpu::BakeFlags::Force32BitIndices);

	if (!params.enableTexCoordDeduplication)
		config.bakeFlags = (::omm::Gpu::BakeFlags)((uint32_t)config.bakeFlags | (uint32_t)::omm::Gpu::BakeFlags::DisableTexCoordDeduplication);

	if (params.computeOnly)
		config.bakeFlags = (::omm::Gpu::BakeFlags)((uint32_t)config.bakeFlags | (uint32_t)::omm::Gpu::BakeFlags::ComputeOnly);
	
	if (!params.enableLevelLineIntersection)
		config.bakeFlags = (::omm::Gpu::BakeFlags)((uint32_t)config.bakeFlags | (uint32_t)::omm::Gpu::BakeFlags::DisableLevelLineIntersection);

	if (params.enableNsightDebugMode)
		config.bakeFlags = (::omm::Gpu::BakeFlags)((uint32_t)config.bakeFlags | (uint32_t)::omm::Gpu::BakeFlags::EnableNsightDebugMode);

	config.alphaTextureWidth					= params.alphaTexture ? params.alphaTexture->getDesc().width : 1;
	config.alphaTextureHeight					= params.alphaTexture ? params.alphaTexture->getDesc().height : 1;
	config.alphaTextureChannel					= params.alphaTextureChannel;
	config.alphaMode							= AlphaMode::Test;
	config.alphaCutoff							= params.alphaCutoff;
	config.alphaCutoffGreater					= params.alphaCutoffGreater;
	config.alphaCutoffLessEqual					= params.alphaCutoffLessEqual;
	config.texCoordFormat						= GetTexCoordFormat(params.texCoordFormat);
	config.texCoordOffsetInBytes				= params.texCoordBufferOffsetInBytes;
	config.texCoordStrideInBytes				= params.texCoordStrideInBytes;
	config.indexFormat							= GetIndexFormat(params.indexBuffer->getDesc().format);
	config.indexCount							= (uint32_t)params.numIndices;
	config.indexOffset							= params.indexOffset;
	config.globalFormat							= params.format == caustica::rhi::rt::OpacityMicromapFormat::OC1_2_State ? Format::OC1_2_State : Format::OC1_4_State;
	config.maxScratchMemorySize					= params.minimalMemoryMode ? Gpu::ScratchMemoryBudget::MB_4 : Gpu::ScratchMemoryBudget::MB_256;
	config.maxOutOmmArraySize				    = params.maxOutOmmArraySize;
	config.maxSubdivisionLevel					= params.maxSubdivisionLevel;
	config.dynamicSubdivisionScale				= params.dynamicSubdivisionScale;
	return config;
}

void GpuBakeRhiImpl::ReserveScratchBuffers(const ::omm::Gpu::PreDispatchInfo& info)
{
	for (uint32_t poolIt = 0; poolIt < info.numTransientPoolBuffers; poolIt++)
	{
		if (m_transientPool.size() <= poolIt)
		{
			m_transientPool.push_back(nullptr);
		}

		const size_t bufferSize = info.transientPoolBufferSizeInBytes[poolIt];

		if (m_transientPool[poolIt] == nullptr || m_transientPool[poolIt]->getDesc().byteSize < bufferSize)
		{
			caustica::rhi::BufferDesc bufferDesc;
			bufferDesc.byteSize = bufferSize;
			bufferDesc.debugName = "::omm::Gpu::ResourceType::TRANSIENT_POOL_" + std::to_string(poolIt);
			bufferDesc.format = caustica::rhi::Format::R32_UINT;
			bufferDesc.canHaveUAVs = true;
			bufferDesc.canHaveRawViews = true;
			bufferDesc.isDrawIndirectArgs = true;
			m_transientPool[poolIt] = m_device->createBuffer(bufferDesc);
		}
	}
}

void GpuBakeRhiImpl::GetPreDispatchInfo(const GpuBakeRhi::Input& params, GpuBakeRhi::PreDispatchInfo& info)
{
	using namespace ::omm;

	Gpu::DispatchConfigDesc config = GetConfig(params);

	Gpu::PreDispatchInfo preBuildInfo;
	::omm::Result res = Gpu::GetPreDispatchInfo(m_pipeline, config, &preBuildInfo);
	assert(res == ::omm::Result::SUCCESS);

	if (preBuildInfo.outOmmIndexBufferFormat == ::omm::IndexFormat::UINT_8) 
	{
		info.ommIndexFormat = caustica::rhi::Format::R8_UINT;
	} 
	else if (preBuildInfo.outOmmIndexBufferFormat == ::omm::IndexFormat::UINT_16) 
	{
		info.ommIndexFormat = caustica::rhi::Format::R16_UINT;
	} 
	else
	{ 
		assert(preBuildInfo.outOmmIndexBufferFormat == ::omm::IndexFormat::UINT_32);
		info.ommIndexFormat = caustica::rhi::Format::R32_UINT;
	}
	info.ommIndexBufferSize = preBuildInfo.outOmmIndexBufferSizeInBytes;
	info.ommIndexHistogramSize = preBuildInfo.outOmmIndexHistogramSizeInBytes;
	info.ommIndexCount = preBuildInfo.outOmmIndexCount;
	info.ommArrayBufferSize = preBuildInfo.outOmmArraySizeInBytes;
	info.ommDescBufferSize = preBuildInfo.outOmmDescSizeInBytes;
	info.ommDescArrayHistogramSize = preBuildInfo.outOmmArrayHistogramSizeInBytes;
	info.ommPostDispatchInfoBufferSize = preBuildInfo.outOmmPostDispatchInfoSizeInBytes;
}

void GpuBakeRhiImpl::Dispatch(
	caustica::rhi::CommandListHandle commandList,
	const GpuBakeRhi::Input& params,
	const GpuBakeRhi::Buffers& result)
{
	using namespace ::omm;

	Gpu::DispatchConfigDesc config = GetConfig(params);

	Gpu::PreDispatchInfo preBuildInfo;
	::omm::Result res = Gpu::GetPreDispatchInfo(m_pipeline, config, &preBuildInfo);
	assert(res == ::omm::Result::SUCCESS);

	ReserveScratchBuffers(preBuildInfo);

	const ::omm::Gpu::DispatchChain* dispatchDesc = nullptr;
	res = Gpu::Dispatch(m_pipeline, config, &dispatchDesc);
	assert(res == ::omm::Result::SUCCESS);

	ExecuteDispatchChain(commandList, params, result, dispatchDesc);
}

void GpuBakeRhiImpl::Clear()
{
	m_bindingCache->Clear();
}

void GpuBakeRhiImpl::ReadPostDispatchInfo(void* pData, size_t byteSize, GpuBakeRhi::PostDispatchInfo& outPostDispatchInfo)
{
	static_assert(sizeof(::omm::Gpu::PostDispatchInfo) == sizeof(GpuBakeRhi::PostDispatchInfo));
	assert(byteSize >= sizeof(GpuBakeRhi::PostDispatchInfo));
	memcpy(&outPostDispatchInfo, pData, sizeof(GpuBakeRhi::PostDispatchInfo));
}

void GpuBakeRhiImpl::ReadUsageDescBuffer(void* pData, size_t byteSize, std::vector<caustica::rhi::rt::OpacityMicromapUsageCount>& outVmUsages)
{
	::omm::Cpu::OpacityMicromapUsageCount* usageDescs = static_cast<::omm::Cpu::OpacityMicromapUsageCount*>(pData);
	const size_t usageDescNum = byteSize / sizeof(::omm::Cpu::OpacityMicromapUsageCount);
	for (uint32_t i = 0; i < usageDescNum; ++i) {
		if (usageDescs[i].count != 0)
		{
			caustica::rhi::rt::OpacityMicromapUsageCount desc;
			desc.count = usageDescs[i].count;
			desc.format = (caustica::rhi::rt::OpacityMicromapFormat)usageDescs[i].format;
			desc.subdivisionLevel = usageDescs[i].subdivisionLevel;
			outVmUsages.push_back(desc);
		}
	}
}

caustica::rhi::TextureHandle GpuBakeRhiImpl::GetTextureResource(const GpuBakeRhi::Input& params, const GpuBakeRhi::Buffers& output, const ::omm::Gpu::Resource& resource)
{
	caustica::rhi::TextureHandle resourceHandle;
	switch (resource.type)
	{
	case ::omm::Gpu::ResourceType::IN_ALPHA_TEXTURE:
		resourceHandle = params.alphaTexture;
		break;
	default:
		assert(!"Unavailable resource type");
		break;
	}

	assert(resourceHandle);

	return resourceHandle;
}

caustica::rhi::BufferHandle GpuBakeRhiImpl::GetBufferResource(
	const GpuBakeRhi::Input& params,
	const GpuBakeRhi::Buffers& output,
	const ::omm::Gpu::Resource& resource,
	uint32_t& offsetInBytes)
{
	offsetInBytes = 0;
	caustica::rhi::BufferHandle resourceHandle;
	switch (resource.type)
	{
	case ::omm::Gpu::ResourceType::OUT_OMM_ARRAY_DATA:
		resourceHandle = output.ommArrayBuffer;
		offsetInBytes = output.ommArrayBufferOffset;
		break;
	case ::omm::Gpu::ResourceType::OUT_OMM_DESC_ARRAY:
		resourceHandle = output.ommDescBuffer;
		offsetInBytes = output.ommDescBufferOffset;
		break;
	case ::omm::Gpu::ResourceType::OUT_OMM_INDEX_BUFFER:
		resourceHandle = output.ommIndexBuffer;
		offsetInBytes = output.ommIndexBufferOffset;
		break;
	case ::omm::Gpu::ResourceType::OUT_OMM_DESC_ARRAY_HISTOGRAM:
		resourceHandle = output.ommDescArrayHistogramBuffer;
		offsetInBytes = output.ommDescArrayHistogramBufferOffset;
		break;
	case ::omm::Gpu::ResourceType::OUT_OMM_INDEX_HISTOGRAM:
		resourceHandle = output.ommIndexHistogramBuffer;
		offsetInBytes = output.ommIndexHistogramBufferOffset;
		break;
	case ::omm::Gpu::ResourceType::OUT_POST_DISPATCH_INFO:
		resourceHandle = output.ommPostDispatchInfoBuffer;
		offsetInBytes = output.ommPostDispatchInfoBufferOffset;
		break;
	case ::omm::Gpu::ResourceType::IN_INDEX_BUFFER:
		resourceHandle = params.indexBuffer;
		offsetInBytes = params.indexBufferOffsetInBytes;
		break;
	case ::omm::Gpu::ResourceType::IN_TEXCOORD_BUFFER:
		resourceHandle = params.texCoordBuffer;
		break;
	case ::omm::Gpu::ResourceType::TRANSIENT_POOL_BUFFER:
		resourceHandle = m_transientPool[resource.indexInPool];
		break;
	case ::omm::Gpu::ResourceType::STATIC_INDEX_BUFFER:
		resourceHandle = m_staticIndexBuffer;
		break;
	case ::omm::Gpu::ResourceType::STATIC_VERTEX_BUFFER:
		resourceHandle = m_staticVertexBuffer;
		break;
	default:
		assert(!"Unavailable resource type");
		break;
	}

	assert(resourceHandle);

	return resourceHandle;
}

void GpuBakeRhiImpl::ExecuteDispatchChain(
	caustica::rhi::CommandListHandle commandList,
	const GpuBakeRhi::Input& params,
	const GpuBakeRhi::Buffers& output,
	const ::omm::Gpu::DispatchChain* dispatchDesc)
{
	caustica::rhi::TextureHandle rtv = m_nullFbo->getDesc().colorAttachments[0].texture;

	commandList->beginTrackingBufferState(m_globalCBuffer, caustica::rhi::ResourceStates::ConstantBuffer);
	
	if (rtv)
		commandList->beginTrackingTextureState(rtv, caustica::rhi::AllSubresources, caustica::rhi::ResourceStates::Common);
	for (auto it : m_transientPool)
		commandList->beginTrackingBufferState(it, caustica::rhi::ResourceStates::Common);

	auto CreateDescriptorRangeDesc = [this, &output, &params](
		caustica::rhi::CommandListHandle commandList,
		const ::omm::Gpu::Resource* resources, uint32_t numResources, 
		const ::omm::Gpu::DescriptorRangeDesc* ranges, uint32_t numRanges)->caustica::rhi::BindingSetDesc {

		caustica::rhi::BindingSetDesc setDesc;

		commandList->setBufferState(m_globalCBuffer, caustica::rhi::ResourceStates::ConstantBuffer);
		setDesc.addItem(caustica::rhi::BindingSetItem::ConstantBuffer(m_globalCBufferSlot, m_globalCBuffer));
		setDesc.addItem(caustica::rhi::BindingSetItem::PushConstants(m_localCBufferSlot, m_localCBufferSize));

		for (uint32_t i = 0; i < m_samplers.size(); ++i)
		{
			setDesc.addItem(caustica::rhi::BindingSetItem::Sampler(m_samplers[i].second, m_samplers[i].first.Get()));
		}

		uint32_t resourceIndex = 0;
		//
		for (uint32_t descriptorRangeIndex = 0; descriptorRangeIndex < numRanges; descriptorRangeIndex++)
		{
			const ::omm::Gpu::DescriptorRangeDesc& descriptorRange = ranges[descriptorRangeIndex];

			for (uint32_t descriptorOffset = 0; descriptorOffset < descriptorRange.descriptorNum; descriptorOffset++)
			{
				// assert(resourceIndex < dispatchDesc.resourceNum);
				const ::omm::Gpu::Resource& resource = resources[resourceIndex];
				assert(resource.stateNeeded == descriptorRange.descriptorType);

				const uint32_t slot = descriptorRange.baseRegisterIndex + descriptorOffset;
				
				if (descriptorRange.descriptorType == ::omm::Gpu::DescriptorType::TextureRead)
				{
					caustica::rhi::TextureSubresourceSet subresources = caustica::rhi::AllSubresources;
					subresources.baseMipLevel = resource.mipOffset;
					subresources.numMipLevels = resource.mipNum;
					caustica::rhi::TextureHandle buffer = GetTextureResource(params, output, resource);
					commandList->setTextureState(buffer, subresources, caustica::rhi::ResourceStates::ShaderResource);
					setDesc.addItem(caustica::rhi::BindingSetItem::Texture_SRV(slot, buffer));
				}
				else if (descriptorRange.descriptorType == ::omm::Gpu::DescriptorType::RawBufferRead)
				{
					uint32_t offset = 0;
					caustica::rhi::BufferHandle buffer = GetBufferResource(params, output, resource, offset);
					commandList->setBufferState(buffer, caustica::rhi::ResourceStates::ShaderResource);
					setDesc.addItem(caustica::rhi::BindingSetItem::RawBuffer_SRV(slot, buffer, caustica::rhi::BufferRange(offset, ~0ull)));
				}
				else if (descriptorRange.descriptorType == ::omm::Gpu::DescriptorType::RawBufferWrite)
				{
					uint32_t offset = 0;
					caustica::rhi::BufferHandle buffer = GetBufferResource(params, output, resource, offset);
					commandList->setBufferState(buffer, caustica::rhi::ResourceStates::UnorderedAccess);
					setDesc.addItem(caustica::rhi::BindingSetItem::RawBuffer_UAV(slot, buffer, caustica::rhi::BufferRange(offset, ~0ull)));
				}
				else if (descriptorRange.descriptorType == ::omm::Gpu::DescriptorType::BufferRead)
				{
					uint32_t offset = 0;
					caustica::rhi::BufferHandle buffer = GetBufferResource(params, output, resource, offset);
					commandList->setBufferState(buffer, caustica::rhi::ResourceStates::ShaderResource);
					setDesc.addItem(caustica::rhi::BindingSetItem::TypedBuffer_SRV(slot, buffer, buffer->getDesc().format, caustica::rhi::BufferRange(offset, ~0ull)));
				}
				else
				{
					assert(false);
				}
				resourceIndex++;
			}
		}
		return setDesc;
	};

	const ::omm::Gpu::PipelineInfoDesc* pipelineDesc;
	::omm::Result res = ::omm::Gpu::GetPipelineDesc(m_pipeline, &pipelineDesc);
	assert(res == ::omm::Result::SUCCESS);

	assert(m_globalCBuffer && m_globalCBuffer->getDesc().byteSize >= pipelineDesc->globalConstantBufferDesc.maxDataSize);

	if (dispatchDesc->globalCBufferDataSize != 0)
		commandList->writeBuffer(m_globalCBuffer, dispatchDesc->globalCBufferData, dispatchDesc->globalCBufferDataSize);
	
	auto SetPushConstants = [this](caustica::rhi::CommandListHandle commandList, const void* localConstantBufferData, size_t localConstantBufferDataSize) {
		if (m_localCBufferSize == 0)
			return;
		assert(m_localCBufferSize < 128);
		uint8_t* pushConstants = (uint8_t*)alloca(m_localCBufferSize);
		memset(pushConstants, 0, m_localCBufferSize);
		if (localConstantBufferDataSize != 0)
			memcpy(pushConstants, localConstantBufferData, localConstantBufferDataSize);
		commandList->setPushConstants(pushConstants, m_localCBufferSize);
	};

	for (uint32_t dispatchIt = 0; dispatchIt < dispatchDesc->numDispatches; ++dispatchIt)
	{
		const ::omm::Gpu::DispatchDesc& desc = dispatchDesc->dispatches[dispatchIt];

		switch (desc.type)
		{
		case ::omm::Gpu::DispatchType::BeginLabel:
		{
			const ::omm::Gpu::BeginLabelDesc& label = desc.beginLabel;
			std::string name = std::string("OMM:") + label.debugName;
			commandList->beginMarker(name.c_str());
		}
		break;
		case ::omm::Gpu::DispatchType::EndLabel:
		{
			commandList->endMarker();
		}
		break;
		case ::omm::Gpu::DispatchType::Compute:
		{
			const ::omm::Gpu::ComputeDesc& compute = desc.compute;
			const ::omm::Gpu::PipelineDesc& pipeline = pipelineDesc->pipelines[compute.pipelineIndex];

			assert(pipeline.type == ::omm::Gpu::PipelineType::Compute);

			caustica::rhi::BindingSetDesc setDesc = CreateDescriptorRangeDesc(
				commandList,
				compute.resources, compute.resourceNum, 
				pipeline.compute.descriptorRanges, pipeline.compute.descriptorRangeNum);
			caustica::rhi::ComputePipeline* csPipeline = caustica::rhi::checked_cast<caustica::rhi::ComputePipeline*>(m_pipelines[compute.pipelineIndex].Get());
			caustica::rhi::BindingSetHandle bindingSet = m_bindingCache->GetOrCreateBindingSet(setDesc, csPipeline->getDesc().bindingLayouts[0].Get());

			commandList->commitBarriers();

			caustica::rhi::ComputeState state;
			state.pipeline = csPipeline;
			state.bindings = { bindingSet };

			commandList->setComputeState(state);

			SetPushConstants(commandList, compute.localConstantBufferData, compute.localConstantBufferDataSize);

			commandList->dispatch(compute.gridWidth, compute.gridHeight, 1);
		}
		break;
		case ::omm::Gpu::DispatchType::ComputeIndirect:
		{
			const ::omm::Gpu::ComputeIndirectDesc& compute = desc.computeIndirect;
			const ::omm::Gpu::PipelineDesc& pipeline = pipelineDesc->pipelines[compute.pipelineIndex];

			assert(pipeline.type == ::omm::Gpu::PipelineType::Compute);

			caustica::rhi::BindingSetDesc setDesc = CreateDescriptorRangeDesc(
				commandList,
				compute.resources, compute.resourceNum,
				pipeline.compute.descriptorRanges, pipeline.compute.descriptorRangeNum);
			caustica::rhi::ComputePipeline* csPipeline = caustica::rhi::checked_cast<caustica::rhi::ComputePipeline*>(m_pipelines[compute.pipelineIndex].Get());
			caustica::rhi::BindingSetHandle bindingSet = m_bindingCache->GetOrCreateBindingSet(setDesc, csPipeline->getDesc().bindingLayouts[0].Get());

			uint32_t indirectArgOffset = 0;
			caustica::rhi::BufferHandle indArg = GetBufferResource(params, output, compute.indirectArg, indirectArgOffset);

			commandList->setBufferState(indArg, caustica::rhi::ResourceStates::IndirectArgument);
			commandList->commitBarriers();

			caustica::rhi::ComputeState state;
			state.pipeline = csPipeline;
			state.bindings = { bindingSet };
			state.indirectParams = indArg;

			commandList->setComputeState(state);
			SetPushConstants(commandList, compute.localConstantBufferData, compute.localConstantBufferDataSize);

			commandList->dispatchIndirect(indirectArgOffset + (uint32_t)compute.indirectArgByteOffset);
		}
		break;
		case ::omm::Gpu::DispatchType::DrawIndexedIndirect:
		{
			const ::omm::Gpu::DrawIndexedIndirectDesc& draw = desc.drawIndexedIndirect;
			const ::omm::Gpu::PipelineDesc& pipeline = pipelineDesc->pipelines[draw.pipelineIndex];
			
			assert(pipeline.type == ::omm::Gpu::PipelineType::Graphics);
			
			caustica::rhi::BindingSetDesc setDesc = CreateDescriptorRangeDesc(
				commandList,
				draw.resources, draw.resourceNum,
				pipeline.graphics.descriptorRanges, pipeline.graphics.descriptorRangeNum);
			caustica::rhi::GraphicsPipeline* gfxPipeline = caustica::rhi::checked_cast<caustica::rhi::GraphicsPipeline*>(m_pipelines[draw.pipelineIndex].Get());
			caustica::rhi::BindingSetHandle bindingSet = m_bindingCache->GetOrCreateBindingSet(setDesc, gfxPipeline->getDesc().bindingLayouts[0].Get());
			
			uint32_t indirectArgOffset = 0;
			caustica::rhi::BufferHandle indArg = GetBufferResource(params, output, draw.indirectArg, indirectArgOffset);

			commandList->setBufferState(indArg, caustica::rhi::ResourceStates::IndirectArgument);
			// commandList->commitBarriers(); UGH. This is done in setGraphicsState.
			
			if (rtv)
				commandList->setTextureState(rtv, caustica::rhi::AllSubresources, caustica::rhi::ResourceStates::RenderTarget);

			caustica::rhi::Viewport viewport;
			viewport.minX = draw.viewport.minWidth;
			viewport.maxX = draw.viewport.maxWidth;
			viewport.minY = draw.viewport.minHeight;
			viewport.maxY = draw.viewport.maxHeight;

			caustica::rhi::GraphicsState state;
			state.addBindingSet(bindingSet);
			state.setPipeline(gfxPipeline);
			state.setFramebuffer(m_nullFbo);
			state.viewport.addViewportAndScissorRect(viewport);
			state.setIndirectParams(indArg);

			uint32_t indexBufferOffset = 0;
			state.setIndexBuffer({ 
				GetBufferResource(params, output, draw.indexBuffer, indexBufferOffset),
				caustica::rhi::Format::R32_UINT,
				indexBufferOffset + draw.indexBufferOffset
			});

			uint32_t vertexBufferOffset = 0;
			state.addVertexBuffer({ 
				GetBufferResource(params, output, draw.vertexBuffer, vertexBufferOffset),
				0u, 
				vertexBufferOffset + draw.vertexBufferOffset });
			
			commandList->setGraphicsState(state);
			SetPushConstants(commandList, draw.localConstantBufferData, draw.localConstantBufferDataSize);

			commandList->drawIndexedIndirect((uint32_t)draw.indirectArgByteOffset);
		}
		break;
		default:
			break;
		}
	}

	if (rtv)
		commandList->setTextureState(rtv, caustica::rhi::AllSubresources, caustica::rhi::ResourceStates::Common);
	commandList->setBufferState(m_globalCBuffer, caustica::rhi::ResourceStates::ConstantBuffer);
	if (output.ommArrayBuffer.Get())
		commandList->setBufferState(output.ommArrayBuffer, caustica::rhi::ResourceStates::Common);
	if (output.ommDescBuffer.Get())
		commandList->setBufferState(output.ommDescBuffer, caustica::rhi::ResourceStates::Common);
	if (output.ommIndexBuffer.Get())
		commandList->setBufferState(output.ommIndexBuffer, caustica::rhi::ResourceStates::Common);
	if (output.ommDescArrayHistogramBuffer.Get())
		commandList->setBufferState(output.ommDescArrayHistogramBuffer, caustica::rhi::ResourceStates::Common);
	if (output.ommIndexHistogramBuffer.Get())
		commandList->setBufferState(output.ommIndexHistogramBuffer, caustica::rhi::ResourceStates::Common);
	for (auto it : m_transientPool)
		commandList->setBufferState(it, caustica::rhi::ResourceStates::Common);
	commandList->commitBarriers();
}

void GpuBakeRhiImpl::DumpDebug(
	const char* folderName,
	const char* debugName,
	const GpuBakeRhi::Input& params,
	const std::vector<uint8_t>& ommArrayBuffer,
	const std::vector<uint8_t>& ommDescBuffer,
	const std::vector<uint8_t>& ommIndexBuffer,
	caustica::rhi::Format indexBufferFormat,
	const std::vector<uint8_t>& ommDescArrayHistogramBuffer,
	const std::vector<uint8_t>& ommIndexHistogramBuffer,
	const void* indexBuffer,
	const uint32_t indexCount,
	caustica::rhi::Format ommTexCoordBufferFormat,
	const void* texCoords,
	const float* imageData,
	const uint32_t width,
	const uint32_t height
)
{
	::omm::IndexFormat ommIndexBufferFormat;
	if (indexBufferFormat == caustica::rhi::Format::R8_UINT)
	{
		ommIndexBufferFormat = ::omm::IndexFormat::UINT_8;
	}
	else if (indexBufferFormat == caustica::rhi::Format::R16_UINT) 
	{
		ommIndexBufferFormat = ::omm::IndexFormat::UINT_16;
	}
	else
	{
		assert(indexBufferFormat == caustica::rhi::Format::R32_UINT);
		ommIndexBufferFormat = ::omm::IndexFormat::UINT_32;
	}

	::omm::Cpu::BakeResultDesc result;
	result.arrayData = ommArrayBuffer.data();
	result.arrayDataSize = (uint32_t)ommArrayBuffer.size();
	result.descArray = (const ::omm::Cpu::OpacityMicromapDesc*)ommDescBuffer.data();
	result.descArrayCount = (uint32_t)(ommDescBuffer.size() / sizeof(::omm::Cpu::OpacityMicromapDesc));
	result.indexBuffer = ommIndexBuffer.data();
	result.indexFormat = ommIndexBufferFormat;
	result.descArrayHistogramCount = (uint32_t)(ommDescArrayHistogramBuffer.size() / sizeof(::omm::Cpu::OpacityMicromapUsageCount));
	result.descArrayHistogram = (const ::omm::Cpu::OpacityMicromapUsageCount*)ommDescArrayHistogramBuffer.data();
	result.indexHistogramCount = (uint32_t)(ommIndexHistogramBuffer.size() / sizeof(::omm::Cpu::OpacityMicromapUsageCount));
	result.indexHistogram = (const ::omm::Cpu::OpacityMicromapUsageCount*)ommIndexHistogramBuffer.data();

	::omm::Cpu::TextureMipDesc mip;
	mip.width = width;
	mip.height = height;
	mip.textureData = imageData;

	::omm::Cpu::TextureDesc texDesc;
	texDesc.format = ::omm::Cpu::TextureFormat::FP32;
	texDesc.mipCount = 1;
	texDesc.mips = &mip;

	::omm::Cpu::Texture texHandle;
	::omm::Result res = ::omm::Cpu::CreateTexture(m_cpuBaker, texDesc, &texHandle);
	assert(res == ::omm::Result::SUCCESS);

	::omm::Cpu::BakeInputDesc config;
	config.alphaMode = /*task.geometry->material->domain == MaterialDomain::AlphaBlended ? ::omm::AlphaMode::Blend : */::omm::AlphaMode::Test;
	config.indexBuffer = indexBuffer;
	config.indexCount = indexCount;
	config.indexFormat = ::omm::IndexFormat::UINT_32;
	config.texture = texHandle;
	config.texCoords = texCoords;
	config.texCoordFormat = GetTexCoordFormat(ommTexCoordBufferFormat);
	config.alphaCutoff = params.alphaCutoff;
	config.runtimeSamplerDesc.addressingMode	= GetTextureAddressMode(params.sampleMode);
	config.runtimeSamplerDesc.filter = params.bilinearFilter ? ::omm::TextureFilterMode::Linear : ::omm::TextureFilterMode::Nearest;

	res = ::omm::Debug::SaveAsImages(
		m_baker, config, &result, 
		{
			.path = folderName,
			.filePostfix = debugName,
			.detailedCutout = false, 
			.dumpOnlyFirstOMM = false,
			.monochromeUnknowns = false,
			.oneFile = false 
		}
	);
	assert(res == ::omm::Result::SUCCESS);

	res = ::omm::Cpu::DestroyTexture(m_cpuBaker, texHandle);
	assert(res == ::omm::Result::SUCCESS);
}

GpuBakeRhi::Stats GpuBakeRhiImpl::GetStats(const ::omm::Cpu::BakeResultDesc& desc)
{
	::omm::Debug::Stats stats;
	::omm::Result statsRes = ::omm::Debug::GetStats(m_baker, &desc, &stats);
	assert(statsRes == ::omm::Result::SUCCESS);

	GpuBakeRhi::Stats s;
	s.totalOpaque = stats.totalOpaque;
	s.totalTransparent = stats.totalTransparent;
	s.totalUnknownTransparent = stats.totalUnknownTransparent;
	s.totalUnknownOpaque = stats.totalUnknownOpaque;
	s.totalFullyOpaque = stats.totalFullyOpaque;
	s.totalFullyTransparent = stats.totalFullyTransparent;
	s.totalFullyUnknownOpaque = stats.totalFullyUnknownOpaque;
	s.totalFullyUnknownTransparent = stats.totalFullyUnknownTransparent;
	return s;
}


GpuBakeRhi::GpuBakeRhi(caustica::rhi::DeviceHandle device, caustica::rhi::CommandListHandle commandList, bool enableDebug, ShaderProvider* shaderProvider, std::optional<MessageCallback> callback)
	:m_impl(std::make_unique<GpuBakeRhiImpl>(device, commandList, enableDebug, shaderProvider, callback))
{

}

GpuBakeRhi::~GpuBakeRhi()
{

}

// CPU side pre-build info.
void GpuBakeRhi::GetPreDispatchInfo(const Input& params, PreDispatchInfo& info)
{
	m_impl->GetPreDispatchInfo(params, info);
}

void GpuBakeRhi::Dispatch(
	caustica::rhi::CommandListHandle commandList,
	const Input& params,
	const Buffers& buffers)
{
	m_impl->Dispatch(commandList, params, buffers);
}

void GpuBakeRhi::Clear()
{
	m_impl->Clear();
}

// This assumes pData is the CPU-side pointer of the contents in vmUsageDescReadbackBufferSize.
void GpuBakeRhi::ReadPostDispatchInfo(void* pData, size_t byteSize, PostDispatchInfo& outPostDispatchInfo)
{
	GpuBakeRhiImpl::ReadPostDispatchInfo(pData, byteSize, outPostDispatchInfo);
}

void GpuBakeRhi::ReadUsageDescBuffer(void* pData, size_t byteSize, std::vector<caustica::rhi::rt::OpacityMicromapUsageCount>& outVmUsages)
{
	GpuBakeRhiImpl::ReadUsageDescBuffer(pData, byteSize, outVmUsages);
}

// Debug dumping
void GpuBakeRhi::DumpDebug(
	const char* folderName,
	const char* debugName,
	const Input& params,
	const std::vector<uint8_t>& ommArrayBuffer,
	const std::vector<uint8_t>& ommDescBuffer,
	const std::vector<uint8_t>& ommIndexBuffer,
	caustica::rhi::Format ommIndexBufferFormat,
	const std::vector<uint8_t>& ommDescArrayHistogramBuffer,
	const std::vector<uint8_t>& ommIndexHistogramBuffer,
	const void* indexBuffer,
	const uint32_t indexCount,
	caustica::rhi::Format ommTexCoordBufferFormat,
	const void* texCoords,
	const float* imageData,
	const uint32_t width,
	const uint32_t height
)
{
	m_impl->DumpDebug(
		folderName,
		debugName,
		params,
		ommArrayBuffer,
		ommDescBuffer,
		ommIndexBuffer,
		ommIndexBufferFormat,
		ommDescArrayHistogramBuffer,
		ommIndexHistogramBuffer,
		indexBuffer,
		indexCount,
		ommTexCoordBufferFormat,
		texCoords,
		imageData,
		width,
		height
	);
}

GpuBakeRhi::Stats GpuBakeRhi::GetStats(const ::omm::Cpu::BakeResultDesc& desc)
{
	return m_impl->GetStats(desc);
}

} // namespace caustica::omm
