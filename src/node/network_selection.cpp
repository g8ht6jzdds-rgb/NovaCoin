#include "node/network_selection.hpp"

namespace nova::node
{

NetworkSelection SelectNetwork(const consensus::NetworkId network) noexcept
{
    const auto& parameters = consensus::GetNetworkParams(network);
    if (consensus::CheckNetworkParams(parameters) != consensus::NetworkParamsError::kNone) {
        return {NetworkSelectionError::kInvalidParameters, nullptr};
    }
    if (!parameters.enabled) {
        return {NetworkSelectionError::kDisabled, nullptr};
    }
    return {NetworkSelectionError::kNone, &parameters};
}

const char* NetworkName(const consensus::NetworkId network) noexcept
{
    switch (network) {
    case consensus::NetworkId::kRegtest:
        return "regtest";
    case consensus::NetworkId::kTestnet:
        return "testnet";
    case consensus::NetworkId::kMainnet:
        return "mainnet";
    }
    return "invalid";
}

} // namespace nova::node
