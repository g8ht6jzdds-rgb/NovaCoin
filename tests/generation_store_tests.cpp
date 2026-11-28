#include "storage/generation_store.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace
{
using nova::storage::GenerationCommitState;
using nova::storage::GenerationError;
using nova::storage::GenerationFailurePoint;
using nova::storage::GenerationState;
using nova::storage::GenerationStore;

class GenerationStoreTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        static std::atomic_uint64_t sequence{};
        root_ = std::filesystem::temp_directory_path() /
                ("nova-generation-test-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                 std::to_string(sequence.fetch_add(1U)));
        ASSERT_TRUE(std::filesystem::create_directory(root_));
        directory_ = root_ / "store";
        ASSERT_TRUE(std::filesystem::create_directory(directory_));
        journal_ = root_ / "input.journal";
        wallet_ = root_ / "input.wallet";
        Write(journal_, "controlled non-secret journal image");
        Write(wallet_, "opaque test image; no wallet or keys");
        store_ = GenerationStore::Open(directory_, nova::consensus::RegtestNetworkParams(), error_);
        ASSERT_NE(store_, nullptr);
        ASSERT_EQ(error_, GenerationError::kNone);
    }
    void TearDown() override
    {
        store_.reset();
        if (!root_.empty()) {
            if (HasFailure()) {
                std::fprintf(stderr, "Retained failed generation-store fixture: %s\n",
                             root_.string().c_str());
                return;
            }
            std::error_code error;
            std::filesystem::remove_all(root_, error);
        }
    }
    static void Write(const std::filesystem::path& path, const std::string& bytes)
    {
        std::ofstream output{path, std::ios::binary | std::ios::trunc};
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        ASSERT_TRUE(output.good());
    }
    static std::string Read(const std::filesystem::path& path)
    {
        std::ifstream input{path, std::ios::binary};
        return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    }
    std::unique_ptr<GenerationStore::Prepared> Prepare()
    {
        return store_->Prepare(journal_, wallet_, error_);
    }
    std::filesystem::path root_, directory_, journal_, wallet_;
    GenerationError error_{};
    std::unique_ptr<GenerationStore> store_;
};

TEST_F(GenerationStoreTest, InitialSelectorErrorsRefuseAdmissionWithoutInventingAuthority)
{
    using Fault = nova::storage::GenerationIoFault;
    unsigned index{};
    for (const auto fault :
         {Fault::kZeroProgress, Fault::kNoSpace, Fault::kPrefixThenNoSpace, Fault::kSyncFailure}) {
        const auto path = root_ / ("initial-io-" + std::to_string(index++));
        ASSERT_TRUE(std::filesystem::create_directory(path));
        auto opened = GenerationStore::OpenForTesting(path, nova::consensus::RegtestNetworkParams(),
                                                      error_, GenerationFailurePoint::kNone, fault);
        ASSERT_EQ(opened, nullptr);
        ASSERT_EQ(error_, GenerationError::kIoFailure);
        ASSERT_FALSE(std::filesystem::exists(path / "generation.current"));
        const auto retained = Read(path / "generation.initial");
        if (fault == Fault::kPrefixThenNoSpace)
            EXPECT_EQ(retained.size(), 1U);
        else if (fault == Fault::kSyncFailure)
            EXPECT_GT(retained.size(), 1U);
        else
            EXPECT_TRUE(retained.empty());
        for (unsigned restart = 0; restart < 2U; ++restart) {
            opened = GenerationStore::Open(path, nova::consensus::RegtestNetworkParams(), error_);
            EXPECT_EQ(opened, nullptr);
            EXPECT_EQ(error_, GenerationError::kMalformed);
            EXPECT_EQ(Read(path / "generation.initial"), retained);
            EXPECT_FALSE(std::filesystem::exists(path / "generation.current"));
        }
    }
}

TEST_F(GenerationStoreTest, InitialSelectorCheckpointsRetainExactAuthorityOrRefuseStartup)
{
    using Point = GenerationFailurePoint;
    unsigned index{};
    const auto expected = Read(directory_ / "generation.current");
    ASSERT_FALSE(expected.empty());
    for (const auto point :
         {Point::kPartialRoot, Point::kRootClose, Point::kPrepareDirectoryBarrier,
          Point::kBeforeRootReplace, Point::kAfterRootReplace, Point::kDirectoryBarrier,
          Point::kRecoveryDirectoryBarrier}) {
        const auto path = root_ / ("initial-point-" + std::to_string(index++));
        ASSERT_TRUE(std::filesystem::create_directory(path));
        auto opened =
            GenerationStore::OpenForTesting(path, nova::consensus::RegtestNetworkParams(), error_,
                                            point, nova::storage::GenerationIoFault::kNone);
        ASSERT_EQ(opened, nullptr);
        ASSERT_EQ(error_, GenerationError::kIoFailure);
        const bool replaced = point == Point::kAfterRootReplace ||
                              point == Point::kDirectoryBarrier ||
                              point == Point::kRecoveryDirectoryBarrier;
        EXPECT_EQ(std::filesystem::exists(path / "generation.current"), replaced);
        for (unsigned restart = 0; restart < 2U; ++restart) {
            opened = GenerationStore::Open(path, nova::consensus::RegtestNetworkParams(), error_);
            if (replaced) {
                ASSERT_NE(opened, nullptr);
                EXPECT_EQ(opened->Observe().state, GenerationState::kEmpty);
                EXPECT_EQ(Read(path / "generation.current"), expected);
            } else {
                EXPECT_EQ(opened, nullptr);
                EXPECT_EQ(error_, GenerationError::kMalformed);
                const auto bytes = Read(path / "generation.initial");
                EXPECT_EQ(bytes, point == Point::kPartialRoot ? expected.substr(0, 1U) : expected);
            }
            opened.reset();
        }
    }
}

TEST_F(GenerationStoreTest, StartupShortWritesCompleteAndRecoveryBarrierFailurePreservesImages)
{
    const auto path = root_ / "initial-short";
    ASSERT_TRUE(std::filesystem::create_directory(path));
    auto opened = GenerationStore::OpenForTesting(path, nova::consensus::RegtestNetworkParams(),
                                                  error_, GenerationFailurePoint::kNone,
                                                  nova::storage::GenerationIoFault::kShortWrite);
    ASSERT_NE(opened, nullptr);
    EXPECT_EQ(Read(path / "generation.current"), Read(directory_ / "generation.current"));
    auto prepared = opened->Prepare(journal_, wallet_, error_);
    ASSERT_NE(prepared, nullptr);
    ASSERT_EQ(opened->Commit(*prepared).state, GenerationCommitState::kCommitted);
    const auto snapshot = opened->Observe().snapshot;
    const auto root = Read(path / "generation.current");
    const auto journal = Read(snapshot.journal);
    const auto wallet = Read(snapshot.wallet);
    prepared.reset();
    opened.reset();
    for (unsigned restart = 0; restart < 2U; ++restart) {
        opened =
            GenerationStore::OpenForTesting(path, nova::consensus::RegtestNetworkParams(), error_,
                                            GenerationFailurePoint::kRecoveryDirectoryBarrier,
                                            nova::storage::GenerationIoFault::kNone);
        EXPECT_EQ(opened, nullptr);
        EXPECT_EQ(error_, GenerationError::kIoFailure);
        EXPECT_EQ(Read(path / "generation.current"), root);
        EXPECT_EQ(Read(snapshot.journal), journal);
        EXPECT_EQ(Read(snapshot.wallet), wallet);
        opened = GenerationStore::Open(path, nova::consensus::RegtestNetworkParams(), error_);
        ASSERT_NE(opened, nullptr);
        EXPECT_EQ(opened->Observe().state, GenerationState::kPresent);
        opened.reset();
    }
}

