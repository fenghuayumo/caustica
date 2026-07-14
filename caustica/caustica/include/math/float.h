#pragma once

#include <stdint.h>
#include <limits>
#include "vector.h"

namespace caustica::math
{
    // 16-bit floating point number, E5M10.
    struct float16_t
    {
        uint16_t bits;
    };

    // Vector of 2 FP16 numbers packed into a single uint32.
    struct float16_t2
    {
        uint32_t bits;
    };

    // Vector of 4 FP16 numbers packed into a single uint64.
    struct float16_t4
    {
        uint64_t bits;
    };

    // 8-bit floating point number, E4M3.
    // This is FLOAT8E4M3FN, not FLOAT8E4M3FNUZ.
    // It does not have an infinity, has two NaNs (0x7f and 0xff), and a signed zero.
    struct float8e4m3_t
    {
        uint8_t bits;
    };

    // Vector of 4 E4M3 numbers packed into a single uint32.
    struct float8e4m3_t4
    {
        uint32_t bits;
    };

    // 8-bit floating point number, E5M2.
    // This is FLOAT8E5M2, not FLOAT8E5M2FNUZ.
    // It has a signed infinity (0x7c and 0xfc), 6 NaNs (0x7d..0x7f, 0xfd..0xff), and a signed zero.
    struct float8e5m2_t
    {
        uint8_t bits;
    };

    // Vector of 4 E5M2 numbers packed into a single uint32.
    struct float8e5m2_t4
    {
        uint32_t bits;
    };

#define DM_DECLARE_EQUALITY_OPERATORS(TY)\
    inline bool operator==(TY a, TY b) { return a.bits == b.bits; } \
    inline bool operator!=(TY a, TY b) { return a.bits != b.bits; }
    
    DM_DECLARE_EQUALITY_OPERATORS(float16_t)
    DM_DECLARE_EQUALITY_OPERATORS(float16_t2)
    DM_DECLARE_EQUALITY_OPERATORS(float16_t4)
    DM_DECLARE_EQUALITY_OPERATORS(float8e4m3_t)
    DM_DECLARE_EQUALITY_OPERATORS(float8e4m3_t4)
    DM_DECLARE_EQUALITY_OPERATORS(float8e5m2_t)
    DM_DECLARE_EQUALITY_OPERATORS(float8e5m2_t4)

#undef DM_DECLARE_EQUALITY_OPERATORS

    uint32_t asuint(float x);
    float asfloat(uint32_t x);

    bool isinf(float16_t x);
    bool isinf(float8e4m3_t x); // Always returns false, provided for completeness.
    bool isinf(float8e5m2_t x);
    
    bool isnan(float16_t x);
    bool isnan(float8e4m3_t x);
    bool isnan(float8e5m2_t x);

    bool isfinite(float16_t x);
    bool isfinite(float8e4m3_t x);
    bool isfinite(float8e5m2_t x);

    bool signbit(float16_t x);
    bool signbit(float8e4m3_t x);
    bool signbit(float8e5m2_t x);

    // Returns true if the CPU supports F16C instructions (x64 only)
    // See https://en.wikipedia.org/wiki/F16C
    bool isF16CSupported();
    
    // Enables the use of F16C instructions, if supported (x64 only)
    void enableF16C(bool enable);

    float16_t float32ToFloat16(float x);
    float16_t2 float32ToFloat16x2(float2 x);
    float16_t4 float32ToFloat16x4(float4 x);
    float float16ToFloat32(float16_t x);
    float2 float16ToFloat32x2(float16_t2 x);
    float4 float16ToFloat32x4(float16_t4 x);

    float8e4m3_t float32ToFloat8E4M3(float x);
    float8e4m3_t4 float32ToFloat8E4M3x4(float4 x);
    float float8E4M3ToFloat32(float8e4m3_t x);
    float4 float8E4M3ToFloat32x4(float8e4m3_t4 x);
    
    float8e5m2_t float32ToFloat8E5M2(float x);
    float8e5m2_t4 float32ToFloat8E5M2x4(float4 x);
    float float8E5M2ToFloat32(float8e5m2_t x);
    float4 float8E5M2ToFloat32x4(float8e5m2_t4 x);
}

