#include <assets/loader/IesProfile.h>
#include <assets/loader/ShaderFactory.h>
#include <render/core/DescriptorTableManager.h>
#include <core/vfs/VFS.h>
#include <math/math.h>
#include <core/log.h>
#include <rhi/utils.h>
#include <sstream>

#if CAUSTICA_WITH_STATIC_SHADERS
#if CAUSTICA_WITH_DX11
#include "compiled_shaders/ies_profile_cs.dxbc.h"
#endif
#if CAUSTICA_WITH_DX12
#include "compiled_shaders/ies_profile_cs.dxil.h"
#endif
#if CAUSTICA_WITH_VULKAN
#include "compiled_shaders/ies_profile_cs.spirv.h"
#endif
#endif

using namespace caustica;

IesProfileLoader::IesProfileLoader(
    caustica::rhi::Device* device, 
    std::shared_ptr<ShaderFactory> shaderFactory, 
    std::shared_ptr<IDescriptorTableManager> descriptorTableManager)
    : m_Device(device)
    , m_ShaderFactory(shaderFactory)
    , m_DescriptorTableManager(descriptorTableManager)
{
    caustica::rhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = caustica::rhi::ShaderType::Compute;
    layoutDesc.bindings = {
        caustica::rhi::BindingLayoutItem::TypedBuffer_SRV(0),
        caustica::rhi::BindingLayoutItem::Texture_UAV(0),
    };
    m_BindingLayout = device->createBindingLayout(layoutDesc);

    m_ComputeShader = m_ShaderFactory->createAutoShader("engine/ies_profile_cs.hlsl", "main", CAUSTICA_MAKE_PLATFORM_SHADER(g_ies_profile_cs), nullptr, caustica::rhi::ShaderType::Compute);

    caustica::rhi::ComputePipelineDesc pipelineDesc;
    pipelineDesc.bindingLayouts = { m_BindingLayout };
    pipelineDesc.CS = m_ComputeShader;
    m_ComputePipeline = device->createComputePipeline(pipelineDesc);
}

static const char* c_SupportedProfiles[] = {
    "IESNA:LM-63-1986",
    "IESNA:LM-63-1991",
    "IESNA91",
    "IESNA:LM-63-1995",
    "IESNA:LM-63-2002",
    "ERCO Leuchten GmbH  BY: ERCO/LUM650/8701",
    "ERCO Leuchten GmbH"
};

// See https://docs.agi32.com/PhotometricToolbox/Content/Open_Tool/iesna_lm-63_format.htm for the format reference. Pasted below.

/*
Each line marked with an asterisk must begin a new line.
Descriptions enclosed by the brackets"<" and ">" refer to the actual data stored on that line.
Lines marked with an "at sign" @ appear only if TILT=INCLUDE.

All data is in standard ASCII format.

*IESNA:LM-63-2002
*<keyword [TEST]>
*<keyword [TESTLAB]>
*<keyword [ISSUEDATE]>
*<keyword [MANUFAC]>
*<keyword 5>
 "
*<keyword n>
*TILT=<filespec> or INCLUDE or NONE
@ *<lamp to luminaire geometry>
@ *<# of pairs of angles and multiplying factors>
@ *<angles>
@ *<multiplying factors>
* <# lamps> <lumens/lamp> <multiplier> <# vertical angles> <# horizontal angles>
  <photometric type> <units type> <width> <length> <height>
* <ballast factor> <ballast lamp factor> <input watts>
* <vertical angles>
* <horizontal angles>
* <candela values for all vertical angles at first horizontal angle>
* <candela values for all vertical angles at second horizontal angle>
* "
* "
<candela values for all vertical angles at last horizontal angle>
*/

enum class IesStatus
{
    Success,
    UnsupportedProfile,
    UnsuppoeredTilt,
    WrongDataSize,
    InvalidData
};

