#include "primitives/transaction.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <utility>

namespace nova::primitives
{
namespace
{

constexpr std::uint32_t kNullOutPointIndex = std::numeric_limits<std::uint32_t>::max();

[[nodiscard]] bool IsAllZero(const crypto::Hash256& hash) noexcept
{
    return std::all_of(hash.bytes().begin(), hash.bytes().end(),
                       [](const std::uint8_t byte) { return byte == 0U; });
}

[[nodiscard]] std::optional<std::uint64_t> ToUint64(const std::size_t value) noexcept
{
    if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t)) {
        if (value > std::numeric_limits<std::uint64_t>::max()) {
            return std::nullopt;
        }
    }
    return static_cast<std::uint64_t>(value);
}

[[nodiscard]] std::uint64_t CompactSizeLength(const std::uint64_t value) noexcept
{
    if (value < 253U) {
        return 1U;
    }
    if (value <= std::numeric_limits<std::uint16_t>::max()) {
        return 3U;
    }
    if (value <= std::numeric_limits<std::uint32_t>::max()) {
        return 5U;
    }
    return 9U;
}

[[nodiscard]] bool CheckedAdd(std::uint64_t& total, const std::uint64_t addend) noexcept
{
    if (addend > std::numeric_limits<std::uint64_t>::max() - total) {
        return false;
    }
    total += addend;
    return true;
}

[[nodiscard]] bool SerializeOutPoint(BinaryWriter& writer, const OutPoint& outpoint) noexcept
{
    return writer.WriteHash256(outpoint.transaction_id) && writer.WriteU32(outpoint.output_index);
}

[[nodiscard]] std::optional<OutPoint> DeserializeOutPoint(BinaryReader& reader) noexcept
{
    const auto transaction_id = reader.ReadHash256();
    const auto output_index = reader.ReadU32();
    if (!transaction_id.has_value() || !output_index.has_value()) {
        return std::nullopt;
    }
    return OutPoint{*transaction_id, *output_index};
}

[[nodiscard]] bool SerializeInput(BinaryWriter& writer, const TxInput& input,
                                  const TransactionLimits& limits) noexcept
{
    return SerializeOutPoint(writer, input.previous_output) &&
           writer.WriteBytes(input.script_sig, limits.max_script_size) &&
           writer.WriteU32(input.sequence);
}

[[nodiscard]] std::optional<TxInput> DeserializeInput(BinaryReader& reader,
                                                      const TransactionLimits& limits) noexcept
{
    const auto outpoint = DeserializeOutPoint(reader);
    auto script_sig = reader.ReadBytes(limits.max_script_size);
    const auto sequence = reader.ReadU32();
    if (!outpoint.has_value() || !script_sig.has_value() || !sequence.has_value()) {
        return std::nullopt;
    }
    return TxInput{*outpoint, std::move(*script_sig), *sequence};
}

[[nodiscard]] bool SerializeOutput(BinaryWriter& writer, const TxOutput& output,
                                   const TransactionLimits& limits) noexcept
{
    return writer.WriteI64(output.value) &&
           writer.WriteBytes(output.script_pubkey, limits.max_script_size);
}

[[nodiscard]] std::optional<TxOutput> DeserializeOutput(BinaryReader& reader,
                                                        const TransactionLimits& limits) noexcept
{
    const auto value = reader.ReadI64();
    auto script_pubkey = reader.ReadBytes(limits.max_script_size);
    if (!value.has_value() || !script_pubkey.has_value()) {
        return std::nullopt;
    }
    return TxOutput{*value, std::move(*script_pubkey)};
}

} // namespace

bool OutPoint::IsNull() const noexcept
{
    return IsAllZero(transaction_id) && output_index == kNullOutPointIndex;
}

bool OutPoint::IsMalformed() const noexcept
{
    return IsAllZero(transaction_id) && output_index != kNullOutPointIndex;
}

bool Transaction::Serialize(BinaryWriter& writer, const TransactionLimits& limits) const noexcept
{
    if (CheckTransactionStructure(*this, limits) != TransactionStructureError::kNone) {
        return false;
    }
    return writer.WriteU32(std::bit_cast<std::uint32_t>(version)) &&
           writer.WriteArray<TxInput>(inputs, limits.max_inputs,
                                      [&limits](BinaryWriter& array_writer, const TxInput& input) {
                                          return SerializeInput(array_writer, input, limits);
                                      }) &&
           writer.WriteArray<TxOutput>(
               outputs, limits.max_outputs,
               [&limits](BinaryWriter& array_writer, const TxOutput& output) {
                   return SerializeOutput(array_writer, output, limits);
               }) &&
           writer.WriteU32(lock_time);
}

std::optional<Transaction> Transaction::Deserialize(const std::span<const std::uint8_t> serialized,
                                                    const TransactionLimits& limits) noexcept
{
    if (limits.max_serialized_size == 0U || serialized.size() > limits.max_serialized_size) {
        return std::nullopt;
    }

    BinaryReader reader{serialized};
    auto transaction = Deserialize(reader, limits);
    if (!transaction.has_value() || !reader.RequireEnd()) {
        return std::nullopt;
    }
    return transaction;
}

