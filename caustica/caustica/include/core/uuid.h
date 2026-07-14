#pragma once

#include <cstdint>
#include <string>
#include <random>
#include <sstream>
#include <iomanip>

namespace caustica
{

// Simple UUID v4 generator (no external dependency).
inline uint64_t generateRandom64()
{
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    static std::uniform_int_distribution<uint64_t> dist;
    return dist(gen);
}

} // namespace caustica