static IesStatus ParseIesFile(char* fileData,
    std::vector<float>& numericData,
    float& maxCandelas)
{
    // count whitespace to get a rough estimate of the number of floats stored
    int numWhitespace = 0;
    for (char* p = fileData; *p; p++)
    {
        if (*p == ' ')
            ++numWhitespace;
    }

    // parse the header line by line
    const char* lineDelimiters = "\r\n";
    const char* dataDelimiters = "\r\n\t ";
    char* line = strtok(fileData, lineDelimiters);
    int lineNumber = 1;

    while (line)
    {
        if (lineNumber == 1)
        {
            bool profileFound = false;
            for (const char* profile : c_SupportedProfiles)
            {
                if (strstr(line, profile))
                {
                    profileFound = true;
                    break;
                }
            }

            if (!profileFound)
            {
                return IesStatus::UnsupportedProfile;
            }
        }
        else
        {
            if (strstr(line, "TILT=NONE") == line)
            {
                break;
            }
            else if (strstr(line, "TILT=") == line)
            {
                return IesStatus::UnsuppoeredTilt;
            }
        }

        line = strtok(NULL, lineDelimiters);
        ++lineNumber;
    }

    numericData.reserve(numWhitespace);
    while ((line = strtok(NULL, dataDelimiters)))
    {
        float value = 0.f;
        if (sscanf(line, "%f", &value) == 1)
            numericData.push_back(value);
    }

    if (numericData.size() < 16)
    {
        return IesStatus::WrongDataSize;
    }

    int numVerticalAngles = int(numericData[3]);
    int numHorizontalAngles = int(numericData[4]);
    int headerSize = 13;

    int expectedDataSize = headerSize + numHorizontalAngles + numVerticalAngles + numHorizontalAngles * numVerticalAngles;
    if (numericData.size() != expectedDataSize)
    {
        return IesStatus::WrongDataSize;
    }

    maxCandelas = 0.f;
    for (int index = headerSize + numHorizontalAngles + numVerticalAngles; index < expectedDataSize; index++)
        maxCandelas = std::max(maxCandelas, numericData[index]);

    return IesStatus::Success;
}

std::shared_ptr<IesProfile> IesProfileLoader::loadIesProfile(caustica::IFileSystem& fs, const std::filesystem::path& path)
{
    auto fileBlob = fs.readFile(path);

    if (!fileBlob)
        return nullptr;

    if (fileBlob->size() == 0)
        return nullptr;

    // make a copy of the data because we need to modify it, and blobs are immutable
    char* fileData = (char*)malloc(fileBlob->size() + 1);
    if (!fileData)
        return nullptr;

    memcpy(fileData, fileBlob->data(), fileBlob->size());
    fileData[fileBlob->size()] = 0;

    fileBlob = nullptr;

    std::vector<float> numericData;
    float maxCandelas;

    IesStatus status = ParseIesFile(fileData, numericData, maxCandelas);

    free(fileData);

    if (status != IesStatus::Success)
        return nullptr;

    // Stash the normalization factor in data[0], we don't use that anyway
    numericData[0] = 1.f / maxCandelas;

    std::shared_ptr<IesProfile> profile = std::make_shared<IesProfile>();
    profile->name = path.filename().generic_string();
    profile->textureIndex = -1;
    profile->rawData = std::move(numericData);

    return profile;
}

void IesProfileLoader::bakeIesProfile(IesProfile& profile, caustica::rhi::CommandList* commandList)
{
    if (profile.texture)
        return;

    caustica::rhi::BufferDesc bufferDesc;
    bufferDesc.byteSize = sizeof(float) * profile.rawData.size();
    bufferDesc.initialState = caustica::rhi::ResourceStates::ShaderResource;
    bufferDesc.format = caustica::rhi::Format::R32_FLOAT;
    bufferDesc.keepInitialState = true;
    bufferDesc.debugName = "IesProfileData";
    bufferDesc.canHaveTypedViews = true;
    caustica::rhi::BufferHandle buffer = m_Device->createBuffer(bufferDesc);

    caustica::rhi::TextureDesc textureDesc;
    textureDesc.dimension = caustica::rhi::TextureDimension::Texture2D;
    textureDesc.width = 128;
    textureDesc.height = 128;
    textureDesc.debugName = profile.name;
    textureDesc.format = caustica::rhi::Format::R16_FLOAT;
    textureDesc.isUAV = true;
    profile.texture = m_Device->createTexture(textureDesc);

    caustica::rhi::BindingSetDesc bindingSetDesc;
    bindingSetDesc.bindings = {
        caustica::rhi::BindingSetItem::TypedBuffer_SRV(0, buffer),
        caustica::rhi::BindingSetItem::Texture_UAV(0, profile.texture)
    };
    caustica::rhi::BindingSetHandle bindingSet = m_Device->createBindingSet(bindingSetDesc, m_BindingLayout);

    commandList->writeBuffer(buffer, profile.rawData.data(), profile.rawData.size() * sizeof(float));

    commandList->beginTrackingTextureState(profile.texture, caustica::rhi::AllSubresources, caustica::rhi::ResourceStates::Common);

    caustica::rhi::ComputeState state;
    state.bindings = { bindingSet };
    state.pipeline = m_ComputePipeline;
    commandList->setComputeState(state);
    commandList->dispatch(8, 8, 1);

    commandList->setPermanentTextureState(profile.texture, caustica::rhi::ResourceStates::ShaderResource);
    commandList->commitBarriers();

    if (m_DescriptorTableManager)
    {
        profile.textureIndex = m_DescriptorTableManager->createDescriptor(caustica::rhi::BindingSetItem::Texture_SRV(0, profile.texture));
    }
}
