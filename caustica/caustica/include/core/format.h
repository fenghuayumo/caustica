#pragma once

#include <math/math.h>
#include <cctype>
#include <charconv>
#include <memory>
#include <string>
#include <stdexcept>

namespace caustica
{

// --- String formatting ---
// printf-style format with automatic buffer sizing.
template<typename... Args>
std::string stringFormat(const std::string& format, Args... args)
{
    int size_s = std::snprintf(nullptr, 0, format.c_str(), args...) + 1;
    if (size_s <= 0)
        throw std::runtime_error("Error during formatting.");
    auto size = static_cast<size_t>(size_s);
    auto buf = std::make_unique<char[]>(size);
    std::snprintf(buf.get(), size, format.c_str(), args...);
    return std::string(buf.get(), buf.get() + size - 1);
}

// --- Hex formatting ---
std::string hexString(unsigned int value);

// --- String cleaning ---
std::string stripNonAsciiAlnum(const std::string& input);

// --- Case-insensitive search ---
// Returns std::string::npos if not found.
size_t findSubStringIgnoreCase(const std::string& text, const std::string& subString);
bool equalsIgnoreCase(const std::string& a, const std::string& b);

// --- Float3 parsing from comma-separated string ---
// Parses up to 3 floats separated by commas. Consumes the parsed portion from 's'.
// Returns true if 3 valid floats were parsed.
bool parseFloat3Consume(std::string& s, dm::float3& out);

} // namespace caustica
