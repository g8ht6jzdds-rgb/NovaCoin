#include "chain/validation.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <set>
#include <utility>
#include <vector>

namespace nova::chain
{
namespace
{

[[nodiscard]] BlockValidationError CheckParameters(const BlockValidationParams& parameters) noexcept
{
    if (consensus::CheckChainParams(parameters.chain_parameters) !=
            consensus::ChainParamsError::kNone ||
        parameters.block_limits.transaction_limits.max_money !=
            parameters.chain_parameters.max_money ||
        parameters.sighash_all != 1U) {
        return BlockValidationError::kInvalidParameters;
    }
    const auto target = consensus::TargetFromCompact(parameters.pow_parameters.pow_limit_compact,
                                                     parameters.pow_parameters);
    if (target.error != consensus::TargetError::kNone || !target.target.has_value() ||
        *target.target != parameters.pow_parameters.pow_limit) {
        return BlockValidationError::kInvalidParameters;
    }
    return BlockValidationError::kNone;
}

[[nodiscard]] BlockValidationError
MapBlockStructureError(const primitives::BlockStructureError error) noexcept
{
    switch (error) {
    case primitives::BlockStructureError::kNone:
        return BlockValidationError::kNone;
    case primitives::BlockStructureError::kNoTransactions:
        return BlockValidationError::kNoTransactions;
    case primitives::BlockStructureError::kTooManyTransactions:
        return BlockValidationError::kTooManyTransactions;
    case primitives::BlockStructureError::kFirstTransactionNotCoinbase:
        return BlockValidationError::kFirstTransactionNotCoinbase;
    case primitives::BlockStructureError::kMultipleCoinbaseTransactions:
        return BlockValidationError::kMultipleCoinbaseTransactions;
    case primitives::BlockStructureError::kInvalidCoinbaseScriptSize:
        return BlockValidationError::kInvalidCoinbaseScriptSize;
    case primitives::BlockStructureError::kInvalidTransaction:
        return BlockValidationError::kInvalidTransactionStructure;
    case primitives::BlockStructureError::kOversizedBlock:
        return BlockValidationError::kOversizedBlock;
    case primitives::BlockStructureError::kDuplicateTransactionId:
        return BlockValidationError::kDuplicateTransactionId;
    case primitives::BlockStructureError::kMerkleComputationFailure:
    case primitives::BlockStructureError::kSerializationFailure:
        return BlockValidationError::kMerkleFailure;
    case primitives::BlockStructureError::kBadMerkleRoot:
        return BlockValidationError::kBadMerkleRoot;
    case primitives::BlockStructureError::kInvalidHeader:
    case primitives::BlockStructureError::kInvalidLimits:
        return BlockValidationError::kInvalidParameters;
    }
    return BlockValidationError::kInvalidParameters;
}

[[nodiscard]] bool IsFinal(const primitives::Transaction& transaction,
                           const TransactionValidationContext& context,
                           const BlockValidationParams& parameters) noexcept
{
    if (transaction.lock_time == 0U) {
        return true;
    }
    const bool all_final = std::all_of(
        transaction.inputs.begin(), transaction.inputs.end(),
        [&parameters](const auto& input) { return input.sequence == parameters.max_sequence; });
    if (all_final) {
        return true;
    }
    const auto comparison = transaction.lock_time < parameters.locktime_threshold
                                ? context.height
                                : context.median_time_past;
    return transaction.lock_time < comparison;
}

[[nodiscard]] bool HasMinimalCoinbaseHeight(const primitives::Transaction& coinbase,
                                            const std::uint32_t height) noexcept
{
    if (coinbase.inputs.size() != 1U) {
        return false;
    }
    const auto& script = coinbase.inputs.front().script_sig;
    if (script.empty()) {
        return false;
    }
    if (height == 0U) {
        return script.front() == 0U;
    }

    std::array<std::uint8_t, 5> encoded{};
    std::size_t encoded_size = 0U;
    auto value = height;
    while (value != 0U) {
        encoded.at(encoded_size++) = static_cast<std::uint8_t>(value & 0xFFU);
        value >>= 8U;
    }
    if ((encoded.at(encoded_size - 1U) & 0x80U) != 0U) {
        encoded.at(encoded_size++) = 0U;
    }
    if (script.size() < encoded_size + 1U ||
        script.front() != static_cast<std::uint8_t>(encoded_size)) {
        return false;
    }
    return std::equal(encoded.begin(), encoded.begin() + static_cast<std::ptrdiff_t>(encoded_size),
                      script.begin() + 1);
}

[[nodiscard]] std::optional<crypto::Hash160>
ParseP2pkhLockingScript(const std::span<const std::uint8_t> script_pubkey) noexcept
{
    if (script_pubkey.size() != 25U || script_pubkey[0] != 0x76U || script_pubkey[1] != 0xA9U ||
        script_pubkey[2] != 0x14U || script_pubkey[23] != 0x88U || script_pubkey[24] != 0xACU) {
        return std::nullopt;
    }
    crypto::Hash160 key_hash{};
    std::copy(script_pubkey.begin() + 3, script_pubkey.begin() + 23, key_hash.begin());
    return key_hash;
}

[[nodiscard]] BlockValidationError
CheckInputAuthorization(const primitives::Transaction& transaction, const std::size_t input_index,
                        const primitives::TxOutput& spent_output,
                        const BlockValidationParams& parameters) noexcept
{
    const auto expected_key_hash = ParseP2pkhLockingScript(spent_output.script_pubkey);
    if (!expected_key_hash.has_value()) {
        return BlockValidationError::kUnsupportedLockingScript;
    }
    const auto& unlocking_script = transaction.inputs.at(input_index).script_sig;
    if (unlocking_script.size() < 44U || unlocking_script.front() < 9U ||
        unlocking_script.front() > 73U) {
        return BlockValidationError::kMalformedUnlockingScript;
    }
    const auto pushed_signature_size = static_cast<std::size_t>(unlocking_script.front());
    if (unlocking_script.size() != pushed_signature_size + 35U ||
        unlocking_script.at(pushed_signature_size + 1U) != 0x21U) {
        return BlockValidationError::kMalformedUnlockingScript;
    }
    const auto der_size = pushed_signature_size - 1U;
    if (unlocking_script.at(der_size + 1U) != parameters.sighash_all) {
        return BlockValidationError::kMalformedUnlockingScript;
    }
    const auto signature = crypto::Signature::FromDer(
        std::span<const std::uint8_t>{unlocking_script}.subspan(1U, der_size));
    if (!signature.has_value()) {
        return BlockValidationError::kMalformedUnlockingScript;
    }
    const auto public_key = crypto::PublicKey::FromCompressed(
        std::span<const std::uint8_t>{unlocking_script}.last(33U));
    if (!public_key.has_value()) {
        return BlockValidationError::kInvalidPublicKey;
    }
    const auto actual_key_hash = crypto::Hash160Digest(public_key->SerializeCompressed());
    if (!actual_key_hash.has_value() || *actual_key_hash != *expected_key_hash) {
        return BlockValidationError::kInvalidPublicKey;
    }
    const auto digest =
        ComputeSignatureHash(transaction, input_index, spent_output.script_pubkey,
                             parameters.block_limits.transaction_limits, parameters.sighash_all);
    if (!digest.has_value()) {
        return BlockValidationError::kSignatureHashFailure;
    }
    return public_key->Verify(*digest, *signature) ? BlockValidationError::kNone
                                                   : BlockValidationError::kInvalidSignature;
}

[[nodiscard]] TransactionValidationResult
CheckTransactionAgainstUtxo(const primitives::Transaction& transaction,
                            const TransactionValidationContext& context,
                            const BlockValidationParams& parameters, const UTXOView& utxos,
                            std::set<UTXOKey>& spent_in_block)
{
    if (primitives::CheckTransactionStructure(transaction,
                                              parameters.block_limits.transaction_limits) !=
        primitives::TransactionStructureError::kNone) {
        return {BlockValidationError::kInvalidTransactionStructure, 0};
    }
    if (!IsFinal(transaction, context, parameters)) {
        return {BlockValidationError::kNonFinalTransaction, 0};
    }

    primitives::Amount input_total = 0;
    for (std::size_t index = 0U; index < transaction.inputs.size(); ++index) {
        const auto& input = transaction.inputs.at(index);
        if (input.previous_output.IsNull()) {
            return {BlockValidationError::kInvalidTransactionStructure, 0};
        }
        const auto key = UTXOKey::FromOutPoint(input.previous_output);
        if (!spent_in_block.insert(key).second) {
            return {BlockValidationError::kDoubleSpend, 0};
        }
        const auto coin = utxos.GetCoin(key);
        if (!coin.has_value()) {
            return {BlockValidationError::kMissingUtxo, 0};
        }
        if (coin->is_coinbase && (context.height < coin->height ||
                                  context.height - coin->height < parameters.coinbase_maturity)) {
            return {BlockValidationError::kImmatureCoinbase, 0};
        }
        if (coin->output.value < 0 || coin->output.value > parameters.chain_parameters.max_money) {
            return {BlockValidationError::kInvalidInputAmount, 0};
        }
        if (coin->output.value > parameters.chain_parameters.max_money - input_total) {
            return {BlockValidationError::kInputAmountOverflow, 0};
        }
        input_total += coin->output.value;
        const auto authorization =
            CheckInputAuthorization(transaction, index, coin->output, parameters);
        if (authorization != BlockValidationError::kNone) {
            return {authorization, 0};
        }
    }

    primitives::Amount output_total = 0;
    for (const auto& output : transaction.outputs) {
        if (output.value < 0 || output.value > parameters.chain_parameters.max_money) {
            return {BlockValidationError::kInvalidOutputAmount, 0};
        }
        if (output.value > parameters.chain_parameters.max_money - output_total) {
            return {BlockValidationError::kInvalidOutputAmount, 0};
        }
        output_total += output.value;
    }
    if (input_total < output_total) {
        return {BlockValidationError::kInputValueTooLow, 0};
    }
    return {BlockValidationError::kNone, input_total - output_total};
}

[[nodiscard]] BlockValidationError MapUtxoError(const UTXOError error) noexcept
{
    switch (error) {
    case UTXOError::kNone:
        return BlockValidationError::kNone;
    case UTXOError::kMissingInput:
        return BlockValidationError::kMissingUtxo;
    case UTXOError::kAllocationFailure:
        return BlockValidationError::kAllocationFailure;
    default:
        return BlockValidationError::kStateApplicationFailure;
    }
}

} // namespace

BlockValidationError CheckBlockHeader(const primitives::BlockHeader& header,
                                      const BlockValidationContext& context,
                                      const BlockValidationParams& parameters) noexcept
{
    if (CheckParameters(parameters) != BlockValidationError::kNone) {
        return BlockValidationError::kInvalidParameters;
    }
    if (context.validation_time >
        std::numeric_limits<std::uint32_t>::max() - parameters.max_future_block_time_seconds) {
        return BlockValidationError::kInvalidContext;
    }
    if (primitives::CheckBlockHeaderStructure(header) !=
        primitives::BlockHeaderStructureError::kNone) {
        return BlockValidationError::kInvalidHeader;
    }
    if (header.previous_block_id != context.parent_block_id) {
        return BlockValidationError::kPreviousBlockMismatch;
    }
    const auto target = consensus::TargetFromCompact(header.bits, parameters.pow_parameters);
    if (target.error != consensus::TargetError::kNone || !target.target.has_value()) {
        return BlockValidationError::kInvalidTarget;
    }
    const auto block_hash = primitives::ComputeBlockHash(header);
    if (!block_hash.has_value()) {
        return BlockValidationError::kBlockHashFailure;
    }
    if (!consensus::CheckProofOfWork(*block_hash, *target.target)) {
        return BlockValidationError::kBadProofOfWork;
    }
    if (header.time <= context.median_time_past) {
        return BlockValidationError::kTimeTooOld;
    }
    if (header.time > context.validation_time + parameters.max_future_block_time_seconds) {
        return BlockValidationError::kTimeTooNew;
    }
    return BlockValidationError::kNone;
}

BlockValidationError CheckBlock(const primitives::Block& block,
                                const BlockValidationContext& context,
                                const BlockValidationParams& parameters) noexcept
{
    const auto header_error = CheckBlockHeader(block.header, context, parameters);
    if (header_error != BlockValidationError::kNone) {
        return header_error;
    }
    return MapBlockStructureError(primitives::CheckBlockStructure(block, parameters.block_limits));
}

std::optional<crypto::Hash256>
ComputeSignatureHash(const primitives::Transaction& transaction, const std::size_t input_index,
                     const std::span<const std::uint8_t> script_pubkey,
                     const primitives::TransactionLimits& limits,
                     const std::uint32_t sighash_all) noexcept
{
    if (input_index >= transaction.inputs.size() || sighash_all != 1U ||
        script_pubkey.size() > limits.max_script_size) {
        return std::nullopt;
    }
    try {
        auto preimage_transaction = transaction;
        for (auto& input : preimage_transaction.inputs) {
            input.script_sig.clear();
        }
        preimage_transaction.inputs.at(input_index)
            .script_sig.assign(script_pubkey.begin(), script_pubkey.end());
        primitives::BinaryWriter writer;
        if (!preimage_transaction.Serialize(writer, limits) || !writer.WriteU32(sighash_all)) {
            return std::nullopt;
        }
        return crypto::Hash256::DoubleSha256(writer.bytes());
    } catch (...) {
        return std::nullopt;
    }
}

TransactionValidationResult ValidateTransactionAgainstUTXOSet(
    const primitives::Transaction& transaction, const TransactionValidationContext& context,
    const BlockValidationParams& parameters, const UTXOView& utxos) noexcept
{
    if (CheckParameters(parameters) != BlockValidationError::kNone) {
        return {BlockValidationError::kInvalidParameters, 0};
    }
    try {
        std::set<UTXOKey> spent;
        return CheckTransactionAgainstUtxo(transaction, context, parameters, utxos, spent);
    } catch (...) {
        return {BlockValidationError::kAllocationFailure, 0};
    }
}

BlockValidationResult ConnectBlock(const primitives::Block& block,
                                   const BlockValidationContext& context,
                                   const BlockValidationParams& parameters, UTXOSet& utxos) noexcept
{
    const auto block_error = CheckBlock(block, context, parameters);
    if (block_error != BlockValidationError::kNone) {
        return {block_error, std::nullopt};
    }
    if (!HasMinimalCoinbaseHeight(block.transactions.front(), context.height)) {
        return {BlockValidationError::kInvalidCoinbaseHeight, std::nullopt};
    }

    const auto block_id = primitives::ComputeBlockHash(block.header);
    if (!block_id.has_value()) {
        return {BlockValidationError::kBlockHashFailure, std::nullopt};
    }
    auto batch = utxos.BeginBatch();
    if (!batch.has_value()) {
        return {BlockValidationError::kAllocationFailure, std::nullopt};
    }
    try {
        std::set<UTXOKey> spent_in_block;
        std::vector<TransactionUndo> regular_undos;
        regular_undos.reserve(block.transactions.size() - 1U);
        primitives::Amount total_fees = 0;

        for (std::size_t index = 1U; index < block.transactions.size(); ++index) {
            const auto& transaction = block.transactions.at(index);
            const auto validated = CheckTransactionAgainstUtxo(
                transaction, TransactionValidationContext{context.height, context.median_time_past},
                parameters, *batch, spent_in_block);
            if (validated.error != BlockValidationError::kNone) {
                return {validated.error, std::nullopt};
            }
            if (validated.fee > parameters.chain_parameters.max_money - total_fees) {
                return {BlockValidationError::kFeeOverflow, std::nullopt};
            }
            auto applied = batch->ApplyTransaction(transaction, context.height, false,
                                                   parameters.block_limits.transaction_limits);
            const auto application_error = MapUtxoError(applied.error);
            if (application_error != BlockValidationError::kNone || !applied.undo.has_value()) {
                return {application_error, std::nullopt};
            }
            total_fees += validated.fee;
            regular_undos.push_back(std::move(*applied.undo));
        }

        const auto reward_error = consensus::CheckCoinbaseReward(
            block.transactions.front(), total_fees, context.height, parameters.chain_parameters);
        if (reward_error != consensus::CoinbaseRewardError::kNone) {
            return {BlockValidationError::kInvalidCoinbaseReward, std::nullopt};
        }
        auto coinbase_applied =
            batch->ApplyTransaction(block.transactions.front(), context.height, true,
                                    parameters.block_limits.transaction_limits);
        const auto coinbase_application_error = MapUtxoError(coinbase_applied.error);
        if (coinbase_application_error != BlockValidationError::kNone ||
            !coinbase_applied.undo.has_value()) {
            return {coinbase_application_error, std::nullopt};
        }

        BlockUndo undo;
        undo.transaction_undos.reserve(block.transactions.size());
        undo.transaction_undos.push_back(std::move(*coinbase_applied.undo));
        for (auto& transaction_undo : regular_undos) {
            undo.transaction_undos.push_back(std::move(transaction_undo));
        }
        if (!batch->Commit()) {
            return {BlockValidationError::kStateApplicationFailure, std::nullopt};
        }
        return {BlockValidationError::kNone,
                ConnectedBlock{std::move(undo), *block_id, total_fees, context.height}};
    } catch (...) {
        return {BlockValidationError::kAllocationFailure, std::nullopt};
    }
}

BlockValidationError DisconnectBlock(const ConnectedBlock& connected_block, UTXOSet& utxos) noexcept
{
    const auto undo_error = utxos.UndoBlock(connected_block.undo);
    if (undo_error == UTXOError::kNone) {
        return BlockValidationError::kNone;
    }
    if (undo_error == UTXOError::kAllocationFailure) {
        return BlockValidationError::kAllocationFailure;
    }
    return BlockValidationError::kDisconnectFailure;
}

} // namespace nova::chain
