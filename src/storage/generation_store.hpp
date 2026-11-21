#pragma once

#include "consensus/network_params.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>

namespace nova::storage
{

enum class GenerationError : std::uint8_t {
    kNone,
    kInvalidPath,
    kLocked,
    kIoFailure,
    kMalformed,
    kIdentityMismatch,
    kHashFailure,
    kStale,
    kExhausted,
    kAllocationFailure,
};

enum class GenerationState : std::uint8_t { kEmpty, kPresent, kUnavailable };
enum class GenerationCommitState : std::uint8_t { kCommitted, kNotCommitted, kUnavailable };

// Controlled candidate-file I/O model, usable only through in-process test APIs.
// These outcomes are not evidence of actual disk exhaustion or device failure.
enum class GenerationIoTarget : std::uint8_t { kJournal, kWallet, kMempool, kDescriptor, kRoot };
enum class GenerationIoFault : std::uint8_t {
    kNone,
    kShortWrite,
    kZeroProgress,
    kNoSpace,
    kPrefixThenNoSpace,
    kSyncFailure,
};

struct GenerationMetadata final {
    std::uint64_t sequence{};
    std::uint64_t journal_size{};
    std::uint64_t wallet_size{};
    crypto::Hash256 journal_hash;
    crypto::Hash256 wallet_hash;
    std::optional<std::uint64_t> mempool_size;
    crypto::Hash256 mempool_hash;
};
struct GenerationMetadataResult final {
    GenerationState state{GenerationState::kUnavailable};
    GenerationError error{GenerationError::kMalformed};
    GenerationMetadata metadata;
};
// Pure fixed-size metadata decoder. It does not open files, decrypt wallets,
// replay blocks, check image contents or admit a network. Expected parameters
// must already have been accepted by the caller's network selector.
[[nodiscard]] GenerationMetadataResult
DecodeGenerationMetadata(std::span<const std::uint8_t> root,
                         std::span<const std::uint8_t> descriptor,
                         const consensus::NetworkParams& expected) noexcept;

struct GenerationSnapshot final {
    std::uint64_t sequence{};
    std::filesystem::path journal;
    std::filesystem::path wallet;
    std::uint64_t journal_size{};
    std::uint64_t wallet_size{};
    crypto::Hash256 journal_hash;
    crypto::Hash256 wallet_hash;
    std::optional<std::filesystem::path> mempool;
    std::optional<std::uint64_t> mempool_size;
    crypto::Hash256 mempool_hash;
};

struct GenerationObservation final {
    GenerationState state{GenerationState::kUnavailable};
    GenerationError error{GenerationError::kIoFailure};
    GenerationSnapshot snapshot;
};

struct GenerationCommitResult final {
    // Authority under the documented platform API barriers, not a claim of
    // hardware power-loss qualification or node-level atomic publication.
    GenerationCommitState state{GenerationCommitState::kUnavailable};
    GenerationError error{GenerationError::kIoFailure};
};

// In-process fault checkpoints, never selectable by node configuration or RPC.
enum class GenerationFailurePoint : std::uint8_t {
    kNone,
    kAfterJournal,
    kAfterWallet,
    kAfterMempool,
    kAfterDescriptor,
    kBeforeRootReplace,
    kAfterRootReplace,
    kDirectoryBarrier,
    kObservation,
    kBeforeJournal,
    kBeforeWallet,
    kBeforeMempool,
    kBeforeDescriptor,
    kPartialJournal,
    kPartialWallet,
    kPartialMempool,
    kPartialDescriptor,
    kPartialRoot,
    // Synthetic close-result errors after real candidate-file close. They do
    // not claim an actual OS/device error or hardware power interruption.
    kJournalClose,
    kWalletClose,
    kMempoolClose,
    kDescriptorClose,
    kRootClose,
    kPrepareDirectoryBarrier,
    kRecoveryDirectoryBarrier,
};

// Exclusive single-writer local-storage primitive. The directory must already
// exist and be under trusted local custody. This does not validate consensus,
// encrypt wallets, provide a concurrent-reader protocol, or publish node state.
class GenerationStore final
{
  public:
    // Identifies this store lease only; it does not pin a generation root.
    class OwnerCheckpoint final
    {
      private:
        std::weak_ptr<const std::uint8_t> owner_;
        friend class GenerationStore;
    };
    [[nodiscard]] OwnerCheckpoint CaptureOwnerCheckpoint() const noexcept;
    [[nodiscard]] bool IsOwnerCheckpointCurrent(const OwnerCheckpoint& checkpoint) const noexcept;
    class Prepared final
    {
      public:
        ~Prepared();
        Prepared(const Prepared&) = delete;
        Prepared& operator=(const Prepared&) = delete;
        Prepared(Prepared&&) = delete;
        Prepared& operator=(Prepared&&) = delete;

