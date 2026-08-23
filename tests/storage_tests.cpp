#include <gtest/gtest.h>

#include "consensus/network_params.hpp"
#include "storage/block_journal.hpp"

#include <filesystem>
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

} // namespace
