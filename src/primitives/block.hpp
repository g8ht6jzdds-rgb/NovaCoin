#pragma once

#include "primitives/transaction.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace nova::primitives
{

struct BlockHeader final {
    std::int32_t version{};
    crypto::Hash256 previous_block_id{};
    crypto::Hash256 merkle_root{};
    std::uint32_t time{};
    std::uint32_t bits{};
    std::uint32_t nonce{};
};

struct CoinbaseTransaction final {
    Transaction transaction;
};

struct BlockLimits final {
    TransactionLimits transaction_limits;
    std::uint32_t max_serialized_size{};
    std::uint32_t max_transactions{};
    std::uint32_t max_coinbase_script_size{};
};

struct Block final {
    BlockHeader header;
    std::vector<Transaction> transactions;

    [[nodiscard]] bool Serialize(BinaryWriter& writer, const BlockLimits& limits) const noexcept;
    [[nodiscard]] static std::optional<Block> Deserialize(std::span<const std::uint8_t> serialized,
                                                          const BlockLimits& limits) noexcept;
    [[nodiscard]] std::optional<std::uint64_t> SerializedSize() const noexcept;
};

enum class BlockHeaderStructureError : std::uint8_t {
    kNone,
};

enum class BlockStructureError : std::uint8_t {
    kNone,
    kInvalidLimits,
    kInvalidHeader,
    kNoTransactions,
    kTooManyTransactions,
    kFirstTransactionNotCoinbase,
    kMultipleCoinbaseTransactions,
    kInvalidCoinbaseScriptSize,
    kInvalidTransaction,
    kOversizedBlock,
    kDuplicateTransactionId,
    kMerkleComputationFailure,
    kBadMerkleRoot,
    kSerializationFailure,
};

[[nodiscard]] BlockHeaderStructureError
CheckBlockHeaderStructure(const BlockHeader& header) noexcept;
[[nodiscard]] bool IsCoinbaseTransaction(const Transaction& transaction) noexcept;
[[nodiscard]] std::optional<crypto::Hash256> ComputeBlockHash(const BlockHeader& header) noexcept;
[[nodiscard]] std::optional<crypto::Hash256>
ComputeMerkleRoot(std::span<const crypto::Hash256> transaction_ids) noexcept;
[[nodiscard]] std::optional<crypto::Hash256>
ComputeMerkleRoot(std::span<const Transaction> transactions,
                  const TransactionLimits& limits) noexcept;
[[nodiscard]] BlockStructureError CheckBlockStructure(const Block& block,
                                                      const BlockLimits& limits) noexcept;

} // namespace nova::primitives
