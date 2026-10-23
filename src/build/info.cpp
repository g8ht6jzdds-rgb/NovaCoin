#include "build/metadata.hpp"

#include <fstream>
#include <iostream>

int main(const int argc, const char* const* argv)
{
    if (nova::build::IsVersionRequest(argc, argv)) {
        nova::build::PrintVersion(std::cout, "nova-build-info");
        return 0;
    }
    if (argc == 1) {
        nova::build::PrintBuildMetadata(std::cout);
        return std::cout.good() ? 0 : 1;
    }
    if (argc != 2 || argv[1][0] == '-') {
        std::cerr << "Usage: nova-build-info [output.json|--version]\n";
        return 2;
    }
    std::ofstream output{argv[1], std::ios::binary | std::ios::trunc};
    if (!output) {
        std::cerr << "Cannot open metadata output\n";
        return 1;
    }
    nova::build::PrintBuildMetadata(output);
    output.close();
    return output.fail() ? 1 : 0;
}
