#include "consensus/network_params.hpp"
#include "storage/block_journal.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size > 4096U)
        return 0;
    const std::string bytes =
        size == 0U ? std::string{} : std::string(reinterpret_cast<const char*>(data), size);
    std::istringstream first{bytes};
    std::istringstream second{bytes};
    const auto& limits = nova::consensus::RegtestNetworkParams().block_limits;
    constexpr nova::storage::JournalReadBudget budget{4096U, 32U, 16U, 16U};
    const auto a = nova::storage::ReadBlockJournal(first, limits, budget);
    const auto b = nova::storage::ReadBlockJournal(second, limits, budget);
    if (a.error != b.error || a.recovery_required != b.recovery_required ||
        a.blocks.size() != b.blocks.size() || a.blocks.size() > budget.blocks ||
        a.bytes_read > budget.bytes || a.record_count > budget.records)
        std::abort();
    for (std::size_t i = 0U; i < a.blocks.size(); ++i) {
        nova::primitives::BinaryWriter left;
        nova::primitives::BinaryWriter right;
        if (!a.blocks[i].Serialize(left, limits) || !b.blocks[i].Serialize(right, limits) ||
            !std::equal(left.bytes().begin(), left.bytes().end(), right.bytes().begin(),
                        right.bytes().end()))
            std::abort();
    }
    return 0;
}
