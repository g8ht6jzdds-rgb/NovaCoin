#include "primitives/block.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <utility>

namespace nova::primitives
{
namespace
{

[[nodiscard]] bool WriteHeader(BinaryWriter& writer, const BlockHeader& header) noexcept
{
    return writer.WriteU32(std::bit_cast<std::uint32_t>(header.version)) &&
           writer.WriteHash256(header.previous_block_id) &&
           writer.WriteHash256(header.merkle_root) && writer.WriteU32(header.time) &&
           writer.WriteU32(header.bits) && writer.WriteU32(header.nonce);
}

[[nodiscard]] std::optional<BlockHeader> ReadHeader(BinaryReader& reader) noexcept
{
    const auto version = reader.ReadU32();
    const auto previous_block_id = reader.ReadHash256();
    const auto merkle_root = reader.ReadHash256();
    const auto time = reader.ReadU32();
    const auto bits = reader.ReadU32();
    const auto nonce = reader.ReadU32();
    if (!version.has_value() || !previous_block_id.has_value() || !merkle_root.has_value() ||
        !time.has_value() || !bits.has_value() || !nonce.has_value()) {
        return std::nullopt;
    }
    return BlockHeader{std::bit_cast<std::int32_t>(*version),
                       *previous_block_id,
                       *merkle_root,
                       *time,
                       *bits,
                       *nonce};
}

[[nodiscard]] std::optional<std::uint64_t> ToUint64(const std::size_t value) noexcept
{
    if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t)) {
        if (value > std::numeric_limits<std::uint64_t>::max()) {
            return std::nullopt;
        }
    }
    return static_cast<std::uint64_t>(value);
}

[[nodiscard]] std::uint64_t CompactSizeLength(const std::uint64_t value) noexcept
{
    if (value < 253U) {
        return 1U;
    }
    if (value <= std::numeric_limits<std::uint16_t>::max()) {
        return 3U;
    }
    if (value <= std::numeric_limits<std::uint32_t>::max()) {
        return 5U;
    }
    return 9U;
}

[[nodiscard]] bool CheckedAdd(std::uint64_t& total, const std::uint64_t addend) noexcept
{
    if (addend > std::numeric_limits<std::uint64_t>::max() - total) {
        return false;
    }
    total += addend;
    return true;
}

} // namespace

bool Block::Serialize(BinaryWriter& writer, const BlockLimits& limits) const noexcept
{
    if (CheckBlockStructure(*this, limits) != BlockStructureError::kNone) {
        return false;
    }
    return WriteHeader(writer, header) &&
           writer.WriteArray<Transaction>(
               transactions, limits.max_transactions,
               [&limits](BinaryWriter& array_writer, const Transaction& transaction) {
                   return transaction.Serialize(array_writer, limits.transaction_limits);
               });
}

std::optional<Block> Block::Deserialize(const std::span<const std::uint8_t> serialized,
                                        const BlockLimits& limits) noexcept
{
    if (limits.max_serialized_size == 0U || serialized.size() > limits.max_serialized_size) {
        return std::nullopt;
    }
    BinaryReader reader{serialized};
    const auto header = ReadHeader(reader);
    auto transactions = reader.ReadArray<Transaction>(
        limits.max_transactions, 60U, [&limits](BinaryReader& array_reader) {
            return Transaction::Deserialize(array_reader, limits.transaction_limits);
        });
    if (!header.has_value() || !transactions.has_value() || !reader.RequireEnd()) {
        return std::nullopt;
    }
    Block block{*header, std::move(*transactions)};
    if (CheckBlockStructure(block, limits) != BlockStructureError::kNone) {
        return std::nullopt;
    }
    return block;
}

std::optional<std::uint64_t> Block::SerializedSize() const noexcept
{
    const auto transaction_count = ToUint64(transactions.size());
    if (!transaction_count.has_value()) {
        return std::nullopt;
    }
    std::uint64_t size = 80U;
    if (!CheckedAdd(size, CompactSizeLength(*transaction_count))) {
        return std::nullopt;
    }
    for (const auto& transaction : transactions) {
        const auto transaction_size = transaction.SerializedSize();
        if (!transaction_size.has_value() || !CheckedAdd(size, *transaction_size)) {
            return std::nullopt;
        }
    }
    return size;
}

BlockHeaderStructureError CheckBlockHeaderStructure(const BlockHeader& header) noexcept
{
    static_cast<void>(header);
    // Every field has a fixed-width canonical encoding. Version, time, target,
    // and linkage policies are separate consensus rules and intentionally absent.
    return BlockHeaderStructureError::kNone;
}

bool IsCoinbaseTransaction(const Transaction& transaction) noexcept
{
    return transaction.inputs.size() == 1U && transaction.inputs.front().previous_output.IsNull();
}

