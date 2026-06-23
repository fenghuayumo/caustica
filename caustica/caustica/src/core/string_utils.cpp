#include <core/string_utils.h>
#include <cstring>

namespace caustica::string_utils
{
	template <> long sto_number(std::string const& s) 
	{ 
		return std::stol(s, nullptr, 0); 
	}
	
	template <> float sto_number(std::string const& s) 
	{ 
		return std::stof(s); 
	
	}

	template <> double sto_number(std::string const& s)
	{ 
		return std::stod(s); 
	}

	template <> std::optional<bool> from_string(std::string const& s) 
	{ 
		return stob(s); 
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
		return stob(s); 
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

	template <> std::optional<dm::bool2> parse(std::string_view s) { return parse_vector<dm::bool2>(s); }
	template <> std::optional<dm::bool3> parse(std::string_view s) { return parse_vector<dm::bool3>(s); }
	template <> std::optional<dm::bool4> parse(std::string_view s) { return parse_vector<dm::bool4>(s); }

	template <> std::optional<dm::int2> parse(std::string_view s) { return parse_vector<dm::int2>(s); }
	template <> std::optional<dm::int3> parse(std::string_view s) { return parse_vector<dm::int3>(s); }
	template <> std::optional<dm::int4> parse(std::string_view s) { return parse_vector<dm::int4>(s); }

	template <> std::optional<dm::uint2> parse(std::string_view s) { return parse_vector<dm::uint2>(s); }
	template <> std::optional<dm::uint3> parse(std::string_view s) { return parse_vector<dm::uint3>(s); }
	template <> std::optional<dm::uint4> parse(std::string_view s) { return parse_vector<dm::uint4>(s); }

	template <> std::optional<dm::float2> parse(std::string_view s) { return parse_vector<dm::float2>(s); }
	template <> std::optional<dm::float3> parse(std::string_view s) { return parse_vector<dm::float3>(s); }
	template <> std::optional<dm::float4> parse(std::string_view s) { return parse_vector<dm::float4>(s); }

} // end namespace caustica::string_utils

