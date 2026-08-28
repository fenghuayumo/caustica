#include <rhi/device_factory.h>

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace
{
    void require(bool condition, const char* message)
    {
        if (!condition)
        {
            std::cerr << "DeviceFactoryTests: " << message << std::endl;
            std::exit(1);
        }
    }
}

int main()
{
    using namespace caustica::rhi;

    AdapterSelector selector;
    std::string error;
    require(parseAdapterSelector("index:2", selector, &error), "index selector should parse");
    require(selector.mode == AdapterSelector::Mode::Index && selector.index == 2, "index selector value");

    require(parseAdapterSelector("name:RTX 5090", selector, &error), "name selector should parse");
    require(selector.mode == AdapterSelector::Mode::Name && selector.value == "RTX 5090", "name selector value");

    require(parseAdapterSelector("uuid:00112233445566778899aabbccddeeff", selector, &error), "UUID selector should parse");
    require(selector.mode == AdapterSelector::Mode::UUID, "UUID selector mode");

    std::vector<AdapterDesc> adapters(3);
    adapters[0].index = 0;
    adapters[0].name = "Integrated Graphics";
    adapters[0].type = AdapterType::Integrated;
    adapters[0].dedicatedVideoMemory = 512ull * 1024ull * 1024ull;

    adapters[1].index = 1;
    adapters[1].name = "NVIDIA RTX 5090";
    adapters[1].type = AdapterType::Discrete;
    adapters[1].dedicatedVideoMemory = 32ull * 1024ull * 1024ull * 1024ull;
    adapters[1].uuid = AdapterDesc::UUID{
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff };

    adapters[2].index = 2;
    adapters[2].name = "NVIDIA RTX 5090 Secondary";
    adapters[2].type = AdapterType::Discrete;
    adapters[2].dedicatedVideoMemory = 24ull * 1024ull * 1024ull * 1024ull;

    AdapterDesc software;
    software.index = 3;
    software.name = "Software Adapter";
    software.type = AdapterType::Software;
    software.software = true;
    software.suitable = false;
    adapters.push_back(software);

    AdapterSelectionResult selection = selectAdapter(adapters, AdapterSelector::automatic());
    require(selection && selection.index == 1, "auto should choose the strongest discrete adapter");

    selection = selectAdapter(adapters, AdapterSelector::byName("Secondary"));
    require(selection && selection.index == 2, "unique substring name selection");

    selection = selectAdapter(adapters, AdapterSelector::byName("RTX 5090"));
    require(!selection && selection.error.find("ambiguous") != std::string::npos, "ambiguous names must fail");

    selection = selectAdapter(adapters, AdapterSelector::byUuid("00112233445566778899aabbccddeeff"));
    require(selection && selection.index == 1, "UUID selection");

    selection = selectAdapter(adapters, AdapterSelector::byIndex(3));
    require(!selection && selection.error.find("not suitable") != std::string::npos,
        "explicit software adapter selection must fail");

    int createdIndex = -2;
    DeviceFactory factory(
        [&](std::vector<AdapterDesc>& output, std::string&) {
            output = adapters;
            return true;
        },
        [&](int index, std::string&) {
            createdIndex = index;
            return true;
        },
        [&]() { return createdIndex < 0 ? 1 : createdIndex; });

    DeviceFactoryCreateResult created = factory.createDevice(AdapterSelector::byName("Secondary"));
    require(created && createdIndex == 2, "factory should pass resolved index to backend");
    require(created.adapter && created.adapter->index == 2, "factory should retain selected descriptor");

    return 0;
}