namespace std
{
    template<> class numeric_limits<caustica::math::float16_t>
    {
    public:
        static constexpr caustica::math::float16_t(min)() noexcept {
            return caustica::math::float16_t{ 0x0400 }; // 6.1035e-5
        }
        static constexpr caustica::math::float16_t(max)() noexcept {
            return caustica::math::float16_t{ 0x7bff }; // 65504.0
        }
        static constexpr caustica::math::float16_t lowest() noexcept {
            return min();
        }
        static constexpr caustica::math::float16_t epsilon() noexcept {
            return caustica::math::float16_t{ 0x1400 }; // f16(0x3c01) - f16(0x3c00)
        }
        static constexpr caustica::math::float16_t round_error() noexcept {
            return caustica::math::float16_t{ 0x3800 }; // 0.5
        }
        static constexpr caustica::math::float16_t denorm_min() noexcept {
            return caustica::math::float16_t{ 0x0001 }; // 5.9604e-8
        }
        static constexpr caustica::math::float16_t infinity() noexcept {
            return caustica::math::float16_t{ 0x7C00 };
        }
        static constexpr caustica::math::float16_t quiet_NaN() noexcept {
            return caustica::math::float16_t{ 0x7FFF };
        }

        static constexpr int digits             = 11;
        static constexpr int max_exponent       = 16;
        static constexpr int min_exponent       = -14;
        static constexpr int radix              = 2;
        static constexpr bool has_infinity      = true;
        static constexpr bool has_quiet_NaN     = true;
        static constexpr bool has_signaling_NaN = false;
        static constexpr bool is_bounded        = true;
        static constexpr bool is_exact          = true;
        static constexpr bool is_iec559         = false;
        static constexpr bool is_integer        = true;
        static constexpr bool is_signed         = true;
        static constexpr bool is_specialized    = true;
        static constexpr float_round_style round_style = round_to_nearest;
    };

    template<> class numeric_limits<caustica::math::float8e4m3_t>
    {
    public:
        static constexpr caustica::math::float8e4m3_t(min)() noexcept {
            return caustica::math::float8e4m3_t{ 0x08 }; // 0.01562
        }
        static constexpr caustica::math::float8e4m3_t(max)() noexcept {
            return caustica::math::float8e4m3_t{ 0x7e }; // 448.0
        }
        static constexpr caustica::math::float8e4m3_t lowest() noexcept {
            return min();
        }
        static constexpr caustica::math::float8e4m3_t epsilon() noexcept {
            return caustica::math::float8e4m3_t{ 0x20 }; // f8(0x39) - f8(0x38)
        }
        static constexpr caustica::math::float8e4m3_t round_error() noexcept {
            return caustica::math::float8e4m3_t{ 0x30 }; // 0.5
        }
        static constexpr caustica::math::float8e4m3_t denorm_min() noexcept {
            return caustica::math::float8e4m3_t{ 0x01 }; // 0.001953	
        }
        static constexpr caustica::math::float8e4m3_t quiet_NaN() noexcept {
            return caustica::math::float8e4m3_t{ 0x7f };
        }

        static constexpr int digits             = 4;
        static constexpr int max_exponent       = 8;
        static constexpr int min_exponent       = -6;
        static constexpr int radix              = 2;
        static constexpr bool has_infinity      = false;
        static constexpr bool has_quiet_NaN     = true;
        static constexpr bool has_signaling_NaN = false;
        static constexpr bool is_bounded        = true;
        static constexpr bool is_exact          = true;
        static constexpr bool is_iec559         = false;
        static constexpr bool is_integer        = true;
        static constexpr bool is_signed         = true;
        static constexpr bool is_specialized    = true;
        static constexpr float_round_style round_style = round_to_nearest;
    };
    
    template<> class numeric_limits<caustica::math::float8e5m2_t>
    {
    public:
        static constexpr caustica::math::float8e5m2_t(min)() noexcept {
            return caustica::math::float8e5m2_t{ 0x04 }; // 0.000061
        }
        static constexpr caustica::math::float8e5m2_t(max)() noexcept {
            return caustica::math::float8e5m2_t{ 0x7b }; // 57344.0
        }
        static constexpr caustica::math::float8e5m2_t lowest() noexcept {
            return min();
        }
        static constexpr caustica::math::float8e5m2_t epsilon() noexcept {
            return caustica::math::float8e5m2_t{ 0x34 }; // f8(0x3d) - f8(0x3c)
        }
        static constexpr caustica::math::float8e5m2_t round_error() noexcept {
            return caustica::math::float8e5m2_t{ 0x38 }; // 0.5
        }
        static constexpr caustica::math::float8e5m2_t denorm_min() noexcept {
            return caustica::math::float8e5m2_t{ 0x01 }; // 0.0000153
        }
        static constexpr caustica::math::float8e5m2_t infinity() noexcept {
            return caustica::math::float8e5m2_t{ 0x7c };
        }
        static constexpr caustica::math::float8e5m2_t quiet_NaN() noexcept {
            return caustica::math::float8e5m2_t{ 0x7f };
        }

        static constexpr int digits             = 3;
        static constexpr int max_exponent       = 16;
        static constexpr int min_exponent       = -14;
        static constexpr int radix              = 2;
        static constexpr bool has_infinity      = true;
        static constexpr bool has_quiet_NaN     = true;
        static constexpr bool has_signaling_NaN = false;
        static constexpr bool is_bounded        = true;
        static constexpr bool is_exact          = true;
        static constexpr bool is_iec559         = false;
        static constexpr bool is_integer        = true;
        static constexpr bool is_signed         = true;
        static constexpr bool is_specialized    = true;
        static constexpr float_round_style round_style = round_to_nearest;
    };
}