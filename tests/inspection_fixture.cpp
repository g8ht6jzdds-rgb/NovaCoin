#include "inspection/chain_report.hpp"
#include "storage/network_identity.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>

int main(int argc, char** argv)
{
    if (argc != 3)
        return 2;
    const std::string_view name{argv[2]};
    if (name != "testnet" && name != "regtest")
        return 2;
    const auto network = name == "testnet" ? nova::consensus::NetworkId::kTestnet
                                           : nova::consensus::NetworkId::kRegtest;
    const auto& parameters = nova::consensus::GetNetworkParams(network);
    const std::filesystem::path root{argv[1]};
    if (!std::filesystem::create_directory(root))
        return 2;
    if (nova::storage::EnsureNetworkIdentity(root, parameters) !=
        nova::storage::NetworkIdentityError::kNone)
        return 2;
    std::ofstream journal{root / "blocks.dat", std::ios::binary};
    journal.close();
    std::ifstream marker{root / "network.identity", std::ios::binary};
    const std::vector<std::uint8_t> identity{std::istreambuf_iterator<char>{marker}, {}};
    const auto identity_hash = nova::crypto::Hash256::Sha256(identity);
    const auto journal_hash = nova::crypto::Hash256::Sha256({});
    auto state = nova::chain::NetworkChainInitializer::Create(
        network, std::make_shared<nova::chain::FixedValidationTimeSource>(1'704'160'000U));
    if (!identity_hash.has_value() || !journal_hash.has_value() || !state.has_value())
        return 2;
    const auto anchor = state->chain->GetBlockIndex(state->chain->active_tip());
    if (!anchor.has_value())
        return 2;
    std::ofstream metadata{root / "snapshot.meta", std::ios::binary};
    metadata << "nova-snapshot-v1\nnetwork=" << name
             << "\nas_of=1704160000\ntip=" << nova::inspection::Hex(parameters.genesis_hash.bytes())
             << "\nheight=0\nwork=" << nova::inspection::Hex(anchor->chain_work.bytes())
             << "\nidentity_sha256=" << nova::inspection::Hex(identity_hash->bytes())
             << "\njournal_sha256=" << nova::inspection::Hex(journal_hash->bytes())
             << "\nprovenance_sha256=" << std::string(64U, 'a')
             << "\ncapture=stopped-immutable-copy\n";
    return metadata.good() ? 0 : 2;
}
