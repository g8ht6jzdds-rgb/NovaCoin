#include <gtest/gtest.h>

#include "consensus/network_params.hpp"
#include "storage/block_journal.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

namespace
{

using nova::storage::BlockJournal;
using nova::storage::BlockJournalError;
using nova::storage::JournalFailurePoint;

class BlockJournalTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        root_ = std::filesystem::temp_directory_path() / "novacoin-block-journal-test";
        std::error_code error;
        std::filesystem::remove_all(root_, error);
        ASSERT_FALSE(error);
        journal_ = BlockJournal::Open(root_, nova::consensus::RegtestNetworkParams().block_limits);
        ASSERT_TRUE(journal_.has_value());
        hash_ = nova::consensus::RegtestNetworkParams().genesis_hash;
    }

    void TearDown() override
    {
        BlockJournal::SetFailurePointForTesting(JournalFailurePoint::kNone);
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    std::filesystem::path root_;
    std::optional<BlockJournal> journal_;
    nova::crypto::Hash256 hash_;
};

TEST_F(BlockJournalTest, RecoversOnlyCommittedRecordsAcrossInjectedWriteFailures)
{
    const auto& block = nova::consensus::RegtestNetworkParams().genesis_block;

    BlockJournal::SetFailurePointForTesting(JournalFailurePoint::kBeforeWrite);
    EXPECT_EQ(journal_->Prepare(block), BlockJournalError::kWriteFailure);
    EXPECT_TRUE(journal_->Load().blocks.empty());

    BlockJournal::SetFailurePointForTesting(JournalFailurePoint::kDuringWrite);
    EXPECT_EQ(journal_->Prepare(block), BlockJournalError::kWriteFailure);
    const auto after_partial_prepare =
        BlockJournal::Open(root_, nova::consensus::RegtestNetworkParams().block_limits);
    ASSERT_TRUE(after_partial_prepare.has_value());
    EXPECT_TRUE(after_partial_prepare->Load().blocks.empty());

    // Start with a fresh journal for a prepare/commit pair.
    std::error_code error;
    std::filesystem::remove(journal_->path(), error);
    ASSERT_FALSE(error);
    ASSERT_EQ(journal_->Prepare(block), BlockJournalError::kNone);
    BlockJournal::SetFailurePointForTesting(JournalFailurePoint::kDuringWrite);
    EXPECT_EQ(journal_->Commit(hash_), BlockJournalError::kWriteFailure);
    EXPECT_FALSE(journal_->IsCommitted(hash_));

    std::filesystem::remove(journal_->path(), error);
    ASSERT_FALSE(error);
    ASSERT_EQ(journal_->Prepare(block), BlockJournalError::kNone);
    BlockJournal::SetFailurePointForTesting(JournalFailurePoint::kAfterDurableWrite);
    EXPECT_EQ(journal_->Commit(hash_), BlockJournalError::kWriteFailure);
    EXPECT_TRUE(journal_->IsCommitted(hash_));
    const auto reopened =
        BlockJournal::Open(root_, nova::consensus::RegtestNetworkParams().block_limits);
    ASSERT_TRUE(reopened.has_value());
    const auto recovered = reopened->Load();
    EXPECT_EQ(recovered.error, BlockJournalError::kNone);
    ASSERT_EQ(recovered.blocks.size(), 1U);
}

