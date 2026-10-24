#include "build/version.hpp"
#include "inspection/freeze_corpus.hpp"

#include <iostream>
#include <string_view>

int main(const int argc, const char* const* argv)
{
    if (nova::build::IsVersionRequest(argc, argv)) {
        nova::build::PrintVersion(std::cout, "nova-freeze-corpus");
        return 0;
    }
    if (argc != 2 || std::string_view{argv[1]} != "--regtest-testnet") {
        std::cerr << "usage: nova-freeze-corpus --regtest-testnet\n";
        return 2;
    }
    const auto corpus = nova::inspection::RenderFreezeCorpus(
        {nova::build::kSourceRevision, nova::build::kSourceTree, nova::build::kSourceState});
    if (!corpus.has_value()) {
        std::cerr << "refusing to emit a freeze corpus for dirty or unrecorded source provenance\n";
        return 1;
    }
    std::cout << *corpus;
    return 0;
}
