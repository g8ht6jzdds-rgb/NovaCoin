#pragma once

#include "chain/utxo.hpp"
#include "consensus/monetary.hpp"
#include "consensus/pow.hpp"
#include "primitives/block.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace nova::chain
{

struct BlockValidationParams final {
    primitives::BlockLimits block_limits;
    consensus::PowParameters pow_parameters;
    consensus::ChainParams chain_parameters;
    std::uint32_t coinbase_maturity{};
    std::uint32_t locktime_threshold{};
    std::uint32_t max_sequence{};
    std::uint32_t sighash_all{};
    std::uint32_t max_future_block_time_seconds{};
};

struct BlockValidationContext final {
    crypto::Hash256 parent_block_id;
    std::uint32_t height{};
    std::uint32_t median_time_past{};
    std::uint32_t validation_time{};
};

struct TransactionValidationContext final {
    std::uint32_t height{};
    std::uint32_t median_time_past{};
};

enum class BlockValidationError : std::uint8_t {
    kNone,
    kInvalidParameters,
    kInvalidContext,
    kInvalidHeader,
    kPreviousBlockMismatch,
    kUnexpectedBits,
    kInvalidTarget,
    kBlockHashFailure,
    kBadProofOfWork,
    kTimeTooOld,
    kTimeTooNew,
    kNoTransactions,
    kTooManyTransactions,
    kFirstTransactionNotCoinbase,
    kMultipleCoinbaseTransactions,
    kInvalidCoinbaseScriptSize,
    kInvalidTransactionStructure,
    kOversizedBlock,
    kDuplicateTransactionId,
    kMerkleFailure,
    kBadMerkleRoot,
    kInvalidCoinbaseHeight,
    kNonFinalTransaction,
    kMissingUtxo,
    kDoubleSpend,
    kImmatureCoinbase,
    kInvalidInputAmount,
    kInputAmountOverflow,
    kInvalidOutputAmount,
    kInputValueTooLow,
    kFeeOverflow,
    kUnsupportedLockingScript,
    kMalformedUnlockingScript,
    kInvalidPublicKey,
    kInvalidSignature,
    kSignatureHashFailure,
    kInvalidCoinbaseReward,
    kStateApplicationFailure,
    kAllocationFailure,
    kDisconnectFailure,
};

struct ConnectedBlock final {
    BlockUndo undo;
    crypto::Hash256 block_id;
    primitives::Amount total_fees{};
    std::uint32_t height{};
};

struct BlockValidationResult final {
    BlockValidationError error{BlockValidationError::kNone};
    std::optional<ConnectedBlock> connected;
};

struct TransactionValidationResult final {
    BlockValidationError error{BlockValidationError::kNone};
    primitives::Amount fee{};
};

[[nodiscard]] BlockValidationError
CheckBlockHeader(const primitives::BlockHeader& header, const BlockValidationContext& context,
                 const BlockValidationParams& parameters) noexcept;
[[nodiscard]] BlockValidationError CheckBlock(const primitives::Block& block,
                                              const BlockValidationContext& context,
                                              const BlockValidationParams& parameters) noexcept;
[[nodiscard]] std::optional<crypto::Hash256>
ComputeSignatureHash(const primitives::Transaction& transaction, std::size_t input_index,
                     std::span<const std::uint8_t> script_pubkey,
                     const primitives::TransactionLimits& limits,
                     std::uint32_t sighash_all) noexcept;
[[nodiscard]] TransactionValidationResult ValidateTransactionAgainstUTXOSet(
    const primitives::Transaction& transaction, const TransactionValidationContext& context,
    const BlockValidationParams& parameters, const UTXOView& utxos) noexcept;
[[nodiscard]] BlockValidationResult ConnectBlock(const primitives::Block& block,
                                                 const BlockValidationContext& context,
                                                 const BlockValidationParams& parameters,
                                                 UTXOSet& utxos) noexcept;
[[nodiscard]] BlockValidationError DisconnectBlock(const ConnectedBlock& connected_block,
                                                   UTXOSet& utxos) noexcept;

} // namespace nova::chain