TEST_F(GenerationStoreTest, PartialCandidateWritesRetainOldAuthorityAcrossRestartsAndRetry)
{
    const auto mempool = root_ / "input.mempool";
    Write(mempool, "old controlled pending image");
    auto initial = store_->Prepare(journal_, wallet_, mempool, error_);
    ASSERT_NE(initial, nullptr);
    ASSERT_EQ(store_->Commit(*initial).state, GenerationCommitState::kCommitted);
    initial.reset();
    for (const auto* payload : {"", "candidate controlled pending image"}) {
        Write(mempool, payload);
        for (const auto& [point, extension] :
             {std::pair{GenerationFailurePoint::kPartialJournal, ".journal"},
              std::pair{GenerationFailurePoint::kPartialWallet, ".wallet"},
              std::pair{GenerationFailurePoint::kPartialMempool, ".mempool"},
              std::pair{GenerationFailurePoint::kPartialDescriptor, ".descriptor"},
              std::pair{GenerationFailurePoint::kPartialRoot, ".root"}}) {
            const auto old = store_->Observe();
            ASSERT_EQ(old.state, GenerationState::kPresent);
            ASSERT_TRUE(old.snapshot.mempool);
            const auto root_bytes = Read(directory_ / "generation.current");
            const auto journal_bytes = Read(old.snapshot.journal);
            const auto wallet_bytes = Read(old.snapshot.wallet);
            const auto pending_bytes = Read(*old.snapshot.mempool);
            store_->FailNextForTesting(point);
            EXPECT_EQ(store_->Prepare(journal_, wallet_, mempool, error_), nullptr);
            EXPECT_EQ(error_, GenerationError::kIoFailure);
            std::array<char, 64U> filename{};
            ASSERT_GT(std::snprintf(filename.data(), filename.size(), "generation-%016llx%s",
                                    static_cast<unsigned long long>(old.snapshot.sequence + 1U),
                                    extension),
                      0);
            const auto partial_path = directory_ / filename.data();
            ASSERT_TRUE(std::filesystem::is_regular_file(partial_path));
            const auto partial_bytes = Read(partial_path);
            if (point == GenerationFailurePoint::kPartialMempool && *payload == '\0')
                EXPECT_TRUE(partial_bytes.empty());
            else
                EXPECT_EQ(partial_bytes.size(), 1U);
            for (unsigned restart = 0U; restart < 2U; ++restart) {
                store_.reset();
                store_ = GenerationStore::Open(directory_, nova::consensus::RegtestNetworkParams(),
                                               error_);
                ASSERT_NE(store_, nullptr);
                const auto restored = store_->Observe();
                ASSERT_EQ(restored.state, GenerationState::kPresent);
                EXPECT_EQ(restored.snapshot.sequence, old.snapshot.sequence);
                EXPECT_EQ(Read(directory_ / "generation.current"), root_bytes);
                EXPECT_EQ(Read(restored.snapshot.journal), journal_bytes);
                EXPECT_EQ(Read(restored.snapshot.wallet), wallet_bytes);
                ASSERT_TRUE(restored.snapshot.mempool);
                EXPECT_EQ(Read(*restored.snapshot.mempool), pending_bytes);
                EXPECT_EQ(Read(partial_path), partial_bytes);
            }
            auto retry = store_->Prepare(journal_, wallet_, mempool, error_);
            ASSERT_NE(retry, nullptr);
            ASSERT_EQ(store_->Commit(*retry).state, GenerationCommitState::kCommitted);
            const auto committed = store_->Observe();
            ASSERT_EQ(committed.state, GenerationState::kPresent);
            EXPECT_GT(committed.snapshot.sequence, old.snapshot.sequence + 1U);
            EXPECT_EQ(Read(committed.snapshot.journal), Read(journal_));
            EXPECT_EQ(Read(committed.snapshot.wallet), Read(wallet_));
            ASSERT_TRUE(committed.snapshot.mempool);
            EXPECT_EQ(Read(*committed.snapshot.mempool), payload);
        }
    }
}

TEST_F(GenerationStoreTest, CandidateCloseErrorsDoNotPublishAndRetrySurvivesRestart)
{
    const auto mempool = root_ / "input.mempool";
    Write(mempool, "old pending image");
    auto initial = store_->Prepare(journal_, wallet_, mempool, error_);
    ASSERT_NE(initial, nullptr);
    ASSERT_EQ(store_->Commit(*initial).state, GenerationCommitState::kCommitted);
    initial.reset();
    for (const bool empty : {false, true}) {
        for (const auto point :
             {GenerationFailurePoint::kJournalClose, GenerationFailurePoint::kWalletClose,
              GenerationFailurePoint::kMempoolClose, GenerationFailurePoint::kDescriptorClose,
              GenerationFailurePoint::kRootClose}) {
            SCOPED_TRACE(static_cast<unsigned>(point));
            SCOPED_TRACE(empty);
            const auto old = store_->Observe();
            ASSERT_EQ(old.state, GenerationState::kPresent);
            ASSERT_TRUE(old.snapshot.mempool);
            const auto root_bytes = Read(directory_ / "generation.current");
            const auto old_journal = Read(old.snapshot.journal);
            const auto old_wallet = Read(old.snapshot.wallet);
            const auto old_pending = Read(*old.snapshot.mempool);
            // These are opaque storage images, not consensus-valid journals or keys.
            Write(journal_, empty ? "" : "new journal image");
            Write(wallet_, empty ? "" : "new encrypted-image stand-in");
            Write(mempool, empty ? "" : "new pending image");
            store_->FailNextForTesting(point);
            EXPECT_EQ(store_->Prepare(journal_, wallet_, mempool, error_), nullptr);
            EXPECT_EQ(error_, GenerationError::kIoFailure);
            EXPECT_EQ(Read(directory_ / "generation.current"), root_bytes);
            for (unsigned restart = 0U; restart < 2U; ++restart) {
                store_.reset();
                store_ = GenerationStore::Open(directory_, nova::consensus::RegtestNetworkParams(),
                                               error_);
                ASSERT_NE(store_, nullptr);
                const auto recovered = store_->Observe();
                ASSERT_EQ(recovered.state, GenerationState::kPresent);
                EXPECT_EQ(recovered.snapshot.sequence, old.snapshot.sequence);
                EXPECT_EQ(Read(directory_ / "generation.current"), root_bytes);
                EXPECT_EQ(Read(recovered.snapshot.journal), old_journal);
                EXPECT_EQ(Read(recovered.snapshot.wallet), old_wallet);
                ASSERT_TRUE(recovered.snapshot.mempool);
                EXPECT_EQ(Read(*recovered.snapshot.mempool), old_pending);
            }
            auto retry = store_->Prepare(journal_, wallet_, mempool, error_);
            ASSERT_NE(retry, nullptr);
            ASSERT_EQ(error_, GenerationError::kNone);
            ASSERT_EQ(store_->Commit(*retry).state, GenerationCommitState::kCommitted);
            retry.reset();
            store_.reset();
            store_ =
                GenerationStore::Open(directory_, nova::consensus::RegtestNetworkParams(), error_);
            ASSERT_NE(store_, nullptr);
            const auto recovered = store_->Observe();
            ASSERT_EQ(recovered.state, GenerationState::kPresent);
            EXPECT_GT(recovered.snapshot.sequence, old.snapshot.sequence);
            EXPECT_EQ(Read(recovered.snapshot.journal), Read(journal_));
            EXPECT_EQ(Read(recovered.snapshot.wallet), Read(wallet_));
            ASSERT_TRUE(recovered.snapshot.mempool);
            EXPECT_EQ(Read(*recovered.snapshot.mempool), Read(mempool));
        }
    }
}

