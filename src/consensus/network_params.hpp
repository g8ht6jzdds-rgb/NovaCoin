#pragma once

#include "consensus/monetary.hpp"
#include "consensus/pow.hpp"
#include "primitives/block.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace nova::consensus
{

enum class NetworkId : std::uint8_t { kRegtest, kTestnet, kMainnet };

struct AddressPrefixes final {
    std::uint8_t p2pkh{};
    std::uint8_t private_key{};
};

struct DifficultyParameters final {
    std::uint32_t target_spacing_seconds{};
    std::uint32_t retarget_interval{};
    std::uint32_t target_timespan_seconds{};
    bool allow_min_difficulty_blocks{};
    bool no_retargeting{};
};

struct GenesisRequest final {
    std::uint32_t timestamp{};
    std::string_view message;
    std::uint32_t compact_target{};
    primitives::Amount reward{};
};

enum class GenesisError : std::uint8_t {
    kNone,
    kInvalidMessage,
    kInvalidReward,
    kInvalidTarget,
    kNoProofOfWork,
    kAllocationFailure,
};

struct GenesisResult final {
    GenesisError error{GenesisError::kNone};
    std::optional<primitives::Block> block;
    std::optional<crypto::Hash256> block_hash;
    std::optional<crypto::Hash256> merkle_root;
    std::uint64_t attempts{};
};

struct NetworkParams final {
    NetworkId id{NetworkId::kRegtest};
    bool enabled{};
    bool deployment_final{};
    std::uint32_t network_magic{};
    std::uint16_t default_p2p_port{};
    std::uint16_t default_rpc_port{};
    AddressPrefixes address_prefixes;
    PowParameters pow;
    DifficultyParameters difficulty;
    ChainParams monetary;
    primitives::BlockLimits block_limits;
    primitives::Block genesis_block;
    crypto::Hash256 genesis_hash;
    crypto::Hash256 genesis_merkle_root;
};

enum class NetworkParamsError : std::uint8_t {
    kNone,
    kInvalidIdentity,
    kInvalidPorts,
    kInvalidMonetaryParameters,
    kInvalidPowParameters,
    kInvalidDifficultyParameters,
    kInvalidBlockLimits,
    kInvalidGenesisStructure,
    kInvalidGenesisParent,
    kInvalidGenesisTarget,
    kInvalidGenesisMerkleRoot,
    kInvalidGenesisHash,
    kInvalidGenesisProofOfWork,
    kTestnetNotFinal,
    kMainnetEnabled,
};

[[nodiscard]] GenesisResult GenerateGenesis(const GenesisRequest& request,
                                            const primitives::BlockLimits& limits,
                                            std::uint64_t maximum_attempts) noexcept;
[[nodiscard]] NetworkParamsError CheckNetworkParams(const NetworkParams& parameters) noexcept;
[[nodiscard]] const NetworkParams& RegtestNetworkParams() noexcept;
[[nodiscard]] const NetworkParams& TestnetNetworkParams() noexcept;
[[nodiscard]] const NetworkParams& MainnetNetworkParams() noexcept;
[[nodiscard]] const NetworkParams& GetNetworkParams(NetworkId network) noexcept;

} // namespace nova::consensus
