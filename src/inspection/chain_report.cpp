#include "inspection/chain_report.hpp"

#include <limits>
#include <map>
#include <memory>

namespace nova::inspection
{
namespace
{
class SnapshotTime final : public chain::ValidationTimeSource
{
  public:
    explicit SnapshotTime(std::uint32_t value) noexcept : value_(value) {}
    std::uint32_t Now() const noexcept override
    {
        return value_;
    }

  private:
    const std::uint32_t value_;
};

bool IsHex(const std::string_view text, const std::size_t size)
{
    return text.size() == size &&
           text.find_first_not_of("0123456789abcdef") == std::string_view::npos;
}

std::string Field(const std::string_view name, const std::string_view value)
{
    return "\"" + std::string{name} + "\":\"" + std::string{value} + "\"";
}
} // namespace

std::string Hex(const std::span<const std::uint8_t> bytes)
{
    constexpr std::string_view digits{"0123456789abcdef"};
    std::string result;
    result.reserve(bytes.size() * 2U);
    for (const auto byte : bytes) {
        result.push_back(digits[byte >> 4U]);
        result.push_back(digits[byte & 15U]);
    }
    return result;
}

bool CheckedAdd(std::int64_t& total, const std::int64_t delta) noexcept
{
    if ((delta > 0 && total > std::numeric_limits<std::int64_t>::max() - delta) ||
        (delta < 0 && total < std::numeric_limits<std::int64_t>::min() - delta))
        return false;
    total += delta;
    return true;
}

InspectionResult InspectJournal(std::istream& journal, const InspectionContext& context,
                                const InspectionLimits limits) noexcept
{
    try {
        if ((context.network != consensus::NetworkId::kRegtest &&
             context.network != consensus::NetworkId::kTestnet) ||
            !IsHex(context.source_revision, 40U) || !IsHex(context.source_tree, 40U) ||
            !IsHex(context.provenance_hash, 64U) || !IsHex(context.identity_hash, 64U) ||
            !IsHex(context.journal_hash, 64U) || !IsHex(context.metadata_hash, 64U) ||
            !IsHex(context.binary_hash, 64U))
            return {"context"};
        const auto& network = consensus::GetNetworkParams(context.network);
        if (context.as_of < network.genesis_block.header.time)
            return {"context_time"};
        auto state = chain::NetworkChainInitializer::Create(
            context.network, std::make_shared<SnapshotTime>(context.as_of));
        if (!state.has_value())
            return {"initialization"};
        std::int64_t committed_initial = 0;
        std::size_t initial_count = 0U;
        if (context.network == consensus::NetworkId::kTestnet) {
            for (const auto& tx : network.genesis_block.transactions) {
                const auto id = tx.TxId(network.block_limits.transaction_limits);
                if (!id.has_value())
                    return {"initialization"};
                for (std::size_t i = 0; i < tx.outputs.size(); ++i) {
                    const auto coin = state->utxos->GetCoin({*id, static_cast<std::uint32_t>(i)});
                    if (!coin.has_value() || coin->height != 0U || !coin->is_coinbase ||
                        coin->output.value != tx.outputs[i].value ||
                        coin->output.script_pubkey != tx.outputs[i].script_pubkey ||
                        !CheckedAdd(committed_initial, coin->output.value))
                        return {"initialization_mismatch"};
                    ++initial_count;
                }
            }
        }
        if (initial_count != state->utxos->size())
            return {"initialization_mismatch"};
        const auto decoded =
            storage::ReadBlockJournal(journal, network.block_limits, limits.journal);
        if (decoded.error != storage::BlockJournalError::kNone)
            return {"journal_" + std::to_string(static_cast<unsigned>(decoded.error))};
        if (decoded.recovery_required)
            return {"incomplete_journal"};
        for (const auto& block : decoded.blocks) {
            const auto accepted = state->chain->AcceptBlock(block);
            if (accepted.error != chain::ChainError::kNone)
                return {"replay_" + std::to_string(static_cast<unsigned>(accepted.error)) + "_" +
                        std::to_string(static_cast<unsigned>(accepted.validation_error))};
        }
        const auto tip = state->chain->GetBlockIndex(state->chain->active_tip());
        if (!tip.has_value() || state->chain->active_tip() != context.expected_tip ||
            state->chain->active_height() != context.expected_height ||
            tip->chain_work != context.expected_work)
            return {"snapshot_expectation"};
        const auto active = state->chain->GetActiveBlockIndexes();
        if (!active.has_value())
            return {"active_export"};
        std::map<chain::UTXOKey, chain::Coin> origins;
        std::int64_t initial = 0, subsidy = 0, fees = 0, coinbase = 0, underclaim = 0, expected = 0;
        std::optional<chain::UTXOKey> creator;
        std::string creator_script, creator_spend;
        if (context.network == consensus::NetworkId::kTestnet) {
            const auto& genesis = network.genesis_block.transactions.front();
            const auto id = genesis.TxId(network.block_limits.transaction_limits);
            if (!id.has_value())
                return {"initialization"};
            creator = chain::UTXOKey{*id, 0U};
            creator_script = Hex(genesis.outputs.front().script_pubkey);
        }
        for (const auto& index : *active) {
            if (!index.block.has_value()) {
                if (index.height != 0U || context.network != consensus::NetworkId::kRegtest)
                    return {"missing_active_body"};
                continue;
            }
            if (index.height > 0U) {
                if (!index.connected.has_value())
                    return {"missing_validated_fees"};
                const auto s = consensus::GetBlockSubsidy(index.height, network.monetary);
                std::int64_t c = 0;
                for (const auto& output : index.block->transactions.front().outputs) {
                    if (!CheckedAdd(c, output.value))
                        return {"accounting_overflow"};
                }
                if (!s.has_value())
                    return {"subsidy"};
                const auto f = index.connected->total_fees;
                auto d = *s;
                if (!CheckedAdd(d, f) || !CheckedAdd(d, -c) || d < 0 || !CheckedAdd(subsidy, *s) ||
                    !CheckedAdd(fees, f) || !CheckedAdd(coinbase, c) ||
                    !CheckedAdd(underclaim, d) || !CheckedAdd(expected, c - f))
                    return {"accounting_overflow"};
            }
            for (const auto& tx : index.block->transactions) {
                const auto id = tx.TxId(network.block_limits.transaction_limits);
                if (!id.has_value())
                    return {"transaction_id"};
                if (creator.has_value() && index.height > 0U) {
                    for (const auto& input : tx.inputs) {
                        if (chain::UTXOKey::FromOutPoint(input.previous_output) == *creator)
                            creator_spend = Hex(id->bytes());
                    }
                }
                for (std::size_t i = 0; i < tx.outputs.size(); ++i) {
                    if (origins.size() >= limits.keys)
                        return {"resource_keys"};
                    if (!origins
                             .emplace(chain::UTXOKey{*id, static_cast<std::uint32_t>(i)},
                                      chain::Coin{tx.outputs[i], index.height,
                                                  primitives::IsCoinbaseTransaction(tx)})
                             .second)
                        return {"ambiguous_origin"};
                    if (index.height == 0U && !CheckedAdd(initial, tx.outputs[i].value))
                        return {"accounting_overflow"};
                }
            }
        }
        if (!CheckedAdd(expected, initial))
            return {"accounting_overflow"};
        if (initial != committed_initial)
            return {"initialization_mismatch"};
        std::string listing;
        std::size_t count = 0U;
        std::int64_t total = 0, creator_balance = 0;
        bool creator_unspent = false;
        for (const auto& [key, origin] : origins) {
            const auto coin = state->utxos->GetCoin(key);
            if (!coin.has_value())
                continue;
            if (coin->output.value != origin.output.value ||
                coin->output.script_pubkey != origin.output.script_pubkey ||
                coin->height != origin.height || coin->is_coinbase != origin.is_coinbase)
                return {"coin_metadata"};
            if (!CheckedAdd(total, coin->output.value))
                return {"accounting_overflow"};
            const auto script = Hex(coin->output.script_pubkey);
            if (creator.has_value() && script == creator_script &&
                !CheckedAdd(creator_balance, coin->output.value))
                return {"accounting_overflow"};
            if (creator.has_value() && key == *creator)
                creator_unspent = true;
            const auto line =
                Hex(key.transaction_id.bytes()) + ":" + std::to_string(key.output_index) + ":" +
                std::to_string(coin->output.value) + ":" + std::to_string(coin->height) + ":" +
                (coin->is_coinbase ? "1:" : "0:") +
                std::to_string(coin->output.script_pubkey.size()) + ":" + script + "\n";
            if (line.size() > limits.output - listing.size())
                return {"resource_output"};
            listing += line;
            ++count;
        }
        if (count != state->utxos->size() || total != expected || total < 0 ||
            total > network.monetary.max_money)
            return {"accounting_mismatch"};
        auto schedule_balance = initial;
        if (!CheckedAdd(schedule_balance, subsidy) || !CheckedAdd(schedule_balance, -underclaim) ||
            schedule_balance != total)
            return {"accounting_mismatch"};
        if (creator.has_value() && !creator_unspent && creator_spend.empty())
            return {"creator_unexplained_absence"};
        const auto digest = crypto::Hash256::Sha256(
            std::span{reinterpret_cast<const std::uint8_t*>(listing.data()), listing.size()});
        if (!digest.has_value())
            return {"digest"};
        std::string report =
            "{" + Field("schema", "nova-offline-inspection-v1") + "," +
            Field("status", "OFFLINE_SNAPSHOT_VALID") +
            ",\"input_complete\":true,\"replay_valid_under_context\":true,\"accounting_balanced\":"
            "true,\"initialization_conformant\":true," +
            Field("source_revision", context.source_revision) + "," +
            Field("source_tree", context.source_tree) + "," +
            Field("binary_sha256", context.binary_hash) + "," +
            Field("provenance_sha256", context.provenance_hash) + "," +
            Field("identity_sha256", context.identity_hash) + "," +
            Field("journal_sha256", context.journal_hash) + "," +
            Field("metadata_sha256", context.metadata_hash) + "," +
            Field("identity_bytes", std::to_string(context.identity_bytes)) + "," +
            Field("journal_bytes", std::to_string(decoded.bytes_read)) + "," +
            Field("metadata_bytes", std::to_string(context.metadata_bytes)) + "," +
            Field("network",
                  context.network == consensus::NetworkId::kTestnet ? "testnet" : "regtest") +
            "," + Field("genesis", Hex(network.genesis_hash.bytes())) + "," +
            Field("height", std::to_string(context.expected_height)) + "," +
            Field("tip", Hex(context.expected_tip.bytes())) + "," +
            Field("chainwork", Hex(tip->chain_work.bytes())) + "," +
            Field("as_of", std::to_string(context.as_of)) + "," +
            Field("committed_blocks", std::to_string(decoded.blocks.size())) + "," +
            Field("active_blocks", std::to_string(state->chain->active_height())) + "," +
            Field("utxo_count", std::to_string(count)) + "," +
            Field("utxo_value", std::to_string(total)) + "," +
            Field("utxo_sha256", Hex(digest->bytes())) + "," +
            Field("initial_value", std::to_string(initial)) + "," +
            Field("scheduled_subsidy", std::to_string(subsidy)) + "," +
            Field("coinbase_payouts", std::to_string(coinbase)) + "," +
            Field("fees", std::to_string(fees)) + "," +
            Field("combined_underclaim", std::to_string(underclaim)) + "," +
            Field("net_supply_change", std::to_string(total - initial)) + "," +
            Field("creator_outpoint",
                  creator.has_value() ? Hex(creator->transaction_id.bytes()) + ":0" : "") +
            "," + Field("creator_initial_value", std::to_string(initial)) + "," +
            Field("creator_script", creator_script) + "," +
            Field("max_journal_bytes", std::to_string(limits.journal.bytes)) + "," +
            Field("max_records", std::to_string(limits.journal.records)) + "," +
            Field("max_committed_blocks", std::to_string(limits.journal.blocks)) + "," +
            Field("max_pending_prepares", std::to_string(limits.journal.pending)) + "," +
            Field("max_candidate_keys", std::to_string(limits.keys)) + "," +
            Field("max_output_bytes", std::to_string(limits.output)) + "," +
            Field("framed_records", std::to_string(decoded.record_count)) + "," +
            Field("candidate_keys", std::to_string(origins.size())) + "," +
            Field("max_money", std::to_string(network.monetary.max_money)) + "," +
            Field("coinbase_maturity",
                  std::to_string(chain::NetworkValidationParams(network).coinbase_maturity)) +
            "," +
            Field("creator_status", creator.has_value() ? (creator_unspent ? "unspent" : "spent")
                                                        : "not_materialized_by_regtest_policy") +
            "," + Field("creator_spend", creator_spend) + "," +
            Field("creator_script_balance", std::to_string(creator_balance)) +
            ",\"anomalies\":[],\"limitations\":[\"external_capture_provenance_not_independently_"
            "verified\",\"snapshot_time_not_historical_arrival_time\",\"no_creator_custody_or_"
            "public_launch_proof\"],\"utxos\":[";
        // Listing bytes have a fixed ASCII alphabet. Embed each complete record
        // as a JSON string; no untrusted free-form strings enter the report.
        std::size_t start = 0U;
        if (report.size() > limits.output || limits.output - report.size() < 3U)
            return {"resource_output"};
        while (start < listing.size()) {
            const auto end = listing.find('\n', start);
            if (end - start + 6U > limits.output - report.size())
                return {"resource_output"};
            if (start != 0U)
                report += ',';
            report += '"';
            report.append(listing, start, end - start);
            report += '"';
            start = end + 1U;
        }
        report += "]}\n";
        if (report.size() > limits.output)
            return {"resource_output"};
        return {"", std::move(report)};
    } catch (...) {
        return {"incomplete_allocation_or_io"};
    }
}
} // namespace nova::inspection
