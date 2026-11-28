#include "chain/mempool_image.hpp"

#include <algorithm>
#include <array>
#include <istream>
#include <limits>
#include <map>
#include <set>

namespace nova::chain
{
namespace
{
constexpr std::array<std::uint8_t, 8U> kMagic{'N', 'V', 'M', 'P', 'O', 'O', 'L', '1'};
using Id = crypto::Hash256::Bytes;

std::optional<std::vector<std::size_t>>
CanonicalOrder(const std::span<const MempoolImageRecord> records, const MempoolParams& params)
{
    if (records.size() > params.max_transactions)
        return std::nullopt;
    const auto& limits = params.validation.block_limits.transaction_limits;
    std::map<Id, std::size_t> indices;
    std::uint64_t total{};
    for (std::size_t i = 0; i < records.size(); ++i) {
        const auto& transaction = records[i].transaction;
        const auto size = transaction.SerializedSize();
        const auto id = transaction.TxId(limits);
        if (primitives::CheckTransactionStructure(transaction, limits) !=
                primitives::TransactionStructureError::kNone ||
            !size || !id || *size > params.max_total_serialized_size - total ||
            !indices.emplace(id->bytes(), i).second)
            return std::nullopt;
        total += *size;
    }
    std::vector<std::vector<std::size_t>> children(records.size());
    std::vector<std::size_t> indegree(records.size());
    for (std::size_t i = 0; i < records.size(); ++i) {
        std::set<std::size_t> parents;
        for (const auto& input : records[i].transaction.inputs) {
            const auto found = indices.find(input.previous_output.transaction_id.bytes());
            if (found != indices.end())
                parents.insert(found->second);
        }
        indegree[i] = parents.size();
        for (const auto parent : parents)
            children[parent].push_back(i);
    }
    std::set<Id> available;
    for (const auto& [id, index] : indices)
        if (indegree[index] == 0U)
            available.insert(id);
    std::vector<std::size_t> order;
    order.reserve(records.size());
    while (!available.empty()) {
        const auto id = *available.begin();
        available.erase(available.begin());
        const auto index = indices.at(id);
        order.push_back(index);
        for (const auto child : children[index])
            if (--indegree[child] == 0U) {
                const auto child_id = records[child].transaction.TxId(limits);
                if (!child_id)
                    return std::nullopt;
                available.insert(child_id->bytes());
            }
    }
    if (order.size() != records.size())
        return std::nullopt;
    return order;
}

} // namespace

std::optional<std::uint64_t> MempoolImageMaxSize(const MempoolParams& params) noexcept
{
    // Each record has at most eight timestamp bytes and nine length bytes.
    constexpr std::uint64_t overhead = 17U;
    constexpr std::uint64_t header = 17U;
    const auto extra = header + overhead * static_cast<std::uint64_t>(params.max_transactions);
    if (params.max_total_serialized_size > std::numeric_limits<std::uint64_t>::max() - extra)
        return std::nullopt;
    return params.max_total_serialized_size + extra;
}

namespace
{
bool ImageSizeAllowed(const std::size_t size, const MempoolParams& params) noexcept
{
    const auto limit = MempoolImageMaxSize(params);
    return limit.has_value() && size <= *limit;
}
} // namespace

std::optional<std::vector<std::uint8_t>>
EncodeMempoolImage(const std::span<const MempoolImageRecord> records,
                   const MempoolParams& params) noexcept
{
    try {
        const auto order = CanonicalOrder(records, params);
        if (!order)
            return std::nullopt;
        primitives::BinaryWriter writer;
        if (!writer.WriteFixedBytes(kMagic) || !writer.WriteCompactSize(records.size()))
            return std::nullopt;
        for (const auto index : *order) {
            primitives::BinaryWriter transaction;
            if (!records[index].transaction.Serialize(
                    transaction, params.validation.block_limits.transaction_limits) ||
                !writer.WriteU64(records[index].time_received) ||
                !writer.WriteBytes(
                    transaction.bytes(),
                    params.validation.block_limits.transaction_limits.max_serialized_size))
                return std::nullopt;
        }
        if (!ImageSizeAllowed(writer.bytes().size(), params))
            return std::nullopt;
        return std::vector<std::uint8_t>{writer.bytes().begin(), writer.bytes().end()};
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::vector<MempoolImageRecord>>
DecodeMempoolImage(const std::span<const std::uint8_t> bytes, const MempoolParams& params) noexcept
{
    try {
        if (!ImageSizeAllowed(bytes.size(), params))
            return std::nullopt;
        primitives::BinaryReader reader{bytes};
        const auto magic = reader.ReadFixedBytes<kMagic.size()>();
        if (!magic || *magic != kMagic)
            return std::nullopt;
        auto records = reader.ReadArray<MempoolImageRecord>(
            params.max_transactions, 9U,
            [&params](primitives::BinaryReader& input) -> std::optional<MempoolImageRecord> {
                const auto time = input.ReadU64();
                const auto data = input.ReadBytes(
                    params.validation.block_limits.transaction_limits.max_serialized_size);
                if (!time || !data)
                    return std::nullopt;
                auto tx = primitives::Transaction::Deserialize(
                    *data, params.validation.block_limits.transaction_limits);
                if (!tx)
                    return std::nullopt;
                return MempoolImageRecord{std::move(*tx), *time};
            });
        if (!records || !reader.RequireEnd())
            return std::nullopt;
        const auto canonical = EncodeMempoolImage(*records, params);
        if (!canonical || !std::ranges::equal(*canonical, bytes))
            return std::nullopt;
        return records;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::vector<std::uint8_t>>
EncodePreparedMempoolImage(const Mempool& pool, const Mempool::PreparedState& prepared,
                           const MempoolParams& params) noexcept
{
    try {
        const auto entries = pool.GetPreparedEntries(prepared);
        if (!entries)
            return std::nullopt;
        std::vector<MempoolImageRecord> records;
        records.reserve(entries->size());
        for (const auto& entry : *entries)
            records.push_back({entry.transaction, entry.time_received});
        return EncodeMempoolImage(records, params);
    } catch (...) {
        return std::nullopt;
    }
}

std::unique_ptr<Mempool> ReplayMempoolImage(const std::span<const std::uint8_t> bytes,
                                            const MempoolParams& params,
                                            const TransactionValidationContext& context,
                                            const UTXOView& utxos) noexcept
{
    const auto records = DecodeMempoolImage(bytes, params);
    if (!records || !utxos.IsCurrent())
        return nullptr;
    auto pool = Mempool::Create(params);
    if (!pool)
        return nullptr;
    for (const auto& record : *records)
        if (pool->AcceptToMempool(record.transaction, context, utxos, record.time_received).error !=
            MempoolError::kNone)
            return nullptr;
    return utxos.IsCurrent() ? std::move(pool) : nullptr;
}

std::optional<std::vector<std::uint8_t>> ReadMempoolImage(std::istream& input,
                                                          const MempoolParams& params) noexcept
{
    try {
        const auto limit = MempoolImageMaxSize(params);
        if (!limit)
            return std::nullopt;
        std::vector<std::uint8_t> bytes;
        std::array<char, 4096U> chunk{};
        while (input) {
            input.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
            const auto count = input.gcount();
            if (count < 0 || bytes.size() > *limit ||
                static_cast<std::uint64_t>(count) > *limit - bytes.size())
                return std::nullopt;
            bytes.insert(bytes.end(), chunk.begin(), chunk.begin() + count);
        }
        if (!input.eof() || input.bad() || !DecodeMempoolImage(bytes, params))
            return std::nullopt;
        return bytes;
    } catch (...) {
        return std::nullopt;
    }
}
} // namespace nova::chain
