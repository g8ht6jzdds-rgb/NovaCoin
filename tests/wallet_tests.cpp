#include <gtest/gtest.h>

#include "chain/validation.hpp"
#include "consensus/monetary.hpp"
#include "consensus/pow.hpp"
#include "wallet/wallet.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
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
    EXPECT_EQ(built.built->transaction.outputs.front().script_pub_key,
              P2pkhScript(address->public_key_hash));
    EXPECT_EQ(built.built->change_output->script_pub_key, P2pkhScript(address->public_key_hash));
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

} // namespace
