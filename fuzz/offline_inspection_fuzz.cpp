#include "inspection/chain_report.hpp"
#include "storage/network_identity.hpp"

#include <cstddef>
#include <cstdint>
#include <sstream>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size > 4096U)
        return 0;
    const auto network = size > 0U && (data[0] & 1U) != 0U ? nova::consensus::NetworkId::kTestnet
                                                           : nova::consensus::NetworkId::kRegtest;
    const auto& params = nova::consensus::GetNetworkParams(network);
    static_cast<void>(nova::storage::ValidateNetworkIdentity(std::span{data, size}, params));
    nova::primitives::BinaryWriter marker;
    if (!marker.WriteU8('N') || !marker.WriteU8('V') || !marker.WriteU8('I') ||
        !marker.WriteU8('D') || !marker.WriteU32(1U) ||
        !marker.WriteU8(static_cast<std::uint8_t>(params.id)) ||
        !marker.WriteU32(params.network_magic) || !marker.WriteHash256(params.genesis_hash))
        return 0;
    static_cast<void>(nova::inspection::ParseMetadata(std::span{data, size}, marker.bytes(), {},
                                                      std::string(64U, 'a'), std::string(40U, 'a'),
                                                      std::string(40U, 'a')));
    auto state = nova::chain::NetworkChainInitializer::Create(
        network, std::make_shared<nova::chain::FixedValidationTimeSource>(1'704'160'000U));
    if (!state.has_value())
        return 0;
    const auto tip = state->chain->GetBlockIndex(state->chain->active_tip());
    if (!tip.has_value())
        return 0;
    const nova::inspection::InspectionContext context{network,
                                                      1'704'160'000U,
                                                      tip->hash,
                                                      0U,
                                                      tip->chain_work,
                                                      std::string(40U, 'a'),
                                                      std::string(64U, 'b'),
                                                      std::string(64U, 'c'),
                                                      std::string(64U, 'd'),
                                                      std::string(64U, 'e'),
                                                      std::string(64U, 'f')};
    std::istringstream input{std::string(reinterpret_cast<const char*>(data), size)};
    static_cast<void>(
        nova::inspection::InspectJournal(input, context, {{4096U, 128U, 64U, 64U}, 512U, 65536U}));
    return 0;
}