std::optional<crypto::Hash256> ComputeBlockHash(const BlockHeader& header) noexcept
{
    BinaryWriter writer;
    if (!WriteHeader(writer, header)) {
        return std::nullopt;
    }
    return crypto::Hash256::DoubleSha256(writer.bytes());
}

std::optional<crypto::Hash256>
ComputeMerkleRoot(const std::span<const crypto::Hash256> transaction_ids) noexcept
{
    if (transaction_ids.empty()) {
        return std::nullopt;
    }
    try {
        std::vector<crypto::Hash256> level{transaction_ids.begin(), transaction_ids.end()};
        while (level.size() > 1U) {
            if (level.size() % 2U != 0U) {
                level.push_back(level.back());
            }
            std::vector<crypto::Hash256> next_level;
            next_level.reserve(level.size() / 2U);
            for (std::size_t index = 0; index < level.size(); index += 2U) {
                std::array<std::uint8_t, 64> preimage{};
                std::copy(level.at(index).bytes().begin(), level.at(index).bytes().end(),
                          preimage.begin());
                std::copy(level.at(index + 1U).bytes().begin(), level.at(index + 1U).bytes().end(),
                          preimage.begin() + 32U);
                const auto parent = crypto::Hash256::DoubleSha256(preimage);
                if (!parent.has_value()) {
                    return std::nullopt;
                }
                next_level.push_back(*parent);
            }
            level = std::move(next_level);
        }
        return level.front();
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<crypto::Hash256> ComputeMerkleRoot(const std::span<const Transaction> transactions,
                                                 const TransactionLimits& limits) noexcept
{
    if (transactions.empty()) {
        return std::nullopt;
    }
    try {
        std::vector<crypto::Hash256> transaction_ids;
        transaction_ids.reserve(transactions.size());
        for (const auto& transaction : transactions) {
            const auto transaction_id = transaction.TxId(limits);
            if (!transaction_id.has_value()) {
                return std::nullopt;
            }
            transaction_ids.push_back(*transaction_id);
        }
        return ComputeMerkleRoot(transaction_ids);
    } catch (...) {
        return std::nullopt;
    }
}

BlockStructureError CheckBlockStructure(const Block& block, const BlockLimits& limits) noexcept
{
    if (limits.max_serialized_size == 0U || limits.max_coinbase_script_size < 2U) {
        return BlockStructureError::kInvalidLimits;
    }
    if (CheckBlockHeaderStructure(block.header) != BlockHeaderStructureError::kNone) {
        return BlockStructureError::kInvalidHeader;
    }
    if (block.transactions.empty()) {
        return BlockStructureError::kNoTransactions;
    }
    if (block.transactions.size() > limits.max_transactions) {
        return BlockStructureError::kTooManyTransactions;
    }
    if (!IsCoinbaseTransaction(block.transactions.front())) {
        return BlockStructureError::kFirstTransactionNotCoinbase;
    }
    const auto coinbase_script_size = block.transactions.front().inputs.front().script_sig.size();
    if (coinbase_script_size < 2U || coinbase_script_size > limits.max_coinbase_script_size) {
        return BlockStructureError::kInvalidCoinbaseScriptSize;
    }

    for (std::size_t left = 0; left < block.transactions.size(); ++left) {
        const auto& transaction = block.transactions.at(left);
        if (CheckTransactionStructure(transaction, limits.transaction_limits) !=
            TransactionStructureError::kNone) {
            return BlockStructureError::kInvalidTransaction;
        }
        if (left != 0U && IsCoinbaseTransaction(transaction)) {
            return BlockStructureError::kMultipleCoinbaseTransactions;
        }
        const auto transaction_id = transaction.TxId(limits.transaction_limits);
        if (!transaction_id.has_value()) {
            return BlockStructureError::kSerializationFailure;
        }
        for (std::size_t right = left + 1U; right < block.transactions.size(); ++right) {
            const auto other_id = block.transactions.at(right).TxId(limits.transaction_limits);
            if (!other_id.has_value()) {
                return BlockStructureError::kSerializationFailure;
            }
            if (*transaction_id == *other_id) {
                return BlockStructureError::kDuplicateTransactionId;
            }
        }
    }

    const auto serialized_size = block.SerializedSize();
    if (!serialized_size.has_value()) {
        return BlockStructureError::kSerializationFailure;
    }
    if (*serialized_size > limits.max_serialized_size) {
        return BlockStructureError::kOversizedBlock;
    }
    const auto merkle_root = ComputeMerkleRoot(block.transactions, limits.transaction_limits);
    if (!merkle_root.has_value()) {
        return BlockStructureError::kMerkleComputationFailure;
    }
    if (*merkle_root != block.header.merkle_root) {
        return BlockStructureError::kBadMerkleRoot;
    }
    return BlockStructureError::kNone;
}

} // namespace nova::primitives