std::optional<Transaction> Transaction::Deserialize(BinaryReader& reader,
                                                    const TransactionLimits& limits) noexcept
{
    const auto version_bits = reader.ReadU32();
    if (!version_bits.has_value()) {
        return std::nullopt;
    }
    auto inputs =
        reader.ReadArray<TxInput>(limits.max_inputs, 41U, [&limits](BinaryReader& array_reader) {
            return DeserializeInput(array_reader, limits);
        });
    auto outputs =
        reader.ReadArray<TxOutput>(limits.max_outputs, 9U, [&limits](BinaryReader& array_reader) {
            return DeserializeOutput(array_reader, limits);
        });
    const auto lock_time = reader.ReadU32();
    if (!inputs.has_value() || !outputs.has_value() || !lock_time.has_value()) {
        return std::nullopt;
    }

    Transaction transaction{std::bit_cast<std::int32_t>(*version_bits), std::move(*inputs),
                            std::move(*outputs), *lock_time};
    if (CheckTransactionStructure(transaction, limits) != TransactionStructureError::kNone) {
        return std::nullopt;
    }
    return transaction;
}

std::optional<std::uint64_t> Transaction::SerializedSize() const noexcept
{
    const auto input_count = ToUint64(inputs.size());
    const auto output_count = ToUint64(outputs.size());
    if (!input_count.has_value() || !output_count.has_value()) {
        return std::nullopt;
    }

    std::uint64_t size = 4U;
    if (!CheckedAdd(size, CompactSizeLength(*input_count))) {
        return std::nullopt;
    }
    for (const auto& input : inputs) {
        const auto script_size = ToUint64(input.script_sig.size());
        if (!script_size.has_value() || !CheckedAdd(size, 36U) ||
            !CheckedAdd(size, CompactSizeLength(*script_size)) || !CheckedAdd(size, *script_size) ||
            !CheckedAdd(size, 4U)) {
            return std::nullopt;
        }
    }
    if (!CheckedAdd(size, CompactSizeLength(*output_count))) {
        return std::nullopt;
    }
    for (const auto& output : outputs) {
        const auto script_size = ToUint64(output.script_pubkey.size());
        if (!script_size.has_value() || !CheckedAdd(size, 8U) ||
            !CheckedAdd(size, CompactSizeLength(*script_size)) || !CheckedAdd(size, *script_size)) {
            return std::nullopt;
        }
    }
    if (!CheckedAdd(size, 4U)) {
        return std::nullopt;
    }
    return size;
}

std::optional<std::uint64_t> Transaction::Weight() const noexcept
{
    return SerializedSize();
}

std::optional<crypto::Hash256> Transaction::TxId(const TransactionLimits& limits) const noexcept
{
    BinaryWriter writer;
    if (!Serialize(writer, limits)) {
        return std::nullopt;
    }
    return crypto::Hash256::DoubleSha256(writer.bytes());
}

TransactionStructureError CheckTransactionStructure(const Transaction& transaction,
                                                    const TransactionLimits& limits) noexcept
{
    if (limits.max_money < 0 || limits.max_serialized_size == 0U) {
        return TransactionStructureError::kInvalidLimits;
    }
    if (transaction.inputs.empty()) {
        return TransactionStructureError::kZeroInputs;
    }
    if (transaction.outputs.empty()) {
        return TransactionStructureError::kZeroOutputs;
    }
    if (transaction.inputs.size() > limits.max_inputs) {
        return TransactionStructureError::kTooManyInputs;
    }
    if (transaction.outputs.size() > limits.max_outputs) {
        return TransactionStructureError::kTooManyOutputs;
    }

    for (std::size_t left = 0; left < transaction.inputs.size(); ++left) {
        const auto& input = transaction.inputs.at(left);
        if (input.previous_output.IsMalformed()) {
            return TransactionStructureError::kMalformedOutPoint;
        }
        if (input.script_sig.size() > limits.max_script_size) {
            return TransactionStructureError::kScriptSigTooLarge;
        }
        for (std::size_t right = left + 1U; right < transaction.inputs.size(); ++right) {
            if (input.previous_output == transaction.inputs.at(right).previous_output) {
                return TransactionStructureError::kDuplicateInput;
            }
        }
    }

    Amount total = 0;
    for (const auto& output : transaction.outputs) {
        if (output.value < 0) {
            return TransactionStructureError::kNegativeOutputAmount;
        }
        if (output.value > limits.max_money) {
            return TransactionStructureError::kOutputAmountExceedsMaxMoney;
        }
        if (output.script_pubkey.size() > limits.max_script_size) {
            return TransactionStructureError::kScriptPubKeyTooLarge;
        }
        if (output.value > std::numeric_limits<Amount>::max() - total) {
            return TransactionStructureError::kTotalOutputOverflow;
        }
        total += output.value;
        if (total > limits.max_money) {
            return TransactionStructureError::kTotalOutputExceedsMaxMoney;
        }
    }

    const auto serialized_size = transaction.SerializedSize();
    if (!serialized_size.has_value()) {
        return TransactionStructureError::kSerializationFailure;
    }
    if (*serialized_size > limits.max_serialized_size) {
        return TransactionStructureError::kOversizedTransaction;
    }
    return TransactionStructureError::kNone;
}

} // namespace nova::primitives
