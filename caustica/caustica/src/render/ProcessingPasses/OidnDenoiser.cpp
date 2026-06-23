#include <render/ProcessingPasses/OidnDenoiser.h>

#include <cstring>
#include <utility>

#if RTXPT_WITH_OIDN
#include <OpenImageDenoise/oidn.hpp>
#endif

struct OidnDenoiser::Impl
{
    std::string LastError;
    std::string DeviceDescription;

#if RTXPT_WITH_OIDN
    oidn::DeviceRef Device;
    oidn::FilterRef Filter;
    oidn::BufferRef ColorBuffer;
    oidn::BufferRef AlbedoBuffer;
    oidn::BufferRef NormalBuffer;
    oidn::BufferRef OutputBuffer;
    uint32_t FilterWidth = 0;
    uint32_t FilterHeight = 0;
    size_t BufferByteSize = 0;
    bool DevicePreferenceUseGPU = true;
    bool DeviceUsesGPU = false;

    static const char* DeviceTypeToString(oidn::DeviceType type)
    {
        switch (type)
        {
        case oidn::DeviceType::CPU: return "CPU";
        case oidn::DeviceType::SYCL: return "SYCL GPU";
        case oidn::DeviceType::CUDA: return "CUDA GPU";
        case oidn::DeviceType::HIP: return "HIP GPU";
        case oidn::DeviceType::Metal: return "Metal GPU";
        default: return "Default";
        }
    }

    static bool IsGPUDeviceType(oidn::DeviceType type)
    {
        return type != oidn::DeviceType::CPU && type != oidn::DeviceType::Default;
    }

    static oidn::Quality ToOIDNQuality(OidnDenoiser::Quality quality)
    {
        switch (quality)
        {
        case OidnDenoiser::Quality::Fast: return oidn::Quality::Fast;
        case OidnDenoiser::Quality::Balanced: return oidn::Quality::Balanced;
        case OidnDenoiser::Quality::High: return oidn::Quality::High;
        default: return oidn::Quality::Balanced;
        }
    }

    static oidn::Quality ToOIDNPrefilterQuality(OidnDenoiser::Prefilter prefilter)
    {
        switch (prefilter)
        {
        case OidnDenoiser::Prefilter::Fast: return oidn::Quality::Fast;
        case OidnDenoiser::Prefilter::Accurate: return oidn::Quality::High;
        default: return oidn::Quality::Balanced;
        }
    }

    void ReleaseBuffers()
    {
        Filter.release();
        ColorBuffer.release();
        AlbedoBuffer.release();
        NormalBuffer.release();
        OutputBuffer.release();
        FilterWidth = 0;
        FilterHeight = 0;
        BufferByteSize = 0;
    }

    void ReleaseDevice()
    {
        ReleaseBuffers();
        Device.release();
        DeviceDescription.clear();
        DeviceUsesGPU = false;
    }

    bool CheckError(const char* action)
    {
        const char* message = nullptr;
        oidn::Error error = Device ? Device.getError(message) : oidn::getError(message);
        if (error == oidn::Error::None)
            return true;

        LastError = "OIDN error while ";
        LastError += action;
        LastError += ": ";
        LastError += (message != nullptr) ? message : "unknown error";
        return false;
    }

    bool TryCreateDevice(int physicalDeviceID, bool useGpuPreference)
    {
        oidn::PhysicalDeviceRef physicalDevice(physicalDeviceID);
        const oidn::DeviceType type = physicalDevice.get<oidn::DeviceType>("type");
        const std::string name = physicalDevice.get<std::string>("name");

        oidn::DeviceRef candidate = physicalDevice.newDevice();
        if (!candidate)
            return CheckError("creating device");

        candidate.commit();

        const char* message = nullptr;
        oidn::Error error = candidate.getError(message);
        if (error != oidn::Error::None)
        {
            LastError = "OIDN error while committing device: ";
            LastError += (message != nullptr) ? message : "unknown error";
            return false;
        }

        Device = std::move(candidate);
        DeviceDescription = DeviceTypeToString(type);
        DevicePreferenceUseGPU = useGpuPreference;
        DeviceUsesGPU = IsGPUDeviceType(type);
        if (!name.empty())
        {
            DeviceDescription += ": ";
            DeviceDescription += name;
        }

        return true;
    }

