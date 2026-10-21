#include "build/version.hpp"

#include <iostream>

int main(const int argc, char* argv[])
{
    if (nova::build::IsVersionRequest(argc, argv)) {
        nova::build::PrintVersion(std::cout, "novacoin-cli");
    }
    return 0;
}
