#pragma once

#include "chain/mempool.hpp"
#include <iosfwd>

namespace nova::chain
{
// Local recovery format only. Parsing proves bounded canonical representation,
// not transaction validity or membership in the current mempool.
struct MempoolImageRecord final {
    primitives::Transaction transaction;
    std::uint64_t time_received{};
};
// Compute the maximum encoded file length before allocating or reading it.
[[nodiscard]] std::optional<std::uint64_t>
MempoolImageMaxSize(const MempoolParams& parameters) noexcept;
// Reads a finite stream without allocating beyond the configured image bound.
// Returns only complete, canonical images; callers must still validate replay.
[[nodiscard]] std::optional<std::vector<std::uint8_t>>
ReadMempoolImage(std::istream& input, const MempoolParams& parameters) noexcept;
[[nodiscard]] std::optional<std::vector<std::uint8_t>>
EncodeMempoolImage(std::span<const MempoolImageRecord> records,
                   const MempoolParams& parameters) noexcept;
[[nodiscard]] std::optional<std::vector<std::uint8_t>>
EncodePreparedMempoolImage(const Mempool& pool, const Mempool::PreparedState& prepared,
                           const MempoolParams& parameters) noexcept;
[[nodiscard]] std::optional<std::vector<MempoolImageRecord>>
DecodeMempoolImage(std::span<const std::uint8_t> bytes, const MempoolParams& parameters) noexcept;
// Revalidates every record in canonical dependency order into a new pool.
// On any rejection the private pool is discarded and the UTXO view is unchanged.
[[nodiscard]] std::unique_ptr<Mempool>
ReplayMempoolImage(std::span<const std::uint8_t> bytes, const MempoolParams& parameters,
                   const TransactionValidationContext& context, const UTXOView& utxos) noexcept;
} // namespace nova::chain