    bool TryCreateTypedDevice(oidn::DeviceType type, bool useGpuPreference, const char* description)
    {
        oidn::DeviceRef candidate = oidn::newDevice(type);
        if (!candidate)
            return CheckError("creating device");

        candidate.commit();

        const char* message = nullptr;
        oidn::Error error = candidate.getError(message);
        if (error != oidn::Error::None)
        {
            LastError = "OIDN error while committing device: ";
            LastError += (message != nullptr) ? message : "unknown error";
            return false;
        }

        Device = std::move(candidate);
        DeviceDescription = description;
        DevicePreferenceUseGPU = useGpuPreference;
        DeviceUsesGPU = IsGPUDeviceType(type);
        return true;
    }

    bool EnsureDevice(bool useGpu)
    {
        if (Device && DevicePreferenceUseGPU == useGpu)
            return true;

        ReleaseDevice();

        int cpuDeviceID = -1;
        int fallbackDeviceID = -1;

        const int deviceCount = oidn::getNumPhysicalDevices();
        for (int i = 0; i < deviceCount; i++)
        {
            oidn::PhysicalDeviceRef physicalDevice(i);
            const oidn::DeviceType type = physicalDevice.get<oidn::DeviceType>("type");

            if (type == oidn::DeviceType::CPU)
            {
                if (cpuDeviceID < 0)
                    cpuDeviceID = i;
            }
            else if (useGpu && fallbackDeviceID < 0)
            {
                fallbackDeviceID = i;
            }
        }

        if (fallbackDeviceID >= 0 && TryCreateDevice(fallbackDeviceID, useGpu))
            return true;

        const std::string gpuError = LastError;
        if (cpuDeviceID >= 0 && TryCreateDevice(cpuDeviceID, useGpu))
        {
            if (!gpuError.empty())
                LastError.clear();
            return true;
        }

        if (!useGpu)
            return TryCreateTypedDevice(oidn::DeviceType::CPU, useGpu, "CPU");

        return TryCreateTypedDevice(oidn::DeviceType::Default, useGpu, "Default OIDN device");
    }

    bool EnsureBuffers(uint32_t width, uint32_t height, bool useAlbedo, bool useNormal)
    {
        const size_t requiredByteSize = size_t(width) * size_t(height) * 3 * sizeof(float);
        if (BufferByteSize != requiredByteSize)
            ReleaseBuffers();

        if (!ColorBuffer)
            ColorBuffer = Device.newBuffer(requiredByteSize);
        if (!OutputBuffer)
            OutputBuffer = Device.newBuffer(requiredByteSize);
        if (useAlbedo && !AlbedoBuffer)
            AlbedoBuffer = Device.newBuffer(requiredByteSize);
        if (useNormal && !NormalBuffer)
            NormalBuffer = Device.newBuffer(requiredByteSize);

        if (!useAlbedo)
            AlbedoBuffer.release();
        if (!useNormal)
            NormalBuffer.release();

        if (!ColorBuffer || !OutputBuffer || (useAlbedo && !AlbedoBuffer) || (useNormal && !NormalBuffer))
            return CheckError("creating image buffers");

        BufferByteSize = requiredByteSize;
        return true;
    }

    bool EnsureFilter(uint32_t width, uint32_t height)
    {
        if (Filter && FilterWidth == width && FilterHeight == height)
            return true;

        Filter.release();
        Filter = Device.newFilter("RT");
        if (!Filter)
            return CheckError("creating RT filter");

        FilterWidth = width;
        FilterHeight = height;
        return true;
    }

    bool ExecutePrefilter(const char* inputName, oidn::BufferRef& buffer, uint32_t width, uint32_t height, oidn::Quality quality)
    {
        oidn::FilterRef prefilter = Device.newFilter("RT");
        if (!prefilter)
            return CheckError("creating auxiliary prefilter");

        prefilter.setImage(inputName, buffer, oidn::Format::Float3, width, height);
        prefilter.setImage("output", buffer, oidn::Format::Float3, width, height);
        prefilter.set("quality", quality);
        prefilter.commit();
        if (!CheckError("committing auxiliary prefilter"))
            return false;

        prefilter.execute();
        return CheckError("executing auxiliary prefilter");
    }
#endif
};

