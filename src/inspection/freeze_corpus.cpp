#include "inspection/freeze_corpus.hpp"

#include "consensus/network_params.hpp"
#include "consensus/pow.hpp"
#include "primitives/serialization.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace nova::inspection
{
namespace
{

[[nodiscard]] bool IsLowerHex(const std::string_view value, const std::size_t size) noexcept
{
    return value.size() == size &&
           std::all_of(value.begin(), value.end(), [](const char character) {
               return (character >= '0' && character <= '9') ||
                      (character >= 'a' && character <= 'f');
           });
}

template <typename Bytes> [[nodiscard]] std::string Hex(const Bytes& bytes)
{
    std::ostringstream output;
    output << std::hex << std::nouppercase << std::setfill('0');
    for (const auto byte : bytes) {
        output << std::setw(2) << static_cast<unsigned int>(byte);
    }
    return output.str();
}

[[nodiscard]] std::string Hex32(const std::uint32_t value)
{
    std::ostringstream output;
    output << "0x" << std::hex << std::nouppercase << std::setfill('0') << std::setw(8) << value;
    return output.str();
}

[[nodiscard]] std::string Bool(const bool value)
{
    return value ? "true" : "false";
}

[[nodiscard]] const char* NetworkName(const consensus::NetworkId id) noexcept
{
    switch (id) {
    case consensus::NetworkId::kRegtest:
        return "REGTEST";
    case consensus::NetworkId::kTestnet:
        return "TESTNET";
    case consensus::NetworkId::kMainnet:
        return "MAINNET";
    }
    return "UNKNOWN";
}

[[nodiscard]] std::optional<std::string> SerializedGenesis(const consensus::NetworkParams& params)
{
    primitives::BinaryWriter writer;
    if (!params.genesis_block.Serialize(writer, params.block_limits)) {
        return std::nullopt;
    }
    return Hex(writer.bytes());
}

[[nodiscard]] std::optional<std::string> PerBlockWork(const consensus::NetworkParams& params)
{
    const auto target = consensus::TargetFromCompact(params.pow.pow_limit_compact, params.pow);
    if (!target.target.has_value()) {
        return std::nullopt;
    }
    const auto work = consensus::CalculateWork(*target.target);
    if (!work.has_value()) {
        return std::nullopt;
    }
    return Hex(work->bytes());
}

[[nodiscard]] bool IsCleanIdentity(const FreezeCorpusIdentity& identity) noexcept
{
    return IsLowerHex(identity.commit, 40U) && IsLowerHex(identity.tree, 40U) &&
           (identity.state == "clean" || identity.state == "archive-declared");
}

void EmitNetwork(std::ostringstream& output, const consensus::NetworkParams& params)
{
    const auto genesis = SerializedGenesis(params);
    const auto work = PerBlockWork(params);
    if (!genesis.has_value() || !work.has_value()) {
        throw std::runtime_error("compiled network parameters could not be rendered");
    }
    const auto& tx = params.block_limits.transaction_limits;
    output << "network=" << NetworkName(params.id) << '\n';
    output << "network.enabled=" << Bool(params.enabled) << '\n';
    output << "network.deployment_final=" << Bool(params.deployment_final) << '\n';
    output << "network.protocol_version=" << params.protocol_version << '\n';
    output << "network.minimum_peer_protocol_version=" << params.minimum_peer_protocol_version
           << '\n';
    output << "network.magic=" << Hex32(params.network_magic) << '\n';
    output << "network.p2p_port=" << params.default_p2p_port << '\n';
    output << "network.rpc_port=" << params.default_rpc_port << '\n';
    output << "network.p2pkh_prefix=" << static_cast<unsigned int>(params.address_prefixes.p2pkh)
           << '\n';
    output << "network.private_key_prefix="
           << static_cast<unsigned int>(params.address_prefixes.private_key) << '\n';
    output << "pow.limit_compact=" << Hex32(params.pow.pow_limit_compact) << '\n';
    output << "pow.limit_target=" << Hex(params.pow.pow_limit.bytes()) << '\n';
    output << "pow.per_block_work=" << *work << '\n';
    output << "difficulty.target_spacing_seconds=" << params.difficulty.target_spacing_seconds
           << '\n';
    output << "difficulty.retarget_interval=" << params.difficulty.retarget_interval << '\n';
    output << "difficulty.target_timespan_seconds=" << params.difficulty.target_timespan_seconds
           << '\n';
    output << "difficulty.allow_min_difficulty_blocks="
           << Bool(params.difficulty.allow_min_difficulty_blocks) << '\n';
    output << "difficulty.no_retargeting=" << Bool(params.difficulty.no_retargeting) << '\n';
    output << "monetary.coin=" << params.monetary.coin << '\n';
    output << "monetary.max_money=" << params.monetary.max_money << '\n';
    output << "monetary.initial_subsidy=" << params.monetary.initial_subsidy << '\n';
    output << "monetary.halving_interval=" << params.monetary.halving_interval << '\n';
    output << "limits.max_block_serialized_size=" << params.block_limits.max_serialized_size
           << '\n';
    output << "limits.max_transactions=" << params.block_limits.max_transactions << '\n';
    output << "limits.max_coinbase_script_size=" << params.block_limits.max_coinbase_script_size
           << '\n';
    output << "limits.max_money=" << tx.max_money << '\n';
    output << "limits.max_tx_serialized_size=" << tx.max_serialized_size << '\n';
    output << "limits.max_tx_inputs=" << tx.max_inputs << '\n';
    output << "limits.max_tx_outputs=" << tx.max_outputs << '\n';
    output << "limits.max_script_size=" << tx.max_script_size << '\n';
    output << "genesis.serialized=" << *genesis << '\n';
    output << "genesis.hash=" << Hex(params.genesis_hash.bytes()) << '\n';
    output << "genesis.merkle_root=" << Hex(params.genesis_merkle_root.bytes()) << '\n';
    output << "genesis.time=" << params.genesis_block.header.time << '\n';
    output << "genesis.bits=" << Hex32(params.genesis_block.header.bits) << '\n';
    output << "genesis.nonce=" << params.genesis_block.header.nonce << '\n';
}

void EmitVectors(std::ostringstream& output)
{
    // These are corpus identifiers and public input/result relations for the
    // owner-approved D1-D7 vectors. They never select a rule or mutate state.
    output << "vector.id=T01\nvector.rule=chainwork.calculate_work\n"
           << "vector.assertion=regtest_pow_limit_work_is_exact_and_nonzero\n";
    output << "vector.id=T02\nvector.rule=difficulty.no_retargeting\n"
           << "vector.assertion=child_inherits_parent_expected_bits\n";
    output << "vector.id=T03\nvector.rule=utxo.ordered_view\n"
           << "vector.assertion=earlier_regular_output_is_visible\n";
    output << "vector.id=T04\nvector.rule=utxo.ordered_view\n"
           << "vector.assertion=forward_reference_and_collisions_reject_atomically\n";
    output << "vector.id=T05\nvector.rule=script.output_admission\n"
           << "vector.assertion=unsupported_outputs_remain_unspendable_utxos\n";
    output << "vector.id=T06\nvector.rule=parameters.bindings\n"
           << "vector.assertion=immutable_network_bindings_are_not_caller_overrides\n";
    output << "vector.id=T07\nvector.rule=coinbase.height_prefix\n"
           << "vector.assertion=ordinary_coinbase_uses_canonical_height_prefix\n";
    output << "vector.id=T09\nvector.rule=parameter.boundaries\n"
           << "vector.assertion=exact_limits_and_derived_values_are_emitted\n";
    output << "vector.id=T10\nvector.rule=immutable_v0\n"
           << "vector.assertion=versions_do_not_select_alternate_consensus_rules\n";
}

[[nodiscard]] bool IsSafeRelativePath(const std::string_view path) noexcept
{
    if (path.empty() || path.front() == '/' || path.find('\\') != std::string_view::npos ||
        path.find("//") != std::string_view::npos) {
        return false;
    }
    std::size_t begin = 0U;
    while (begin < path.size()) {
        const auto end = path.find('/', begin);
        const auto part =
            path.substr(begin, end == std::string_view::npos ? path.size() - begin : end - begin);
        if (part.empty() || part == "." || part == "..") {
            return false;
        }
        begin = end == std::string_view::npos ? path.size() : end + 1U;
    }
    return true;
}

} // namespace

std::optional<std::string> RenderFreezeCorpus(const FreezeCorpusIdentity& identity) noexcept
{
    if (!IsCleanIdentity(identity)) {
        return std::nullopt;
    }
    try {
        std::ostringstream output;
        output << "format=novacoin-gate1-parameter-corpus-v1\n";
        output << "candidate.commit=" << identity.commit << '\n';
        output << "candidate.tree=" << identity.tree << '\n';
        output << "candidate.state=" << identity.state << '\n';
        EmitNetwork(output, consensus::RegtestNetworkParams());
        EmitNetwork(output, consensus::TestnetNetworkParams());
        EmitVectors(output);
        return output.str();
    } catch (...) {
        return std::nullopt;
    }
}

bool IsCanonicalSha256Manifest(const std::string_view manifest) noexcept
{
    if (manifest.empty() || manifest.back() != '\n') {
        return false;
    }
    std::set<std::string_view> paths;
    std::size_t begin = 0U;
    while (begin < manifest.size()) {
        const auto end = manifest.find('\n', begin);
        if (end == std::string_view::npos || end == begin || end - begin < 67U) {
            return false;
        }
        const auto line = manifest.substr(begin, end - begin);
        if (!IsLowerHex(line.substr(0U, 64U), 64U) || line.substr(64U, 2U) != "  ") {
            return false;
        }
        const auto path = line.substr(66U);
        if (!IsSafeRelativePath(path) || !paths.insert(path).second) {
            return false;
        }
        begin = end + 1U;
    }
    return true;
}

} // namespace nova::inspection
