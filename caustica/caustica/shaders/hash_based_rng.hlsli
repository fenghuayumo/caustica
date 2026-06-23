#ifndef HASH_BASED_RNG_HLSLI
#define HASH_BASED_RNG_HLSLI

#if __SLANG__
#define _RNG_SLANG_MUTATING [mutating]
#else
#define _RNG_SLANG_MUTATING
#endif

// This struct provides a simple white-noise-like random number generator.
// For screen-space use, initialize a HashBasedRNG object using Create2D(pixelPosition.xy, 0).
// Then call NextUint() or NextFloat*() to obtain random numbers.
struct HashBasedRNG
{
// public:
    // Initializes an RNG object using a 1D index and offset.
    // The offset can be used to create uncorrelated random sequences in various passes inside one frame.
    static HashBasedRNG Create(uint linearIndex, uint offset)
    {
        HashBasedRNG rng;
        rng.m_index = 1;
        rng.m_seed = JenkinsHash(linearIndex) + offset;
        return rng;
    }

    // Initializes an RNG object using a 2D pixel position and offset.
    static HashBasedRNG Create2D(uint2 pixelPosition, uint offset)
    {
        return Create(pixelPosition.x + (pixelPosition.y << 16), offset);
    }

    // Returns a random 32-bit unsigned integer.
    _RNG_SLANG_MUTATING uint NextUint()
    {
        uint v = Murmur3Hash(m_seed, m_index);
        ++m_index;
        return v;
    }

    // Returns a random float from 0 (inclusive) to 1 (non-inclusive).
    _RNG_SLANG_MUTATING float NextFloat()
    {
        uint v = Murmur3Hash(m_seed, m_index);
        ++m_index;
        const uint one = asuint(1.f);
        const uint mask = (1 << 23) - 1;
        return asfloat((mask & v) | one) - 1.f;
    }

    // Returns a random 2D vector of [0, 1) floats.
    _RNG_SLANG_MUTATING float2 NextFloat2()
    {
        return float2(NextFloat(), NextFloat());
    }

    // Returns a random 3D vector of [0, 1) floats.
    _RNG_SLANG_MUTATING float3 NextFloat3()
    {
        return float3(NextFloat(), NextFloat(), NextFloat());
    }

    // Returns a random 4D vector of [0, 1) floats.
    _RNG_SLANG_MUTATING float4 NextFloat4()
    {
        return float4(NextFloat(), NextFloat(), NextFloat(), NextFloat());
    }

    // Returns a random 4D vector of [0, 1) floats with lower bit depth than NextFloat4()
    // using just one hash evaluation for all 4 results for performance.
    _RNG_SLANG_MUTATING float4 Next4LowPrecisionFloats()
    {
        uint v = NextUint();
        float4 result;
        [unroll]
        for (int i = 0; i < 4; ++i)
        {
            result[i] = float(v & 0xff) / 256.f;
            v = v >> 8;
        }
        return result;
    }

// private:
    uint m_seed;
    uint m_index;

    static uint JenkinsHash(uint a)
    {
        // http://burtleburtle.net/bob/hash/integer.html
        a = (a + 0x7ed55d16) + (a << 12);
        a = (a ^ 0xc761c23c) ^ (a >> 19);
        a = (a + 0x165667b1) + (a << 5);
        a = (a + 0xd3a2646c) ^ (a << 9);
        a = (a + 0xfd7046c5) + (a << 3);
        a = (a ^ 0xb55a4f09) ^ (a >> 16);
        return a;
    }

    static uint Murmur3Hash(uint hash, uint k)
    {
#define _RNG_ROT32(x, y) ((x << y) | (x >> (32 - y)))

        // https://en.wikipedia.org/wiki/MurmurHash
        uint c1 = 0xcc9e2d51;
        uint c2 = 0x1b873593;
        uint r1 = 15;
        uint r2 = 13;
        uint m = 5;
        uint n = 0xe6546b64;

        k *= c1;
        k = _RNG_ROT32(k, r1);
        k *= c2;

        hash ^= k;
        hash = _RNG_ROT32(hash, r2) * m + n;

        hash ^= 4;
        hash ^= (hash >> 16);
        hash *= 0x85ebca6b;
        hash ^= (hash >> 13);
        hash *= 0xc2b2ae35;
        hash ^= (hash >> 16);

#undef _RNG_ROT32

        return hash;
    }
};

#undef _RNG_SLANG_MUTATING

#endif