std::string ReadBytes(const std::filesystem::path& path)
{
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

TEST_F(BlockJournalTest, RecoveryIsRepeatableAtEveryRepairFailurePoint)
{
    const auto& network = nova::consensus::RegtestNetworkParams();
    const auto& block = network.genesis_block;
    auto next = block;
    ++next.header.nonce;
    const auto next_hash = nova::primitives::ComputeBlockHash(next.header);
    ASSERT_TRUE(next_hash.has_value());
    for (const auto point :
         {JournalFailurePoint::kBeforeRecoveryCopy, JournalFailurePoint::kDuringRecoveryWrite,
          JournalFailurePoint::kBeforeRecoveryReplace,
          JournalFailurePoint::kAfterRecoveryReplace}) {
        const auto directory = root_ / std::to_string(static_cast<unsigned int>(point));
        auto journal = BlockJournal::Open(directory, network.block_limits);
        ASSERT_TRUE(journal.has_value());
        ASSERT_EQ(journal->Prepare(block), BlockJournalError::kNone);
        ASSERT_EQ(journal->Commit(hash_), BlockJournalError::kNone);
        {
            std::ofstream output{journal->path(), std::ios::binary | std::ios::app};
            output.put(static_cast<char>(1));
        }
        const auto damaged = ReadBytes(journal->path());
        BlockJournal::SetFailurePointForTesting(point);
        EXPECT_FALSE(BlockJournal::Open(directory, network.block_limits).has_value());
        auto repaired = BlockJournal::Open(directory, network.block_limits);
        ASSERT_TRUE(repaired.has_value());
        EXPECT_EQ(ReadBytes(repaired->path().string() + ".recovery-0"), damaged);
        EXPECT_TRUE(repaired->IsCommitted(hash_));
        ASSERT_EQ(repaired->Prepare(next), BlockJournalError::kNone);
        ASSERT_EQ(repaired->Commit(*next_hash), BlockJournalError::kNone);
        const auto reopened = BlockJournal::Open(directory, network.block_limits);
        ASSERT_TRUE(reopened.has_value());
        EXPECT_TRUE(reopened->IsCommitted(hash_));
        EXPECT_TRUE(reopened->IsCommitted(*next_hash));
        EXPECT_EQ(reopened->Load().blocks.size(), 2U);
        EXPECT_FALSE(reopened->Load().recovery_required);
    }
}

TEST_F(BlockJournalTest, RemovesAbandonedPrepareAndPartialCommitBeforeRetry)
{
    const auto& network = nova::consensus::RegtestNetworkParams();
    for (const bool partial_commit : {false, true}) {
        const auto directory = root_ / (partial_commit ? "partial" : "abandoned");
        auto journal = BlockJournal::Open(directory, network.block_limits);
        ASSERT_TRUE(journal.has_value());
        ASSERT_EQ(journal->Prepare(network.genesis_block), BlockJournalError::kNone);
        if (partial_commit) {
            BlockJournal::SetFailurePointForTesting(JournalFailurePoint::kDuringWrite);
            EXPECT_EQ(journal->Commit(hash_), BlockJournalError::kWriteFailure);
        }
        auto reopened = BlockJournal::Open(directory, network.block_limits);
        ASSERT_TRUE(reopened.has_value());
        EXPECT_TRUE(reopened->Load().blocks.empty());
        ASSERT_EQ(reopened->Prepare(network.genesis_block), BlockJournalError::kNone);
        ASSERT_EQ(reopened->Commit(hash_), BlockJournalError::kNone);
        EXPECT_TRUE(reopened->IsCommitted(hash_));
    }
}

TEST_F(BlockJournalTest, RejectsAmbiguousCorruptionWithoutChangingBytes)
{
    for (const std::string& suffix :
         {std::string(1U, static_cast<char>(99)), std::string("\1\0\0\0\0", 5U),
          std::string("\1\377\377\377\377", 5U), std::string(33U, static_cast<char>(2))}) {
        std::ofstream output{journal_->path(), std::ios::binary | std::ios::trunc};
        output.write(suffix.data(), static_cast<std::streamsize>(suffix.size()));
        output.close();
        EXPECT_FALSE(BlockJournal::Open(root_, nova::consensus::RegtestNetworkParams().block_limits)
                         .has_value());
        EXPECT_EQ(ReadBytes(journal_->path()), suffix);
    }
}

TEST_F(BlockJournalTest, FailedAppendCannotBypassRecoveryOnSameHandle)
{
    const auto& block = nova::consensus::RegtestNetworkParams().genesis_block;
    BlockJournal::SetFailurePointForTesting(JournalFailurePoint::kDuringWrite);
    EXPECT_EQ(journal_->Prepare(block), BlockJournalError::kWriteFailure);
    BlockJournal::SetFailurePointForTesting(JournalFailurePoint::kBeforeRecoveryCopy);
    EXPECT_EQ(journal_->Prepare(block), BlockJournalError::kWriteFailure);
    ASSERT_EQ(journal_->Prepare(block), BlockJournalError::kNone);
    ASSERT_EQ(journal_->Commit(hash_), BlockJournalError::kNone);
    EXPECT_TRUE(journal_->IsCommitted(hash_));
}

TEST_F(BlockJournalTest, RepairsEveryTruncatedFinalRecordBoundary)
{
    const auto& network = nova::consensus::RegtestNetworkParams();
    ASSERT_EQ(journal_->Prepare(network.genesis_block), BlockJournalError::kNone);
    ASSERT_EQ(journal_->Commit(hash_), BlockJournalError::kNone);
    const auto pair = ReadBytes(journal_->path());
    auto next = network.genesis_block;
    ++next.header.nonce;
    const auto next_hash = nova::primitives::ComputeBlockHash(next.header);
    ASSERT_TRUE(next_hash.has_value());
    for (std::size_t count = 1; count < pair.size(); ++count) {
        const auto directory = root_ / ("boundary-" + std::to_string(count));
        std::filesystem::create_directory(directory);
        const auto path = directory / "blocks.dat";
        {
            std::ofstream output{path, std::ios::binary};
            output.write(pair.data(), static_cast<std::streamsize>(pair.size()));
            output.write(pair.data(), static_cast<std::streamsize>(count));
        }
        auto recovered = BlockJournal::Open(directory, network.block_limits);
        ASSERT_TRUE(recovered.has_value()) << count;
        ASSERT_EQ(recovered->Load().blocks.size(), 1U) << count;
        ASSERT_EQ(recovered->Prepare(next), BlockJournalError::kNone);
        ASSERT_EQ(recovered->Commit(*next_hash), BlockJournalError::kNone);
        const auto restarted = BlockJournal::Open(directory, network.block_limits);
        ASSERT_TRUE(restarted.has_value());
        EXPECT_EQ(restarted->Load().blocks.size(), 2U) << count;
        EXPECT_TRUE(restarted->IsCommitted(*next_hash));
    }
}

} // namespace
