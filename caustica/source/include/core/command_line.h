#pragma once

#include <string>
#include <vector>
#include <unordered_map>

namespace caustica
{

// Simple command-line argument parser.
// Supports "--key value", "--key=value", "--flag", "-flag" forms.
class CommandLine
{
public:
    CommandLine() = default;

    // Parse argc/argv into key-value pairs
    void parse(int argc, const char* const* argv);

    // Check if an option exists
    bool hasOption(const std::string& name) const;

    // Get the value for an option (empty string if flag-only or missing)
    const std::string& getOption(const std::string& name) const;

    // Bool check: true if the option exists and its value is "1", "true", "yes", "on", or empty
    bool getOptionBool(const std::string& name) const;

    // Get positional arguments (non-option tokens)
    const std::vector<std::string>& getPositionalArgs() const { return m_Positional; }

    // Number of positional arguments
    size_t positionalCount() const { return m_Positional.size(); }

private:
    std::unordered_map<std::string, std::string> m_Options;
    std::vector<std::string> m_Positional;
};

} // namespace caustica