TEST_F(GenerationStoreTest, ShortWritesCompleteExactCandidateImagesAcrossChunkBoundaries)
{
    using Target = nova::storage::GenerationIoTarget;
    using Fault = nova::storage::GenerationIoFault;
    const auto mempool = root_ / "input.mempool";
    std::string bytes(65537U, 'x');
    for (std::size_t i = 0U; i < bytes.size(); ++i)
        bytes[i] = static_cast<char>('a' + i % 26U);
    Write(journal_, bytes);
    Write(wallet_, bytes + "wallet");
    Write(mempool, bytes + "pending");
    for (const auto target : {Target::kJournal, Target::kWallet, Target::kMempool,
                              Target::kDescriptor, Target::kRoot}) {
        SCOPED_TRACE(static_cast<unsigned>(target));
        store_->SetIoFaultForTesting(target, Fault::kShortWrite);
        auto prepared = store_->Prepare(journal_, wallet_, mempool, error_);
        ASSERT_NE(prepared, nullptr);
        ASSERT_TRUE(store_->IoFaultTriggeredForTesting());
        ASSERT_EQ(store_->Commit(*prepared).state, GenerationCommitState::kCommitted);
        const auto current = store_->Observe();
        ASSERT_EQ(current.state, GenerationState::kPresent);
        EXPECT_EQ(Read(current.snapshot.journal), bytes);
        EXPECT_EQ(Read(current.snapshot.wallet), bytes + "wallet");
        ASSERT_TRUE(current.snapshot.mempool);
        EXPECT_EQ(Read(*current.snapshot.mempool), bytes + "pending");
    }
}

TEST_F(GenerationStoreTest, WriteAndSyncErrorsPreserveOldAuthorityAcrossRestartsAndRetry)
{
    using Target = nova::storage::GenerationIoTarget;
    using Fault = nova::storage::GenerationIoFault;
    const auto mempool = root_ / "input.mempool";
    Write(mempool, "old pending image");
    auto initial = store_->Prepare(journal_, wallet_, mempool, error_);
    ASSERT_NE(initial, nullptr);
    ASSERT_EQ(store_->Commit(*initial).state, GenerationCommitState::kCommitted);
    initial.reset();
    for (const auto& [target, extension] :
         {std::pair{Target::kJournal, ".journal"}, std::pair{Target::kWallet, ".wallet"},
          std::pair{Target::kMempool, ".mempool"}, std::pair{Target::kDescriptor, ".descriptor"},
          std::pair{Target::kRoot, ".root"}}) {
        for (const auto fault : {Fault::kZeroProgress, Fault::kNoSpace, Fault::kPrefixThenNoSpace,
                                 Fault::kSyncFailure}) {
            SCOPED_TRACE(static_cast<unsigned>(target));
            SCOPED_TRACE(static_cast<unsigned>(fault));
            const auto old = store_->Observe();
            ASSERT_EQ(old.state, GenerationState::kPresent);
            ASSERT_TRUE(old.snapshot.mempool);
            const auto root_bytes = Read(directory_ / "generation.current");
            const auto old_journal = Read(old.snapshot.journal);
            const auto old_wallet = Read(old.snapshot.wallet);
            const auto old_pending = Read(*old.snapshot.mempool);
            Write(journal_, std::string(65537U, 'j'));
            Write(wallet_, "new wallet image");
            Write(mempool, "new pending image");
            store_->SetIoFaultForTesting(target, fault);
            EXPECT_EQ(store_->Prepare(journal_, wallet_, mempool, error_), nullptr);
            EXPECT_EQ(error_, GenerationError::kIoFailure);
            ASSERT_TRUE(store_->IoFaultTriggeredForTesting());
            std::array<char, 64U> filename{};
            ASSERT_GT(std::snprintf(filename.data(), filename.size(), "generation-%016llx%s",
                                    static_cast<unsigned long long>(old.snapshot.sequence + 1U),
                                    extension),
                      0);
            const auto candidate = directory_ / filename.data();
            ASSERT_TRUE(std::filesystem::is_regular_file(candidate));
            const auto failed_bytes = Read(candidate);
            if (fault == Fault::kPrefixThenNoSpace)
                EXPECT_EQ(failed_bytes.size(), 1U);
            else if (fault == Fault::kSyncFailure)
                EXPECT_GT(failed_bytes.size(), 1U);
            else
                EXPECT_TRUE(failed_bytes.empty());
            for (unsigned restart = 0U; restart < 2U; ++restart) {
                store_.reset();
                store_ = GenerationStore::Open(directory_, nova::consensus::RegtestNetworkParams(),
                                               error_);
                ASSERT_NE(store_, nullptr);
                const auto current = store_->Observe();
                ASSERT_EQ(current.state, GenerationState::kPresent);
                EXPECT_EQ(current.snapshot.sequence, old.snapshot.sequence);
                EXPECT_EQ(Read(directory_ / "generation.current"), root_bytes);
                EXPECT_EQ(Read(current.snapshot.journal), old_journal);
                EXPECT_EQ(Read(current.snapshot.wallet), old_wallet);
                ASSERT_TRUE(current.snapshot.mempool);
                EXPECT_EQ(Read(*current.snapshot.mempool), old_pending);
                EXPECT_EQ(Read(candidate), failed_bytes);
            }
            auto retry = store_->Prepare(journal_, wallet_, mempool, error_);
            ASSERT_NE(retry, nullptr);
            ASSERT_EQ(store_->Commit(*retry).state, GenerationCommitState::kCommitted);
            const auto current = store_->Observe();
            ASSERT_EQ(current.state, GenerationState::kPresent);
            EXPECT_GT(current.snapshot.sequence, old.snapshot.sequence);
            EXPECT_EQ(Read(current.snapshot.journal), Read(journal_));
            EXPECT_EQ(Read(current.snapshot.wallet), Read(wallet_));
            ASSERT_TRUE(current.snapshot.mempool);
            EXPECT_EQ(Read(*current.snapshot.mempool), Read(mempool));
        }
    }
}

TEST_F(GenerationStoreTest, EmptyImagesDoNotInventWriteCallsButStillRequireSuccessfulSync)
{
    using Target = nova::storage::GenerationIoTarget;
    using Fault = nova::storage::GenerationIoFault;
    const auto mempool = root_ / "input.mempool";
    Write(journal_, "");
    Write(wallet_, "");
    Write(mempool, "");
    for (const auto target : {Target::kJournal, Target::kWallet, Target::kMempool}) {
        SCOPED_TRACE(static_cast<unsigned>(target));
        store_->SetIoFaultForTesting(target, Fault::kNoSpace);
        auto prepared = store_->Prepare(journal_, wallet_, mempool, error_);
        ASSERT_NE(prepared, nullptr);
        EXPECT_FALSE(store_->IoFaultTriggeredForTesting());
        ASSERT_EQ(store_->Commit(*prepared).state, GenerationCommitState::kCommitted);
        const auto root_bytes = Read(directory_ / "generation.current");
        store_->SetIoFaultForTesting(target, Fault::kSyncFailure);
        EXPECT_EQ(store_->Prepare(journal_, wallet_, mempool, error_), nullptr);
        EXPECT_EQ(error_, GenerationError::kIoFailure);
        EXPECT_TRUE(store_->IoFaultTriggeredForTesting());
        EXPECT_EQ(Read(directory_ / "generation.current"), root_bytes);
        EXPECT_EQ(store_->Observe().state, GenerationState::kPresent);
    }
}

