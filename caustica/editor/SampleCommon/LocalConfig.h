/*
* Copyright (c) 2025, NVIDIA CORPORATION.  All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto.  Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

#pragma once

#include <string>

#include <shaders/PathTracer/Config.h>

class Sample;
struct SampleUIData;

namespace caustica
{
    struct Material;
}

struct LocalConfig
{
    static void        PreferredSceneOverride( std::string & preferredScene );
    static void        PostAppInit(SampleUIData & sampleUI );
    static void        PostMaterialLoad( caustica::Material & material );
    static void        PostSceneLoad( Sample & sample, SampleUIData & sampleUI );
};