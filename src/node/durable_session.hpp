#pragma once

#include "node/generation_recovery.hpp"
#include "node/prepared_block.hpp"
#include "node/sync.hpp"

namespace nova::node
{

enum class DurableSessionError : std::uint8_t {
    kNone,
    kChain,
    kWallet,
    kMempool,
    kMining,
    kContext,
    kStorage,
    kUnavailable
};
struct DurableSessionResult final {
    DurableSessionError error{DurableSessionError::kUnavailable};
    chain::ChainResult chain_result;
};
struct DurableSessionView final {
    crypto::Hash256 tip;
    std::uint32_t height{};
    std::uint32_t next_time{};
    primitives::Amount confirmed_balance{};
    std::size_t mempool_size{};
    std::size_t receiving_address_count{};
    chain::ChainWork chain_work;
    std::uint64_t mempool_bytes{};
};
struct DurableAddressResult final {
    DurableSessionError error{DurableSessionError::kUnavailable};
    std::optional<wallet::WalletAddress> address;
};
struct DurableTransactionResult final {
    DurableSessionError error{DurableSessionError::kUnavailable};
    // On kUnavailable this can be an unconfirmed candidate, not evidence of
    // admission; callers must recover and reconcile before taking action.
    std::optional<primitives::Transaction> transaction;
};
struct DurableBlockLookup final {
    bool available{};
    std::optional<chain::BlockIndex> block;
};
struct DurableMempoolLookup final {
    bool available{};
    std::optional<chain::MempoolEntry> entry;
};
enum class DurableBodyState : std::uint8_t { kPending, kValidated, kStale };
struct DurableWalletView final {
    primitives::Amount confirmed{};
    primitives::Amount unconfirmed{};
    std::vector<wallet::WalletUTXO> coins;
};

// Owns a verified existing generation and exposes no mutable components.
// Single-threaded, non-reentrant use and a trusted, private staging directory
// are required. All supported daemon writer paths use this owner. Legacy
// storage can be supplied only through a separately verified offline migration.
class DurableSession final
{
  public:
    DurableSession(const DurableSession&) = delete;
    DurableSession& operator=(const DurableSession&) = delete;
    [[nodiscard]] static std::unique_ptr<DurableSession>
    Open(std::unique_ptr<storage::GenerationStore> store, consensus::NetworkId network,
         std::shared_ptr<const chain::ValidationTimeSource> time_source, std::uint32_t start_time,
         std::span<const std::uint8_t> passphrase) noexcept;
    [[nodiscard]] static std::unique_ptr<DurableSession>
    CreateFresh(std::unique_ptr<storage::GenerationStore> store, consensus::NetworkId network,
                std::shared_ptr<const chain::ValidationTimeSource> time_source,
                std::uint32_t start_time, std::span<const std::uint8_t> passphrase,
                const std::filesystem::path& fresh_staging_directory) noexcept;
    [[nodiscard]] std::optional<DurableSessionView> View() const noexcept;
    [[nodiscard]] std::optional<std::vector<chain::BlockIndex>> ActiveBlocks() const noexcept;
    // Copies committed history, including side branches, under current authority.
    [[nodiscard]] std::optional<std::vector<primitives::Block>> CommittedBlocks() const noexcept;
    [[nodiscard]] const consensus::NetworkParams& network_params() const noexcept
    {
        return *network_;
    }
    // A missing block is distinct from unavailable session authority.
    [[nodiscard]] DurableBlockLookup FindBlock(const crypto::Hash256& hash) const noexcept;
    [[nodiscard]] DurableMempoolLookup FindMempoolEntry(const crypto::Hash256& hash) const noexcept;
    [[nodiscard]] chain::CoinReadResult ReadCoin(const chain::UTXOKey& key) const noexcept;
    [[nodiscard]] std::optional<DurableWalletView> WalletView() const noexcept;
    // Header synchronization is session-owned; block messages must use ApplyBlock.
    [[nodiscard]] SyncResult StartHeaderSync() noexcept;
    [[nodiscard]] SyncResult HandleSyncHeaders(const net::FramedMessage& message) noexcept;
    // Unavailable authority is distinct from a stale or missing header.
    [[nodiscard]] std::optional<DurableBodyState>
    HeaderBodyState(const crypto::Hash256& hash) const noexcept;
    // A missing result means unavailable authority, invalid bounds or allocation failure.
    [[nodiscard]] std::optional<net::HeadersMessage>
    HeadersForLocator(const net::GetHeadersMessage& request) const noexcept;
    // Returns a locally constructed transaction without admitting or relaying it.
    [[nodiscard]] std::optional<wallet::TransactionBuildResult>
    BuildTransaction(std::span<const wallet::Recipient> recipients) const noexcept;
    [[nodiscard]] std::optional<wallet::TransactionSignResult>
    SignTransaction(const primitives::Transaction& transaction) const noexcept;
    [[nodiscard]] std::optional<bool>
    VerifyTransaction(const primitives::Transaction& transaction) const noexcept;
    [[nodiscard]] DurableAddressResult
    GenerateAddress(const std::filesystem::path& fresh_staging_directory,
                    std::span<const std::uint8_t> passphrase) noexcept;
    [[nodiscard]] DurableSessionError
    AdmitTransaction(const primitives::Transaction& transaction,
                     const std::filesystem::path& fresh_staging_directory,
                     std::span<const std::uint8_t> passphrase) noexcept;
    [[nodiscard]] DurableTransactionResult
    BuildAndAdmit(std::span<const wallet::Recipient> recipients,
                  const std::filesystem::path& fresh_staging_directory,
                  std::span<const std::uint8_t> passphrase) noexcept;
    [[nodiscard]] DurableSessionResult
    ApplyBlock(const primitives::Block& block, std::span<const primitives::Transaction> known,
               const std::filesystem::path& fresh_staging_directory,
               std::span<const std::uint8_t> passphrase) noexcept;
    [[nodiscard]] DurableSessionResult
    MineBlock(std::span<const primitives::Transaction> known,
              const std::filesystem::path& fresh_staging_directory,
              std::span<const std::uint8_t> passphrase) noexcept;
    void FailStoreForTesting(storage::GenerationFailurePoint point) noexcept;
    void SetStoreIoFaultForTesting(storage::GenerationIoTarget target,
                                   storage::GenerationIoFault fault) noexcept;
    [[nodiscard]] bool StoreIoFaultTriggeredForTesting() const noexcept;
    void FailAdvanceForTesting() noexcept;

  private:
    DurableSession(std::unique_ptr<storage::GenerationStore> store,
                   std::unique_ptr<RecoveredGeneration> recovered,
                   std::unique_ptr<chain::Mempool> mempool,
                   const consensus::NetworkParams& network) noexcept;
    [[nodiscard]] bool AuthorityMatches() const noexcept;
    [[nodiscard]] DurableSessionResult
    ApplyBlockInternal(const primitives::Block& block,
                       std::span<const primitives::Transaction> known,
                       const std::filesystem::path& fresh_staging_directory,
                       std::span<const std::uint8_t> passphrase,
                       wallet::Wallet::PreparedAddress* address) noexcept;
    // Declaration order ensures recovered state is destroyed before store.
    std::unique_ptr<storage::GenerationStore> store_;
    std::unique_ptr<RecoveredGeneration> recovered_;
    std::unique_ptr<chain::Mempool> mempool_;
    const consensus::NetworkParams* network_{};
    std::unique_ptr<Synchronizer> synchronizer_;
    bool quarantined_{};
};
} // namespace nova::node