TEST_F(GenerationStoreTest, CandidateCopyBindsExactBytesAcrossTransferBufferBoundaries)
{
    const auto mempool = root_ / "input.mempool";
    for (const std::size_t size : {0U, 65535U, 65536U, 65537U, 131073U}) {
        std::vector<std::uint8_t> bytes(size);
        for (std::size_t index = 0U; index < size; ++index)
            bytes[index] = static_cast<std::uint8_t>(index % 127U);
        const std::string image{bytes.begin(), bytes.end()};
        const auto digest = nova::crypto::Hash256::Sha256(bytes);
        ASSERT_TRUE(digest);
        Write(journal_, image);
        Write(wallet_, image);
        Write(mempool, image);
        auto prepared = store_->Prepare(journal_, wallet_, mempool, error_);
        ASSERT_NE(prepared, nullptr);
        ASSERT_EQ(store_->Commit(*prepared).state, GenerationCommitState::kCommitted);
        const auto committed = store_->Observe();
        ASSERT_EQ(committed.state, GenerationState::kPresent);
        ASSERT_TRUE(committed.snapshot.mempool);
        for (const auto& path :
             {committed.snapshot.journal, committed.snapshot.wallet, *committed.snapshot.mempool})
            EXPECT_EQ(Read(path), image);
        EXPECT_EQ(committed.snapshot.journal_size, size);
        EXPECT_EQ(committed.snapshot.wallet_size, size);
        EXPECT_EQ(committed.snapshot.mempool_size, size);
        EXPECT_EQ(committed.snapshot.journal_hash, *digest);
        EXPECT_EQ(committed.snapshot.wallet_hash, *digest);
        EXPECT_EQ(committed.snapshot.mempool_hash, *digest);
        if (size != 0U) {
            auto corrupted = image;
            corrupted.back() ^= 1;
            Write(committed.snapshot.journal, corrupted);
            EXPECT_EQ(store_->Observe().state, GenerationState::kUnavailable);
            EXPECT_EQ(Read(committed.snapshot.journal), corrupted);
            Write(committed.snapshot.journal, image);
            EXPECT_EQ(store_->Observe().state, GenerationState::kPresent);
        }
    }
}

TEST_F(GenerationStoreTest, CompletePairBecomesAuthoritativeOnlyAtRootReplacement)
{
    EXPECT_EQ(store_->Observe().state, GenerationState::kEmpty);
    auto prepared = Prepare();
    ASSERT_NE(prepared, nullptr);
    const auto staged = store_->InspectPrepared(*prepared);
    ASSERT_EQ(staged.state, GenerationState::kPresent);
    EXPECT_EQ(Read(staged.snapshot.journal), Read(journal_));
    EXPECT_EQ(Read(staged.snapshot.wallet), Read(wallet_));
    EXPECT_EQ(store_->Observe().state, GenerationState::kEmpty);
    EXPECT_EQ(store_->Resolve(*prepared).state, GenerationCommitState::kNotCommitted);
    EXPECT_EQ(store_->Commit(*prepared).state, GenerationCommitState::kCommitted);
    const auto snapshot = store_->Observe();
    ASSERT_EQ(snapshot.state, GenerationState::kPresent);
    EXPECT_EQ(snapshot.snapshot.sequence, 1U);
    EXPECT_EQ(Read(snapshot.snapshot.journal), Read(journal_));
    EXPECT_EQ(Read(snapshot.snapshot.wallet), Read(wallet_));
    EXPECT_EQ(store_->Resolve(*prepared).state, GenerationCommitState::kCommitted);
    EXPECT_EQ(store_->Commit(*prepared).state, GenerationCommitState::kUnavailable);
    store_.reset();
    store_ = GenerationStore::Open(directory_, nova::consensus::RegtestNetworkParams(), error_);
    ASSERT_NE(store_, nullptr);
    EXPECT_EQ(store_->Observe().snapshot.sequence, 1U);
}

TEST_F(GenerationStoreTest, SecondGenerationRetainsOldImagesAndRejectsStalePreparation)
{
    auto first = Prepare();
    auto stale = Prepare();
    ASSERT_NE(first, nullptr);
    ASSERT_NE(stale, nullptr);
    ASSERT_EQ(store_->Commit(*first).state, GenerationCommitState::kCommitted);
    const auto old = store_->Observe().snapshot;
    EXPECT_EQ(store_->Commit(*stale).state, GenerationCommitState::kUnavailable);
    EXPECT_EQ(store_->Observe().snapshot.sequence, old.sequence);
    Write(journal_, "new journal");
    Write(wallet_, "new opaque wallet image");
    auto next = Prepare();
    ASSERT_NE(next, nullptr);
    ASSERT_EQ(store_->Commit(*next).state, GenerationCommitState::kCommitted);
    const auto current = store_->Observe();
    EXPECT_GT(current.snapshot.sequence, old.sequence);
    EXPECT_EQ(Read(current.snapshot.journal), "new journal");
    EXPECT_EQ(Read(old.journal), "controlled non-secret journal image");
    EXPECT_EQ(Read(old.wallet), "opaque test image; no wallet or keys");
}

TEST_F(GenerationStoreTest, ThreeImageGenerationIsBoundAndCannotBeDowngraded)
{
    auto legacy = Prepare();
    ASSERT_NE(legacy, nullptr);
    ASSERT_EQ(store_->Commit(*legacy).state, GenerationCommitState::kCommitted);
    EXPECT_FALSE(store_->Observe().snapshot.mempool.has_value());
    const auto input = root_ / "input.mempool";
    Write(input, "controlled mempool image");
    auto next = store_->Prepare(journal_, wallet_, input, error_);
    ASSERT_NE(next, nullptr);
    const auto staged = store_->InspectPrepared(*next);
    ASSERT_EQ(staged.state, GenerationState::kPresent);
    ASSERT_TRUE(staged.snapshot.mempool.has_value());
    EXPECT_EQ(Read(*staged.snapshot.mempool), Read(input));
    EXPECT_FALSE(store_->Observe().snapshot.mempool.has_value());
    ASSERT_EQ(store_->Commit(*next).state, GenerationCommitState::kCommitted);
    const auto current = store_->Observe();
    ASSERT_EQ(current.state, GenerationState::kPresent);
    ASSERT_TRUE(current.snapshot.mempool.has_value());
    EXPECT_EQ(Read(*current.snapshot.mempool), Read(input));
    EXPECT_EQ(Prepare(), nullptr);
    EXPECT_EQ(error_, GenerationError::kMalformed);
    store_.reset();
    store_ = GenerationStore::Open(directory_, nova::consensus::RegtestNetworkParams(), error_);
    ASSERT_NE(store_, nullptr);
    ASSERT_TRUE(store_->Observe().snapshot.mempool.has_value());
    Write(*current.snapshot.mempool, "corrupt");
    EXPECT_EQ(store_->Observe().state, GenerationState::kUnavailable);
    store_.reset();
    EXPECT_EQ(GenerationStore::Open(directory_, nova::consensus::RegtestNetworkParams(), error_),
              nullptr);
}

