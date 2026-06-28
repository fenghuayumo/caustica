/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: LicenseRef-NvidiaProprietary
 *
 * NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
 * property and proprietary rights in and to this material, related
 * documentation and any modifications thereto. Any use, reproduction,
 * disclosure or distribution of this material and related documentation
 * without an express license agreement from NVIDIA CORPORATION or
 * its affiliates is strictly prohibited.
 */

#ifndef RTXDI_COLOR_HLSLI
#define RTXDI_COLOR_HLSLI

// RTXPT's RTXDI baseline keeps the color helpers in Math.hlsli. The ReSTIR PT
// files copied from newer RTXDI include Color.hlsli directly, so route them to
// the existing definitions instead of redefining the same functions.
#include "Rtxdi/Utils/Math.hlsli"

#endif // RTXDI_COLOR_HLSLI
