// Test-only child; no installation or package-publication rule is added.
#include "consensus/monetary.hpp"
#include "consensus/pow.hpp"
#include "daemon/durable_runtime.hpp"
#include "node/durable_session.hpp"
#include "node/generation_migration.hpp"
#include "node/mining_helpers.hpp"
#include "node/regtest_node.hpp"
#include "storage/generation_store.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

int main(const int argc, char** argv)
{
    if (argc != 3)
        return 10;
    try {
        std::string_view mode{argv[1]};
        const bool testnet_mode = mode.starts_with("testnet-");
        if (testnet_mode)
            mode.remove_prefix(8U);
        const bool runtime_mode = mode.starts_with("runtime-");
        if (runtime_mode)
            mode.remove_prefix(8U);
        const bool migration_mode = mode.starts_with("migration-");
        if (migration_mode)
            mode.remove_prefix(10U);
        const std::uint32_t reorg_depth = mode.starts_with("reorg100-")  ? 100U
                                          : mode.starts_with("reorg10-") ? 10U
                                                                         : 1U;
        const bool reorg_mode = mode.starts_with("reorg-") || reorg_depth != 1U;
        if (reorg_mode)
            mode.remove_prefix(reorg_depth == 100U ? 9U : reorg_depth == 10U ? 8U : 6U);
        if (testnet_mode && reorg_mode)
            return 10;
        const bool session_mode = runtime_mode || reorg_mode || mode.starts_with("session-");
        if (session_mode && !reorg_mode && !runtime_mode)
            mode.remove_prefix(8U);
        const bool mine_mode = session_mode && mode.starts_with("mine-");
        if (mine_mode)
            mode.remove_prefix(5U);
        const bool pay_mode = session_mode && mode.starts_with("pay-");
        if (pay_mode)
            mode.remove_prefix(4U);
        const bool admit_mode = session_mode && mode.starts_with("admit-");
        if (admit_mode)
            mode.remove_prefix(6U);
        const bool block_mode = session_mode && testnet_mode && mode.starts_with("block-");
        if (block_mode)
            mode.remove_prefix(6U);
        const bool with_mempool = session_mode || mode.starts_with("v2-");
        if (!session_mode && with_mempool)
            mode.remove_prefix(3U);
        const std::filesystem::path fixture{argv[2]};
        if (!fixture.is_absolute() ||
            fixture.filename().string().find("novacoin-generation-crash-") != 0U ||
            !std::filesystem::is_regular_file(
                std::filesystem::symlink_status(fixture / "OWNED-TEST-FIXTURE")))
            return 11;
        std::ifstream marker{fixture / "OWNED-TEST-FIXTURE", std::ios::binary};
        constexpr std::string_view expected_marker{"NOVA-GENERATION-CRASH-FIXTURE\n"};
        std::array<char, expected_marker.size()> ownership{};
        marker.read(ownership.data(), static_cast<std::streamsize>(ownership.size()));
        if (marker.gcount() != static_cast<std::streamsize>(ownership.size()) ||
            std::string_view{ownership.data(), ownership.size()} != expected_marker ||
            marker.peek() != EOF)
            return 11;
        using Point = nova::storage::GenerationFailurePoint;
        if (mode.starts_with("startup-") && !session_mode && !migration_mode) {
            mode.remove_prefix(8U);
            constexpr std::array startup_points{
                std::pair{"partial", Point::kPartialRoot},
                std::pair{"close", Point::kRootClose},
                std::pair{"prepare-barrier", Point::kPrepareDirectoryBarrier},
                std::pair{"before-root", Point::kBeforeRootReplace},
                std::pair{"after-root", Point::kAfterRootReplace},
                std::pair{"barrier", Point::kDirectoryBarrier},
                std::pair{"recovery-barrier", Point::kRecoveryDirectoryBarrier}};
            for (const auto& [name, startup_point] : startup_points) {
                if (mode != name)
                    continue;
                nova::storage::GenerationError startup_error{};
                const auto opened = nova::storage::GenerationStore::OpenForTesting(
                    fixture / "store",
                    testnet_mode ? nova::consensus::TestnetNetworkParams()
                                 : nova::consensus::RegtestNetworkParams(),
                    startup_error, startup_point, nova::storage::GenerationIoFault::kNone,
                    [](Point) noexcept { std::_Exit(42); });
                return opened ? 20 : 21; // Selected checkpoint must terminate the child.
            }
            return 10;
        }
        constexpr std::array points{
            std::pair{"normal", Point::kNone},
            std::pair{"before-journal", Point::kBeforeJournal},
            std::pair{"before-wallet", Point::kBeforeWallet},
            std::pair{"before-mempool", Point::kBeforeMempool},
            std::pair{"before-descriptor", Point::kBeforeDescriptor},
            std::pair{"partial-journal", Point::kPartialJournal},
            std::pair{"partial-wallet", Point::kPartialWallet},
            std::pair{"partial-mempool", Point::kPartialMempool},
            std::pair{"partial-descriptor", Point::kPartialDescriptor},
            std::pair{"partial-root", Point::kPartialRoot},
            std::pair{"journal-close", Point::kJournalClose},
            std::pair{"wallet-close", Point::kWalletClose},
            std::pair{"mempool-close", Point::kMempoolClose},
            std::pair{"descriptor-close", Point::kDescriptorClose},
            std::pair{"root-close", Point::kRootClose},
            std::pair{"journal", Point::kAfterJournal},
            std::pair{"wallet", Point::kAfterWallet},
            std::pair{"mempool", Point::kAfterMempool},
            std::pair{"descriptor", Point::kAfterDescriptor},
            std::pair{"before-root", Point::kBeforeRootReplace},
            std::pair{"after-root", Point::kAfterRootReplace},
            std::pair{"barrier", Point::kDirectoryBarrier},
        };
        Point point{};
        bool found = mode == "inspect" ||
                     (reorg_mode &&
                      (mode == "payment" || mode == "reject" || mode.starts_with("replay-"))) ||
                     ((session_mode || migration_mode) && mode == "init");
        for (const auto& [name, value] : points) {
            if (mode == name) {
                point = value;
                found = true;
            }
        }
        if (!found || (!with_mempool && point == Point::kAfterMempool))
            return 10;
        nova::storage::GenerationError error{};
        const auto& selected_network = testnet_mode ? nova::consensus::TestnetNetworkParams()
                                                    : nova::consensus::RegtestNetworkParams();
        auto store =
            nova::storage::GenerationStore::Open(fixture / "store", selected_network, error);
        if (!store)
            return 12;
        if (migration_mode) {
            const auto time = std::make_shared<nova::chain::SystemValidationTimeSource>();
            constexpr std::array<std::uint8_t, 4U> passphrase{'t', 'e', 's', 't'};
            constexpr std::uint32_t start_time = 1'704'067'800U;
            if (mode == "init") {
                nova::node::RegtestNodeConfig config{"source",
                                                     fixture / "source",
                                                     fixture / "logs" / "node.log",
                                                     18444U,
                                                     18443U,
                                                     start_time,
                                                     {passphrase.begin(), passphrase.end()},
                                                     nova::consensus::NetworkId::kRegtest};
                auto node = nova::node::RegtestNode::Create(std::move(config));
                if (!node || node->MineBlock().error != nova::node::RegtestNodeError::kNone)
                    return 23;
                std::cout << node->height() << ' ';
                constexpr char hex[] = "0123456789abcdef";
                for (const auto byte : node->tip().bytes())
                    std::cout << hex[byte >> 4U] << hex[byte & 15U];
                std::cout << ' ' << node->wallet().ConfirmedBalance() << '\n';
                return 0;
            }
            if (mode == "inspect") {
                const auto observed = store->Observe();
                if (observed.state == nova::storage::GenerationState::kEmpty) {
                    std::cout << "empty\n";
                    return 0;
                }
                auto recovered = nova::node::RecoverGeneration(
                    *store, nova::consensus::NetworkId::kRegtest, time, start_time, passphrase);
                if (recovered.error != nova::node::GenerationRecoveryError::kNone ||
                    !recovered.recovered)
                    return 24;
                std::cout << recovered.recovered->chain().active_height() << ' ';
                constexpr char hex[] = "0123456789abcdef";
                for (const auto byte : recovered.recovered->chain().active_tip().bytes())
                    std::cout << hex[byte >> 4U] << hex[byte & 15U];
                std::cout << ' ' << recovered.recovered->wallet().ConfirmedBalance() << '\n';
                return 0;
            }
            if (point != Point::kNone) {
                store->SetFailureHookForTesting([](Point) noexcept { std::_Exit(42); });
                store->FailNextForTesting(point);
            }
            auto prepared = nova::node::PrepareLegacyMigration(
                *store, nova::consensus::NetworkId::kRegtest, time, start_time, passphrase,
                fixture / "source" / "blocks.dat", fixture / "source" / "wallet.dat",
                fixture / "validation");
            if (prepared.error != nova::node::MigrationError::kNone || !prepared.prepared)
                return 25;
            const auto result = nova::node::CommitLegacyMigration(
                *prepared.prepared, *store, nova::consensus::NetworkId::kRegtest, time, start_time,
                passphrase);
            return result.state == nova::storage::GenerationCommitState::kCommitted ? 0 : 26;
        }
        if (session_mode) {
            const auto time = std::make_shared<nova::chain::SystemValidationTimeSource>();
            constexpr std::array<std::uint8_t, 4U> passphrase{'t', 'e', 's', 't'};
            const std::uint32_t start_time = selected_network.genesis_block.header.time + 600U;
            if (mode == "init") {
                auto session = nova::node::DurableSession::CreateFresh(
                    std::move(store), selected_network.id, time, start_time, passphrase,
                    fixture / "fresh");
                if (!session)
                    return 17;
                if (!reorg_mode)
                    return 0;
                const auto first = session->MineBlock({}, fixture / "first", passphrase);
                if (first.error != nova::node::DurableSessionError::kNone ||
                    !first.chain_result.block_hash)
                    return 27;
                const auto first_index = session->FindBlock(*first.chain_result.block_hash);
                if (!first_index.block || !first_index.block->block)
                    return 27;
                const auto& limits = nova::consensus::RegtestNetworkParams().block_limits;
                const std::array<nova::wallet::Recipient, 1U> recipients{
                    nova::wallet::Recipient{nova::crypto::Hash160{}, 1}};
                const auto payment =
                    session->BuildAndAdmit(recipients, fixture / "payment", passphrase);
                if (payment.error != nova::node::DurableSessionError::kNone || !payment.transaction)
                    return 27;
                const auto payment_id = payment.transaction->TxId(limits.transaction_limits);
                if (!payment_id)
                    return 27;
                auto active = *first_index.block->block;
                active.header.previous_block_id = *first.chain_result.block_hash;
                active.header.time += 1U;
                active.transactions.front().inputs.front().script_sig =
                    nova::node::CoinbaseHeightScript(2U);
                active.transactions.push_back(*payment.transaction);
                const auto active_base = active;
                const auto choose_proof = [](nova::primitives::Block& candidate, bool low_hash) {
                    for (std::uint32_t delta = 0; delta < 1024U; ++delta) {
                        auto header = candidate.header;
                        header.time += delta;
                        const auto proof = nova::consensus::MineRegtestBlock(header, 4096U);
                        if (!proof)
                            return false;
                        if ((proof->block_hash.bytes().front() < 128U) == low_hash) {
                            candidate.header = proof->header;
                            return true;
                        }
                    }
                    return false;
                };
                const auto set_height = [&](nova::primitives::Block& candidate,
                                            std::uint32_t height) {
                    const auto subsidy =
                        nova::consensus::GetBlockSubsidy(height, selected_network.monetary);
                    if (!subsidy)
                        return false;
                    candidate.transactions.front().inputs.front().script_sig =
                        nova::node::CoinbaseHeightScript(height);
                    candidate.transactions.front().outputs.front().value = *subsidy;
                    const auto merkle = nova::primitives::ComputeMerkleRoot(
                        candidate.transactions, limits.transaction_limits);
                    if (!merkle)
                        return false;
                    candidate.header.merkle_root = *merkle;
                    return true;
                };
                for (std::uint32_t height = 2U; height <= reorg_depth + 1U; ++height) {
                    if (height != 2U) {
                        const auto parent = nova::primitives::ComputeBlockHash(active.header);
                        if (!parent)
                            return 27;
                        active.header.previous_block_id = *parent;
                        active.header.time += 1U;
                        active.transactions.resize(1U);
                    }
                    if (!set_height(active, height) || !choose_proof(active, true))
                        return 27;
                    const auto accepted = session->ApplyBlock(
                        active, {}, fixture / ("active-" + std::to_string(height)), passphrase);
                    if (accepted.error != nova::node::DurableSessionError::kNone ||
                        !accepted.chain_result.became_active || !session->View() ||
                        session->View()->height != height)
                        return 27;
                }
                auto side = active_base;
                side.transactions.resize(1U);
                side.transactions.front().outputs.front().script_pubkey[3] ^= 1U;
                for (std::uint32_t height = 2U; height <= reorg_depth + 1U; ++height) {
                    if (height != 2U) {
                        const auto parent = nova::primitives::ComputeBlockHash(side.header);
                        if (!parent)
                            return 27;
                        side.header.previous_block_id = *parent;
                        side.header.time += 1U;
                    }
                    if (!set_height(side, height) || !choose_proof(side, false))
                        return 27;
                    const auto accepted = session->ApplyBlock(
                        side, {}, fixture / ("side-" + std::to_string(height)), passphrase);
                    if (accepted.error != nova::node::DurableSessionError::kNone ||
                        accepted.chain_result.became_active || !session->View() ||
                        session->View()->height != reorg_depth + 1U)
                        return 27;
                }
                auto winning = side;
                const auto side_hash = nova::primitives::ComputeBlockHash(side.header);
                if (!side_hash)
                    return 27;
                winning.header.previous_block_id = *side_hash;
                winning.header.time += 1U;
                if (!set_height(winning, reorg_depth + 2U))
                    return 27;
                const auto proof = nova::consensus::MineRegtestBlock(winning.header, 4096U);
                if (!proof)
                    return 27;
                winning.header = proof->header;
                nova::primitives::BinaryWriter writer;
                if (!winning.Serialize(writer, limits))
                    return 27;
                std::ofstream output{fixture / "winning.block", std::ios::binary};
                output.write(reinterpret_cast<const char*>(writer.bytes().data()),
                             static_cast<std::streamsize>(writer.bytes().size()));
                output.close();
                if (!output)
                    return 27;
                constexpr char hex[] = "0123456789abcdef";
                std::ofstream payment_file{fixture / "payment.id", std::ios::binary};
                for (const auto byte : payment_id->bytes())
                    payment_file << hex[byte >> 4U] << hex[byte & 15U];
                payment_file.close();
                if (!payment_file)
                    return 27;
                for (const auto byte : proof->block_hash.bytes())
                    std::cout << hex[byte >> 4U] << hex[byte & 15U];
                std::cout << '\n';
                return 0;
            }
            if (reorg_mode && mode.starts_with("replay-")) {
                using Checkpoint = nova::node::GenerationRecoveryCheckpoint;
                const nova::node::GenerationRecoveryObserver observer{
                    &mode, [](const void* context, Checkpoint checkpoint, std::size_t completed,
                              std::size_t total) noexcept {
                        const auto selected = *static_cast<const std::string_view*>(context);
                        const bool block = checkpoint == Checkpoint::kAfterBlock && total > 2U;
                        if ((block && ((selected == "replay-first" && completed == 1U) ||
                                       (selected == "replay-middle" &&
                                        completed == total / 2U + total % 2U) ||
                                       (selected == "replay-last" && completed == total))) ||
                            (selected == "replay-before" &&
                             checkpoint == Checkpoint::kBeforeReplay) ||
                            (selected == "replay-before-wallet" &&
                             checkpoint == Checkpoint::kBeforeWallet) ||
                            (selected == "replay-wallet" &&
                             checkpoint == Checkpoint::kAfterWallet) ||
                            (selected == "replay-before-mempool" &&
                             checkpoint == Checkpoint::kBeforeMempool) ||
                            (selected == "replay-mempool" &&
                             checkpoint == Checkpoint::kAfterMempool) ||
                            (selected == "replay-admission" &&
                             checkpoint == Checkpoint::kBeforeAdmission)) {
                            std::cout << "recovery-checkpoint=" << selected
                                      << ";completed=" << completed << ";total=" << total << '\n'
                                      << std::flush;
                            std::_Exit(42);
                        }
                    }};
                const auto recovered = nova::node::RecoverGeneration(
                    *store, selected_network.id, time, start_time, passphrase, observer);
                return recovered.error == nova::node::GenerationRecoveryError::kNone ? 35 : 36;
            }
            if (mode != "inspect" && mode != "payment" && point != Point::kNone) {
                store->SetFailureHookForTesting([](Point) noexcept { std::_Exit(42); });
                store->FailNextForTesting(point);
            }
            auto session = nova::node::DurableSession::Open(std::move(store), selected_network.id,
                                                            time, start_time, passphrase);
            if (!session)
                return 18;
            if (mode == "inspect") {
                const auto view = session->View();
                const auto wallet = session->WalletView();
                if (!view || !wallet)
                    return 19;
                std::cout << view->height << ' ' << view->receiving_address_count << ' '
                          << view->mempool_size << ' ' << view->confirmed_balance << ' ';
                constexpr char hex[] = "0123456789abcdef";
                for (const auto byte : view->tip.bytes())
                    std::cout << hex[byte >> 4U] << hex[byte & 15U];
                const auto pending =
                    std::count_if(wallet->coins.begin(), wallet->coins.end(),
                                  [](const auto& coin) { return coin.pending_spend; });
                std::cout << ' ' << wallet->unconfirmed << ' ' << wallet->coins.size() << ' '
                          << pending << '\n';
                return 0;
            }
            if (reorg_mode && mode == "payment") {
                std::ifstream file{fixture / "payment.id", std::ios::binary};
                std::array<char, 64U> encoded{};
                file.read(encoded.data(), static_cast<std::streamsize>(encoded.size()));
                if (file.gcount() != static_cast<std::streamsize>(encoded.size()) ||
                    file.peek() != EOF)
                    return 30;
                const auto id = nova::crypto::Hash256::FromHex(
                    std::string_view{encoded.data(), encoded.size()});
                if (!id)
                    return 30;
                const auto entry = session->FindMempoolEntry(*id);
                if (!entry.available)
                    return 30;
                std::cout << (entry.entry ? "present" : "absent") << '\n';
                return 0;
            }
            if (reorg_mode) {
                const auto limits = nova::consensus::RegtestNetworkParams().block_limits;
                std::error_code file_error;
                const auto size = std::filesystem::file_size(fixture / "winning.block", file_error);
                if (file_error || size > limits.max_serialized_size)
                    return 28;
                std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
                std::ifstream input{fixture / "winning.block", std::ios::binary};
                input.read(reinterpret_cast<char*>(bytes.data()),
                           static_cast<std::streamsize>(bytes.size()));
                if (!input || input.peek() != EOF)
                    return 28;
                const auto block = nova::primitives::Block::Deserialize(bytes, limits);
                if (!block)
                    return 28;
                auto candidate = *block;
                if (mode == "reject") {
                    if (candidate.transactions.empty() ||
                        candidate.transactions.front().outputs.empty())
                        return 28;
                    candidate.transactions.front().outputs.front().value = -1;
                }
                const auto result =
                    session->ApplyBlock(candidate, {}, fixture / "reorg-stage", passphrase);
                if (mode == "reject")
                    return result.error == nova::node::DurableSessionError::kChain &&
                                   !result.chain_result.became_active
                               ? 0
                               : 29;
                return result.error == nova::node::DurableSessionError::kNone &&
                               result.chain_result.became_active
                           ? 0
                           : 29;
            }
            if (runtime_mode && mine_mode) {
                std::error_code workspace_error;
                if (!std::filesystem::create_directory(fixture / "runtime-workspace",
                                                       workspace_error) ||
                    workspace_error)
                    return 33;
                auto workspace = nova::node::StagingWorkspace::Open(fixture / "runtime-workspace");
                const nova::rpc::RpcConfig config{"127.0.0.1",
                                                  "user",
                                                  "pass",
                                                  {256U, 8192U, 4096U},
                                                  nova::consensus::NetworkId::kRegtest};
                auto runtime = nova::daemon::DurableRuntime::Create(
                    std::move(session), std::move(workspace), passphrase, config);
                if (!runtime)
                    return 33;
                const auto result = runtime->Rpc().HandleHttpPost(
                    {"POST", "Basic dXNlcjpwYXNz",
                     R"({"jsonrpc":"2.0","id":1,"method":"generateregtestblock"})"});
                return mode == "normal" && result.body.find("\"error\"") == std::string::npos ? 0
                                                                                              : 34;
            }
            if (block_mode) {
                const auto subsidy =
                    nova::consensus::GetBlockSubsidy(1U, selected_network.monetary);
                if (!subsidy)
                    return 31;
                nova::primitives::Transaction coinbase{
                    1,
                    {{nova::primitives::OutPoint{nova::crypto::Hash256{},
                                                 std::numeric_limits<std::uint32_t>::max()},
                      nova::node::CoinbaseHeightScript(1U), 0U}},
                    {{*subsidy, {0x51U}}},
                    0U};
                nova::primitives::Block block{{1,
                                               selected_network.genesis_hash,
                                               {},
                                               start_time,
                                               selected_network.pow.pow_limit_compact,
                                               0U},
                                              {coinbase}};
                const auto merkle = nova::primitives::ComputeMerkleRoot(
                    block.transactions, selected_network.block_limits.transaction_limits);
                if (!merkle)
                    return 31;
                block.header.merkle_root = *merkle;
                bool found_hash = false;
                for (std::uint32_t nonce = 0U; nonce < 4096U; ++nonce) {
                    block.header.nonce = nonce;
                    const auto hash = nova::primitives::ComputeBlockHash(block.header);
                    if (hash &&
                        nova::consensus::CheckProofOfWork(*hash, selected_network.pow.pow_limit)) {
                        found_hash = true;
                        break;
                    }
                }
                if (!found_hash)
                    return 31;
                const auto result =
                    session->ApplyBlock(block, {}, fixture / "testnet-block", passphrase);
                return mode == "normal" && result.error == nova::node::DurableSessionError::kNone
                           ? 0
                           : 32;
            }
            if (mine_mode) {
                const auto block = session->MineBlock(
                    {}, fixture / ("session-mine-" + std::string{mode}), passphrase);
                return mode == "normal" && block.error == nova::node::DurableSessionError::kNone
                           ? 0
                           : 21;
            }
            if (admit_mode) {
                const std::array<nova::wallet::Recipient, 1U> recipients{
                    nova::wallet::Recipient{nova::crypto::Hash160{}, 1}};
                const auto built = session->BuildTransaction(recipients);
                if (!built || !built->built)
                    return 22;
                const auto result = session->AdmitTransaction(
                    built->built->transaction, fixture / "session-admit", passphrase);
                return mode == "normal" && result == nova::node::DurableSessionError::kNone ? 0
                                                                                            : 22;
            }
            if (pay_mode) {
                const std::array<nova::wallet::Recipient, 1U> recipients{
                    nova::wallet::Recipient{nova::crypto::Hash160{}, 1}};
                const auto payment = session->BuildAndAdmit(
                    recipients, fixture / ("session-pay-" + std::string{mode}), passphrase);
                return mode == "normal" && payment.error == nova::node::DurableSessionError::kNone
                           ? 0
                           : 22;
            }
            const auto address = session->GenerateAddress(fixture / "session-stage", passphrase);
            return mode == "normal" && address.error == nova::node::DurableSessionError::kNone ? 0
                                                                                               : 20;
        }
        if (mode == "inspect") {
            const auto observed = store->Observe();
            if (observed.state == nova::storage::GenerationState::kUnavailable)
                return 13;
            std::cout << observed.snapshot.sequence << '\n';
            return 0;
        }
        if (point != Point::kNone)
            store->SetFailureHookForTesting([](Point) noexcept { std::_Exit(42); });
        store->FailNextForTesting(point);
        auto prepared =
            with_mempool
                ? store->Prepare(fixture / "input.journal", fixture / "input.wallet",
                                 fixture / "input.mempool", error)
                : store->Prepare(fixture / "input.journal", fixture / "input.wallet", error);
        if (!prepared) {
            return 14;
        }
        const auto result = store->Commit(*prepared);
        if (point == Point::kNone &&
            result.state == nova::storage::GenerationCommitState::kCommitted)
            std::_Exit(0);
        return 15;
    } catch (...) {
        return 16;
    }
}