TEST_F(GenerationStoreTest, MempoolStageFailureDoesNotPublishAndTamperingRejectsCommit)
{
    const auto input = root_ / "input.mempool";
    Write(input, "controlled mempool image");
    for (const auto point :
         {GenerationFailurePoint::kBeforeMempool, GenerationFailurePoint::kAfterMempool}) {
        store_->FailNextForTesting(point);
        EXPECT_EQ(store_->Prepare(journal_, wallet_, input, error_), nullptr);
        EXPECT_EQ(store_->Observe().state, GenerationState::kEmpty);
    }
    auto staged = store_->Prepare(journal_, wallet_, input, error_);
    ASSERT_NE(staged, nullptr);
    const auto snapshot = store_->InspectPrepared(*staged);
    ASSERT_TRUE(snapshot.snapshot.mempool.has_value());
    Write(*snapshot.snapshot.mempool, "tampered");
    EXPECT_EQ(store_->Commit(*staged).state, GenerationCommitState::kNotCommitted);
    EXPECT_EQ(store_->Observe().state, GenerationState::kEmpty);
}

TEST_F(GenerationStoreTest, ThreeImageDescriptorRejectsTruncationAndTrailingBytes)
{
    const auto input = root_ / "input.mempool";
    Write(input, "");
    auto staged = store_->Prepare(journal_, wallet_, input, error_);
    ASSERT_NE(staged, nullptr);
    ASSERT_EQ(store_->Commit(*staged).state, GenerationCommitState::kCommitted);
    const auto root = Read(directory_ / "generation.current");
    const auto descriptor = Read(directory_ / "generation-0000000000000001.descriptor");
    const auto bytes = [](const std::string& value) {
        return std::span{reinterpret_cast<const std::uint8_t*>(value.data()), value.size()};
    };
    const auto& network = nova::consensus::RegtestNetworkParams();
    const auto decoded =
        nova::storage::DecodeGenerationMetadata(bytes(root), bytes(descriptor), network);
    ASSERT_EQ(decoded.state, GenerationState::kPresent);
    ASSERT_TRUE(decoded.metadata.mempool_size.has_value());
    EXPECT_EQ(*decoded.metadata.mempool_size, 0U);
    for (std::size_t length = 0U; length < descriptor.size(); ++length)
        EXPECT_EQ(nova::storage::DecodeGenerationMetadata(bytes(root),
                                                          bytes(descriptor).first(length), network)
                      .state,
                  GenerationState::kUnavailable);
    const auto trailing = descriptor + "x";
    EXPECT_EQ(nova::storage::DecodeGenerationMetadata(bytes(root), bytes(trailing), network).state,
              GenerationState::kUnavailable);
}

TEST_F(GenerationStoreTest, PrepublicationFailuresLeavePriorPairAuthoritative)
{
    auto first = Prepare();
    ASSERT_NE(first, nullptr);
    ASSERT_EQ(store_->Commit(*first).state, GenerationCommitState::kCommitted);
    const auto original = store_->Observe().snapshot;
    for (const auto point :
         {GenerationFailurePoint::kBeforeJournal, GenerationFailurePoint::kBeforeWallet,
          GenerationFailurePoint::kBeforeDescriptor, GenerationFailurePoint::kAfterJournal,
          GenerationFailurePoint::kAfterWallet, GenerationFailurePoint::kAfterDescriptor}) {
        store_->FailNextForTesting(point);
        EXPECT_EQ(Prepare(), nullptr);
        EXPECT_EQ(store_->Observe().snapshot.sequence, original.sequence);
        EXPECT_EQ(Read(original.journal), Read(journal_));
    }
    auto before = Prepare();
    ASSERT_NE(before, nullptr);
    store_->FailNextForTesting(GenerationFailurePoint::kBeforeRootReplace);
    EXPECT_EQ(store_->Commit(*before).state, GenerationCommitState::kNotCommitted);
    EXPECT_EQ(store_->Observe().snapshot.sequence, original.sequence);
}

