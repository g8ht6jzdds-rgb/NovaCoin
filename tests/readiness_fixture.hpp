#pragma once

#include "chain/chainstate.hpp"
#include "primitives/serialization.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

// Offline REGTEST-only fixture. No MAINNET genesis generation or node startup.
namespace nova::readiness
{

inline std::string Hex(const std::span<const std::uint8_t> bytes)
{
    constexpr char kDigits[] = "0123456789abcdef";
    std::string result;
    for (const auto byte : bytes) {
        result.push_back(kDigits[byte >> 4U]);
        result.push_back(kDigits[byte & 15U]);
    }
    return result;
}

inline chain::ChainStateParams Parameters()
{
    const auto& network = consensus::RegtestNetworkParams();
    return {{network.block_limits, network.pow, network.monetary, 0U, 500'000'000U, 0xFFFF'FFFFU,
             1U, 600U},
            network.difficulty,
            3U,
            std::make_shared<chain::FixedValidationTimeSource>(100'000U)};
}

inline std::optional<primitives::Block> Block(const crypto::Hash256& parent,
                                              const std::uint32_t height, const std::uint8_t branch,
                                              const bool excess = false)
{
    const auto parameters = Parameters();
    const auto subsidy =
        consensus::GetBlockSubsidy(height, parameters.block_validation.chain_parameters);
    if (!subsidy.has_value()) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> magnitude;
    auto remaining = height;
    while (remaining != 0U) {
        magnitude.push_back(static_cast<std::uint8_t>(remaining & 255U));
        remaining >>= 8U;
    }
    if (!magnitude.empty() && (magnitude.back() & 128U) != 0U) {
        magnitude.push_back(0U);
    }
    std::vector<std::uint8_t> script{static_cast<std::uint8_t>(magnitude.size())};
    script.insert(script.end(), magnitude.begin(), magnitude.end());
    script.push_back(branch);
    primitives::Transaction coinbase{1,
                                     {{{crypto::Hash256{}, 0xFFFF'FFFFU}, script, 0U}},
                                     {{*subsidy + (excess ? 1 : 0), {0x51U}}},
                                     0U};
    primitives::Block block{{1, parent, {}, 1'000U + height * 10U + branch, 0x207F'FFFFU, 0U},
                            {coinbase}};
    const auto merkle = primitives::ComputeMerkleRoot(
        block.transactions, parameters.block_validation.block_limits.transaction_limits);
    if (!merkle.has_value()) {
        return std::nullopt;
    }
    block.header.merkle_root = *merkle;
    const auto mined = consensus::MineRegtestBlock(block.header, 4'096U);
    if (!mined.has_value()) {
        return std::nullopt;
    }
    block.header = mined->header;
    return block;
}

struct Fingerprint final {
    std::string blocks;
    std::string transactions;
    std::string utxos;
    std::string tip;
    std::string chainwork;
    std::uint32_t height{};
    std::size_t coins{};

    friend bool operator==(const Fingerprint&, const Fingerprint&) = default;
};

inline std::optional<Fingerprint> Snapshot(const chain::ChainState& state,
                                           const chain::UTXOSet& utxos)
{
    const auto indexes = state.GetActiveBlockIndexes();
    if (!indexes.has_value()) {
        return std::nullopt;
    }
    std::map<chain::UTXOKey, chain::Coin> expected;
    primitives::BinaryWriter blocks;
    primitives::BinaryWriter transactions;
    const auto limits = Parameters().block_validation.block_limits.transaction_limits;
    for (const auto& index : *indexes) {
        if (!blocks.WriteHash256(index.hash)) {
            return std::nullopt;
        }
        if (index.height == 0U) {
            continue;
        }
        if (!index.block.has_value()) {
            return std::nullopt;
        }
        for (const auto& transaction : index.block->transactions) {
            const auto txid = transaction.TxId(limits);
            if (!txid.has_value() || !transactions.WriteHash256(*txid)) {
                return std::nullopt;
            }
            // This bounded fixture deliberately contains only one-output coinbases.
            if (!primitives::IsCoinbaseTransaction(transaction) ||
                transaction.outputs.size() != 1U) {
                return std::nullopt;
            }
            expected.emplace(chain::UTXOKey{*txid, 0U},
                             chain::Coin{transaction.outputs.front(), index.height, true});
        }
    }
    if (utxos.size() != expected.size()) {
        return std::nullopt;
    }
    primitives::BinaryWriter coins;
    for (const auto& [key, expected_coin] : expected) {
        const auto actual = utxos.GetCoin(key);
        if (!actual.has_value() || actual->output.value != expected_coin.output.value ||
            actual->output.script_pubkey != expected_coin.output.script_pubkey ||
            actual->height != expected_coin.height || !actual->is_coinbase ||
            !coins.WriteHash256(key.transaction_id) || !coins.WriteU32(key.output_index) ||
            !coins.WriteI64(actual->output.value) || !coins.WriteU32(actual->height) ||
            !coins.WriteU8(1U) || !coins.WriteBytes(actual->output.script_pubkey, 256U)) {
            return std::nullopt;
        }
    }
    const auto block_hash = crypto::Hash256::DoubleSha256(blocks.bytes());
    const auto tx_hash = crypto::Hash256::DoubleSha256(transactions.bytes());
    const auto coin_hash = crypto::Hash256::DoubleSha256(coins.bytes());
    const auto tip = state.GetBlockIndex(state.active_tip());
    if (!block_hash.has_value() || !tx_hash.has_value() || !coin_hash.has_value() ||
        !tip.has_value()) {
        return std::nullopt;
    }
    return Fingerprint{Hex(block_hash->bytes()),
                       Hex(tx_hash->bytes()),
                       Hex(coin_hash->bytes()),
                       Hex(state.active_tip().bytes()),
                       Hex(tip->chain_work.bytes()),
                       state.active_height(),
                       utxos.size()};
}

inline std::optional<Fingerprint> Reorganize(const std::uint32_t depth)
{
    if (depth != 1U && depth != 10U && depth != 100U) {
        return std::nullopt;
    }
    chain::UTXOSet utxos;
    const auto& network = consensus::RegtestNetworkParams();
    const auto target = consensus::TargetFromCompact(network.pow.pow_limit_compact);
    if (!target.target.has_value()) {
        return std::nullopt;
    }
    const auto work = consensus::CalculateWork(*target.target);
    if (!work.has_value()) {
        return std::nullopt;
    }
    const auto state = chain::ChainState::Create(Parameters(),
                                                 {network.genesis_hash,
                                                  {},
                                                  0U,
                                                  1'000U,
                                                  *target.target,
                                                  *work,
                                                  chain::ChainWork::FromPerBlockWork(*work)},
                                                 utxos);
    if (state == nullptr) {
        return std::nullopt;
    }
    for (const auto branch : {std::uint8_t{0U}, std::uint8_t{1U}}) {
        auto parent = network.genesis_hash;
        const auto count = depth + static_cast<std::uint32_t>(branch);
        for (std::uint32_t height = 1U; height <= count; ++height) {
            const auto block = Block(parent, height, branch);
            if (!block.has_value()) {
                return std::nullopt;
            }
            const auto accepted = state->AcceptBlock(*block);
            if (accepted.error != chain::ChainError::kNone || !accepted.block_hash.has_value() ||
                !Snapshot(*state, utxos).has_value()) {
                return std::nullopt;
            }
            parent = *accepted.block_hash;
        }
        if (state->active_tip() != parent || state->active_height() != count) {
            return std::nullopt;
        }
    }
    const auto before = Snapshot(*state, utxos);
    const auto invalid = Block(state->active_tip(), depth + 2U, 2U, true);
    if (!before.has_value() || !invalid.has_value() ||
        state->AcceptBlock(*invalid).error == chain::ChainError::kNone ||
        Snapshot(*state, utxos) != before) {
        return std::nullopt;
    }
    return before;
}

} // namespace nova::readiness
