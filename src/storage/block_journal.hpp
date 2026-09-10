#pragma once

#include "primitives/block.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace nova::storage
{

enum class BlockJournalError : std::uint8_t {
    kNone,
    kInvalidPath,
    kFilesystemFailure,
    kOpenFailure,
    kWriteFailure,
    kTruncatedRecord,
    kOversizedRecord,
    kMalformedRecord,
    kAllocationFailure,
};

// A one-shot test seam for deterministic crash/recovery regression tests. It
// is inert unless an in-process test explicitly enables it.
enum class JournalFailurePoint : std::uint8_t {
    kNone,
    kBeforeWrite,
    kDuringWrite,
    kAfterDurableWrite,
    kBeforeRecoveryCopy,
    kDuringRecoveryWrite,
    kBeforeRecoveryReplace,
    kAfterRecoveryReplace,
};

struct BlockJournalLoadResult final {
    BlockJournalError error{BlockJournalError::kNone};
    std::vector<primitives::Block> blocks;
    bool recovery_required{};
};

class BlockJournal final
{
  public:
    BlockJournal(const BlockJournal&) = delete;
    BlockJournal& operator=(const BlockJournal&) = delete;
    BlockJournal(BlockJournal&&) noexcept = default;
    BlockJournal& operator=(BlockJournal&&) noexcept = default;

    [[nodiscard]] static std::optional<BlockJournal>
    Open(const std::filesystem::path& data_directory, primitives::BlockLimits limits) noexcept;
    [[nodiscard]] BlockJournalError Prepare(const primitives::Block& block) const noexcept;
    [[nodiscard]] BlockJournalError Commit(const crypto::Hash256& block_hash) const noexcept;
    [[nodiscard]] BlockJournalLoadResult Load() const noexcept;
    [[nodiscard]] bool IsCommitted(const crypto::Hash256& block_hash) const noexcept;
    [[nodiscard]] const std::filesystem::path& path() const noexcept;

    static void SetFailurePointForTesting(JournalFailurePoint point) noexcept;

  private:
    BlockJournal(std::filesystem::path path, primitives::BlockLimits limits) noexcept;
    [[nodiscard]] BlockJournalError EnsureRecovered() const noexcept;

    std::filesystem::path path_;
    primitives::BlockLimits limits_;
    mutable bool recovery_required_{true};
};

} // namespace nova::storage
