#pragma once

#include "chain/utxo.hpp"
#include "crypto/crypto.hpp"
#include "primitives/transaction.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace nova::wallet
{

struct WalletAddress final {
    crypto::Hash160 public_key_hash{};
    crypto::PublicKey public_key;
    std::uint32_t index{};
};

struct WalletUTXO final {
    chain::UTXOKey key;
    primitives::TxOutput output;
    std::uint32_t height{};
    bool is_coinbase{};
    bool confirmed{};
    bool pending_spend{};
};

struct Recipient final {
    crypto::Hash160 public_key_hash{};
    primitives::Amount amount{};
};

struct WalletParams final {
    primitives::TransactionLimits transaction_limits;
    primitives::Amount fee_rate_per_byte{};
    std::uint32_t sighash_all{1U};
    bool allow_coinbase_spends{};
};

enum class WalletError : std::uint8_t {
    kNone,
    kInvalidParameters,
    kKeyGenerationFailure,
    kNoReceivingAddress,
    kInvalidRecipient,
    kAmountOverflow,
    kInsufficientFunds,
    kTransactionTooLarge,
    kSigningFailure,
    kLocalSignatureCheckFailed,
    kMalformedTransaction,
    kBroadcastRejected,
    kAllocationFailure,
};

struct BuiltTransaction final {
    primitives::Transaction transaction;
    primitives::Amount fee{};
    std::vector<chain::UTXOKey> selected_inputs;
    std::optional<primitives::TxOutput> change_output;
};

struct TransactionBuildResult final {
    WalletError error{WalletError::kNone};
    std::optional<BuiltTransaction> built;
};

struct TransactionSignResult final {
    WalletError error{WalletError::kNone};
    std::optional<primitives::Transaction> transaction;
};

struct BroadcastResult final {
    bool accepted{};
};

class TransactionBroadcaster
{
  public:
    virtual ~TransactionBroadcaster() = default;
    [[nodiscard]] virtual BroadcastResult Broadcast(const primitives::Transaction& transaction) = 0;
};

class KeyStore final
{
  public:
    KeyStore() = default;
    KeyStore(const KeyStore&) = delete;
    KeyStore& operator=(const KeyStore&) = delete;
    KeyStore(KeyStore&&) noexcept = default;
    KeyStore& operator=(KeyStore&&) noexcept = default;

    [[nodiscard]] std::optional<WalletAddress> GenerateReceivingAddress() noexcept;
    [[nodiscard]] std::vector<WalletAddress> ReceivingAddresses() const;
    [[nodiscard]] bool OwnsScript(std::span<const std::uint8_t> script_pubkey) const noexcept;

  private:
    struct StoredKey final {
        crypto::KeyPair key_pair;
        std::uint32_t index{};
    };

    [[nodiscard]] const crypto::KeyPair*
    FindKey(const crypto::Hash160& public_key_hash) const noexcept;

    std::map<crypto::Hash160, StoredKey> keys_;
    std::uint32_t next_index_{};

    friend class TransactionBuilder;
    friend class Wallet;
};

class TransactionBuilder final
{
  public:
    [[nodiscard]] static std::optional<primitives::Amount>
    EstimateFee(std::size_t input_count, std::size_t output_count,
                const WalletParams& parameters) noexcept;
    [[nodiscard]] static TransactionBuildResult Build(const KeyStore& key_store,
                                                      std::span<const WalletUTXO> available_coins,
                                                      std::span<const Recipient> recipients,
                                                      const WalletParams& parameters) noexcept;
};

class Wallet final
{
  public:
    Wallet(const Wallet&) = delete;
    Wallet& operator=(const Wallet&) = delete;
    Wallet(Wallet&&) noexcept = default;
    Wallet& operator=(Wallet&&) noexcept = default;

    [[nodiscard]] static std::unique_ptr<Wallet> Create(const WalletParams& parameters) noexcept;
    [[nodiscard]] std::optional<WalletAddress> GenerateReceivingAddress() noexcept;
    [[nodiscard]] std::vector<WalletAddress> ListReceivingAddresses() const;
    [[nodiscard]] std::vector<WalletUTXO> ListUTXOs() const;
    [[nodiscard]] primitives::Amount ConfirmedBalance() const noexcept;
    [[nodiscard]] primitives::Amount UnconfirmedBalance() const noexcept;
    [[nodiscard]] const primitives::TransactionLimits& transaction_limits() const noexcept;

    // An external chain/mempool observer supplies transaction state.  This
    // method only updates wallet-local bookkeeping and cannot update consensus state.
    [[nodiscard]] bool ObserveTransaction(const primitives::Transaction& transaction,
                                          std::uint32_t height, bool confirmed) noexcept;
    // Rebuilds wallet-local confirmed state from the active chain. Keys remain
    // intact; unconfirmed observations may be applied again by the caller.
    void ClearConfirmedState() noexcept;
    [[nodiscard]] TransactionBuildResult
    CreateTransaction(std::span<const Recipient> recipients) const noexcept;
    [[nodiscard]] TransactionSignResult
    SignTransaction(const primitives::Transaction& transaction) const noexcept;
    [[nodiscard]] bool
    VerifySignaturesLocally(const primitives::Transaction& transaction) const noexcept;
    [[nodiscard]] WalletError Broadcast(const primitives::Transaction& transaction,
                                        TransactionBroadcaster& broadcaster) const noexcept;
    // Persists an authenticated-encrypted wallet file. Empty passphrases and
    // plaintext private-key export are deliberately unsupported.
    [[nodiscard]] bool SaveEncrypted(const std::filesystem::path& path,
                                     std::span<const std::uint8_t> passphrase) const noexcept;
    [[nodiscard]] static std::unique_ptr<Wallet>
    LoadEncrypted(const WalletParams& parameters, const std::filesystem::path& path,
                  std::span<const std::uint8_t> passphrase) noexcept;

  private:
    explicit Wallet(WalletParams parameters) noexcept;

    WalletParams parameters_;
    KeyStore key_store_;
    std::map<chain::UTXOKey, WalletUTXO> coins_;
};

} // namespace nova::wallet
