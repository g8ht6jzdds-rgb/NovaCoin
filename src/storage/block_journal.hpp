#pragma once

#include "primitives/block.hpp"

#include <cstdint>
#include <filesystem>
#include <istream>
#include <limits>
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
    kResourceLimit,
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
    std::uint64_t bytes_read{};
    std::size_t record_count{};
};

struct JournalReadBudget final {
    std::uint64_t bytes{std::numeric_limits<std::uint64_t>::max()};
    std::size_t records{std::numeric_limits<std::size_t>::max()};
    std::size_t blocks{std::numeric_limits<std::size_t>::max()};
    std::size_t pending{std::numeric_limits<std::size_t>::max()};
};

// Pure decoder: never opens, creates, recovers or rewrites a file. Ordinary
// journal loading uses unlimited aggregate budgets to preserve existing policy.
[[nodiscard]] BlockJournalLoadResult ReadBlockJournal(std::istream& stream,
                                                      primitives::BlockLimits limits,
                                                      JournalReadBudget budget = {}) noexcept;

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
