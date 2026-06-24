#include <core/format.h>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <format>
#include <string>

namespace caustica
{

std::string HexString(unsigned int value)
{
    return std::format("{:08x}", value);
}

std::string StripNonAsciiAlnum(const std::string& input)
{
    std::string result;
    result.reserve(input.size());
    std::copy_if(input.begin(), input.end(), std::back_inserter(result),
        [](unsigned char c) {
            return std::isalnum(c) && c < 128;
        });
    return result;
}

size_t FindSubStringIgnoreCase(const std::string& text, const std::string& subString)
{
    auto toLower = [](char ch) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    };

    if (subString.empty() || text.size() < subString.size())
        return std::string::npos;

    for (size_t i = 0; i <= text.size() - subString.size(); ++i)
    {
        bool match = true;
        for (size_t j = 0; j < subString.size(); ++j)
        {
            if (toLower(text[i + j]) != toLower(subString[j]))
            {
                match = false;
                break;
            }
        }
        if (match)
            return i;
    }
    return std::string::npos;
}

bool EqualsIgnoreCase(const std::string& a, const std::string& b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
    {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

namespace
{
    inline void SkipWs(const char*& p, const char* e)
    {
        while (p < e && std::isspace(static_cast<unsigned char>(*p)))
            ++p;
    }
}

bool ParseFloat3Consume(std::string& s, dm::float3& out)
{
    const char* begin = s.data();
    const char* p = begin;
    const char* e = begin + s.size();

    for (int i = 0; i < 3; ++i)
    {
        SkipWs(p, e);
        if (p == e)
            return false;

        float v = 0.0f;
        auto r = std::from_chars(p, e, v, std::chars_format::general);
        if (r.ec != std::errc{})
            return false;

        p = r.ptr;
        SkipWs(p, e);

        if (i < 2)
        {
            if (p == e || *p != ',')
                return false;
            ++p;
        }

        out[i] = v;
    }

    SkipWs(p, e);
    if (p < e && *p == ',')
        ++p;
    SkipWs(p, e);

    s.assign(p, static_cast<size_t>(e - p));
    return true;
}

} // namespace caustica
