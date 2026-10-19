#pragma once

#include "chain/network_chainstate.hpp"
#include "storage/block_journal.hpp"

#include <string>
#include <string_view>

namespace nova::inspection
{
struct InspectionLimits final {
    storage::JournalReadBudget journal{64U * 1024U * 1024U, 100'000U, 10'000U, 10'000U};
    std::size_t keys{1'000'000U};
    std::size_t output{256U * 1024U * 1024U};
};

struct InspectionContext final {
    consensus::NetworkId network{consensus::NetworkId::kRegtest};
    std::uint32_t as_of{};
    crypto::Hash256 expected_tip;
    std::uint32_t expected_height{};
    chain::ChainWork expected_work;
    std::string source_revision;
    std::string provenance_hash;
    std::string identity_hash;
    std::string journal_hash;
    std::string metadata_hash;
    std::string binary_hash;
    std::size_t identity_bytes{45U};
    std::size_t metadata_bytes{};
    std::string source_tree = std::string(40U, '0');
};

struct InspectionResult final {
    std::string error{"incomplete"};
    std::string report{};
};

[[nodiscard]] std::string Hex(std::span<const std::uint8_t> bytes);
[[nodiscard]] bool CheckedAdd(std::int64_t& total, std::int64_t delta) noexcept;
// Caller supplies immutable bytes and externally captured expectations/time.
// This function has no filesystem, wallet, network, repair or activation path.
[[nodiscard]] InspectionResult InspectJournal(std::istream& journal,
                                              const InspectionContext& context,
                                              InspectionLimits limits = {}) noexcept;
} // namespace nova::inspection
