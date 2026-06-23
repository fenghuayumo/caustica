#include "platform/os.h"

namespace caustica
{

OS* OS::s_Instance = nullptr;

std::string OS::powerStateToString(PowerState state)
{
    switch (state)
    {
    case PowerState::POWERSTATE_ON_BATTERY:   return "On Battery";
    case PowerState::POWERSTATE_NO_BATTERY:   return "No Battery (AC)";
    case PowerState::POWERSTATE_CHARGING:     return "Charging";
    case PowerState::POWERSTATE_CHARGED:      return "Charged";
    default:                                  return "Unknown";
    }
}

} // namespace caustica
