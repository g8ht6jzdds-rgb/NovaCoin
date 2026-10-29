#pragma once

#include "consensus/network_params.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
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

struct GenerationMetadata final {
    std::uint64_t sequence{};
    std::uint64_t journal_size{};
    std::uint64_t wallet_size{};
    crypto::Hash256 journal_hash;
    crypto::Hash256 wallet_hash;
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
    kAfterDescriptor,
    kBeforeRootReplace,
    kAfterRootReplace,
    kDirectoryBarrier,
    kObservation,
};

// Exclusive single-writer local-storage primitive. The directory must already
// exist and be under trusted local custody. This does not validate consensus,
// encrypt wallets, provide a concurrent-reader protocol, or publish node state.
class GenerationStore final
{
  public:
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
    [[nodiscard]] GenerationObservation Observe() const noexcept;
    // Inputs must be stable, private, already validated journal/encrypted-wallet
    // images. No raw keys or passphrases are accepted. Copying is streamed.
    [[nodiscard]] std::unique_ptr<Prepared>
    Prepare(const std::filesystem::path& journal_image,
            const std::filesystem::path& encrypted_wallet_image, GenerationError& error) noexcept;
    [[nodiscard]] GenerationCommitResult Commit(Prepared& prepared) noexcept;
    // A post-replacement error is never reported as rejection. Resolve checks
    // exact old/new authority and retries the directory barrier before deciding.
    [[nodiscard]] GenerationCommitResult Resolve(const Prepared& prepared) const noexcept;
    void FailNextForTesting(GenerationFailurePoint point) noexcept;

  private:
    struct State;
    explicit GenerationStore(std::unique_ptr<State> state) noexcept;
    std::unique_ptr<State> state_;
};

} // namespace nova::storage
