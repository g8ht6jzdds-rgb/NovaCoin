#include "consensus/network_params.hpp"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <limits>
#include <new>
#include <utility>
#include <vector>

namespace nova::consensus
{
namespace
{

constexpr primitives::TransactionLimits kTransactionLimits{MAX_MONEY, 100'000U, 128U, 128U, 256U};
constexpr primitives::BlockLimits kBlockLimits{kTransactionLimits, 1'000'000U, 256U, 100U};
constexpr std::size_t kMaximumGenesisMessage = 64U;

[[nodiscard]] bool IsAllZero(const crypto::Hash256& hash) noexcept
{
    return std::all_of(hash.bytes().begin(), hash.bytes().end(),
                       [](const std::uint8_t value) { return value == 0U; });
}

[[nodiscard]] bool IsPrintableAscii(const std::string_view message) noexcept
{
    return !message.empty() && message.size() <= kMaximumGenesisMessage &&
           std::all_of(message.begin(), message.end(), [](const char character) {
               const auto value = static_cast<unsigned char>(character);
               return value >= 0x20U && value <= 0x7EU;
           });
}

[[nodiscard]] std::vector<std::uint8_t> CoinbaseScript(const std::uint32_t timestamp,
                                                       const std::string_view message)
{
    std::vector<std::uint8_t> script;
    script.reserve(4U + message.size());
    for (std::uint32_t offset = 0U; offset < 4U; ++offset) {
        script.push_back(static_cast<std::uint8_t>((timestamp >> (offset * 8U)) & 0xFFU));
    }
    for (const auto character : message) {
        script.push_back(static_cast<std::uint8_t>(static_cast<unsigned char>(character)));
    }
    return script;
}

[[nodiscard]] std::optional<primitives::Block>
MakeGenesisBlock(const GenesisRequest& request, const primitives::BlockLimits& limits,
                 const std::uint32_t nonce) noexcept
{
    if (!IsPrintableAscii(request.message) || request.reward < 0 ||
        request.reward > limits.transaction_limits.max_money) {
        return std::nullopt;
    }
    try {
        primitives::Transaction coinbase{
            1,
            {{primitives::OutPoint{crypto::Hash256{}, std::numeric_limits<std::uint32_t>::max()},
              CoinbaseScript(request.timestamp, request.message), 0U}},
            {{request.reward, {0x51U}}},
            0U};
        primitives::Block block{{1, crypto::Hash256{}, crypto::Hash256{}, request.timestamp,
                                 request.compact_target, nonce},
                                {std::move(coinbase)}};
        const auto merkle_root =
            primitives::ComputeMerkleRoot(block.transactions, limits.transaction_limits);
        if (!merkle_root.has_value()) {
            return std::nullopt;
        }
        block.header.merkle_root = *merkle_root;
        return block;
    } catch (...) {
        return std::nullopt;
    }
}

[[nodiscard]] Target256 Target(std::initializer_list<std::uint8_t> bytes)
{
    Target256::Bytes target{};
    std::copy(bytes.begin(), bytes.end(), target.begin());
    return Target256{target};
}

[[nodiscard]] crypto::Hash256 Hash(std::initializer_list<std::uint8_t> bytes)
{
    crypto::Hash256::Bytes hash{};
    std::copy(bytes.begin(), bytes.end(), hash.begin());
    return crypto::Hash256{hash};
}

[[nodiscard]] NetworkParams
MakeNetwork(const NetworkId id, const bool enabled, const bool deployment_final,
            const std::uint32_t magic, const std::uint16_t p2p_port, const std::uint16_t rpc_port,
            const AddressPrefixes prefixes, const PowParameters& pow,
            const DifficultyParameters& difficulty, const ChainParams& monetary,
            const GenesisRequest& genesis_request, const std::uint32_t genesis_nonce,
            const crypto::Hash256& expected_hash, const crypto::Hash256& expected_merkle) noexcept
{
    const auto genesis = MakeGenesisBlock(genesis_request, kBlockLimits, genesis_nonce);
    return NetworkParams{id,
                         enabled,
                         deployment_final,
                         magic,
                         p2p_port,
                         rpc_port,
                         prefixes,
                         pow,
                         difficulty,
                         monetary,
                         kBlockLimits,
                         genesis.value_or(primitives::Block{}),
                         expected_hash,
                         expected_merkle};
}

} // namespace

GenesisResult GenerateGenesis(const GenesisRequest& request, const primitives::BlockLimits& limits,
                              const std::uint64_t maximum_attempts) noexcept
{
    if (!IsPrintableAscii(request.message)) {
        return {GenesisError::kInvalidMessage, std::nullopt, std::nullopt, std::nullopt, 0U};
    }
    if (request.reward < 0 || request.reward > limits.transaction_limits.max_money) {
        return {GenesisError::kInvalidReward, std::nullopt, std::nullopt, std::nullopt, 0U};
    }
    const auto decoded_target = TargetFromCompact(request.compact_target);
    if (decoded_target.error != TargetError::kNone || !decoded_target.target.has_value()) {
        return {GenesisError::kInvalidTarget, std::nullopt, std::nullopt, std::nullopt, 0U};
    }
    const auto attempts = std::min(maximum_attempts, std::uint64_t{1} << 32U);
    if (attempts == 0U) {
        return {GenesisError::kNoProofOfWork, std::nullopt, std::nullopt, std::nullopt, 0U};
    }
    for (std::uint64_t attempt = 0U; attempt < attempts; ++attempt) {
        const auto block = MakeGenesisBlock(request, limits, static_cast<std::uint32_t>(attempt));
        if (!block.has_value()) {
            return {GenesisError::kAllocationFailure, std::nullopt, std::nullopt, std::nullopt,
                    attempt};
        }
        const auto hash = primitives::ComputeBlockHash(block->header);
        if (!hash.has_value()) {
            return {GenesisError::kAllocationFailure, std::nullopt, std::nullopt, std::nullopt,
                    attempt};
        }
        if (CheckProofOfWork(*hash, *decoded_target.target)) {
            return {GenesisError::kNone, *block, *hash, block->header.merkle_root, attempt + 1U};
        }
    }
    return {GenesisError::kNoProofOfWork, std::nullopt, std::nullopt, std::nullopt, attempts};
}

NetworkParamsError CheckNetworkParams(const NetworkParams& parameters) noexcept
{
    if (parameters.network_magic == 0U) {
        return NetworkParamsError::kInvalidIdentity;
    }
    if (parameters.default_p2p_port == 0U || parameters.default_rpc_port == 0U ||
        parameters.default_p2p_port == parameters.default_rpc_port) {
        return NetworkParamsError::kInvalidPorts;
    }
    if (parameters.id == NetworkId::kMainnet && parameters.enabled) {
        return NetworkParamsError::kMainnetEnabled;
    }
    // Testnet activation is a deliberate release decision. A fixture cannot
    // become active merely by changing its enabled bit without also recording
    // a final, reviewed deployment parameter set.
    if (parameters.id == NetworkId::kTestnet && parameters.enabled &&
        !parameters.deployment_final) {
        return NetworkParamsError::kTestnetNotFinal;
    }
    // A disabled, explicitly non-final mainnet table is an admission gate, not
    // a genesis commitment.  It must never be used to initialize validation.
    if (parameters.id == NetworkId::kMainnet && !parameters.deployment_final) {
        return NetworkParamsError::kNone;
    }
    if (CheckChainParams(parameters.monetary) != ChainParamsError::kNone) {
        return NetworkParamsError::kInvalidMonetaryParameters;
    }
    const auto pow_target = TargetFromCompact(parameters.pow.pow_limit_compact, parameters.pow);
    if (pow_target.error != TargetError::kNone || !pow_target.target.has_value() ||
        *pow_target.target != parameters.pow.pow_limit) {
        return NetworkParamsError::kInvalidPowParameters;
    }
    if (parameters.difficulty.target_spacing_seconds == 0U ||
        parameters.difficulty.retarget_interval == 0U ||
        parameters.difficulty.target_timespan_seconds == 0U ||
        static_cast<std::uint64_t>(parameters.difficulty.target_spacing_seconds) *
                parameters.difficulty.retarget_interval !=
            parameters.difficulty.target_timespan_seconds) {
        return NetworkParamsError::kInvalidDifficultyParameters;
    }
    if (parameters.block_limits.transaction_limits.max_money != parameters.monetary.max_money ||
        parameters.block_limits.max_serialized_size == 0U ||
        parameters.block_limits.max_transactions == 0U) {
        return NetworkParamsError::kInvalidBlockLimits;
    }
    if (primitives::CheckBlockStructure(parameters.genesis_block, parameters.block_limits) !=
        primitives::BlockStructureError::kNone) {
        return NetworkParamsError::kInvalidGenesisStructure;
    }
    if (!IsAllZero(parameters.genesis_block.header.previous_block_id)) {
        return NetworkParamsError::kInvalidGenesisParent;
    }
    if (parameters.genesis_block.header.bits != parameters.pow.pow_limit_compact) {
        return NetworkParamsError::kInvalidGenesisTarget;
    }
    if (parameters.genesis_block.header.merkle_root != parameters.genesis_merkle_root) {
        return NetworkParamsError::kInvalidGenesisMerkleRoot;
    }
    const auto block_hash = primitives::ComputeBlockHash(parameters.genesis_block.header);
    if (!block_hash.has_value() || *block_hash != parameters.genesis_hash) {
        return NetworkParamsError::kInvalidGenesisHash;
    }
    return CheckProofOfWork(*block_hash, parameters.pow.pow_limit)
               ? NetworkParamsError::kNone
               : NetworkParamsError::kInvalidGenesisProofOfWork;
}

const NetworkParams& RegtestNetworkParams() noexcept
{
    static const NetworkParams parameters = MakeNetwork(
        NetworkId::kRegtest, true, true, 0xDAB5'BFFAU, 18'444U, 18'443U, {111U, 239U},
        PowParameters{Target({0x7FU, 0xFFU, 0xFFU}), 0x207F'FFFFU},
        DifficultyParameters{600U, 1U, 600U, true, true},
        ChainParams{COIN, MAX_MONEY, INITIAL_SUBSIDY, 150U},
        GenesisRequest{1'704'067'200U, "NovaCoin Regtest Genesis", 0x207F'FFFFU, 50LL * COIN}, 3U,
        Hash({0xC9U, 0x74U, 0xD1U, 0x1AU, 0x52U, 0x76U, 0xCAU, 0x7EU, 0xB6U, 0x9BU, 0x1EU,
              0xC0U, 0xA8U, 0x06U, 0x2FU, 0xB4U, 0x7BU, 0x49U, 0xC0U, 0x2FU, 0x5AU, 0xC7U,
              0x26U, 0x53U, 0x24U, 0x83U, 0xD0U, 0x90U, 0xACU, 0x53U, 0xEBU, 0x79U}),
        Hash({0x53U, 0x21U, 0x98U, 0xBBU, 0x92U, 0xE4U, 0x8CU, 0x70U, 0x59U, 0xDDU, 0xCBU,
              0x81U, 0x88U, 0x74U, 0xD5U, 0x99U, 0x3AU, 0x19U, 0xFCU, 0x6EU, 0xE5U, 0xC2U,
              0x80U, 0x9FU, 0xF2U, 0x7EU, 0x6BU, 0xB0U, 0x74U, 0x79U, 0xA1U, 0x46U}));
    return parameters;
}

const NetworkParams& TestnetNetworkParams() noexcept
{
    // Development fixture only. See docs/testnet-genesis-review.md; explicit
    // reviewer approval is required before this can become a deployment set.
    static const NetworkParams parameters = MakeNetwork(
        NetworkId::kTestnet, false, false, 0xDAB5'BFFBU, 28'333U, 28'332U, {112U, 240U},
        PowParameters{Target({0x70U, 0xFFU, 0xFFU}), 0x2070'FFFFU},
        DifficultyParameters{600U, 2'016U, 1'209'600U, true, false},
        ChainParams{COIN, MAX_MONEY, INITIAL_SUBSIDY, 210'000U},
        GenesisRequest{1'704'153'600U, "NovaCoin Testnet Genesis", 0x2070'FFFFU, 50LL * COIN}, 0U,
        Hash({0x25U, 0xF9U, 0x44U, 0xA0U, 0x0FU, 0x3DU, 0x55U, 0x94U, 0x52U, 0xB9U, 0x56U,
              0x53U, 0xA2U, 0x0AU, 0x03U, 0x93U, 0x22U, 0xABU, 0x32U, 0x43U, 0xA5U, 0x77U,
              0xD1U, 0xCCU, 0x3BU, 0x8FU, 0x48U, 0xE4U, 0xF3U, 0x0FU, 0xD0U, 0x48U}),
        Hash({0xE0U, 0xE0U, 0xD4U, 0x3CU, 0x6EU, 0xF8U, 0xF4U, 0x2FU, 0x2EU, 0x2DU, 0x07U,
              0xEAU, 0xF8U, 0x9BU, 0x85U, 0x66U, 0xD7U, 0x6FU, 0xBCU, 0x1DU, 0x69U, 0x8DU,
              0x82U, 0x6EU, 0x5FU, 0x4AU, 0x26U, 0xE6U, 0xA3U, 0xD7U, 0x72U, 0x4CU}));
    return parameters;
}

const NetworkParams& MainnetNetworkParams() noexcept
{
    // NOT FINAL — DO NOT DEPLOY.  This provisional fixture is compiled only
    // to keep the table structurally complete; it is not an activation commitment.
    static const NetworkParams parameters = MakeNetwork(
        NetworkId::kMainnet, false, false, 0xDAB5'BFFCU, 39'333U, 39'332U, {68U, 128U},
        PowParameters{Target({0x60U, 0xFFU, 0xFFU}), 0x2060'FFFFU},
        DifficultyParameters{600U, 2'016U, 1'209'600U, false, false},
        ChainParams{COIN, MAX_MONEY, INITIAL_SUBSIDY, 210'000U},
        GenesisRequest{1'704'240'000U, "NovaCoin Mainnet NOT FINAL", 0x2060'FFFFU, 50LL * COIN}, 0U,
        Hash({0xD7U, 0x82U, 0xFAU, 0xCAU, 0xBBU, 0xA1U, 0x09U, 0x5DU, 0x7FU, 0xE0U, 0x33U,
              0x8AU, 0xA8U, 0xDFU, 0xB3U, 0xF9U, 0x8DU, 0xD5U, 0x05U, 0x7DU, 0xE2U, 0x58U,
              0x84U, 0x8DU, 0xB0U, 0x37U, 0x6CU, 0x6AU, 0x99U, 0xE9U, 0xB3U, 0x23U}),
        Hash({0x7AU, 0x05U, 0x70U, 0x0BU, 0x18U, 0xBAU, 0xE8U, 0x09U, 0xCDU, 0xA3U, 0x67U,
              0xDAU, 0x34U, 0xB7U, 0x40U, 0x19U, 0xA3U, 0x03U, 0x8DU, 0x2FU, 0xFBU, 0x85U,
              0x0DU, 0x7BU, 0x98U, 0xEEU, 0x50U, 0x15U, 0xF7U, 0x77U, 0x97U, 0xD9U}));
    return parameters;
}

const NetworkParams& GetNetworkParams(const NetworkId network) noexcept
{
    switch (network) {
    case NetworkId::kRegtest:
        return RegtestNetworkParams();
    case NetworkId::kTestnet:
        return TestnetNetworkParams();
    case NetworkId::kMainnet:
        return MainnetNetworkParams();
    }
    return MainnetNetworkParams();
}

} // namespace nova::consensus
