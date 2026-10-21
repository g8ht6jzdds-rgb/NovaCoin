#pragma once

#include "build/version_data.hpp"

#include <ostream>
#include <string_view>

namespace nova::build
{

inline bool IsVersionRequest(const int argc, const char* const* argv)
{
    return argc == 2 && std::string_view{argv[1]} == "--version";
}

inline void PrintVersion(std::ostream& output, const std::string_view executable)
{
    output << executable << ' ' << kApplicationVersion << " revision=" << kSourceRevision
           << " tree=" << kSourceTree << " state=" << kSourceState << '\n';
}

} // namespace nova::build
