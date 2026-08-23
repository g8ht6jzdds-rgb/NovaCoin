#pragma once

#include "primitives/serialization.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace nova::primitives
{

using Amount = std::int64_t;

struct TransactionLimits final {
    Amount max_money{};
    std::uint32_t max_serialized_size{};
    std::uint32_t max_inputs{};
    std::uint32_t max_outputs{};
    std::uint32_t max_script_size{};
};

struct OutPoint final {
    crypto::Hash256 transaction_id{};
    std::uint32_t output_index{};

    [[nodiscard]] bool IsNull() const noexcept;
    [[nodiscard]] bool IsMalformed() const noexcept;

    friend bool operator==(const OutPoint&, const OutPoint&) = default;
};

struct TxInput final {
    OutPoint previous_output{};
    std::vector<std::uint8_t> script_sig;
    std::uint32_t sequence{};
};

struct TxOutput final {
    Amount value{};
    std::vector<std::uint8_t> script_pubkey;
};

struct Transaction final {
    std::int32_t version{};
    std::vector<TxInput> inputs;
    std::vector<TxOutput> outputs;
    std::uint32_t lock_time{};

    [[nodiscard]] bool Serialize(BinaryWriter& writer,
                                 const TransactionLimits& limits) const noexcept;
    [[nodiscard]] static std::optional<Transaction>
    Deserialize(std::span<const std::uint8_t> serialized, const TransactionLimits& limits) noexcept;
    [[nodiscard]] static std::optional<Transaction>
    Deserialize(BinaryReader& reader, const TransactionLimits& limits) noexcept;
    [[nodiscard]] std::optional<std::uint64_t> SerializedSize() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> Weight() const noexcept;
    [[nodiscard]] std::optional<crypto::Hash256>
    TxId(const TransactionLimits& limits) const noexcept;
};

enum class TransactionStructureError : std::uint8_t {
    kNone,
    kInvalidLimits,
    kZeroInputs,
    kZeroOutputs,
    kTooManyInputs,
    kTooManyOutputs,
    kMalformedOutPoint,
    kDuplicateInput,
    kScriptSigTooLarge,
    kScriptPubKeyTooLarge,
    kNegativeOutputAmount,
    kOutputAmountExceedsMaxMoney,
    kTotalOutputOverflow,
    kTotalOutputExceedsMaxMoney,
    kOversizedTransaction,
    kSerializationFailure,
};

[[nodiscard]] TransactionStructureError
CheckTransactionStructure(const Transaction& transaction, const TransactionLimits& limits) noexcept;

} // namespace nova::primitives
