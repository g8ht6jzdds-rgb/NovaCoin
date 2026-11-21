// Test-only offline inspector for disposable REGTEST and TESTNET fixtures.
#include "node/generation_recovery.hpp"
#include "storage/block_journal.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
#include <span>
#include <string>
#include <string_view>

namespace
{
void Hex(std::span<const std::uint8_t> bytes)
{
    constexpr char digits[] = "0123456789abcdef";
    for (const auto byte : bytes)
        std::cout << digits[byte >> 4U] << digits[byte & 15U];
}
} // namespace

int main(const int argc, char** argv)
{
    if ((argc != 3 && argc != 4) || (argc == 4 && std::string_view{argv[3]} != "--full-state") ||
        (std::string_view{argv[2]} != "a" && std::string_view{argv[2]} != "b" &&
         std::string_view{argv[2]} != "store" && std::string_view{argv[2]} != "testnet-store"))
        return 10;
    try {
        const bool testnet = std::string_view{argv[2]} == "testnet-store";
        const bool crash_fixture = std::string_view{argv[2]} == "store" || testnet;
        std::error_code error;
        const auto root = std::filesystem::canonical(argv[1], error);
        const auto temporary =
            std::filesystem::canonical(std::filesystem::temp_directory_path(), error);
        if (error || root.parent_path() != temporary ||
            !root.filename().string().starts_with(crash_fixture ? "novacoin-generation-crash-"
                                                                : "novacoin-durable-reorg-"))
            return 11;
        std::ifstream marker{root / "OWNED-TEST-FIXTURE", std::ios::binary};
        const std::string_view expected =
            crash_fixture ? "NOVA-GENERATION-CRASH-FIXTURE\n" : "NOVA-DURABLE-REORG-FIXTURE\n";
        std::array<char, 32U> marker_text{};
        marker.read(marker_text.data(), static_cast<std::streamsize>(expected.size()));
        if (marker.gcount() != static_cast<std::streamsize>(expected.size()) ||
            std::string_view{marker_text.data(), expected.size()} != expected ||
            marker.peek() != EOF)
            return 11;
        const auto& network = testnet ? nova::consensus::TestnetNetworkParams()
                                      : nova::consensus::RegtestNetworkParams();
        nova::storage::GenerationError store_error{};
        const auto store_path = crash_fixture ? root / "store" : root / argv[2] / "generations";
        auto store = nova::storage::GenerationStore::Open(store_path, network, store_error);
        if (!store)
            return 12;
        const auto observation = store->Observe();
        if (observation.state != nova::storage::GenerationState::kPresent)
            return 13;
        const std::string_view password = crash_fixture ? "test" : "disposable-wallet-passphrase";
        const auto bytes =
            std::span{reinterpret_cast<const std::uint8_t*>(password.data()), password.size()};
        auto recovered = nova::node::RecoverGeneration(
            *store, network.id, std::make_shared<nova::chain::SystemValidationTimeSource>(),
            network.genesis_block.header.time + 600U, bytes);
        if (recovered.error != nova::node::GenerationRecoveryError::kNone || !recovered.recovered)
            return 14;
        if (argc == 4) {
            const auto& chain = recovered.recovered->chain();
            const auto tip = chain.GetBlockIndex(chain.active_tip());
            if (!tip || !recovered.mempool)
                return 22;
            std::cout << "state-v1\nheight=" << chain.active_height() << ";tip=";
            Hex(chain.active_tip().bytes());
            std::cout << ";work=";
            Hex(tip->chain_work.bytes());
            std::cout << ";next-time=" << recovered.recovered->next_time() << '\n';
            const auto active = chain.GetActiveBlockIndexes();
            if (!active)
                return 22;
            for (const auto& index : *active) {
                std::cout << "active=" << index.height << ':';
                Hex(index.hash.bytes());
                std::cout << ':';
                Hex(index.target.bytes());
                std::cout << ':';
                Hex(index.per_block_work.bytes());
                std::cout << ':';
                Hex(index.chain_work.bytes());
                std::cout << '\n';
            }
            auto& wallet = recovered.recovered->wallet();
            auto addresses = wallet.ListReceivingAddresses();
            std::sort(addresses.begin(), addresses.end(),
                      [](const auto& a, const auto& b) { return a.index < b.index; });
            std::cout << "wallet=" << wallet.ConfirmedBalance() << ':'
                      << wallet.UnconfirmedBalance() << ':' << wallet.NextReceivingAddressIndex()
                      << ':' << addresses.size() << '\n';
            for (const auto& address : addresses) {
                std::cout << "public-key=" << address.index << ':';
                Hex(address.public_key_hash);
                std::cout << ':';
                const auto key = address.public_key.SerializeCompressed();
                Hex(key);
                std::cout << '\n';
            }
            auto coins = wallet.ListUTXOs();
            std::sort(coins.begin(), coins.end(),
                      [](const auto& a, const auto& b) { return a.key < b.key; });
            for (const auto& coin : coins) {
                std::cout << "wallet-coin=";
                Hex(coin.key.transaction_id.bytes());
                std::cout << ':' << coin.key.output_index << ':' << coin.output.value << ':'
                          << coin.height << ':' << coin.is_coinbase << ':' << coin.confirmed << ':'
                          << coin.pending_spend << ':';
                Hex(coin.output.script_pubkey);
                std::cout << '\n';
            }
            std::cout << "pending=" << recovered.mempool->size() << ':'
                      << recovered.mempool->total_serialized_size() << ':';
            if (observation.snapshot.mempool) {
                std::ifstream pending{*observation.snapshot.mempool, std::ios::binary};
                const auto image =
                    nova::chain::ReadMempoolImage(pending, recovered.mempool->parameters());
                const auto records =
                    image ? nova::chain::DecodeMempoolImage(*image, recovered.mempool->parameters())
                          : std::nullopt;
                if (!records || records->size() != recovered.mempool->size())
                    return 23;
                for (const auto& record : *records) {
                    const auto id =
                        record.transaction.TxId(network.block_limits.transaction_limits);
                    if (!id || !recovered.mempool->Contains(*id))
                        return 23;
                }
                Hex(*image);
            } else if (recovered.mempool->size() != 0U) {
                return 23;
            }
            std::cout << "\nutxos\n";
        }
        std::ifstream input{observation.snapshot.journal, std::ios::binary};
        const auto journal = nova::storage::ReadBlockJournal(input, network.block_limits);
        if (journal.error != nova::storage::BlockJournalError::kNone || journal.recovery_required)
            return 15;
        std::set<nova::chain::UTXOKey> candidates;
        if (testnet) {
            for (const auto& tx : network.genesis_block.transactions) {
                const auto id = tx.TxId(network.block_limits.transaction_limits);
                if (!id || tx.outputs.size() > std::numeric_limits<std::uint32_t>::max())
                    return 16;
                for (std::size_t index = 0; index < tx.outputs.size(); ++index)
                    candidates.insert({*id, static_cast<std::uint32_t>(index)});
            }
        }
        for (const auto& block : journal.blocks) {
            for (const auto& tx : block.transactions) {
                const auto id = tx.TxId(network.block_limits.transaction_limits);
                if (!id || tx.outputs.size() > std::numeric_limits<std::uint32_t>::max())
                    return 16;
                for (std::size_t index = 0; index < tx.outputs.size(); ++index)
                    candidates.insert({*id, static_cast<std::uint32_t>(index)});
            }
        }
        std::size_t found{};
        std::int64_t total{};
        for (const auto& key : candidates) {
            const auto read = recovered.recovered->utxos().ReadCoin(key);
            if (read.error != nova::chain::CoinReadError::kNone)
                return 17;
            if (!read.coin)
                continue;
            if (read.coin->output.value < 0 ||
                read.coin->output.value > std::numeric_limits<std::int64_t>::max() - total)
                return 18;
            total += read.coin->output.value;
            ++found;
            Hex(key.transaction_id.bytes());
            std::cout << ':' << key.output_index << ':' << read.coin->output.value << ':'
                      << read.coin->height << ':' << (read.coin->is_coinbase ? 1 : 0) << ':';
            Hex(read.coin->output.script_pubkey);
            std::cout << '\n';
        }
        if (found != recovered.recovered->utxos().size())
            return 19;
        std::cout << "count=" << found << ";value=" << total << '\n';
        return std::cout ? 0 : 20;
    } catch (...) {
        return 21;
    }
}