OidnDenoiser::OidnDenoiser()
    : m_impl(std::make_unique<Impl>())
{
}

OidnDenoiser::~OidnDenoiser() = default;

bool OidnDenoiser::IsAvailable() const
{
#if RTXPT_WITH_OIDN
    return true;
#else
    return false;
#endif
}

const std::string& OidnDenoiser::GetLastError() const
{
    return m_impl->LastError;
}

const std::string& OidnDenoiser::GetDeviceDescription() const
{
    return m_impl->DeviceDescription;
}

bool OidnDenoiser::Denoise(const float* inputRgb, uint32_t width, uint32_t height, const Options& options, std::vector<float>& outputRgb)
{
    m_impl->LastError.clear();

    if (inputRgb == nullptr || width == 0 || height == 0)
    {
        m_impl->LastError = "OIDN input image is empty.";
        return false;
    }

#if RTXPT_WITH_OIDN
    const bool useAlbedo = (options.GuidePasses == Passes::Albedo || options.GuidePasses == Passes::AlbedoNormal) && options.AlbedoRgb != nullptr;
    const bool useNormal = options.GuidePasses == Passes::AlbedoNormal && options.NormalRgb != nullptr;
    const bool prefilterAux = options.GuidePrefilter != Prefilter::None && (useAlbedo || useNormal);

    if (!m_impl->EnsureDevice(options.UseGPU))
        return false;

    if (!m_impl->EnsureBuffers(width, height, useAlbedo, useNormal))
        return false;

    if (!m_impl->EnsureFilter(width, height))
        return false;

    outputRgb.resize(size_t(width) * size_t(height) * 3);

    m_impl->ColorBuffer.write(0, m_impl->BufferByteSize, inputRgb);
    if (!m_impl->CheckError("uploading color image"))
        return false;

    if (useAlbedo)
    {
        m_impl->AlbedoBuffer.write(0, m_impl->BufferByteSize, options.AlbedoRgb);
        if (!m_impl->CheckError("uploading albedo image"))
            return false;
    }

    if (useNormal)
    {
        m_impl->NormalBuffer.write(0, m_impl->BufferByteSize, options.NormalRgb);
        if (!m_impl->CheckError("uploading normal image"))
            return false;
    }

    if (prefilterAux)
    {
        const oidn::Quality prefilterQuality = Impl::ToOIDNPrefilterQuality(options.GuidePrefilter);
        if (useAlbedo && !m_impl->ExecutePrefilter("albedo", m_impl->AlbedoBuffer, width, height, prefilterQuality))
            return false;
        if (useNormal && !m_impl->ExecutePrefilter("normal", m_impl->NormalBuffer, width, height, prefilterQuality))
            return false;
    }

    m_impl->Filter.setImage("color", m_impl->ColorBuffer, oidn::Format::Float3, width, height);
    if (useAlbedo)
        m_impl->Filter.setImage("albedo", m_impl->AlbedoBuffer, oidn::Format::Float3, width, height);
    else
        m_impl->Filter.unsetImage("albedo");

    if (useNormal)
        m_impl->Filter.setImage("normal", m_impl->NormalBuffer, oidn::Format::Float3, width, height);
    else
        m_impl->Filter.unsetImage("normal");

    m_impl->Filter.setImage("output", m_impl->OutputBuffer, oidn::Format::Float3, width, height);
    m_impl->Filter.set("hdr", true);
    m_impl->Filter.set("cleanAux", prefilterAux);
    m_impl->Filter.set("quality", Impl::ToOIDNQuality(options.FilterQuality));
    m_impl->Filter.commit();
    if (!m_impl->CheckError("committing RT filter"))
        return false;

    m_impl->Filter.execute();
    if (!m_impl->CheckError("executing RT filter"))
        return false;

    m_impl->OutputBuffer.read(0, m_impl->BufferByteSize, outputRgb.data());
    return m_impl->CheckError("downloading denoised image");
#else
    (void)inputRgb;
    (void)width;
    (void)height;
    (void)options;
    (void)outputRgb;
    m_impl->LastError = "RTXPT_WITH_OIDN is disabled in this build.";
    return false;
#endif
}

void OidnDenoiser::Reset()
{
#if RTXPT_WITH_OIDN
    m_impl->ReleaseBuffers();
#endif
    m_impl->LastError.clear();
}
