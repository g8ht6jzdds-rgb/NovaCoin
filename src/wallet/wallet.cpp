#include "wallet/wallet.hpp"

#include "chain/validation.hpp"
#include "primitives/serialization.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <climits>
#include <filesystem>
#include <fstream>
#include <limits>
#include <new>
#include <string>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace nova::wallet
{
namespace
{

constexpr std::uint8_t kOpDup = 0x76U;
constexpr std::uint8_t kOpHash160 = 0xA9U;
constexpr std::uint8_t kPushHash160 = 0x14U;
constexpr std::uint8_t kOpEqualVerify = 0x88U;
constexpr std::uint8_t kOpCheckSig = 0xACU;
constexpr std::uint8_t kPushCompressedPublicKey = 0x21U;
constexpr std::uint32_t kFinalSequence = 0xFFFF'FFFFU;
constexpr std::size_t kP2pkhScriptSize = 25U;
constexpr std::size_t kMaximumP2pkhUnlockingScriptSize = 108U;
constexpr std::size_t kSerializedInputOverhead = 36U + 1U + 4U;
constexpr std::size_t kSerializedOutputSize = 8U + 1U + kP2pkhScriptSize;
constexpr std::array<std::uint8_t, 4U> kWalletFileMagic{'N', 'W', 'L', 'T'};
constexpr std::uint32_t kWalletFileVersion = 1U;
constexpr std::uint32_t kWalletKdfIterations = 300'000U;
constexpr std::size_t kWalletSaltSize = 16U;
constexpr std::size_t kWalletNonceSize = 12U;
constexpr std::size_t kWalletTagSize = 16U;
constexpr std::size_t kWalletEncryptionKeySize = 32U;
constexpr std::uint64_t kMaximumWalletFileSize = 64U * 1024U * 1024U;
constexpr std::uint64_t kMaximumWalletKeys = 100'000U;
constexpr std::uint64_t kMaximumWalletCoins = 1'000'000U;

class SensitiveBytes final
{
  public:
    SensitiveBytes() = default;
    explicit SensitiveBytes(std::vector<std::uint8_t> bytes) noexcept : bytes_(std::move(bytes)) {}
    SensitiveBytes(const SensitiveBytes&) = delete;
    SensitiveBytes& operator=(const SensitiveBytes&) = delete;
    SensitiveBytes(SensitiveBytes&& other) noexcept : bytes_(std::move(other.bytes_)) {}
    SensitiveBytes& operator=(SensitiveBytes&& other) noexcept
    {
        if (this != &other) {
            if (!bytes_.empty()) {
                OPENSSL_cleanse(bytes_.data(), bytes_.size());
            }
            bytes_ = std::move(other.bytes_);
        }
        return *this;
    }
    ~SensitiveBytes()
    {
        if (!bytes_.empty()) {
            OPENSSL_cleanse(bytes_.data(), bytes_.size());
        }
    }

    [[nodiscard]] std::span<const std::uint8_t> view() const noexcept
    {
        return bytes_;
    }
    [[nodiscard]] std::vector<std::uint8_t>& mutable_view() noexcept
    {
        return bytes_;
    }

  private:
    std::vector<std::uint8_t> bytes_;
};

void Cleanse(std::array<std::uint8_t, kWalletEncryptionKeySize>& bytes) noexcept
{
    OPENSSL_cleanse(bytes.data(), bytes.size());
}

[[nodiscard]] bool WriteWalletFileAtomically(const std::filesystem::path& path,
                                             std::span<const std::uint8_t> bytes) noexcept
{
    if (path.empty() || bytes.empty()) {
        return false;
    }
    const auto temporary = path.string() + ".tmp";
    std::error_code error;
    std::filesystem::remove(temporary, error);
    try {
        {
            std::ofstream output{temporary, std::ios::binary | std::ios::trunc};
            if (!output.is_open()) {
                return false;
            }
            output.write(reinterpret_cast<const char*>(bytes.data()),
                         static_cast<std::streamsize>(bytes.size()));
            output.flush();
            if (!output.good()) {
                output.close();
                std::filesystem::remove(temporary, error);
                return false;
            }
        }
#ifdef _WIN32
        const auto temporary_handle = CreateFileA(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (temporary_handle == INVALID_HANDLE_VALUE || FlushFileBuffers(temporary_handle) == 0) {
            if (temporary_handle != INVALID_HANDLE_VALUE) {
                (void)CloseHandle(temporary_handle);
            }
            std::filesystem::remove(temporary, error);
            return false;
        }
        (void)CloseHandle(temporary_handle);
        if (!MoveFileExA(temporary.c_str(), path.string().c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            std::filesystem::remove(temporary, error);
            return false;
        }
#else
        std::filesystem::rename(temporary, path, error);
        if (error) {
            std::filesystem::remove(temporary, error);
            return false;
        }
#endif
        return true;
    } catch (...) {
        std::filesystem::remove(temporary, error);
        return false;
    }
}

[[nodiscard]] bool
DeriveWalletEncryptionKey(const std::span<const std::uint8_t> passphrase,
                          const std::array<std::uint8_t, kWalletSaltSize>& salt,
                          std::array<std::uint8_t, kWalletEncryptionKeySize>& key) noexcept
{
    if (passphrase.empty() || passphrase.size() > static_cast<std::size_t>(INT_MAX)) {
        return false;
    }
    return PKCS5_PBKDF2_HMAC(reinterpret_cast<const char*>(passphrase.data()),
                             static_cast<int>(passphrase.size()), salt.data(),
                             static_cast<int>(salt.size()), static_cast<int>(kWalletKdfIterations),
                             EVP_sha256(), static_cast<int>(key.size()), key.data()) == 1;
}

[[nodiscard]] std::optional<std::vector<std::uint8_t>>
EncryptWallet(const std::span<const std::uint8_t> plaintext,
              const std::array<std::uint8_t, kWalletEncryptionKeySize>& key,
              const std::array<std::uint8_t, kWalletNonceSize>& nonce,
              std::array<std::uint8_t, kWalletTagSize>& tag) noexcept
{
    if (plaintext.size() > static_cast<std::size_t>(INT_MAX)) {
        return std::nullopt;
    }
    EVP_CIPHER_CTX* context = EVP_CIPHER_CTX_new();
    if (context == nullptr) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> ciphertext(plaintext.size() + EVP_MAX_BLOCK_LENGTH);
    int written{};
    int finalized{};
    const bool ok =
        EVP_EncryptInit_ex(context, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
        EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(nonce.size()),
                            nullptr) == 1 &&
        EVP_EncryptInit_ex(context, nullptr, nullptr, key.data(), nonce.data()) == 1 &&
        EVP_EncryptUpdate(context, ciphertext.data(), &written, plaintext.data(),
                          static_cast<int>(plaintext.size())) == 1 &&
        EVP_EncryptFinal_ex(context, ciphertext.data() + written, &finalized) == 1 &&
        EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_GET_TAG, static_cast<int>(tag.size()),
                            tag.data()) == 1;
    EVP_CIPHER_CTX_free(context);
    if (!ok || written < 0 || finalized < 0) {
        if (!ciphertext.empty()) {
            OPENSSL_cleanse(ciphertext.data(), ciphertext.size());
        }
        return std::nullopt;
    }
    ciphertext.resize(static_cast<std::size_t>(written + finalized));
    return ciphertext;
}

[[nodiscard]] std::optional<SensitiveBytes>
DecryptWallet(const std::span<const std::uint8_t> ciphertext,
              const std::array<std::uint8_t, kWalletEncryptionKeySize>& key,
              const std::array<std::uint8_t, kWalletNonceSize>& nonce,
              const std::array<std::uint8_t, kWalletTagSize>& tag) noexcept
{
    if (ciphertext.size() > static_cast<std::size_t>(INT_MAX)) {
        return std::nullopt;
    }
    EVP_CIPHER_CTX* context = EVP_CIPHER_CTX_new();
    if (context == nullptr) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> plaintext(ciphertext.size() + EVP_MAX_BLOCK_LENGTH);
    int written{};
    int finalized{};
    const bool ok =
        EVP_DecryptInit_ex(context, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
        EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(nonce.size()),
                            nullptr) == 1 &&
        EVP_DecryptInit_ex(context, nullptr, nullptr, key.data(), nonce.data()) == 1 &&
        EVP_DecryptUpdate(context, plaintext.data(), &written, ciphertext.data(),
                          static_cast<int>(ciphertext.size())) == 1 &&
        EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_TAG, static_cast<int>(tag.size()),
                            const_cast<std::uint8_t*>(tag.data())) == 1 &&
        EVP_DecryptFinal_ex(context, plaintext.data() + written, &finalized) == 1;
    EVP_CIPHER_CTX_free(context);
    if (!ok || written < 0 || finalized < 0) {
        if (!plaintext.empty()) {
            OPENSSL_cleanse(plaintext.data(), plaintext.size());
        }
        return std::nullopt;
    }
    plaintext.resize(static_cast<std::size_t>(written + finalized));
    return SensitiveBytes{std::move(plaintext)};
}

[[nodiscard]] std::optional<crypto::Hash160>
ParseP2pkhScript(const std::span<const std::uint8_t> script) noexcept
{
    if (script.size() != kP2pkhScriptSize || script[0U] != kOpDup || script[1U] != kOpHash160 ||
        script[2U] != kPushHash160 || script[23U] != kOpEqualVerify || script[24U] != kOpCheckSig) {
        return std::nullopt;
    }
    crypto::Hash160 result{};
    std::copy_n(script.begin() + 3, result.size(), result.begin());
    return result;
}

[[nodiscard]] std::vector<std::uint8_t> MakeP2pkhScript(const crypto::Hash160& key_hash)
{
    std::vector<std::uint8_t> script;
    script.reserve(kP2pkhScriptSize);
    script.push_back(kOpDup);
    script.push_back(kOpHash160);
    script.push_back(kPushHash160);
    for (const auto byte : key_hash) {
        script.push_back(byte);
    }
    script.push_back(kOpEqualVerify);
    script.push_back(kOpCheckSig);
    return script;
}

[[nodiscard]] bool CheckedAdd(const primitives::Amount left, const primitives::Amount right,
                              primitives::Amount& result) noexcept
{
    if ((right > 0 && left > std::numeric_limits<primitives::Amount>::max() - right) ||
        (right < 0 && left < std::numeric_limits<primitives::Amount>::min() - right)) {
        return false;
    }
    result = left + right;
    return true;
}

[[nodiscard]] bool CheckedMultiply(const std::uint64_t left, const primitives::Amount right,
                                   primitives::Amount& result) noexcept
{
    if (right < 0) {
        return false;
    }
    if (right == 0) {
        result = 0;
        return true;
    }
    if (left > static_cast<std::uint64_t>(std::numeric_limits<primitives::Amount>::max() / right)) {
        return false;
    }
    result = static_cast<primitives::Amount>(left) * right;
    return true;
}

[[nodiscard]] std::uint64_t CompactSizeLength(const std::size_t value) noexcept
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

[[nodiscard]] bool CheckedAddSize(std::uint64_t& total, const std::uint64_t addend) noexcept
{
    if (addend > std::numeric_limits<std::uint64_t>::max() - total) {
        return false;
    }
    total += addend;
    return true;
}

[[nodiscard]] bool IsValidParameters(const WalletParams& parameters) noexcept
{
    return parameters.transaction_limits.max_money >= 0 &&
           parameters.transaction_limits.max_serialized_size > 0U &&
           parameters.transaction_limits.max_inputs > 0U &&
           parameters.transaction_limits.max_outputs > 0U &&
           parameters.transaction_limits.max_script_size >= kMaximumP2pkhUnlockingScriptSize &&
           parameters.fee_rate_per_byte >= 0 && parameters.sighash_all == 1U;
}

[[nodiscard]] std::optional<std::vector<std::uint8_t>>
MakeUnlockingScript(const crypto::Signature& signature, const crypto::PublicKey& public_key,
                    const std::uint32_t sighash) noexcept
{
    const auto der = signature.SerializeDer();
    if (sighash != 1U || der.empty() || der.size() > crypto::Signature::kMaximumDerSize ||
        der.size() + 1U > 73U) {
        return std::nullopt;
    }
    try {
        std::vector<std::uint8_t> script;
        script.reserve(der.size() + 36U);
        script.push_back(static_cast<std::uint8_t>(der.size() + 1U));
        script.insert(script.end(), der.begin(), der.end());
        script.push_back(static_cast<std::uint8_t>(sighash));
        script.push_back(kPushCompressedPublicKey);
        const auto& serialized_key = public_key.SerializeCompressed();
        script.insert(script.end(), serialized_key.begin(), serialized_key.end());
        return script;
    } catch (...) {
        return std::nullopt;
    }
}

[[nodiscard]] bool VerifyInput(const primitives::Transaction& transaction,
                               const std::size_t input_index,
                               const primitives::TxOutput& spent_output,
                               const WalletParams& parameters) noexcept
{
    const auto expected_hash = ParseP2pkhScript(spent_output.script_pubkey);
    if (!expected_hash.has_value() || input_index >= transaction.inputs.size()) {
        return false;
    }
    const auto& script = transaction.inputs[input_index].script_sig;
    if (script.size() < 44U || script.front() < 9U || script.front() > 73U) {
        return false;
    }
    const auto pushed_signature_size = static_cast<std::size_t>(script.front());
    if (script.size() != pushed_signature_size + 35U ||
        script[pushed_signature_size + 1U] != kPushCompressedPublicKey) {
        return false;
    }
    const auto der_size = pushed_signature_size - 1U;
    if (script[der_size + 1U] != parameters.sighash_all) {
        return false;
    }
    const auto signature =
        crypto::Signature::FromDer(std::span<const std::uint8_t>{script}.subspan(1U, der_size));
    const auto public_key = crypto::PublicKey::FromCompressed(
        std::span<const std::uint8_t>{script}.last(crypto::PublicKey::kCompressedSize));
    if (!signature.has_value() || !public_key.has_value()) {
        return false;
    }
    const auto actual_hash = crypto::Hash160Digest(public_key->SerializeCompressed());
    if (!actual_hash.has_value() || *actual_hash != *expected_hash) {
        return false;
    }
    const auto digest =
        chain::ComputeSignatureHash(transaction, input_index, spent_output.script_pubkey,
                                    parameters.transaction_limits, parameters.sighash_all);
    return digest.has_value() && public_key->Verify(*digest, *signature);
}

} // namespace

std::optional<WalletAddress> KeyStore::GenerateReceivingAddress() noexcept
{
    try {
        if (next_index_ == std::numeric_limits<std::uint32_t>::max()) {
            return std::nullopt;
        }
        auto key_pair = crypto::KeyPair::Generate();
        if (!key_pair.has_value()) {
            return std::nullopt;
        }
        const auto public_key = key_pair->public_key();
        const auto key_hash = crypto::Hash160Digest(public_key.SerializeCompressed());
        if (!key_hash.has_value() || keys_.contains(*key_hash)) {
            return std::nullopt;
        }
        const auto index = next_index_;
        const auto inserted = keys_.emplace(*key_hash, StoredKey{std::move(*key_pair), index});
        if (!inserted.second) {
            return std::nullopt;
        }
        ++next_index_;
        return WalletAddress{*key_hash, public_key, index};
    } catch (...) {
        return std::nullopt;
    }
}

std::vector<WalletAddress> KeyStore::ReceivingAddresses() const
{
    std::vector<WalletAddress> addresses;
    addresses.reserve(keys_.size());
    for (const auto& [key_hash, stored_key] : keys_) {
        addresses.push_back(
            WalletAddress{key_hash, stored_key.key_pair.public_key(), stored_key.index});
    }
    std::sort(addresses.begin(), addresses.end(),
              [](const WalletAddress& left, const WalletAddress& right) {
                  return left.index < right.index;
              });
    return addresses;
}

bool KeyStore::OwnsScript(const std::span<const std::uint8_t> script_pubkey) const noexcept
{
    const auto key_hash = ParseP2pkhScript(script_pubkey);
    return key_hash.has_value() && FindKey(*key_hash) != nullptr;
}

const crypto::KeyPair* KeyStore::FindKey(const crypto::Hash160& public_key_hash) const noexcept
{
    const auto iterator = keys_.find(public_key_hash);
    return iterator == keys_.end() ? nullptr : &iterator->second.key_pair;
}

std::optional<primitives::Amount>
TransactionBuilder::EstimateFee(const std::size_t input_count, const std::size_t output_count,
                                const WalletParams& parameters) noexcept
{
    if (!IsValidParameters(parameters) || input_count == 0U || output_count == 0U ||
        input_count > parameters.transaction_limits.max_inputs ||
        output_count > parameters.transaction_limits.max_outputs) {
        return std::nullopt;
    }
    std::uint64_t size = 4U;
    if (!CheckedAddSize(size, CompactSizeLength(input_count)) ||
        !CheckedAddSize(size, static_cast<std::uint64_t>(input_count) *
                                  (kSerializedInputOverhead + kMaximumP2pkhUnlockingScriptSize)) ||
        !CheckedAddSize(size, CompactSizeLength(output_count)) ||
        !CheckedAddSize(size, static_cast<std::uint64_t>(output_count) * kSerializedOutputSize) ||
        !CheckedAddSize(size, 4U) || size > parameters.transaction_limits.max_serialized_size) {
        return std::nullopt;
    }
    primitives::Amount fee{};
    if (!CheckedMultiply(size, parameters.fee_rate_per_byte, fee) ||
        fee > parameters.transaction_limits.max_money) {
        return std::nullopt;
    }
    return fee;
}

TransactionBuildResult TransactionBuilder::Build(const KeyStore& key_store,
                                                 const std::span<const WalletUTXO> available_coins,
                                                 const std::span<const Recipient> recipients,
                                                 const WalletParams& parameters) noexcept
{
    if (!IsValidParameters(parameters)) {
        return {WalletError::kInvalidParameters, std::nullopt};
    }
    try {
        if (recipients.empty() || recipients.size() >= parameters.transaction_limits.max_outputs) {
            return {WalletError::kInvalidRecipient, std::nullopt};
        }
        primitives::Amount recipient_total{};
        std::vector<primitives::TxOutput> outputs;
        outputs.reserve(recipients.size() + 1U);
        for (const auto& recipient : recipients) {
            if (recipient.amount <= 0 ||
                recipient.amount > parameters.transaction_limits.max_money) {
                return {WalletError::kInvalidRecipient, std::nullopt};
            }
            primitives::Amount updated_total{};
            if (!CheckedAdd(recipient_total, recipient.amount, updated_total) ||
                updated_total > parameters.transaction_limits.max_money) {
                return {WalletError::kAmountOverflow, std::nullopt};
            }
            recipient_total = updated_total;
            outputs.push_back(
                primitives::TxOutput{recipient.amount, MakeP2pkhScript(recipient.public_key_hash)});
        }

        const auto addresses = key_store.ReceivingAddresses();
        if (addresses.empty()) {
            return {WalletError::kNoReceivingAddress, std::nullopt};
        }
        std::vector<const WalletUTXO*> candidates;
        for (const auto& coin : available_coins) {
            if (coin.confirmed && !coin.pending_spend &&
                (!coin.is_coinbase || parameters.allow_coinbase_spends) && coin.output.value > 0 &&
                coin.output.value <= parameters.transaction_limits.max_money &&
                key_store.OwnsScript(coin.output.script_pubkey)) {
                candidates.push_back(&coin);
            }
        }
        std::sort(candidates.begin(), candidates.end(),
                  [](const WalletUTXO* left, const WalletUTXO* right) {
                      if (left->output.value != right->output.value) {
                          return left->output.value > right->output.value;
                      }
                      return left->key < right->key;
                  });

        primitives::Amount input_total{};
        std::vector<const WalletUTXO*> selected;
        std::optional<primitives::Amount> fee;
        for (const auto* coin : candidates) {
            primitives::Amount updated_total{};
            if (!CheckedAdd(input_total, coin->output.value, updated_total) ||
                updated_total > parameters.transaction_limits.max_money) {
                return {WalletError::kAmountOverflow, std::nullopt};
            }
            input_total = updated_total;
            selected.push_back(coin);
            fee = EstimateFee(selected.size(), outputs.size() + 1U, parameters);
            if (!fee.has_value()) {
                return {WalletError::kTransactionTooLarge, std::nullopt};
            }
            primitives::Amount required{};
            if (!CheckedAdd(recipient_total, *fee, required)) {
                return {WalletError::kAmountOverflow, std::nullopt};
            }
            if (input_total >= required) {
                break;
            }
        }
        if (!fee.has_value()) {
            return {WalletError::kInsufficientFunds, std::nullopt};
        }
        primitives::Amount required{};
        if (!CheckedAdd(recipient_total, *fee, required)) {
            return {WalletError::kAmountOverflow, std::nullopt};
        }
        if (input_total < required) {
            return {WalletError::kInsufficientFunds, std::nullopt};
        }
        const auto change = input_total - required;
        std::optional<primitives::TxOutput> change_output;
        if (change > 0) {
            change_output =
                primitives::TxOutput{change, MakeP2pkhScript(addresses.front().public_key_hash)};
            outputs.push_back(*change_output);
        }

        primitives::Transaction transaction{1, {}, std::move(outputs), 0U};
        transaction.inputs.reserve(selected.size());
        std::vector<chain::UTXOKey> selected_keys;
        selected_keys.reserve(selected.size());
        for (const auto* coin : selected) {
            transaction.inputs.push_back(primitives::TxInput{
                primitives::OutPoint{coin->key.transaction_id, coin->key.output_index},
                {},
                kFinalSequence});
            selected_keys.push_back(coin->key);
        }
        for (std::size_t index = 0U; index < selected.size(); ++index) {
            const auto key_hash = ParseP2pkhScript(selected[index]->output.script_pubkey);
            const auto* signing_key = key_hash.has_value() ? key_store.FindKey(*key_hash) : nullptr;
            if (signing_key == nullptr) {
                return {WalletError::kSigningFailure, std::nullopt};
            }
            const auto digest = chain::ComputeSignatureHash(
                transaction, index, selected[index]->output.script_pubkey,
                parameters.transaction_limits, parameters.sighash_all);
            if (!digest.has_value()) {
                return {WalletError::kSigningFailure, std::nullopt};
            }
            const auto signature = signing_key->Sign(*digest);
            if (!signature.has_value()) {
                return {WalletError::kSigningFailure, std::nullopt};
            }
            const auto script =
                MakeUnlockingScript(*signature, signing_key->public_key(), parameters.sighash_all);
            if (!script.has_value() ||
                script->size() > parameters.transaction_limits.max_script_size) {
                return {WalletError::kSigningFailure, std::nullopt};
            }
            transaction.inputs[index].script_sig = *script;
        }
        if (primitives::CheckTransactionStructure(transaction, parameters.transaction_limits) !=
            primitives::TransactionStructureError::kNone) {
            return {WalletError::kMalformedTransaction, std::nullopt};
        }
        for (std::size_t index = 0U; index < selected.size(); ++index) {
            if (!VerifyInput(transaction, index, selected[index]->output, parameters)) {
                return {WalletError::kLocalSignatureCheckFailed, std::nullopt};
            }
        }
        return {WalletError::kNone,
                BuiltTransaction{std::move(transaction), *fee, std::move(selected_keys),
                                 std::move(change_output)}};
    } catch (const std::bad_alloc&) {
        return {WalletError::kAllocationFailure, std::nullopt};
    } catch (...) {
        return {WalletError::kMalformedTransaction, std::nullopt};
    }
}

Wallet::Wallet(WalletParams parameters) noexcept : parameters_(parameters) {}

std::unique_ptr<Wallet> Wallet::Create(const WalletParams& parameters) noexcept
{
    if (!IsValidParameters(parameters)) {
        return nullptr;
    }
    try {
        return std::unique_ptr<Wallet>{new Wallet{parameters}};
    } catch (...) {
        return nullptr;
    }
}

std::optional<WalletAddress> Wallet::GenerateReceivingAddress() noexcept
{
    return key_store_.GenerateReceivingAddress();
}

std::vector<WalletAddress> Wallet::ListReceivingAddresses() const
{
    return key_store_.ReceivingAddresses();
}

std::vector<WalletUTXO> Wallet::ListUTXOs() const
{
    std::vector<WalletUTXO> result;
    result.reserve(coins_.size());
    for (const auto& [key, coin] : coins_) {
        (void)key;
        result.push_back(coin);
    }
    return result;
}

primitives::Amount Wallet::ConfirmedBalance() const noexcept
{
    primitives::Amount total{};
    for (const auto& [key, coin] : coins_) {
        (void)key;
        if (coin.confirmed && !coin.pending_spend && coin.output.value > 0 &&
            coin.output.value <= parameters_.transaction_limits.max_money &&
            coin.output.value <= parameters_.transaction_limits.max_money - total) {
            total += coin.output.value;
        }
    }
    return total;
}

primitives::Amount Wallet::UnconfirmedBalance() const noexcept
{
    primitives::Amount total{};
    for (const auto& [key, coin] : coins_) {
        (void)key;
        if (!coin.confirmed && !coin.pending_spend && coin.output.value > 0 &&
            coin.output.value <= parameters_.transaction_limits.max_money &&
            coin.output.value <= parameters_.transaction_limits.max_money - total) {
            total += coin.output.value;
        }
    }
    return total;
}

const primitives::TransactionLimits& Wallet::transaction_limits() const noexcept
{
    return parameters_.transaction_limits;
}

bool Wallet::ObserveTransaction(const primitives::Transaction& transaction,
                                const std::uint32_t height, const bool confirmed) noexcept
{
    if (primitives::CheckTransactionStructure(transaction, parameters_.transaction_limits) !=
        primitives::TransactionStructureError::kNone) {
        return false;
    }
    const auto transaction_id = transaction.TxId(parameters_.transaction_limits);
    if (!transaction_id.has_value()) {
        return false;
    }
    try {
        const auto is_coinbase = primitives::IsCoinbaseTransaction(transaction);
        auto updated_coins = coins_;
        for (const auto& input : transaction.inputs) {
            const auto iterator =
                updated_coins.find(chain::UTXOKey::FromOutPoint(input.previous_output));
            if (iterator == updated_coins.end()) {
                continue;
            }
            if (confirmed) {
                updated_coins.erase(iterator);
            } else {
                iterator->second.pending_spend = true;
            }
        }
        for (std::size_t index = 0U; index < transaction.outputs.size(); ++index) {
            const auto& output = transaction.outputs[index];
            if (!key_store_.OwnsScript(output.script_pubkey)) {
                continue;
            }
            const chain::UTXOKey key{*transaction_id, static_cast<std::uint32_t>(index)};
            updated_coins.insert_or_assign(
                key, WalletUTXO{key, output, height, is_coinbase, confirmed, false});
        }
        coins_.swap(updated_coins);
        return true;
    } catch (...) {
        return false;
    }
}

void Wallet::ClearConfirmedState() noexcept
{
    for (auto iterator = coins_.begin(); iterator != coins_.end();) {
        if (iterator->second.confirmed) {
            iterator = coins_.erase(iterator);
        } else {
            iterator->second.pending_spend = false;
            ++iterator;
        }
    }
}

TransactionBuildResult
Wallet::CreateTransaction(const std::span<const Recipient> recipients) const noexcept
{
    try {
        std::vector<WalletUTXO> coins;
        coins.reserve(coins_.size());
        for (const auto& [key, coin] : coins_) {
            (void)key;
            coins.push_back(coin);
        }
        return TransactionBuilder::Build(key_store_, coins, recipients, parameters_);
    } catch (const std::bad_alloc&) {
        return {WalletError::kAllocationFailure, std::nullopt};
    } catch (...) {
        return {WalletError::kMalformedTransaction, std::nullopt};
    }
}

TransactionSignResult
Wallet::SignTransaction(const primitives::Transaction& transaction) const noexcept
{
    if (primitives::CheckTransactionStructure(transaction, parameters_.transaction_limits) !=
        primitives::TransactionStructureError::kNone) {
        return {WalletError::kMalformedTransaction, std::nullopt};
    }
    try {
        auto signed_transaction = transaction;
        for (std::size_t index = 0U; index < signed_transaction.inputs.size(); ++index) {
            const auto key =
                chain::UTXOKey::FromOutPoint(signed_transaction.inputs[index].previous_output);
            const auto coin = coins_.find(key);
            if (coin == coins_.end()) {
                return {WalletError::kSigningFailure, std::nullopt};
            }
            const auto key_hash = ParseP2pkhScript(coin->second.output.script_pubkey);
            const auto* signing_key =
                key_hash.has_value() ? key_store_.FindKey(*key_hash) : nullptr;
            if (signing_key == nullptr) {
                return {WalletError::kSigningFailure, std::nullopt};
            }
            const auto digest = chain::ComputeSignatureHash(
                signed_transaction, index, coin->second.output.script_pubkey,
                parameters_.transaction_limits, parameters_.sighash_all);
            if (!digest.has_value()) {
                return {WalletError::kSigningFailure, std::nullopt};
            }
            const auto signature = signing_key->Sign(*digest);
            if (!signature.has_value()) {
                return {WalletError::kSigningFailure, std::nullopt};
            }
            const auto script =
                MakeUnlockingScript(*signature, signing_key->public_key(), parameters_.sighash_all);
            if (!script.has_value() ||
                script->size() > parameters_.transaction_limits.max_script_size) {
                return {WalletError::kSigningFailure, std::nullopt};
            }
            signed_transaction.inputs[index].script_sig = *script;
        }
        return VerifySignaturesLocally(signed_transaction)
                   ? TransactionSignResult{WalletError::kNone, std::move(signed_transaction)}
                   : TransactionSignResult{WalletError::kLocalSignatureCheckFailed, std::nullopt};
    } catch (const std::bad_alloc&) {
        return {WalletError::kAllocationFailure, std::nullopt};
    } catch (...) {
        return {WalletError::kSigningFailure, std::nullopt};
    }
}

bool Wallet::VerifySignaturesLocally(const primitives::Transaction& transaction) const noexcept
{
    if (primitives::CheckTransactionStructure(transaction, parameters_.transaction_limits) !=
        primitives::TransactionStructureError::kNone) {
        return false;
    }
    for (std::size_t index = 0U; index < transaction.inputs.size(); ++index) {
        const auto key = chain::UTXOKey::FromOutPoint(transaction.inputs[index].previous_output);
        const auto iterator = coins_.find(key);
        if (iterator == coins_.end() ||
            !VerifyInput(transaction, index, iterator->second.output, parameters_)) {
            return false;
        }
    }
    return true;
}

WalletError Wallet::Broadcast(const primitives::Transaction& transaction,
                              TransactionBroadcaster& broadcaster) const noexcept
{
    if (!VerifySignaturesLocally(transaction)) {
        return WalletError::kLocalSignatureCheckFailed;
    }
    try {
        return broadcaster.Broadcast(transaction).accepted ? WalletError::kNone
                                                           : WalletError::kBroadcastRejected;
    } catch (...) {
        return WalletError::kBroadcastRejected;
    }
}

bool Wallet::SaveEncrypted(const std::filesystem::path& path,
                           const std::span<const std::uint8_t> passphrase) const noexcept
{
    if (path.empty() || passphrase.empty() || !IsValidParameters(parameters_) ||
        key_store_.keys_.size() > kMaximumWalletKeys || coins_.size() > kMaximumWalletCoins) {
        return false;
    }
    try {
        primitives::BinaryWriter plaintext_writer;
        if (!plaintext_writer.WriteU32(kWalletFileVersion) ||
            !plaintext_writer.WriteU32(key_store_.next_index_) ||
            !plaintext_writer.WriteCompactSize(key_store_.keys_.size())) {
            return false;
        }
        for (const auto& [key_hash, stored_key] : key_store_.keys_) {
            (void)key_hash;
            if (!plaintext_writer.WriteFixedBytes(stored_key.key_pair.private_key().bytes()) ||
                !plaintext_writer.WriteU32(stored_key.index)) {
                return false;
            }
        }
        if (!plaintext_writer.WriteCompactSize(coins_.size())) {
            return false;
        }
        for (const auto& [key, coin] : coins_) {
            if (!plaintext_writer.WriteHash256(key.transaction_id) ||
                !plaintext_writer.WriteU32(key.output_index) ||
                !plaintext_writer.WriteI64(coin.output.value) ||
                !plaintext_writer.WriteBytes(coin.output.script_pubkey,
                                             parameters_.transaction_limits.max_script_size) ||
                !plaintext_writer.WriteU32(coin.height) ||
                !plaintext_writer.WriteU8(coin.is_coinbase ? 1U : 0U) ||
                !plaintext_writer.WriteU8(coin.confirmed ? 1U : 0U) ||
                !plaintext_writer.WriteU8(coin.pending_spend ? 1U : 0U)) {
                return false;
            }
        }
        SensitiveBytes plaintext{std::vector<std::uint8_t>{plaintext_writer.bytes().begin(),
                                                           plaintext_writer.bytes().end()}};
        std::array<std::uint8_t, kWalletSaltSize> salt{};
        std::array<std::uint8_t, kWalletNonceSize> nonce{};
        if (RAND_bytes(salt.data(), static_cast<int>(salt.size())) != 1 ||
            RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) != 1) {
            return false;
        }
        std::array<std::uint8_t, kWalletEncryptionKeySize> key{};
        const bool derived = DeriveWalletEncryptionKey(passphrase, salt, key);
        if (!derived) {
            Cleanse(key);
            return false;
        }
        std::array<std::uint8_t, kWalletTagSize> tag{};
        const auto ciphertext = EncryptWallet(plaintext.view(), key, nonce, tag);
        Cleanse(key);
        if (!ciphertext.has_value() || ciphertext->size() > kMaximumWalletFileSize) {
            return false;
        }
        primitives::BinaryWriter file_writer;
        if (!file_writer.WriteFixedBytes(kWalletFileMagic) ||
            !file_writer.WriteU32(kWalletFileVersion) ||
            !file_writer.WriteU32(kWalletKdfIterations) || !file_writer.WriteFixedBytes(salt) ||
            !file_writer.WriteFixedBytes(nonce) ||
            !file_writer.WriteBytes(*ciphertext, kMaximumWalletFileSize) ||
            !file_writer.WriteFixedBytes(tag)) {
            return false;
        }
        return WriteWalletFileAtomically(path, file_writer.bytes());
    } catch (...) {
        return false;
    }
}

