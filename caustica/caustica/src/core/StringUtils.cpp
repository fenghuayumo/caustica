#include <core/StringUtils.h>
#include <cstring>

namespace caustica::string_utils
{
	template <> long stringToNumber(std::string const& s)
	{
		return std::stol(s, nullptr, 0);
	}

	template <> float stringToNumber(std::string const& s)
	{
		return std::stof(s);

	}

	template <> double stringToNumber(std::string const& s)
	{
		return std::stod(s);
	}

	template <> std::optional<bool> fromString(std::string const& s)
	{
		return toBool(s);
	}

	template <> std::optional<float> parse(std::string_view s)
	{
		trim(s);
		trim(s, '+');

		char buf[32];
		buf[sizeof(buf) - 1] = 0;
		strncpy(buf, s.data(), std::min(s.size(), sizeof(buf) - 1));
		char* endptr = buf;
		float value = strtof(buf, &endptr);

		if (endptr == buf)
			return std::optional<float>();

		return value;
	}

	template <> std::optional<double> parse(std::string_view s)
	{
		trim(s);
		trim(s, '+');

		char buf[32];
		buf[sizeof(buf) - 1] = 0;
		strncpy(buf, s.data(), std::min(s.size(), sizeof(buf) - 1));
		char* endptr = buf;
		double value = strtod(buf, &endptr);

		if (endptr == buf)
			return std::optional<double>();

		return value;
	}

	template <> std::optional<bool> parse<bool>(std::string_view s)
	{
		return toBool(s);
	}

	template <> std::optional<std::string_view> parse<std::string_view>(std::string_view s)
	{
		trim(s);
		trim(s, '"');
		return s;
	}

	template <> std::optional<std::string> parse<std::string>(std::string_view s)
	{
		if (auto r = parse<std::string_view>(s))
			return std::string(*r);
		return std::nullopt;
	}

	template <> std::optional<math::bool2> parse(std::string_view s) { return parseVector<math::bool2>(s); }
	template <> std::optional<math::bool3> parse(std::string_view s) { return parseVector<math::bool3>(s); }
	template <> std::optional<math::bool4> parse(std::string_view s) { return parseVector<math::bool4>(s); }

	template <> std::optional<math::int2> parse(std::string_view s) { return parseVector<math::int2>(s); }
	template <> std::optional<math::int3> parse(std::string_view s) { return parseVector<math::int3>(s); }
	template <> std::optional<math::int4> parse(std::string_view s) { return parseVector<math::int4>(s); }

	template <> std::optional<math::uint2> parse(std::string_view s) { return parseVector<math::uint2>(s); }
	template <> std::optional<math::uint3> parse(std::string_view s) { return parseVector<math::uint3>(s); }
	template <> std::optional<math::uint4> parse(std::string_view s) { return parseVector<math::uint4>(s); }

	template <> std::optional<math::float2> parse(std::string_view s) { return parseVector<math::float2>(s); }
	template <> std::optional<math::float3> parse(std::string_view s) { return parseVector<math::float3>(s); }
	template <> std::optional<math::float4> parse(std::string_view s) { return parseVector<math::float4>(s); }

} // end namespace caustica::string_utils

