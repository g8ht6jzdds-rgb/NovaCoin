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
#include <system_error>
#include <tuple>

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

TEST_F(GenerationStoreTest, CompletePairBecomesAuthoritativeOnlyAtRootReplacement)
{
    EXPECT_EQ(store_->Observe().state, GenerationState::kEmpty);
    auto prepared = Prepare();
    ASSERT_NE(prepared, nullptr);
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

TEST_F(GenerationStoreTest, PrepublicationFailuresLeavePriorPairAuthoritative)
{
    auto first = Prepare();
    ASSERT_NE(first, nullptr);
    ASSERT_EQ(store_->Commit(*first).state, GenerationCommitState::kCommitted);
    const auto original = store_->Observe().snapshot;
    for (const auto point :
         {GenerationFailurePoint::kAfterJournal, GenerationFailurePoint::kAfterWallet,
          GenerationFailurePoint::kAfterDescriptor}) {
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