#ifdef _WIN32
TEST_F(GenerationStoreTest, NativeRootLeaseDenialRetainsOldRootAndImagesAcrossRestarts)
{
    const auto pending = root_ / "input.pending";
    Write(pending, "old pending bytes");
    auto first = store_->Prepare(journal_, wallet_, pending, error_);
    ASSERT_NE(first, nullptr);
    ASSERT_EQ(store_->Commit(*first).state, GenerationCommitState::kCommitted);
    const auto old = store_->Observe();
    ASSERT_EQ(old.state, GenerationState::kPresent);
    ASSERT_TRUE(old.snapshot.mempool);
    const auto selector = directory_ / "generation.current";
    const auto root_bytes = Read(selector);
    const auto journal_bytes = Read(old.snapshot.journal);
    const auto wallet_bytes = Read(old.snapshot.wallet);
    const auto pending_bytes = Read(*old.snapshot.mempool);
    Write(journal_, "new journal bytes");
    Write(wallet_, "new opaque wallet bytes");
    Write(pending, "new pending bytes");
    auto candidate = store_->Prepare(journal_, wallet_, pending, error_);
    ASSERT_NE(candidate, nullptr);
    const auto staged = store_->InspectPrepared(*candidate);
    ASSERT_EQ(staged.state, GenerationState::kPresent);
    ASSERT_EQ(staged.snapshot.sequence, 2U);
    const auto temporary_root = directory_ / "generation-0000000000000002.root";
    {
        // Real Windows sharing semantics prevent deletion/replacement while
        // allowing the store's read-only observation. No failure seam is armed.
        struct ReadLease final {
            HANDLE handle;
            ~ReadLease()
            {
                if (handle != INVALID_HANDLE_VALUE)
                    (void)CloseHandle(handle);
            }
        } lease{CreateFileW(selector.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr)};
        ASSERT_NE(lease.handle, INVALID_HANDLE_VALUE);
        ReadLease delete_probe{CreateFileW(selector.c_str(), DELETE, FILE_SHARE_READ, nullptr,
                                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        const auto delete_error = GetLastError();
        ASSERT_EQ(delete_probe.handle, INVALID_HANDLE_VALUE);
        ASSERT_EQ(delete_error, ERROR_SHARING_VIOLATION);
        SetLastError(ERROR_SUCCESS);
        const auto replaced = MoveFileExW(temporary_root.c_str(), selector.c_str(),
                                          MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        const auto native_error = GetLastError();
        ASSERT_EQ(replaced, 0);
        // MoveFileEx reports destination replacement denial as ACCESS_DENIED
        // on this Windows API path; the direct DELETE probe above establishes
        // the actual sharing obstruction independently.
        ASSERT_EQ(native_error, ERROR_ACCESS_DENIED);
        EXPECT_EQ(store_->Commit(*candidate).state, GenerationCommitState::kNotCommitted);
        EXPECT_EQ(store_->Resolve(*candidate).state, GenerationCommitState::kNotCommitted);
        EXPECT_EQ(Read(selector), root_bytes);
        EXPECT_EQ(store_->Observe().snapshot.sequence, old.snapshot.sequence);
    }
    // A failed attempt cannot be blindly replayed, even after the obstruction
    // disappears. Recovery uses the unchanged authoritative root.
    EXPECT_EQ(store_->Commit(*candidate).state, GenerationCommitState::kUnavailable);
    EXPECT_EQ(store_->Resolve(*candidate).state, GenerationCommitState::kNotCommitted);
    for (unsigned restart = 0U; restart < 2U; ++restart) {
        store_.reset();
        store_ = GenerationStore::Open(directory_, nova::consensus::RegtestNetworkParams(), error_);
        ASSERT_NE(store_, nullptr);
        const auto restored = store_->Observe();
        ASSERT_EQ(restored.state, GenerationState::kPresent);
        EXPECT_EQ(restored.snapshot.sequence, old.snapshot.sequence);
        EXPECT_EQ(Read(selector), root_bytes);
        EXPECT_EQ(Read(restored.snapshot.journal), journal_bytes);
        EXPECT_EQ(Read(restored.snapshot.wallet), wallet_bytes);
        ASSERT_TRUE(restored.snapshot.mempool);
        EXPECT_EQ(Read(*restored.snapshot.mempool), pending_bytes);
        EXPECT_TRUE(std::filesystem::is_regular_file(temporary_root));
    }
    auto retry = store_->Prepare(journal_, wallet_, pending, error_);
    ASSERT_NE(retry, nullptr);
    ASSERT_EQ(store_->Commit(*retry).state, GenerationCommitState::kCommitted);
    const auto committed = store_->Observe();
    ASSERT_EQ(committed.state, GenerationState::kPresent);
    EXPECT_GT(committed.snapshot.sequence, staged.snapshot.sequence);
    EXPECT_EQ(Read(committed.snapshot.journal), Read(journal_));
    EXPECT_EQ(Read(committed.snapshot.wallet), Read(wallet_));
    ASSERT_TRUE(committed.snapshot.mempool);
    EXPECT_EQ(Read(*committed.snapshot.mempool), Read(pending));
    EXPECT_EQ(Read(old.snapshot.journal), journal_bytes);
    EXPECT_EQ(Read(old.snapshot.wallet), wallet_bytes);
    EXPECT_EQ(Read(*old.snapshot.mempool), pending_bytes);
}
#endif

TEST_F(GenerationStoreTest, PostReplacementErrorsAreUnavailableUntilResolved)
{
    for (const auto point :
         {GenerationFailurePoint::kAfterRootReplace, GenerationFailurePoint::kDirectoryBarrier}) {
        auto prepared = Prepare();
        ASSERT_NE(prepared, nullptr);
        store_->FailNextForTesting(point);
        EXPECT_EQ(store_->Commit(*prepared).state, GenerationCommitState::kUnavailable);
        EXPECT_EQ(store_->Observe().state, GenerationState::kPresent);
        store_->FailNextForTesting(GenerationFailurePoint::kObservation);
        EXPECT_EQ(store_->Resolve(*prepared).state, GenerationCommitState::kUnavailable);
        EXPECT_EQ(store_->Resolve(*prepared).state, GenerationCommitState::kCommitted);
    }
}

TEST_F(GenerationStoreTest, ChangedStagedImageCannotBecomeAuthoritative)
{
    auto prepared = Prepare();
    ASSERT_NE(prepared, nullptr);
    const auto staged = directory_ / "generation-0000000000000001.wallet";
    Write(staged, "changed staged image");
    EXPECT_EQ(store_->Commit(*prepared).state, GenerationCommitState::kNotCommitted);
    EXPECT_EQ(store_->Observe().state, GenerationState::kEmpty);
}

TEST_F(GenerationStoreTest, CorruptCommittedImageNeverFallsBackToOlderGeneration)
{
    auto first = Prepare();
    ASSERT_NE(first, nullptr);
    ASSERT_EQ(store_->Commit(*first).state, GenerationCommitState::kCommitted);
    auto next = Prepare();
    ASSERT_NE(next, nullptr);
    ASSERT_EQ(store_->Commit(*next).state, GenerationCommitState::kCommitted);
    const auto current = store_->Observe().snapshot;
    Write(current.wallet, "corrupt");
    EXPECT_EQ(store_->Observe().state, GenerationState::kUnavailable);
    EXPECT_EQ(store_->Resolve(*next).state, GenerationCommitState::kUnavailable);
    store_.reset();
    EXPECT_EQ(GenerationStore::Open(directory_, nova::consensus::RegtestNetworkParams(), error_),
              nullptr);
    EXPECT_EQ(error_, GenerationError::kHashFailure);
}

TEST_F(GenerationStoreTest, EveryCommittedImageTruncationRefusesRestartWithoutRepair)
{
    const auto pending = root_ / "input.mempool";
    Write(pending, "controlled pending transaction image");
    auto prepared = store_->Prepare(journal_, wallet_, pending, error_);
    ASSERT_NE(prepared, nullptr);
    ASSERT_EQ(store_->Commit(*prepared).state, GenerationCommitState::kCommitted);
    const auto snapshot = store_->Observe().snapshot;
    ASSERT_TRUE(snapshot.mempool.has_value());
    const std::array paths{directory_ / "generation.current",
                           directory_ / "generation-0000000000000001.descriptor", snapshot.journal,
                           snapshot.wallet, *snapshot.mempool};
    std::array<std::string, 5U> originals;
    for (std::size_t i = 0U; i < paths.size(); ++i)
        originals[i] = Read(paths[i]);
    const auto other_images_unchanged = [&](const std::filesystem::path& damaged) {
        for (std::size_t i = 0U; i < paths.size(); ++i)
            if (paths[i] != damaged)
                EXPECT_EQ(Read(paths[i]), originals[i]) << paths[i].string();
    };
    store_.reset();
    for (const auto& path : paths) {
        const auto original = Read(path);
        ASSERT_FALSE(original.empty());
        for (std::size_t length = 0U; length < original.size(); ++length) {
            SCOPED_TRACE(path.filename().string() + ":" + std::to_string(length));
            const auto truncated = original.substr(0U, length);
            Write(path, truncated);
            auto reopened =
                GenerationStore::Open(directory_, nova::consensus::RegtestNetworkParams(), error_);
            EXPECT_EQ(reopened, nullptr);
            reopened.reset();
            EXPECT_EQ(Read(path), truncated);
            other_images_unchanged(path);
        }
        for (const auto& extra : {std::string(1U, '\0'), std::string{"trailing"}}) {
            const auto extended = original + extra;
            Write(path, extended);
            auto reopened =
                GenerationStore::Open(directory_, nova::consensus::RegtestNetworkParams(), error_);
            EXPECT_EQ(reopened, nullptr);
            reopened.reset();
            EXPECT_EQ(Read(path), extended);
            other_images_unchanged(path);
        }
        for (std::size_t offset = 0U; offset < original.size(); ++offset) {
            SCOPED_TRACE(path.filename().string() + ":bitflip:" + std::to_string(offset));
            auto corrupted = original;
            corrupted[offset] =
                static_cast<char>(static_cast<unsigned char>(corrupted[offset]) ^ 1U);
            Write(path, corrupted);
            auto reopened =
                GenerationStore::Open(directory_, nova::consensus::RegtestNetworkParams(), error_);
            EXPECT_EQ(reopened, nullptr);
            reopened.reset();
            EXPECT_EQ(Read(path), corrupted);
            other_images_unchanged(path);
        }
        Write(path, original);
        auto reopened =
            GenerationStore::Open(directory_, nova::consensus::RegtestNetworkParams(), error_);
        ASSERT_NE(reopened, nullptr);
        EXPECT_EQ(reopened->Observe().state, GenerationState::kPresent);
    }
}

TEST_F(GenerationStoreTest, MissingRootIsNotAnEmptyDatabaseAndMalformedRootsReject)
{
    const auto root = directory_ / "generation.current";
    const auto original = Read(root);
    ASSERT_EQ(original.size(), 85U);
    Write(root, original + "trailing");
    EXPECT_EQ(store_->Observe().state, GenerationState::kUnavailable);
    Write(root, original.substr(0U, original.size() - 1U));
    EXPECT_EQ(store_->Observe().state, GenerationState::kUnavailable);
    Write(root, original);
    auto prepared = Prepare();
    ASSERT_NE(prepared, nullptr);
    ASSERT_EQ(store_->Commit(*prepared).state, GenerationCommitState::kCommitted);
    store_.reset();
    ASSERT_TRUE(std::filesystem::remove(root));
    EXPECT_EQ(GenerationStore::Open(directory_, nova::consensus::RegtestNetworkParams(), error_),
              nullptr);
    EXPECT_EQ(error_, GenerationError::kMalformed);
}

TEST_F(GenerationStoreTest, ExclusiveLeaseWrongOwnerAndWrongNetworkReject)
{
    EXPECT_EQ(GenerationStore::Open(directory_, nova::consensus::RegtestNetworkParams(), error_),
              nullptr);
    EXPECT_EQ(error_, GenerationError::kLocked);
    const auto other_directory = root_ / "other";
    ASSERT_TRUE(std::filesystem::create_directory(other_directory));
    auto other =
        GenerationStore::Open(other_directory, nova::consensus::RegtestNetworkParams(), error_);
    ASSERT_NE(other, nullptr);
    auto prepared = Prepare();
    ASSERT_NE(prepared, nullptr);
    EXPECT_EQ(other->Commit(*prepared).state, GenerationCommitState::kUnavailable);
    EXPECT_EQ(other->Observe().state, GenerationState::kEmpty);
    store_.reset();
    EXPECT_EQ(GenerationStore::Open(directory_, nova::consensus::TestnetNetworkParams(), error_),
              nullptr);
    EXPECT_EQ(error_, GenerationError::kIdentityMismatch);
    EXPECT_EQ(GenerationStore::Open(directory_, nova::consensus::MainnetNetworkParams(), error_),
              nullptr);
    EXPECT_FALSE(nova::consensus::MainnetNetworkParams().enabled);
}

TEST_F(GenerationStoreTest, EmptyAndChunkBoundaryImagesHaveExactLengthAndHash)
{
    for (const std::size_t count : {0U, 65535U, 65536U, 65537U}) {
        const std::string payload(count, 'x');
        Write(journal_, payload);
        Write(wallet_, payload);
        auto prepared = Prepare();
        ASSERT_NE(prepared, nullptr);
        ASSERT_EQ(store_->Commit(*prepared).state, GenerationCommitState::kCommitted);
        const auto result = store_->Observe();
        ASSERT_EQ(result.state, GenerationState::kPresent);
        EXPECT_EQ(result.snapshot.journal_size, count);
        EXPECT_EQ(result.snapshot.wallet_size, count);
        EXPECT_EQ(Read(result.snapshot.journal), payload);
        const auto hash = nova::crypto::Hash256::Sha256(
            std::span{reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size()});
        ASSERT_TRUE(hash.has_value());
        EXPECT_EQ(result.snapshot.journal_hash, *hash);
    }
}

TEST_F(GenerationStoreTest, SourceAndCommittedHardLinksAreRejected)
{
    const auto alias = root_ / "alias";
    std::error_code error;
    std::filesystem::create_hard_link(journal_, alias, error);
    ASSERT_FALSE(error);
    EXPECT_EQ(Prepare(), nullptr);
    EXPECT_EQ(store_->Observe().state, GenerationState::kEmpty);
    ASSERT_TRUE(std::filesystem::remove(alias));
    auto prepared = Prepare();
    ASSERT_NE(prepared, nullptr);
    ASSERT_EQ(store_->Commit(*prepared).state, GenerationCommitState::kCommitted);
    const auto current = store_->Observe().snapshot;
    std::filesystem::create_hard_link(current.wallet, alias, error);
    ASSERT_FALSE(error);
    EXPECT_EQ(store_->Observe().state, GenerationState::kUnavailable);
}

TEST_F(GenerationStoreTest, EmptyRootHasGoldenFixedWidthIdentityAndNoTrailingBytes)
{
    const auto bytes = Read(directory_ / "generation.current");
    std::string expected = "NVROOT01";
    expected.append("\x00\xfa\xbf\xb5\xda", 5U);
    const auto& hash = nova::consensus::RegtestNetworkParams().genesis_hash.bytes();
    expected.append(reinterpret_cast<const char*>(hash.data()), hash.size());
    expected.append(8U + 32U, '\0');
    EXPECT_EQ(bytes, expected);
    EXPECT_EQ(bytes.size(), 85U);
}

TEST_F(GenerationStoreTest, PureDecoderRejectsEveryTruncationAndBindsDescriptor)
{
    auto root = Read(directory_ / "generation.current");
    auto as_bytes = [](const std::string& bytes) {
        return std::span{reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()};
    };
    const auto& network = nova::consensus::RegtestNetworkParams();
    EXPECT_EQ(nova::storage::DecodeGenerationMetadata(as_bytes(root), {}, network).state,
              GenerationState::kEmpty);
    for (std::size_t length = 0U; length < root.size(); ++length)
        EXPECT_EQ(nova::storage::DecodeGenerationMetadata(as_bytes(root).first(length), {}, network)
                      .state,
                  GenerationState::kUnavailable);
    auto prepared = Prepare();
    ASSERT_NE(prepared, nullptr);
    ASSERT_EQ(store_->Commit(*prepared).state, GenerationCommitState::kCommitted);
    root = Read(directory_ / "generation.current");
    auto descriptor = Read(directory_ / "generation-0000000000000001.descriptor");
    const auto result =
        nova::storage::DecodeGenerationMetadata(as_bytes(root), as_bytes(descriptor), network);
    ASSERT_EQ(result.state, GenerationState::kPresent);
    EXPECT_EQ(result.metadata.sequence, 1U);
    EXPECT_EQ(result.metadata.journal_size, Read(journal_).size());
    EXPECT_EQ(result.metadata.wallet_hash, store_->Observe().snapshot.wallet_hash);
    for (std::size_t length = 0U; length < descriptor.size(); ++length)
        EXPECT_EQ(nova::storage::DecodeGenerationMetadata(
                      as_bytes(root), as_bytes(descriptor).first(length), network)
                      .state,
                  GenerationState::kUnavailable);
    descriptor[0] = 'X';
    EXPECT_EQ(nova::storage::DecodeGenerationMetadata(as_bytes(root), as_bytes(descriptor), network)
                  .error,
              GenerationError::kHashFailure);
    EXPECT_EQ(nova::storage::DecodeGenerationMetadata(as_bytes(root), as_bytes(descriptor),
                                                      nova::consensus::TestnetNetworkParams())
                  .error,
              GenerationError::kIdentityMismatch);
}

TEST_F(GenerationStoreTest, CommittedFuzzSeedsHaveTheirDocumentedDecoderOutcomes)
{
    const auto corpus = std::filesystem::path{__FILE__}.parent_path().parent_path() /
                        "fuzz/corpus/generation_metadata";
    const std::array cases{std::tuple{"empty-testnet.bin", GenerationState::kEmpty,
                                      GenerationError::kNone, std::uint64_t{0U}},
                           std::tuple{"present-testnet.bin", GenerationState::kPresent,
                                      GenerationError::kNone, std::uint64_t{1U}},
                           std::tuple{"present-v2-testnet.bin", GenerationState::kPresent,
                                      GenerationError::kNone, std::uint64_t{1U}},
                           std::tuple{"maximum-sequence.bin", GenerationState::kPresent,
                                      GenerationError::kNone,
                                      std::numeric_limits<std::uint64_t>::max()},
                           std::tuple{"root-trailing.bin", GenerationState::kUnavailable,
                                      GenerationError::kMalformed, std::uint64_t{0U}},
                           std::tuple{"descriptor-bad-hash.bin", GenerationState::kUnavailable,
                                      GenerationError::kHashFailure, std::uint64_t{0U}}};
    for (const auto& [name, state, error, sequence] : cases) {
        std::ifstream input{corpus / name, std::ios::binary};
        std::array<std::uint8_t, 512U> bytes{};
        input.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        ASSERT_GE(input.gcount(), 2);
        const auto count = static_cast<std::size_t>(input.gcount());
        ASSERT_EQ(input.peek(), EOF);
        ASSERT_EQ(bytes[0], 1U);
        const auto rest = std::span{bytes}.first(count).subspan(2U);
        ASSERT_LE(bytes[1], rest.size());
        const auto decoded = nova::storage::DecodeGenerationMetadata(
            rest.first(bytes[1]), rest.subspan(bytes[1]), nova::consensus::TestnetNetworkParams());
        EXPECT_EQ(decoded.state, state) << name;
        EXPECT_EQ(decoded.error, error) << name;
        EXPECT_EQ(decoded.metadata.sequence, sequence) << name;
        if (std::string_view{name} == "present-v2-testnet.bin") {
            ASSERT_TRUE(decoded.metadata.mempool_size);
            EXPECT_EQ(*decoded.metadata.mempool_size, 0U);
            const auto empty_digest = nova::crypto::Hash256::Sha256({});
            ASSERT_TRUE(empty_digest);
            EXPECT_EQ(decoded.metadata.mempool_hash, *empty_digest);
            EXPECT_EQ(rest.subspan(bytes[1]).size(), 173U);
        } else {
            EXPECT_FALSE(decoded.metadata.mempool_size) << name;
        }
    }
}

TEST_F(GenerationStoreTest, DescriptorVersionAndCanonicalLengthAreCheckedBeyondChecksum)
{
    auto prepared = Prepare();
    ASSERT_NE(prepared, nullptr);
    ASSERT_EQ(store_->Commit(*prepared).state, GenerationCommitState::kCommitted);
    const auto descriptor_path = directory_ / "generation-0000000000000001.descriptor";
    const auto root_path = directory_ / "generation.current";
    auto descriptor = Read(descriptor_path);
    ASSERT_EQ(descriptor.size(), 133U);
    auto root = Read(root_path);
    ASSERT_EQ(root.size(), 85U);
    descriptor[7] = '2';
    Write(descriptor_path, descriptor);
    const auto digest = nova::crypto::Hash256::Sha256(
        std::span{reinterpret_cast<const std::uint8_t*>(descriptor.data()), descriptor.size()});
    ASSERT_TRUE(digest.has_value());
    root.replace(53U, 32U, reinterpret_cast<const char*>(digest->bytes().data()), 32U);
    Write(root_path, root);
    EXPECT_EQ(store_->Observe().error, GenerationError::kMalformed);
    Write(descriptor_path, descriptor + "trailing");
    EXPECT_EQ(store_->Observe().error, GenerationError::kMalformed);
}

TEST_F(GenerationStoreTest, MaximumSequenceFailsClosedWithoutWrapping)
{
    auto prepared = Prepare();
    ASSERT_NE(prepared, nullptr);
    ASSERT_EQ(store_->Commit(*prepared).state, GenerationCommitState::kCommitted);
    auto root = Read(directory_ / "generation.current");
    auto descriptor = Read(directory_ / "generation-0000000000000001.descriptor");
    ASSERT_EQ(root.size(), 85U);
    ASSERT_EQ(descriptor.size(), 133U);
    root.replace(45U, 8U, 8U, static_cast<char>(0xFFU));
    descriptor.replace(45U, 8U, 8U, static_cast<char>(0xFFU));
    const auto hash = nova::crypto::Hash256::Sha256(
        std::span{reinterpret_cast<const std::uint8_t*>(descriptor.data()), descriptor.size()});
    ASSERT_TRUE(hash.has_value());
    root.replace(53U, 32U, reinterpret_cast<const char*>(hash->bytes().data()), 32U);
    Write(directory_ / "generation-ffffffffffffffff.descriptor", descriptor);
    Write(directory_ / "generation-ffffffffffffffff.journal", Read(journal_));
    Write(directory_ / "generation-ffffffffffffffff.wallet", Read(wallet_));
    Write(directory_ / "generation.current", root);
    ASSERT_EQ(store_->Observe().state, GenerationState::kPresent);
    EXPECT_EQ(store_->Observe().snapshot.sequence, std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(Prepare(), nullptr);
    EXPECT_EQ(error_, GenerationError::kExhausted);
    EXPECT_EQ(Read(directory_ / "generation.current"), root);
}

TEST_F(GenerationStoreTest, UnavailableAuthorityAndNonregularSourcesCannotBePublished)
{
    auto prepared = Prepare();
    ASSERT_NE(prepared, nullptr);
    store_->FailNextForTesting(GenerationFailurePoint::kObservation);
    EXPECT_EQ(store_->Commit(*prepared).state, GenerationCommitState::kUnavailable);
    EXPECT_EQ(store_->Observe().state, GenerationState::kEmpty);
    EXPECT_EQ(store_->Commit(*prepared).state, GenerationCommitState::kCommitted);
    EXPECT_EQ(store_->Prepare(root_, wallet_, error_), nullptr);
    EXPECT_EQ(store_->Observe().snapshot.sequence, 1U);
}

#ifndef _WIN32
TEST_F(GenerationStoreTest, SymlinksAreRejectedWithoutFollowingTheirTargets)
{
    const auto alias = root_ / "alias";
    std::filesystem::create_symlink(journal_, alias);
    EXPECT_EQ(store_->Prepare(alias, wallet_, error_), nullptr);
    EXPECT_EQ(store_->Observe().state, GenerationState::kEmpty);
    auto prepared = Prepare();
    ASSERT_NE(prepared, nullptr);
    ASSERT_EQ(store_->Commit(*prepared).state, GenerationCommitState::kCommitted);
    const auto current = store_->Observe().snapshot;
    ASSERT_TRUE(std::filesystem::remove(current.wallet));
    std::filesystem::create_symlink(wallet_, current.wallet);
    EXPECT_EQ(store_->Observe().state, GenerationState::kUnavailable);
    EXPECT_EQ(Read(wallet_), "opaque test image; no wallet or keys");
}
#endif
} // namespace
