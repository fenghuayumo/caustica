/*
* Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto. Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

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