std::unique_ptr<Wallet>
Wallet::LoadEncrypted(const WalletParams& parameters, const std::filesystem::path& path,
                      const std::span<const std::uint8_t> passphrase) noexcept
{
    if (path.empty() || passphrase.empty() || !IsValidParameters(parameters)) {
        return nullptr;
    }
    try {
        std::error_code error;
        const auto file_size = std::filesystem::file_size(path, error);
        if (error || file_size == 0U || file_size > kMaximumWalletFileSize) {
            return nullptr;
        }
        std::vector<std::uint8_t> encoded(static_cast<std::size_t>(file_size));
        std::ifstream input{path, std::ios::binary};
        if (!input.is_open()) {
            return nullptr;
        }
        input.read(reinterpret_cast<char*>(encoded.data()),
                   static_cast<std::streamsize>(encoded.size()));
        if (input.gcount() != static_cast<std::streamsize>(encoded.size())) {
            return nullptr;
        }
        primitives::BinaryReader file_reader{encoded};
        const auto magic = file_reader.ReadFixedBytes<kWalletFileMagic.size()>();
        const auto version = file_reader.ReadU32();
        const auto iterations = file_reader.ReadU32();
        const auto salt = file_reader.ReadFixedBytes<kWalletSaltSize>();
        const auto nonce = file_reader.ReadFixedBytes<kWalletNonceSize>();
        const auto ciphertext = file_reader.ReadBytes(kMaximumWalletFileSize);
        const auto tag = file_reader.ReadFixedBytes<kWalletTagSize>();
        if (!magic.has_value() || *magic != kWalletFileMagic || !version.has_value() ||
            *version != kWalletFileVersion || !iterations.has_value() ||
            *iterations != kWalletKdfIterations || !salt.has_value() || !nonce.has_value() ||
            !ciphertext.has_value() || ciphertext->empty() || !tag.has_value() ||
            !file_reader.RequireEnd()) {
            return nullptr;
        }
        std::array<std::uint8_t, kWalletEncryptionKeySize> key{};
        const bool derived = DeriveWalletEncryptionKey(passphrase, *salt, key);
        if (!derived) {
            Cleanse(key);
            return nullptr;
        }
        const auto plaintext = DecryptWallet(*ciphertext, key, *nonce, *tag);
        Cleanse(key);
        if (!plaintext.has_value()) {
            return nullptr;
        }
        primitives::BinaryReader reader{plaintext->view()};
        const auto plaintext_version = reader.ReadU32();
        const auto next_index = reader.ReadU32();
        const auto key_count = reader.ReadCompactSize(kMaximumWalletKeys);
        if (!plaintext_version.has_value() || *plaintext_version != kWalletFileVersion ||
            !next_index.has_value() || !key_count.has_value()) {
            return nullptr;
        }
        KeyStore restored_keys;
        for (std::uint64_t index = 0U; index < *key_count; ++index) {
            const auto private_key_bytes = reader.ReadFixedBytes<crypto::PrivateKey::kSize>();
            const auto key_index = reader.ReadU32();
            if (!private_key_bytes.has_value() || !key_index.has_value() ||
                *key_index >= *next_index) {
                return nullptr;
            }
            auto private_key = crypto::PrivateKey::FromBytes(*private_key_bytes);
            if (!private_key.has_value()) {
                return nullptr;
            }
            auto key_pair = crypto::KeyPair::FromPrivateKey(std::move(*private_key));
            if (!key_pair.has_value()) {
                return nullptr;
            }
            const auto key_hash =
                crypto::Hash160Digest(key_pair->public_key().SerializeCompressed());
            if (!key_hash.has_value() ||
                !restored_keys.keys_
                     .emplace(*key_hash, KeyStore::StoredKey{std::move(*key_pair), *key_index})
                     .second) {
                return nullptr;
            }
        }
        restored_keys.next_index_ = *next_index;
        const auto coin_count = reader.ReadCompactSize(kMaximumWalletCoins);
        if (!coin_count.has_value()) {
            return nullptr;
        }
        std::map<chain::UTXOKey, WalletUTXO> restored_coins;
        for (std::uint64_t index = 0U; index < *coin_count; ++index) {
            const auto transaction_id = reader.ReadHash256();
            const auto output_index = reader.ReadU32();
            const auto value = reader.ReadI64();
            auto script = reader.ReadBytes(parameters.transaction_limits.max_script_size);
            const auto height = reader.ReadU32();
            const auto is_coinbase = reader.ReadU8();
            const auto confirmed = reader.ReadU8();
            const auto pending_spend = reader.ReadU8();
            if (!transaction_id.has_value() || !output_index.has_value() || !value.has_value() ||
                !script.has_value() || !height.has_value() || !is_coinbase.has_value() ||
                !confirmed.has_value() || !pending_spend.has_value() || *value < 0 ||
                *value > parameters.transaction_limits.max_money || *is_coinbase > 1U ||
                *confirmed > 1U || *pending_spend > 1U) {
                return nullptr;
            }
            const chain::UTXOKey coin_key{*transaction_id, *output_index};
            const WalletUTXO coin{
                coin_key,         primitives::TxOutput{*value, std::move(*script)},
                *height,          *is_coinbase == 1U,
                *confirmed == 1U, *pending_spend == 1U};
            if (!restored_coins.emplace(coin_key, coin).second) {
                return nullptr;
            }
        }
        if (!reader.RequireEnd()) {
            return nullptr;
        }
        auto wallet = Wallet::Create(parameters);
        if (wallet == nullptr) {
            return nullptr;
        }
        wallet->key_store_ = std::move(restored_keys);
        wallet->coins_ = std::move(restored_coins);
        return wallet;
    } catch (...) {
        return nullptr;
    }
}

} // namespace nova::wallet