      private:
        struct Data;
        explicit Prepared(std::unique_ptr<Data> data) noexcept;
        std::unique_ptr<Data> data_;
        friend class GenerationStore;
    };

    ~GenerationStore();
    GenerationStore(const GenerationStore&) = delete;
    GenerationStore& operator=(const GenerationStore&) = delete;
    GenerationStore(GenerationStore&&) = delete;
    GenerationStore& operator=(GenerationStore&&) = delete;

    [[nodiscard]] static std::unique_ptr<GenerationStore>
    Open(const std::filesystem::path& directory, const consensus::NetworkParams& network,
         GenerationError& error) noexcept;
    // Controlled startup faults only; no configuration/RPC entry point. Hooks
    // and injected errors are cleared before a successful store is returned.
    [[nodiscard]] static std::unique_ptr<GenerationStore>
    OpenForTesting(const std::filesystem::path& directory, const consensus::NetworkParams& network,
                   GenerationError& error, GenerationFailurePoint point, GenerationIoFault fault,
                   void (*hook)(GenerationFailurePoint) noexcept = nullptr) noexcept;
    [[nodiscard]] GenerationObservation Observe() const noexcept;
    [[nodiscard]] bool MatchesNetwork(const consensus::NetworkParams& network) const noexcept;
    // Inputs must be stable, private, already validated journal/encrypted-wallet
    // images. No raw keys or passphrases are accepted. Copying is streamed.
    [[nodiscard]] std::unique_ptr<Prepared>
    Prepare(const std::filesystem::path& journal_image,
            const std::filesystem::path& encrypted_wallet_image, GenerationError& error) noexcept;
    [[nodiscard]] std::unique_ptr<Prepared>
    Prepare(const std::filesystem::path& journal_image,
            const std::filesystem::path& encrypted_wallet_image,
            const std::filesystem::path& mempool_image, GenerationError& error) noexcept;
    // Read-only verification of the exact staged images. This does not make
    // the candidate authoritative or validate journal/wallet semantics.
    [[nodiscard]] GenerationObservation InspectPrepared(const Prepared& prepared) const noexcept;
    [[nodiscard]] GenerationCommitResult Commit(Prepared& prepared) noexcept;
    // A post-replacement error is never reported as rejection. Resolve checks
    // exact old/new authority and retries the directory barrier before deciding.
    [[nodiscard]] GenerationCommitResult Resolve(const Prepared& prepared) const noexcept;
    void FailNextForTesting(GenerationFailurePoint point) noexcept;
    void SetIoFaultForTesting(GenerationIoTarget target, GenerationIoFault fault) noexcept;
    [[nodiscard]] bool IoFaultTriggeredForTesting() const noexcept;
    // Test-only per-instance hook, invoked inside the selected checkpoint.
    // The application must never derive this callback from external input.
    void SetFailureHookForTesting(void (*hook)(GenerationFailurePoint) noexcept) noexcept;

  private:
    struct State;
    explicit GenerationStore(std::unique_ptr<State> state) noexcept;
    [[nodiscard]] static std::unique_ptr<GenerationStore>
    OpenImpl(const std::filesystem::path& directory, const consensus::NetworkParams& network,
             GenerationError& error, GenerationFailurePoint point, GenerationIoFault fault,
             void (*hook)(GenerationFailurePoint) noexcept) noexcept;
    [[nodiscard]] std::unique_ptr<Prepared>
    PrepareImages(const std::filesystem::path& journal_image,
                  const std::filesystem::path& encrypted_wallet_image,
                  const std::filesystem::path* mempool_image, GenerationError& error) noexcept;
    std::unique_ptr<State> state_;
};

} // namespace nova::storage
