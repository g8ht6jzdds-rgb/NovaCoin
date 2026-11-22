// Test-only child; no installation or package-publication rule is added.
#include "storage/generation_store.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

int main(const int argc, char** argv)
{
    if (argc != 3)
        return 10;
    try {
        const std::string_view mode{argv[1]};
        const std::filesystem::path fixture{argv[2]};
        if (!fixture.is_absolute() ||
            fixture.filename().string().find("novacoin-generation-crash-") != 0U ||
            !std::filesystem::is_regular_file(
                std::filesystem::symlink_status(fixture / "OWNED-TEST-FIXTURE")))
            return 11;
        std::ifstream marker{fixture / "OWNED-TEST-FIXTURE", std::ios::binary};
        constexpr std::string_view expected_marker{"NOVA-GENERATION-CRASH-FIXTURE\n"};
        std::array<char, expected_marker.size()> ownership{};
        marker.read(ownership.data(), static_cast<std::streamsize>(ownership.size()));
        if (marker.gcount() != static_cast<std::streamsize>(ownership.size()) ||
            std::string_view{ownership.data(), ownership.size()} != expected_marker ||
            marker.peek() != EOF)
            return 11;
        using Point = nova::storage::GenerationFailurePoint;
        constexpr std::array points{
            std::pair{"normal", Point::kNone},
            std::pair{"journal", Point::kAfterJournal},
            std::pair{"wallet", Point::kAfterWallet},
            std::pair{"descriptor", Point::kAfterDescriptor},
            std::pair{"before-root", Point::kBeforeRootReplace},
            std::pair{"after-root", Point::kAfterRootReplace},
            std::pair{"barrier", Point::kDirectoryBarrier},
        };
        Point point{};
        bool found = mode == "inspect";
        for (const auto& [name, value] : points) {
            if (mode == name) {
                point = value;
                found = true;
            }
        }
        if (!found)
            return 10;
        nova::storage::GenerationError error{};
        auto store = nova::storage::GenerationStore::Open(
            fixture / "store", nova::consensus::RegtestNetworkParams(), error);
        if (!store)
            return 12;
        if (mode == "inspect") {
            const auto observed = store->Observe();
            if (observed.state == nova::storage::GenerationState::kUnavailable)
                return 13;
            std::cout << observed.snapshot.sequence << '\n';
            return 0;
        }
        store->FailNextForTesting(point);
        auto prepared = store->Prepare(fixture / "input.journal", fixture / "input.wallet", error);
        if (!prepared) {
            if (point == Point::kAfterJournal || point == Point::kAfterWallet ||
                point == Point::kAfterDescriptor)
                std::_Exit(42);
            return 14;
        }
        const auto result = store->Commit(*prepared);
        if (point == Point::kNone &&
            result.state == nova::storage::GenerationCommitState::kCommitted)
            std::_Exit(0);
        if ((point == Point::kBeforeRootReplace &&
             result.state == nova::storage::GenerationCommitState::kNotCommitted) ||
            ((point == Point::kAfterRootReplace || point == Point::kDirectoryBarrier) &&
             result.state == nova::storage::GenerationCommitState::kUnavailable))
            std::_Exit(42);
        return 15;
    } catch (...) {
        return 16;
    }
}
