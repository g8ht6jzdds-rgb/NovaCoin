#include <gtest/gtest.h>

#include "chain/chainstate.hpp"
#include "chain/validation.hpp"
#include "consensus/monetary.hpp"
#include "consensus/pow.hpp"
#include "primitives/serialization.hpp"
#include "wallet/secure_memory.hpp"
#include "wallet/wallet.hpp"
#include <algorithm>
#include <array>
#include <type_traits>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace
{

using nova::chain::TransactionValidationContext;
using nova::chain::UTXOKey;
using nova::chain::UTXOSet;
using nova::chain::ValidateTransactionAgainstUTXOSet;
using nova::consensus::ChainParams;
using nova::consensus::COIN;
using nova::consensus::INITIAL_SUBSIDY;
using nova::consensus::MAX_MONEY;
using nova::primitives::Amount;
using nova::primitives::BlockLimits;
using nova::primitives::OutPoint;
using nova::primitives::Transaction;
using nova::primitives::TransactionLimits;
using nova::primitives::TxInput;
using nova::primitives::TxOutput;
using nova::wallet::BroadcastResult;
using nova::wallet::Recipient;
using nova::wallet::TransactionBroadcaster;
using nova::wallet::Wallet;
using nova::wallet::WalletError;
using nova::wallet::WalletParams;

constexpr TransactionLimits kTransactionLimits{MAX_MONEY, 100'000U, 8U, 8U, 128U};
constexpr WalletParams kWalletParams{kTransactionLimits, 2, 1U};

nova::chain::BlockValidationParams RegtestValidationParams()
{
    return nova::chain::BlockValidationParams{BlockLimits{kTransactionLimits, 100'000U, 8U, 64U},
                                              nova::consensus::RegtestPowParameters(),
                                              ChainParams{COIN, MAX_MONEY, INITIAL_SUBSIDY, 10U},
                                              0U,
                                              500'000'000U,
                                              0xFFFF'FFFFU,
                                              1U,
                                              600U};
}

nova::crypto::Hash256 HashWithLastByte(const std::uint8_t byte)
{
    nova::crypto::Hash256::Bytes bytes{};
    bytes.back() = byte;
    return nova::crypto::Hash256{bytes};
}

std::vector<std::uint8_t> P2pkhScript(const nova::crypto::Hash160& key_hash)
{
    std::vector<std::uint8_t> script{0x76U, 0xA9U, 0x14U};
    script.insert(script.end(), key_hash.begin(), key_hash.end());
    script.insert(script.end(), {0x88U, 0xACU});
    return script;
}

Transaction FundingTransaction(const std::uint8_t id, const Amount amount,
                               const nova::crypto::Hash160& recipient_hash)
{
    return Transaction{
        1, std::vector<TxInput>{TxInput{OutPoint{HashWithLastByte(id), 0U}, {}, 0xFFFF'FFFFU}},
        std::vector<TxOutput>{TxOutput{amount, P2pkhScript(recipient_hash)}}, 0U};
}

class RecordingBroadcaster final : public TransactionBroadcaster
{
  public:
    explicit RecordingBroadcaster(const bool accepted) : accepted_(accepted) {}

    BroadcastResult Broadcast(const Transaction& transaction) override
    {
        observed_ = transaction;
        return BroadcastResult{accepted_};
    }

    bool accepted_{};
    std::optional<Transaction> observed_;
};

std::filesystem::path UniqueWalletPath()
{
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           ("novacoin-wallet-test-" + std::to_string(nonce) + ".dat");
}

TEST(Wallet, CreatesWalletAndGeneratesOrderedReceivingAddresses)
{
    auto wallet = Wallet::Create(kWalletParams);
    ASSERT_NE(wallet, nullptr);
    EXPECT_TRUE(wallet->ListReceivingAddresses().empty());

    const auto first = wallet->GenerateReceivingAddress();
    const auto second = wallet->GenerateReceivingAddress();
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_NE(first->public_key_hash, second->public_key_hash);

    const auto addresses = wallet->ListReceivingAddresses();
    ASSERT_EQ(addresses.size(), 2U);
    EXPECT_EQ(addresses[0].index, 0U);
    EXPECT_EQ(addresses[1].index, 1U);
}

TEST(Wallet, SeparatesConfirmedAndUnconfirmedBalances)
{
    auto wallet = Wallet::Create(kWalletParams);
    ASSERT_NE(wallet, nullptr);
    const auto address = wallet->GenerateReceivingAddress();
    ASSERT_TRUE(address.has_value());

    EXPECT_TRUE(wallet->ObserveTransaction(
        FundingTransaction(1U, 3LL * COIN, address->public_key_hash), 101U, true));
    EXPECT_TRUE(wallet->ObserveTransaction(
        FundingTransaction(2U, 2LL * COIN, address->public_key_hash), 0U, false));
    EXPECT_EQ(wallet->ConfirmedBalance(), 3LL * COIN);
    EXPECT_EQ(wallet->UnconfirmedBalance(), 2LL * COIN);
    EXPECT_EQ(wallet->ListUTXOs().size(), 2U);
}

TEST(Wallet, BuildsSignsVerifiesAndBroadcastsRegtestSpend)
{
    auto wallet = Wallet::Create(kWalletParams);
    ASSERT_NE(wallet, nullptr);
    const auto address = wallet->GenerateReceivingAddress();
    ASSERT_TRUE(address.has_value());
    EXPECT_TRUE(wallet->ObserveTransaction(
        FundingTransaction(3U, 5LL * COIN, address->public_key_hash), 11U, true));

    const std::vector<Recipient> recipients{{address->public_key_hash, 1LL * COIN}};
    const auto built = wallet->CreateTransaction(recipients);
    ASSERT_EQ(built.error, WalletError::kNone);
    ASSERT_TRUE(built.built.has_value());
    EXPECT_EQ(built.built->selected_inputs.size(), 1U);
    EXPECT_TRUE(built.built->change_output.has_value());
    ASSERT_EQ(built.built->transaction.outputs.size(), 2U);
    EXPECT_EQ(built.built->transaction.outputs.front().script_pubkey,
              P2pkhScript(address->public_key_hash));
    EXPECT_EQ(built.built->change_output->script_pubkey, P2pkhScript(address->public_key_hash));
    EXPECT_TRUE(wallet->VerifySignaturesLocally(built.built->transaction));

    const auto funding_id =
        FundingTransaction(3U, 5LL * COIN, address->public_key_hash).TxId(kTransactionLimits);
    ASSERT_TRUE(funding_id.has_value());
    UTXOSet regtest_utxos;
    ASSERT_TRUE(regtest_utxos.AddCoin(
        UTXOKey{*funding_id, 0U},
        nova::chain::Coin{TxOutput{5LL * COIN, P2pkhScript(address->public_key_hash)}, 11U,
                          false}));
    const auto validation = ValidateTransactionAgainstUTXOSet(
        built.built->transaction, TransactionValidationContext{12U, 1'000U},
        RegtestValidationParams(), regtest_utxos);
    EXPECT_EQ(validation.error, nova::chain::BlockValidationError::kNone);

    RecordingBroadcaster accepted{true};
    EXPECT_EQ(wallet->Broadcast(built.built->transaction, accepted), WalletError::kNone);
    EXPECT_TRUE(accepted.observed_.has_value());

    RecordingBroadcaster rejected{false};
    EXPECT_EQ(wallet->Broadcast(built.built->transaction, rejected),
              WalletError::kBroadcastRejected);
}

TEST(Wallet, RejectsInsufficientAndInvalidPaymentsWithoutBroadcasting)
{
    auto wallet = Wallet::Create(kWalletParams);
    ASSERT_NE(wallet, nullptr);
    const auto address = wallet->GenerateReceivingAddress();
    ASSERT_TRUE(address.has_value());
    EXPECT_TRUE(wallet->ObserveTransaction(FundingTransaction(4U, COIN, address->public_key_hash),
                                           5U, true));

    const std::vector<Recipient> too_large{{address->public_key_hash, 2LL * COIN}};
    EXPECT_EQ(wallet->CreateTransaction(too_large).error, WalletError::kInsufficientFunds);
    const std::vector<Recipient> negative{{address->public_key_hash, -1}};
    EXPECT_EQ(wallet->CreateTransaction(negative).error, WalletError::kInvalidRecipient);
}

TEST(Wallet, LocalVerificationRejectsModifiedSignature)
{
    auto wallet = Wallet::Create(kWalletParams);
    ASSERT_NE(wallet, nullptr);
    const auto address = wallet->GenerateReceivingAddress();
    ASSERT_TRUE(address.has_value());
    EXPECT_TRUE(wallet->ObserveTransaction(
        FundingTransaction(5U, 5LL * COIN, address->public_key_hash), 7U, true));

    const std::vector<Recipient> recipients{{address->public_key_hash, COIN}};
    auto built = wallet->CreateTransaction(recipients);
    ASSERT_TRUE(built.built.has_value());
    auto modified = built.built->transaction;
    ASSERT_FALSE(modified.inputs.front().script_sig.empty());
    modified.inputs.front().script_sig[1U] ^= 0x01U;
    EXPECT_FALSE(wallet->VerifySignaturesLocally(modified));
    RecordingBroadcaster broadcaster{true};
    EXPECT_EQ(wallet->Broadcast(modified, broadcaster), WalletError::kLocalSignatureCheckFailed);
    EXPECT_FALSE(broadcaster.observed_.has_value());
}

TEST(Wallet, ChoosesLargestConfirmedCoinAndExcludesPendingAndUnconfirmedCoins)
{
    auto wallet = Wallet::Create(kWalletParams);
    ASSERT_NE(wallet, nullptr);
    const auto address = wallet->GenerateReceivingAddress();
    ASSERT_TRUE(address.has_value());
    const auto small = FundingTransaction(6U, 2LL * COIN, address->public_key_hash);
    const auto large = FundingTransaction(7U, 4LL * COIN, address->public_key_hash);
    const auto unconfirmed = FundingTransaction(8U, 10LL * COIN, address->public_key_hash);
    ASSERT_TRUE(wallet->ObserveTransaction(small, 1U, true));
    ASSERT_TRUE(wallet->ObserveTransaction(large, 2U, true));
    ASSERT_TRUE(wallet->ObserveTransaction(unconfirmed, 0U, false));
    const auto large_id = large.TxId(kTransactionLimits);
    ASSERT_TRUE(large_id.has_value());

    const std::vector<Recipient> recipients{{address->public_key_hash, COIN}};
    const auto built = wallet->CreateTransaction(recipients);
    ASSERT_TRUE(built.built.has_value());
    ASSERT_EQ(built.built->selected_inputs.size(), 1U);
    const UTXOKey expected_key{*large_id, 0U};
    EXPECT_EQ(built.built->selected_inputs.front(), expected_key);
}

TEST(Wallet, RejectsInvalidFeeParametersAndOversizedFeeEstimate)
{
    auto invalid = kWalletParams;
    invalid.fee_rate_per_byte = -1;
    EXPECT_EQ(Wallet::Create(invalid), nullptr);
    EXPECT_FALSE(nova::wallet::TransactionBuilder::EstimateFee(1U, 1U, invalid).has_value());

    auto excessive = kWalletParams;
    excessive.fee_rate_per_byte = MAX_MONEY;
    EXPECT_FALSE(nova::wallet::TransactionBuilder::EstimateFee(1U, 2U, excessive).has_value());
}

struct CleanseObservation final {
    static inline std::size_t calls{};
    static inline bool all_zero{true};
    static void AfterCleanse(const std::span<const std::uint8_t> bytes) noexcept
    {
        ++calls;
        all_zero = all_zero && std::all_of(bytes.begin(), bytes.end(),
                                           [](const auto value) { return value == 0U; });
    }
};

TEST(WalletSecureMemory, WipesGrowthMoveReplacementAndFinalAllocationBeforeFree)
{
    CleanseObservation::calls = 0U;
    CleanseObservation::all_zero = true;
    const std::array<std::uint8_t, 7U> public_dummy{'p', 'u', 'b', 'l', 'i', 'c', '!'};
    {
        nova::wallet::detail::BasicSensitiveBytes<CleanseObservation> first;
        ASSERT_TRUE(first.Append(public_dummy, 8192U));
        const auto before = CleanseObservation::calls;
        first.mutable_view().reserve(4096U);
        EXPECT_GT(CleanseObservation::calls, before);
        nova::wallet::detail::BasicSensitiveBytes<CleanseObservation> moved{std::move(first)};
        EXPECT_TRUE(first.view().empty());
        nova::wallet::detail::BasicSensitiveBytes<CleanseObservation> replaced;
        ASSERT_TRUE(replaced.Append(public_dummy, 8192U));
        replaced = std::move(moved);
        EXPECT_TRUE(std::equal(replaced.view().begin(), replaced.view().end(), public_dummy.begin(),
                               public_dummy.end()));
    }
    EXPECT_GT(CleanseObservation::calls, 0U);
    EXPECT_TRUE(CleanseObservation::all_zero);
}

TEST(WalletSecureMemory, BoundAliasAndAllocationFailurePreserveExistingBytes)
{
    using namespace nova::wallet::detail;
    const std::array<std::uint8_t, 7U> public_dummy{'p', 'u', 'b', 'l', 'i', 'c', '!'};
    BasicSensitiveBytes<CleanseObservation> bytes;
    ASSERT_TRUE(bytes.Append(public_dummy, 7U));
    EXPECT_FALSE(bytes.Append(public_dummy, 7U));
    EXPECT_FALSE(bytes.Append(bytes.view(), 64U));
    EXPECT_TRUE(bytes.Append({}, 7U));
    const auto capacity = bytes.mutable_view().capacity();
    std::vector<std::uint8_t> larger(capacity + 1U, 0x5AU); // public dummy bytes, not a scalar
    {
        ScopedSensitiveFailureForTesting fault{SensitiveFailureStage::kAllocation};
        EXPECT_FALSE(bytes.Append(larger, capacity + 8U));
        EXPECT_TRUE(fault.triggered());
    }
    EXPECT_TRUE(std::equal(bytes.view().begin(), bytes.view().end(), public_dummy.begin(),
                           public_dummy.end()));
    EXPECT_TRUE(bytes.Append(public_dummy, 64U));
}

TEST(WalletSecureMemory, CanonicalWriterMatchesExistingGrammarAtEveryIntegerWidth)
{
    nova::wallet::detail::SensitiveWriter secure{1024U};
    nova::primitives::BinaryWriter reference;
    for (const std::uint64_t value : {0ULL, 252ULL, 253ULL, 65535ULL, 65536ULL, 0xFFFFFFFFULL,
                                      0x100000000ULL, 0xFFFFFFFFFFFFFFFFULL}) {
        ASSERT_TRUE(secure.WriteCompactSize(value));
        ASSERT_TRUE(reference.WriteCompactSize(value));
    }
    ASSERT_TRUE(secure.WriteU32(0x89ABCDEFU));
    ASSERT_TRUE(reference.WriteU32(0x89ABCDEFU));
    ASSERT_TRUE(secure.WriteI64(-1));
    ASSERT_TRUE(reference.WriteI64(-1));
    const std::array<std::uint8_t, 3U> dummy{'p', 'u', 'b'};
    ASSERT_TRUE(secure.WriteBytes(dummy, 3U));
    ASSERT_TRUE(reference.WriteBytes(dummy, 3U));
    EXPECT_TRUE(std::equal(secure.bytes().begin(), secure.bytes().end(), reference.bytes().begin(),
                           reference.bytes().end()));
    nova::wallet::detail::SensitiveWriter boundary{9U};
    EXPECT_TRUE(boundary.WriteCompactSize(std::numeric_limits<std::uint64_t>::max()));
    EXPECT_EQ(boundary.bytes().size(), 9U);
    EXPECT_FALSE(boundary.WriteU8(1U));
    EXPECT_EQ(boundary.bytes().size(), 9U);
    nova::wallet::detail::SensitiveWriter rejected{16U};
    EXPECT_FALSE(rejected.WriteBytes(dummy, 2U));
    EXPECT_TRUE(rejected.bytes().empty());
}

TEST(WalletSecureMemory, ScopedFailureIsOneShotAndNestedScopeRestoresPriorStage)
{
    using namespace nova::wallet::detail;
    ScopedSensitiveFailureForTesting outer{SensitiveFailureStage::kEncryptionScratch};
    {
        ScopedSensitiveFailureForTesting inner{SensitiveFailureStage::kDecryptionScratch};
        EXPECT_THROW(SensitiveAllocationCheckpoint(SensitiveFailureStage::kDecryptionScratch),
                     std::bad_alloc);
        EXPECT_TRUE(inner.triggered());
        EXPECT_NO_THROW(SensitiveAllocationCheckpoint(SensitiveFailureStage::kDecryptionScratch));
    }
    EXPECT_THROW(SensitiveAllocationCheckpoint(SensitiveFailureStage::kEncryptionScratch),
                 std::bad_alloc);
    EXPECT_TRUE(outer.triggered());
}

TEST(WalletSecureMemory, SaveAndLoadScratchFailureReturnWithoutReplacingFileOrKeys)
{
    static_assert(!std::is_nothrow_default_constructible_v<nova::wallet::detail::SensitiveBytes>);
    static_assert(!std::is_nothrow_move_constructible_v<nova::wallet::detail::SensitiveBytes>);
    using namespace nova::wallet::detail;
    const auto path = UniqueWalletPath();
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup()
        {
            std::error_code error;
            std::filesystem::remove(path, error);
        }
    } cleanup{path};
    const std::vector<std::uint8_t> pass{'t', 'e', 's', 't', '-', 'o', 'n', 'l', 'y'};
    auto wallet = Wallet::Create(kWalletParams);
    ASSERT_NE(wallet, nullptr);
    const auto address = wallet->GenerateReceivingAddress();
    ASSERT_TRUE(address.has_value());
    ASSERT_TRUE(wallet->SaveEncrypted(path, pass));
    auto read = [&] {
        std::ifstream input{path, std::ios::binary};
        return std::vector<char>{std::istreambuf_iterator<char>{input},
                                 std::istreambuf_iterator<char>{}};
    };
    const auto original = read(); // encrypted disposable test image, never logged
    {
        ScopedSensitiveFailureForTesting fault{SensitiveFailureStage::kEncryptionScratch};
        EXPECT_FALSE(wallet->SaveEncrypted(path, pass));
        EXPECT_TRUE(fault.triggered());
    }
    EXPECT_TRUE(read() == original);
    {
        ScopedSensitiveFailureForTesting fault{SensitiveFailureStage::kFileStaging};
        EXPECT_FALSE(wallet->SaveEncrypted(path, pass));
        EXPECT_TRUE(fault.triggered());
    }
    EXPECT_TRUE(read() == original);
    {
        ScopedSensitiveFailureForTesting fault{SensitiveFailureStage::kAllocation};
        EXPECT_FALSE(wallet->SaveEncrypted(path, pass));
        EXPECT_TRUE(fault.triggered());
    }
    EXPECT_TRUE(read() == original);
    {
        ScopedSensitiveFailureForTesting fault{SensitiveFailureStage::kDecryptionScratch};
        EXPECT_EQ(Wallet::LoadEncrypted(kWalletParams, path, pass), nullptr);
        EXPECT_TRUE(fault.triggered());
    }
    ASSERT_EQ(wallet->ListReceivingAddresses().size(), 1U);
    EXPECT_EQ(wallet->ListReceivingAddresses().front().public_key_hash, address->public_key_hash);
    const auto restored = Wallet::LoadEncrypted(kWalletParams, path, pass);
    ASSERT_NE(restored, nullptr);
    ASSERT_EQ(restored->ListReceivingAddresses().size(), 1U);
    EXPECT_EQ(restored->ListReceivingAddresses().front().public_key_hash, address->public_key_hash);
}

TEST(WalletSecureMemory, ReadsOriginalVersionOneEmptyFixtureWithoutReencodingIt)
{
    const auto fixture = std::filesystem::path{__FILE__}.parent_path().parent_path() /
                         "test/data/wallet/empty-v1.nwlt";
    const std::vector<std::uint8_t> public_passphrase{'e', 'm', 'p', 't', 'y', '-', 'v', '1',
                                                      '-', 'f', 'i', 'x', 't', 'u', 'r', 'e'};
    const auto restored = Wallet::LoadEncrypted(kWalletParams, fixture, public_passphrase);
    ASSERT_NE(restored, nullptr);
    EXPECT_TRUE(restored->ListReceivingAddresses().empty());
    EXPECT_TRUE(restored->ListUTXOs().empty());
    EXPECT_EQ(restored->ConfirmedBalance(), 0);
    EXPECT_EQ(restored->UnconfirmedBalance(), 0);
}

TEST(Wallet, EncryptedPersistenceRestoresKeysAndUtxosAfterRestart)
{
    const auto path = UniqueWalletPath();
    const std::vector<std::uint8_t> passphrase{'r', 'e', 'g', 't', 'e', 's', 't'};
    auto wallet = Wallet::Create(kWalletParams);
    ASSERT_NE(wallet, nullptr);
    const auto first = wallet->GenerateReceivingAddress();
    const auto second = wallet->GenerateReceivingAddress();
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    ASSERT_TRUE(wallet->ObserveTransaction(
        FundingTransaction(21U, 3LL * COIN, first->public_key_hash), 21U, true));
    ASSERT_TRUE(wallet->ObserveTransaction(
        FundingTransaction(22U, 2LL * COIN, second->public_key_hash), 0U, false));

    ASSERT_TRUE(wallet->SaveEncrypted(path, passphrase));
    const auto reloaded = Wallet::LoadEncrypted(kWalletParams, path, passphrase);
    ASSERT_NE(reloaded, nullptr);
    const auto addresses = reloaded->ListReceivingAddresses();
    ASSERT_EQ(addresses.size(), 2U);
    EXPECT_EQ(addresses[0].public_key_hash, first->public_key_hash);
    EXPECT_EQ(addresses[1].public_key_hash, second->public_key_hash);
    EXPECT_EQ(reloaded->ConfirmedBalance(), 3LL * COIN);
    EXPECT_EQ(reloaded->UnconfirmedBalance(), 2LL * COIN);
    EXPECT_EQ(reloaded->ListUTXOs().size(), 2U);
    EXPECT_FALSE(std::filesystem::exists(path.string() + ".tmp"));
    std::filesystem::remove(path);
}

TEST(Wallet, EncryptedPersistenceRejectsWrongPassphraseAndTampering)
{
    const auto path = UniqueWalletPath();
    const std::vector<std::uint8_t> passphrase{'c', 'o', 'r', 'r', 'e', 'c', 't'};
    const std::vector<std::uint8_t> wrong_passphrase{'w', 'r', 'o', 'n', 'g'};
    auto wallet = Wallet::Create(kWalletParams);
    ASSERT_NE(wallet, nullptr);
    ASSERT_TRUE(wallet->GenerateReceivingAddress().has_value());
    ASSERT_TRUE(wallet->SaveEncrypted(path, passphrase));
    EXPECT_EQ(Wallet::LoadEncrypted(kWalletParams, path, wrong_passphrase), nullptr);

    std::fstream file{path, std::ios::binary | std::ios::in | std::ios::out};
    ASSERT_TRUE(file.is_open());
    file.seekg(-1, std::ios::end);
    char byte{};
    file.read(&byte, 1);
    byte ^= 0x01;
    file.seekp(-1, std::ios::end);
    file.write(&byte, 1);
    file.close();
    EXPECT_EQ(Wallet::LoadEncrypted(kWalletParams, path, passphrase), nullptr);
    std::filesystem::remove(path);
}

TEST(Wallet, EncryptedPersistenceRejectsEmptyPassphrase)
{
    const auto path = UniqueWalletPath();
    auto wallet = Wallet::Create(kWalletParams);
    ASSERT_NE(wallet, nullptr);
    EXPECT_FALSE(wallet->SaveEncrypted(path, {}));
    EXPECT_EQ(Wallet::LoadEncrypted(kWalletParams, path, {}), nullptr);
}

nova::chain::BlockIndex WalletObservationBlock(const Transaction& transaction)
{
    nova::chain::BlockIndex index;
    index.height = 11U;
    index.block.emplace();
    index.block->transactions.push_back(transaction);
    return index;
}

TEST(Wallet, PreparedConfirmedStatePublishesOnlyOnceAndPreservesUnconfirmedCoins)
{
    static_assert(!std::is_move_constructible_v<Wallet>);
    static_assert(!std::is_copy_constructible_v<Wallet::PreparedConfirmedState>);
    auto wallet = Wallet::Create(kWalletParams);
    ASSERT_NE(wallet, nullptr);
    const auto address = wallet->GenerateReceivingAddress();
    ASSERT_TRUE(address.has_value());
    ASSERT_TRUE(wallet->ObserveTransaction(
        FundingTransaction(60U, 3LL * COIN, address->public_key_hash), 1U, true));
    ASSERT_TRUE(wallet->ObserveTransaction(FundingTransaction(61U, COIN, address->public_key_hash),
                                           0U, false));
    const std::array active{
        WalletObservationBlock(FundingTransaction(62U, 2LL * COIN, address->public_key_hash))};
    auto prepared = wallet->PrepareConfirmedState(active);
    ASSERT_NE(prepared, nullptr);
    EXPECT_EQ(wallet->ConfirmedBalance(), 3LL * COIN);
    EXPECT_EQ(wallet->UnconfirmedBalance(), COIN);
    ASSERT_TRUE(wallet->CommitPreparedState(*prepared));
    EXPECT_EQ(wallet->ConfirmedBalance(), 2LL * COIN);
    EXPECT_EQ(wallet->UnconfirmedBalance(), COIN);
    const auto coins = wallet->ListUTXOs();
    ASSERT_EQ(coins.size(), 2U);
    EXPECT_EQ(coins[0].height == 11U || coins[1].height == 11U, true);
    EXPECT_FALSE(wallet->CommitPreparedState(*prepared));
    EXPECT_EQ(wallet->ConfirmedBalance(), 2LL * COIN);
}

TEST(Wallet, PreparedStateRejectsWrongOwnerStaleAndDestroyedOwner)
{
    auto wallet = Wallet::Create(kWalletParams);
    auto other = Wallet::Create(kWalletParams);
    ASSERT_NE(wallet, nullptr);
    ASSERT_NE(other, nullptr);
    auto prepared = wallet->PrepareConfirmedState({});
    ASSERT_NE(prepared, nullptr);
    EXPECT_FALSE(other->CommitPreparedState(*prepared));
    ASSERT_TRUE(wallet->GenerateReceivingAddress().has_value());
    EXPECT_FALSE(wallet->CommitPreparedState(*prepared));
    auto current = wallet->PrepareConfirmedState({});
    ASSERT_NE(current, nullptr);
    wallet.reset();
    auto replacement = Wallet::Create(kWalletParams);
    ASSERT_NE(replacement, nullptr);
    EXPECT_FALSE(replacement->CommitPreparedState(*current));
    EXPECT_TRUE(replacement->ListUTXOs().empty());
}

TEST(Wallet, PreparedStateInvalidatesOnObservationClearAndSiblingCommit)
{
    auto wallet = Wallet::Create(kWalletParams);
    ASSERT_NE(wallet, nullptr);
    const auto address = wallet->GenerateReceivingAddress();
    ASSERT_TRUE(address.has_value());
    auto before_observe = wallet->PrepareConfirmedState({});
    ASSERT_NE(before_observe, nullptr);
    ASSERT_TRUE(wallet->ObserveTransaction(FundingTransaction(63U, COIN, address->public_key_hash),
                                           1U, true));
    EXPECT_FALSE(wallet->CommitPreparedState(*before_observe));
    auto before_clear = wallet->PrepareConfirmedState({});
    ASSERT_NE(before_clear, nullptr);
    wallet->ClearConfirmedState();
    EXPECT_FALSE(wallet->CommitPreparedState(*before_clear));
    auto first = wallet->PrepareConfirmedState({});
    auto sibling = wallet->PrepareConfirmedState({});
    ASSERT_NE(first, nullptr);
    ASSERT_NE(sibling, nullptr);
    ASSERT_TRUE(wallet->CommitPreparedState(*first));
    EXPECT_FALSE(wallet->CommitPreparedState(*sibling));
}

TEST(Wallet, PreparedRebuildFailureDoesNotPublishEarlierTransactions)
{
    auto wallet = Wallet::Create(kWalletParams);
    ASSERT_NE(wallet, nullptr);
    const auto address = wallet->GenerateReceivingAddress();
    ASSERT_TRUE(address.has_value());
    const auto original = FundingTransaction(64U, 3LL * COIN, address->public_key_hash);
    ASSERT_TRUE(wallet->ObserveTransaction(original, 1U, true));
    auto block = WalletObservationBlock(FundingTransaction(65U, COIN, address->public_key_hash));
    block.block->transactions.push_back(Transaction{});
    const std::array active{block};
    EXPECT_EQ(wallet->PrepareConfirmedState(active), nullptr);
    EXPECT_FALSE(wallet->RebuildConfirmedState(active));
    EXPECT_EQ(wallet->ConfirmedBalance(), 3LL * COIN);
    const auto coins = wallet->ListUTXOs();
    ASSERT_EQ(coins.size(), 1U);
    EXPECT_EQ(coins.front().key.transaction_id, *original.TxId(kTransactionLimits));
    EXPECT_EQ(coins.front().height, 1U);
}

TEST(Wallet, PreparedEncryptedImageRestoresCandidateWithoutPublishingLiveState)
{
    const auto path = UniqueWalletPath();
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup()
        {
            std::error_code error;
            std::filesystem::remove(path, error);
        }
    } cleanup{path};
    const std::vector<std::uint8_t> passphrase{'s', 't', 'a', 'g', 'e', 'd'};
    auto wallet = Wallet::Create(kWalletParams);
    auto other = Wallet::Create(kWalletParams);
    ASSERT_NE(wallet, nullptr);
    ASSERT_NE(other, nullptr);
    const auto address = wallet->GenerateReceivingAddress();
    ASSERT_TRUE(address.has_value());
    const std::array active{
        WalletObservationBlock(FundingTransaction(66U, COIN, address->public_key_hash))};
    auto prepared = wallet->PrepareConfirmedState(active);
    ASSERT_NE(prepared, nullptr);
    EXPECT_FALSE(other->SavePreparedEncrypted(*prepared, path, passphrase));
    EXPECT_FALSE(std::filesystem::exists(path));
    EXPECT_FALSE(wallet->SavePreparedEncrypted(*prepared, path, {}));
    EXPECT_FALSE(std::filesystem::exists(path));
    ASSERT_TRUE(wallet->SavePreparedEncrypted(*prepared, path, passphrase));
    EXPECT_EQ(wallet->ConfirmedBalance(), 0);
    const auto restored = Wallet::LoadEncrypted(kWalletParams, path, passphrase);
    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(restored->ConfirmedBalance(), COIN);
    ASSERT_EQ(restored->ListUTXOs().size(), 1U);
    EXPECT_EQ(restored->ListUTXOs().front().height, 11U);
    ASSERT_EQ(restored->ListReceivingAddresses().size(), 1U);
    EXPECT_EQ(restored->ListReceivingAddresses().front().public_key_hash, address->public_key_hash);
    ASSERT_TRUE(wallet->CommitPreparedState(*prepared));
    EXPECT_EQ(wallet->ConfirmedBalance(), COIN);
    EXPECT_FALSE(wallet->SavePreparedEncrypted(*prepared, path, passphrase));
}

TEST(Wallet, FailedPreparedSaveAndObservationLeaveCandidateUsable)
{
    const auto path = UniqueWalletPath();
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup()
        {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    } cleanup{path};
    ASSERT_TRUE(std::filesystem::create_directory(path));
    const std::vector<std::uint8_t> passphrase{'r', 'e', 't', 'r', 'y'};
    auto wallet = Wallet::Create(kWalletParams);
    ASSERT_NE(wallet, nullptr);
    const auto address = wallet->GenerateReceivingAddress();
    ASSERT_TRUE(address.has_value());
    const std::array active{
        WalletObservationBlock(FundingTransaction(67U, COIN, address->public_key_hash))};
    auto prepared = wallet->PrepareConfirmedState(active);
    ASSERT_NE(prepared, nullptr);
    EXPECT_FALSE(wallet->SavePreparedEncrypted(*prepared, path, passphrase));
    EXPECT_TRUE(std::filesystem::is_directory(path));
    EXPECT_EQ(wallet->ConfirmedBalance(), 0);
    EXPECT_FALSE(wallet->ObserveTransaction(Transaction{}, 1U, true));
    const auto image = path / "prepared.dat";
    ASSERT_TRUE(wallet->SavePreparedEncrypted(*prepared, image, passphrase));
    const auto restored = Wallet::LoadEncrypted(kWalletParams, image, passphrase);
    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(restored->ConfirmedBalance(), COIN);
    ASSERT_TRUE(wallet->CommitPreparedState(*prepared));
    EXPECT_EQ(wallet->ConfirmedBalance(), COIN);
}

} // namespace
